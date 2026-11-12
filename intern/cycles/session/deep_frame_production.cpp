/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_frame_production_internal.h"
#include "device/deep_queue_memory.h"
#include "integrator/deep_gpu_frame_work.h"
#include "session/deep_emission_expression.h"
#include "session/buffers.h"
#include "util/half.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

CCL_NAMESPACE_BEGIN

using namespace deep_production_detail;

DeepFrameProduction::DeepFrameProduction(const DeepFrameKey &frame,
                                         const DeepRasterWindow &window,
                                         const size_t memory_bytes,
                                         const std::span<const uint32_t> consumers,
                                         const bool retain_curve_sources,
                                         const DeepNativeCurvePolicy &curve_policy,
                                         const DeepProductionRoute route)
    : pipeline_(memory_bytes),
      device_budget_(pipeline_.memory_budget()),
      retain_curve_sources_(retain_curve_sources),
      route_(route),
      curve_policy_(curve_policy)
{
  initialize(frame, window, consumers);
}
DeepFrameProduction::DeepFrameProduction(const DeepFrameKey &frame,
                                         const DeepRasterWindow &window,
                                         const DeepMemoryBudget &budget,
                                         const std::span<const uint32_t> consumers,
                                         const bool retain_curve_sources,
                                         const DeepNativeCurvePolicy &curve_policy,
                                         const DeepProductionRoute route)
    : DeepFrameProduction(frame,
                          window,
                          budget,
                          budget,
                          consumers,
                          retain_curve_sources,
                          make_int2(0, 0),
                          curve_policy,
                          route)
{
}
DeepFrameProduction::DeepFrameProduction(const DeepFrameKey &frame,
                                         const DeepRasterWindow &window,
                                         const DeepMemoryBudget &host_budget,
                                         const DeepMemoryBudget &device_budget,
                                         const std::span<const uint32_t> consumers,
                                         const bool retain_curve_sources,
                                         const int2 native_to_canonical,
                                         const DeepNativeCurvePolicy &curve_policy,
                                         const DeepProductionRoute route)
    : pipeline_(host_budget),
      device_budget_(device_budget),
      native_to_canonical_(native_to_canonical),
      retain_curve_sources_(retain_curve_sources),
      route_(route),
      curve_policy_(curve_policy)
{
  initialize(frame, window, consumers);
}
DeepFrameProduction::~DeepFrameProduction() = default;

void DeepFrameProduction::initialize(const DeepFrameKey &frame,
                                     const DeepRasterWindow &window,
                                     const std::span<const uint32_t> consumers)
{
  if (!pipeline_.begin_frame(frame, window, consumers))
    return;
  try {
    storage_ = std::allocate_shared<DeepFrameProductionStorage>(
        DeepBudgetAllocator<DeepFrameProductionStorage>(memory_budget()), memory_budget());
    storage_->frame = frame;
    storage_->window = window;
    storage_->consumer_ids.assign(consumers.begin(), consumers.end());
    if (uint64_t(window.width) * window.height > std::numeric_limits<size_t>::max()) {
      fail(DeepErrorCode::InvalidCount, "Deep production pixel count overflow");
      return;
    }
    const size_t count = size_t(window.width) * window.height;
    storage_->pixels.resize(count);
    if (route_ == DeepProductionRoute::Reference) {
      storage_->reference = std::allocate_shared<ReferenceStorage>(
          DeepBudgetAllocator<ReferenceStorage>(memory_budget()), memory_budget(), *this);
      storage_->reference->curves.resize(count);
    }
  }
  catch (const std::bad_alloc &) {
    fail(DeepErrorCode::BudgetExceeded, "Deep production owner quota exhausted");
  }
  catch (const std::length_error &) {
    fail(DeepErrorCode::InvalidCount, "Deep production owner count overflow");
  }
}

DeepFramePipeline &DeepFrameProduction::pipeline()
{
  return pipeline_;
}
const DeepFramePipeline &DeepFrameProduction::pipeline() const
{
  return pipeline_;
}
const DeepStatus &DeepFrameProduction::error() const
{
  return pipeline_.error();
}
DeepStatus DeepFrameProduction::configure_consumers(
    const std::span<const DeepFrozenConsumer> consumers)
{
  const auto invalid = [&](const char *message) {
    return pipeline_.check_producer_status(DeepStatus::fail(DeepErrorCode::InvalidState, message));
  };
  if (!storage_ || !error())
    return error();
  if (storage_->consumers_frozen || !storage_->domains.empty() ||
      pipeline_.state() != DeepFrameState::Capturing ||
      consumers.size() != storage_->consumer_ids.size())
    return invalid("Deep consumer configuration must freeze once before native admission");
  for (size_t i = 0; i < consumers.size(); i++) {
    const auto &consumer = consumers[i];
    if (!consumer.id || !consumer.source_key || !consumer.config_version ||
        std::find(storage_->consumer_ids.begin(), storage_->consumer_ids.end(), consumer.id) ==
            storage_->consumer_ids.end())
      return invalid("Unknown or invalid frozen Deep consumer identity");
    if (const auto status = deep_validate_canonical_encode_config(consumer.config); !status)
      return pipeline_.check_producer_status(status);
    for (size_t j = 0; j < i; j++)
      if (consumers[j].id == consumer.id || consumers[j].source_key == consumer.source_key)
        return invalid("Duplicate or ambiguous frozen Deep consumer identity");
  }
  try {
    storage_->consumers.assign(consumers.begin(), consumers.end());
    storage_->pixel_cache_tiles.assign(storage_->pixels.size(), no_index);
    storage_->encoding_reports.resize(consumers.size());
  }
  catch (const std::bad_alloc &) {
    return pipeline_.check_producer_status(DeepStatus::fail(
        DeepErrorCode::BudgetExceeded, "Frozen Deep consumers exceed frame quota"));
  }
  storage_->consumers_frozen = true;
  return {};
}

std::span<const DeepFrozenConsumer> DeepFrameProduction::frozen_consumers() const
{
  return storage_ ? std::span<const DeepFrozenConsumer>(storage_->consumers) :
                    std::span<const DeepFrozenConsumer>();
}

DeepCaptureDemand DeepFrameProduction::capture_demand() const
{
  DeepCaptureDemand demand;
  if (storage_) {
    demand.simple_mode = storage_->frame.simple_mode;
    for (const auto &consumer : storage_->consumers)
      demand.needs_rgb |= !consumer.config.alpha_only;
  }
  return demand;
}

DeepStatus DeepFrameProduction::consumer_token(const uint64_t source_key,
                                               const DeepCanonicalEncodeConfig &config,
                                               const int compression,
                                               DeepConsumerToken &token) const
{
  token = {};
  if (!storage_ || !error())
    return error();
  if (!storage_->consumers_frozen)
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep consumers were not frozen");
  for (const auto &consumer : storage_->consumers) {
    if (consumer.source_key != source_key)
      continue;
    if (!(consumer.config == config) || consumer.compression != compression)
      return DeepStatus::fail(DeepErrorCode::InvalidState,
                              "Frozen Deep consumer parameters changed; render a new frame");
    token = {storage_->frame, consumer.id, consumer.config_version};
    return {};
  }
  return DeepStatus::fail(DeepErrorCode::InvalidReference,
                          "Unknown or late Deep consumer; render a new frame");
}

DeepStatus DeepFrameProduction::validate_consumer_token(const DeepConsumerToken &token) const
{
  if (!storage_ || !error())
    return error();
  if (!(token.frame == storage_->frame))
    return DeepStatus::fail(DeepErrorCode::InvalidFrame,
                            "Deep consumer token belongs to another frame");
  for (const auto &consumer : storage_->consumers)
    if (consumer.id == token.id)
      return token.config_version == consumer.config_version ?
                 DeepStatus{} :
                 DeepStatus::fail(DeepErrorCode::InvalidState,
                                  "Stale Deep consumer config version");
  return DeepStatus::fail(DeepErrorCode::InvalidReference, "Unknown Deep consumer token");
}

DeepStatus DeepFrameProduction::encoding_report(const DeepConsumerToken &token,
                                                DeepCanonicalEncodeReport &report) const
{
  report = {};
  if (const auto status = validate_consumer_token(token); !status)
    return status;
  if (std::any_of(storage_->pixels.begin(), storage_->pixels.end(), [](const PixelState &pixel) {
        return !pixel.closed;
      }))
    return DeepStatus::fail(DeepErrorCode::Incomplete,
                            "Deep encoding report requires complete pixels");
  for (size_t i = 0; i < storage_->consumers.size(); i++)
    if (storage_->consumers[i].id == token.id) {
      report = storage_->encoding_reports[i];
      return {};
    }
  return DeepStatus::fail(DeepErrorCode::InvalidReference, "Unknown encoded consumer");
}

std::span<const DeepNativeCurve> DeepFrameProduction::curves() const
{
  return storage_ && storage_->reference ? std::span<const DeepNativeCurve>(storage_->reference->curves) :
                    std::span<const DeepNativeCurve>();
}
const DeepMemoryBudget &DeepFrameProduction::memory_budget() const
{
  return pipeline_.memory_budget();
}
const DeepMemoryBudget &DeepFrameProduction::device_memory_budget() const
{
  return device_budget_;
}
bool DeepFrameProduction::fail(const DeepErrorCode code, const std::string &message)
{
  pipeline_.check_producer_status(DeepStatus::fail(code, message));
  return false;
}

bool DeepFrameProduction::begin_reference_capture(const DeepNativeCaptureDomain &native_domain)
{
  if (route_ != DeepProductionRoute::Reference || bounded_processing_ ||
      !storage_ || !storage_->reference || !error())
    return false;
  DeepNativeCaptureDomain domain = native_domain;
  if (!translated_coordinate(native_domain.x, native_to_canonical_.x, domain.x) ||
      !translated_coordinate(native_domain.y, native_to_canonical_.y, domain.y) ||
      (native_domain.core_width &&
       (!translated_coordinate(native_domain.core_x, native_to_canonical_.x, domain.core_x) ||
        !translated_coordinate(native_domain.core_y, native_to_canonical_.y, domain.core_y))))
    return fail(DeepErrorCode::InvalidLayout, "Native Deep raster mapping overflow");
  if (deep_frame_pipeline_requested(std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
    std::fprintf(stderr,
                 "DEEP_FRAME_DOMAIN_LAYOUT domain=%llu canonical_x=%d canonical_y=%d native_w=%d "
                 "native_h=%d frame_x=%d frame_y=%d frame_w=%u frame_h=%u start=%d samples=%d "
                 "offset=%d device_generation=%llu layout_generation=%llu error=%u\n",
                 static_cast<unsigned long long>(domain.id),
                 domain.x,
                 domain.y,
                 domain.width,
                 domain.height,
                 storage_->window.x,
                 storage_->window.y,
                 storage_->window.width,
                 storage_->window.height,
                 domain.start_sample,
                 domain.samples_num,
                 domain.sample_offset,
                 static_cast<unsigned long long>(domain.device_generation),
                 static_cast<unsigned long long>(domain.layout_generation),
                 domain.error_bits);
  size_t first;
  if (!domain.id || !domain.device_generation || !domain.layout_generation || domain.error_bits ||
      domain.start_sample < 0 || domain.samples_num <= 0 || domain.sample_offset < 0 ||
      !storage_->tile(domain.x, domain.y, domain.width, domain.height, first))
    return fail(DeepErrorCode::InvalidLayout, "Invalid native Deep production domain binding");
  const bool has_core = domain.core_width || domain.core_height;
  if (has_core &&
      (domain.core_width <= 0 || domain.core_height <= 0 || domain.core_x < domain.x ||
       domain.core_y < domain.y ||
       int64_t(domain.core_x) + domain.core_width > int64_t(domain.x) + domain.width ||
       int64_t(domain.core_y) + domain.core_height > int64_t(domain.y) + domain.height))
    return fail(DeepErrorCode::InvalidLayout, "Native output core lies outside evaluated halo");
  const DeepRasterWindow core{has_core ? domain.core_x : domain.x,
                              has_core ? domain.core_y : domain.y,
                              uint32_t(has_core ? domain.core_width : domain.width),
                              uint32_t(has_core ? domain.core_height : domain.height)};
  for (const auto &known : storage_->domains)
    if (known.binding.id == domain.id)
      return fail(DeepErrorCode::InvalidReference, "Native Deep domain already admitted");
  try {
    if (const auto status = pipeline_.bind_native_domain(
            completion(storage_->frame, domain, false), core);
        !status)
      return false;
    storage_->domains.push_back({domain, false});
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Native Deep domain registry quota exhausted");
  }
  catch (const std::length_error &) {
    return fail(DeepErrorCode::InvalidCount, "Native Deep domain registry count overflow");
  }
}

bool DeepFrameProduction::enable_gpu_processing()
{
  return enable_bounded_processing(false);
}

bool DeepFrameProduction::enable_cpu_processing()
{
  return enable_bounded_processing(true);
}

bool DeepFrameProduction::enable_bounded_processing(const bool cpu)
{
  if (!storage_ || !error() || !storage_->consumers_frozen || !storage_->domains.empty()) return false;
  for (const auto &consumer : storage_->consumers) {
    if (consumer.config.rgb_absolute_tolerance != 0.006 ||
        consumer.config.rgb_relative_tolerance != 0.002)
      return fail(DeepErrorCode::InvalidState,
                  "Bounded Deep supports fixed RGB tolerances 0.006 absolute / 0.002 relative");
  }
  if (const auto status = pipeline_.enable_device_encoding(); !status) return false;
  storage_->reference.reset();
  storage_->pixel_cache_tiles.resize(storage_->pixels.size(), no_index);
  bounded_processing_ = true;
  cpu_processing_ = cpu;
  return true;
}



bool deep_extract_bounded_beauty(
    const RenderBuffers &buffers,
    const DeepRasterWindow &w,
    std::vector<float, DeepBudgetAllocator<float>> &beauty)
{
  const auto &p = buffers.params;
  const int rgb = p.get_pass_offset(PASS_COMBINED, PassMode::NOISY);
  const int samples = p.get_pass_offset(PASS_SAMPLE_COUNT);
  const int64_t x0 = int64_t(w.x) - p.full_x;
  const int64_t y0 = int64_t(w.y) - p.full_y;
  if (rgb < 0 || rgb + 3 > p.pass_stride || samples < 0 || samples >= p.pass_stride ||
      p.stride < p.width || p.pass_stride <= 0 || x0 < 0 || y0 < 0 ||
      x0 + w.width > p.width || y0 + w.height > p.height || !buffers.buffer.data() ||
      buffers.buffer.size() < size_t(p.stride) * p.height * p.pass_stride)
  {
    return false;
  }
  beauty.resize(size_t(w.width) * w.height * 3);
  for (uint32_t y = 0; y < w.height; ++y) {
    for (uint32_t x = 0; x < w.width; ++x) {
      const float *pixel = buffers.buffer.data() + ((y0 + y) * p.stride + x0 + x) * p.pass_stride;
      uint32_t population;
      std::memcpy(&population, pixel + samples, sizeof(population));
      const float scale = population ? p.exposure / float(population) : 0.0f;
      float *dst = beauty.data() + (size_t(y) * w.width + x) * 3;
      for (int c = 0; c < 3; ++c) {
        dst[c] = population ? pixel[rgb + c] * scale : 0.0f;
      }
    }
  }
  return true;
}

bool DeepFrameProduction::set_final_bounded_beauty(const RenderBuffers &buffers)
{
  if (!storage_ || !error() || !awaits_final_bounded_beauty()) {
    return false;
  }
  auto window = storage_->window;
  const int64_t x = int64_t(window.x) - native_to_canonical_.x;
  const int64_t y = int64_t(window.y) - native_to_canonical_.y;
  if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
    return fail(DeepErrorCode::InvalidLayout, "Final Deep Beauty raster translation overflow");
  }
  window.x = int32_t(x);
  window.y = int32_t(y);
  try {
    using Color = std::vector<float, DeepBudgetAllocator<float>>;
    auto rgb = std::allocate_shared<Color>(DeepBudgetAllocator<Color>(memory_budget()),
                                           DeepBudgetAllocator<float>(memory_budget()));
    if (!deep_extract_bounded_beauty(buffers, window, *rgb)) {
      return fail(DeepErrorCode::InvalidLayout, "Final Deep Beauty Film source is incompatible");
    }
    final_beauty_ = std::move(rgb);
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Final Deep Beauty exceeds the host quota");
  }
}

bool DeepFrameProduction::publish_bounded_tile(DeepBoundedHostTile &&source)
{
  if (!bounded_processing_ || !storage_ || !error() || storage_->next_tile == UINT64_MAX ||
      source.consumers.size() != storage_->consumers.size())
  {
    return fail(DeepErrorCode::InvalidState, "Invalid bounded Deep tile publication");
  }
  auto &tile = source.window;
  if (!translated_coordinate(tile.x, native_to_canonical_.x, tile.x) ||
      !translated_coordinate(tile.y, native_to_canonical_.y, tile.y))
  {
    return fail(DeepErrorCode::InvalidLayout, "Bounded Deep tile raster translation overflow");
  }
  size_t first;
  if (!storage_->tile(tile.x, tile.y, tile.width, tile.height, first)) {
    return fail(DeepErrorCode::InvalidLayout, "Bounded Deep tile outside frame");
  }
  const size_t pixels = size_t(tile.width) * tile.height;
  for (size_t i = 0; i < source.consumers.size(); ++i) {
    const auto &payload = source.consumers[i];
    if (!payload || payload->offsets.size() != pixels + 1 ||
        payload->offsets.back() != payload->count ||
        (payload->simple_rgb && source.beauty.size() != pixels * 3 &&
         !(deferred_beauty_ && source.beauty.empty())) ||
        storage_->encoding_reports[i].output_samples > UINT64_MAX - payload->count)
    {
      return fail(DeepErrorCode::InvalidPayload, "Incomplete bounded Deep host payload");
    }
  }
  try {
    auto encoded = std::allocate_shared<DeepEncodedTileStorage>(
        DeepBudgetAllocator<DeepEncodedTileStorage>(memory_budget()), memory_budget());
    encoded->window = tile;
    encoded->bounded = std::allocate_shared<DeepBoundedHostTile>(
        DeepBudgetAllocator<DeepBoundedHostTile>(memory_budget()), std::move(source));
    if (!pipeline_.close_bounded_device_tile(storage_->frame, storage_->next_tile++, encoded->window)) {
      return false;
    }
    const size_t tile_index = storage_->encoded_tiles.size();
    storage_->encoded_tiles.push_back(encoded);
    for (uint32_t y = 0; y < encoded->window.height; ++y) {
      for (uint32_t x = 0; x < encoded->window.width; ++x) {
        const size_t pixel = first + size_t(y) * storage_->window.width + x;
        storage_->pixels[pixel].closed = true;
        storage_->pixel_cache_tiles[pixel] = tile_index;
      }
    }
    for (size_t i = 0; i < encoded->bounded->consumers.size(); ++i) {
      auto &report = storage_->encoding_reports[i];
      report.output_samples += encoded->bounded->consumers[i]->count;
      report.bounded_payload_checked = report.quantized_checked = true;
    }
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Bounded Deep host payload quota exhausted");
  }
}

void DeepFrameProduction::abort_native_capture(const uint64_t domain_id, const std::string &reason)
{
  if (!storage_ || !error())
    return;
  const auto domain = std::find_if(storage_->domains.begin(),
                                   storage_->domains.end(),
                                   [&](const auto &d) { return d.binding.id == domain_id; });
  if (domain == storage_->domains.end() || domain->complete)
    fail(DeepErrorCode::InvalidReference,
         "Native Deep abort refers to an unknown/completed domain");
  else
    fail(DeepErrorCode::Incomplete, reason.empty() ? "Native Deep capture aborted" : reason);
}

void DeepFrameProduction::cancel_native_capture(const uint64_t domain_id)
{
  if (!storage_ || !error())
    return; /* A later user cancellation cannot replace the first real failure. */
  const auto domain = std::find_if(storage_->domains.begin(),
                                   storage_->domains.end(),
                                   [&](const auto &d) { return d.binding.id == domain_id; });
  if (domain == storage_->domains.end() || domain->complete)
    fail(DeepErrorCode::InvalidReference,
         "Native Deep cancellation refers to an unknown/completed domain");
  else
    pipeline_.cancel();
}

DeepStatus DeepFrameProduction::finish_inputs()
{
  if (!storage_ || !error())
    return error();
  std::fprintf(
      stderr,
      "DEEP_FRAME_CAPTURE_MEMORY host_peak=%llu device_peak=%llu host_used=%llu device_used=%llu "
      "host_quota=%llu device_quota=%llu\n",
      static_cast<unsigned long long>(memory_budget().peak()),
      static_cast<unsigned long long>(device_memory_budget().peak()),
      static_cast<unsigned long long>(memory_budget().used()),
      static_cast<unsigned long long>(device_memory_budget().used()),
      static_cast<unsigned long long>(memory_budget().quota()),
      static_cast<unsigned long long>(device_memory_budget().quota()));
  const auto auxiliary = DeepQueueAuxiliaryBudget::statistics();
  std::fprintf(stderr,
      "DEEP_QUEUE_AUXILIARY_MEMORY reserved_credit=%llu pinned_resident=%llu process_pinned_peak=%llu "
      "frame_extra_credit=%llu frame_allocated_current=%llu\n",
      static_cast<unsigned long long>(auxiliary.reserved),
      static_cast<unsigned long long>(auxiliary.pinned_resident),
      static_cast<unsigned long long>(auxiliary.process_pinned_peak),
      static_cast<unsigned long long>(memory_budget().reserved_credit()),
      static_cast<unsigned long long>(memory_budget().used() - memory_budget().reserved_credit()));
  return pipeline_.seal_frame_inputs();
}
CCL_NAMESPACE_END
