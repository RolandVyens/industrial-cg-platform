/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_bounded_work_internal.h"
#include "integrator/deep_bounded_cpu.h"
#include "integrator/deep_bounded_plan.h"

#include "device/device.h"
#include "device/memory.h"
#include "device/queue.h"
#include "kernel/integrator/state.h"
#include "session/buffers.h"
#include "session/deep_frame_production.h"

#include <array>
#include <cstring>

CCL_NAMESPACE_BEGIN

namespace {

std::array<int64_t, 4> window(const BufferParams &b)
{
  const int64_t x = int64_t(b.full_x) + b.window_x;
  const int64_t y = int64_t(b.full_y) + b.window_y;
  return {x, y, x + b.window_width, y + b.window_height};
}

uint32_t blocks_for(const uint32_t size)
{
  return uint32_t((uint64_t(size) + 255) / 256);
}

}  // namespace

bool deep_bounded_admit_host(DeepFrameCaptureConsumer &consumer,
                             const BufferParams &frame,
                             const size_t tile_pixels,
                             const size_t devices,
                             const size_t available_ram,
                             bool &retain_native_film,
                             const size_t cpu_threads)
{
  retain_native_film = false;
  if (frame.window_width <= 0 || frame.window_height <= 0 || frame.width <= 0 ||
      frame.height <= 0 || frame.pass_stride <= 0 || !tile_pixels || !devices)
  {
    return false;
  }
  const size_t pixels = size_t(frame.window_width) * frame.window_height;
  const size_t film_pixels = size_t(frame.width) * frame.height;
  const auto configs = consumer.bounded_consumers();
  const auto demand = consumer.capture_demand();
  const auto encoding = deep_bounded_encoding_plan(configs, demand.simple_mode ?
      DeepBoundedPolicy::SIMPLE : DeepBoundedPolicy::PHYSICAL);
  size_t needed = 0, largest_record = 0, maximum_planes = 0;
  for (size_t i = 0; i < configs.size(); ++i) {
    if (deep_bounded_encoding_owner(configs, i) != i) {
      continue;
    }
    const auto &config = configs[i].config;
    const bool simple_rgb = demand.simple_mode && !config.alpha_only;
    const size_t bytes = deep_bounded_record_bytes(config.precision == DeepCanonicalPrecision::Half,
                                                  !config.alpha_only && !demand.simple_mode);
    const size_t per_pixel = bytes * DEEP_BOUNDED_K + (simple_rgb ? 4 : 0);
    size_t payload = 0;
    if (!deep_bounded_mul(pixels, per_pixel + sizeof(uint64_t), payload) ||
        !deep_bounded_add(payload, sizeof(uint64_t), payload) ||
        !deep_bounded_add(needed, payload, needed))
    {
      return false;
    }
    const size_t output_record = 8 + (config.precision == DeepCanonicalPrecision::Half ? 2 : 4) *
                                      (config.alpha_only ? 1 : 4);
    largest_record = std::max(largest_record, output_record);
    maximum_planes = std::max(maximum_planes, per_pixel);
  }
  const char *codec = getenv("CYCLES_TEST_DEEP_CODEC_ROWS");
  const size_t codec_positions = codec && std::strcmp(codec, "4") == 0 ? 4 : 16;
  size_t pinned = 0, row = 0, beauty = 0, film = 0, row_views = 0;
  if (!largest_record ||
      !deep_bounded_mul(consumer.uses_cpu_processing() ? 0 : std::min(pixels, tile_pixels),
                        maximum_planes * encoding.banks,
                        pinned) ||
      !deep_bounded_mul(pinned, devices, pinned) ||
      !deep_bounded_add(
          pinned,
          consumer.uses_cpu_processing() ? 0 : devices * encoding.banks * sizeof(DeepBoundedEncodedHeader),
          pinned) ||
      !deep_bounded_mul(frame.window_width, largest_record * DEEP_BOUNDED_K + 4, row) ||
      /* Each private codec holds its input, result and temporary byte buffers. */
      !deep_bounded_mul(row, codec_positions * 12, row) ||
      /* One reusable view/owner workspace per output sink, including the
       * pessimistic case of a separate tile run for every pixel. */
      !deep_bounded_mul(frame.window_width,
                        sizeof(DeepEncodedPixelView) +
                            sizeof(std::shared_ptr<const DeepEncodedTileStorage>), row_views) ||
      !deep_bounded_mul(row_views, configs.size(), row_views) ||
      !deep_bounded_mul(pixels, demand.simple_mode && demand.needs_rgb ? 12 : 0, beauty) ||
      !deep_bounded_mul(film_pixels, size_t(frame.pass_stride) * sizeof(float), film) ||
      !deep_bounded_add(needed, pinned, needed) || !deep_bounded_add(needed, row, needed) ||
      !deep_bounded_add(needed, row_views, needed) ||
      !deep_bounded_add(needed, beauty, needed) ||
      /* Stdio storage, two-bank task metadata and alignment margin per sink.
       * Payload/codec allocations above still charge the shared quota. */
      !deep_bounded_add(needed, size_t(131072), needed))
  {
    return false;
  }
  if (consumer.uses_cpu_processing()) {
    DeepCPUAdmission cpu;
    size_t extra;
    if (tile_pixels >= (1u << 30) || cpu_threads > UINT32_MAX ||
        !deep_cpu_admission(consumer, uint32_t(tile_pixels), uint32_t(cpu_threads), cpu) ||
        !deep_bounded_add(cpu.arena, cpu.scratch, extra) ||
        !deep_bounded_add(needed, extra, needed))
      return false;
    /* Frame payload and Beauty are already included above. No duplicate host
     * charge for CPU's output credit and no fictitious pinned/D2H bank. */
  }
  const auto &budget = consumer.memory_budget();
  const size_t available = budget.quota() - budget.used();
  /* Keep the native Film in memory only when Deep's full admission estimate
   * still fits after reserving its bytes. Otherwise retain the tiled disk sink. */
  retain_native_film = film <= available && needed <= available - film;
  return needed <= available &&
         (!available_ram || (film <= available_ram && needed <= available_ram - film));
}

DeepBoundedWork::DeepBoundedWork(Device &device,
                                 DeviceQueue &queue,
                                 DeepFrameCaptureConsumer &consumer)
    : storage_(std::make_unique<DeepBoundedWorkStorage>(device, queue, consumer))
{
}

DeepBoundedWork::~DeepBoundedWork() = default;

DeepBoundedDeviceAdmission DeepBoundedWork::admission_snapshot(
    const uint32_t native_states, const bool compact_range) const
{
  const auto &o = *storage_;
  DeepBoundedDeviceAdmission result;
  result.native_states = native_states;
  result.compact_identity = o.compact_identity_requested && compact_range;
  result.retained.source = o.resources.source_charge;
  result.retained.collector = o.resources.collector_mem.bytes();
  result.retained.phase = o.resources.phase.bytes();
  result.retained.encoded = o.resources.encoded_a.bytes();
  result.retained.encoded_b = o.resources.encoded_b.bytes();
  result.retained.control = o.resources.control.bytes();
  /* Credit only bytes this owner actually charged, even if its banks differ. */
  result.retained.known_total = o.resources.source_charge + o.resources.collector_mem.bytes() +
      o.resources.phase.bytes() + o.resources.encoded_a.bytes() + o.resources.encoded_b.bytes() +
      o.resources.control.bytes();
  result.adaptive_bytes = o.resources.native_active.bytes();
  result.pack_active = o.pack_active_pixels;
  size_t total = 0, free = 0;
  o.device.get_device_memory_info(total, free);
  result.physical_capacity = total ? free : SIZE_MAX;
  return result;
}

void DeepBoundedWork::set_admission_limit(const uint32_t samples, const size_t arena_bytes,
                                           const size_t auxiliary_growth)
{
  storage_->admitted_samples = samples;
  storage_->admitted_bytes = arena_bytes;
  storage_->admitted_auxiliary_growth = auxiliary_growth;
}

bool DeepBoundedWork::bind_sources(const uint32_t native_states, IntegratorStateGPU &state)
{
  auto &owner = *storage_;
  if (!native_states || owner.policy == DeepBoundedPolicy::NONE ||
      owner.device.deep_bounded_policy() != uint32_t(owner.policy))
  {
    owner.device.set_error("Invalid frozen Deep source state binding");
    return false;
  }
  const bool physical = owner.policy == DeepBoundedPolicy::PHYSICAL;
  DeepBoundedArenaLayout layout;
  const size_t slot_offset = layout.reserve(native_states, sizeof(uint32_t));
  const size_t anchor_offset = physical ? layout.reserve(native_states, sizeof(float)) : 0;
  const size_t shadow_offset = physical ? layout.reserve(native_states, sizeof(uint32_t)) : 0;
  const size_t shadow_anchor_offset = physical ? layout.reserve(native_states, sizeof(float)) : 0;
  if (!layout.valid || (owner.resources.native_states && owner.resources.native_states != native_states)) {
    owner.device.set_error("Deep source capacity changed within a frozen frame");
    return false;
  }
  if (!owner.resources.native_states) {
    size_t total = 0, free = 0;
    owner.device.get_device_memory_info(total, free);
    if (total && layout.bytes > free) {
      owner.device.set_error("Deep source state exceeds available device memory");
      return false;
    }
    try {
      owner.budget.acquire(layout.bytes);
      owner.resources.source_charge = layout.bytes;
      owner.resources.sources.alloc_to_device(layout.bytes);
    }
    catch (const std::bad_alloc &) {
      owner.device.set_error("Deep source state exceeds the device quota");
      return false;
    }
    if (!owner.resources.sources.device_pointer || owner.device.have_error()) {
      return false;
    }
    owner.resources.native_states = native_states;
  }
  const device_ptr base = owner.resources.sources.device_pointer;
  state.path.deep_bounded_slot_kind = reinterpret_cast<uint32_t *>(base + slot_offset);
  state.path.deep_bounded_anchor_z = physical ? reinterpret_cast<float *>(base + anchor_offset) :
                                                nullptr;
  state.shadow_path.deep_bounded_slot_kind = physical ?
                                                 reinterpret_cast<uint32_t *>(base + shadow_offset) :
                                                 nullptr;
  state.shadow_path.deep_bounded_anchor_z = physical ?
                                               reinterpret_cast<float *>(base + shadow_anchor_offset) :
                                               nullptr;
  return true;
}

void DeepBoundedWork::discard(IntegratorStateGPU &state)
{
  auto &o = *storage_;
  const bool compute_joined = o.queue.synchronize_for_state_rebind();
  const bool copy_touched = (o.transport.copies[0].submitted_stages | o.transport.copies[1].submitted_stages) &
                            (1u << 31);
  const bool copy_joined = !copy_touched || o.queue.deep_copy_join();
  if (compute_joined && copy_joined) {
    for (auto &slot : o.transport.copies) {
      if (slot.pinned) {
        o.queue.deep_unpin(slot.pinned);
        o.consumer.memory_budget().relinquish(slot.pinned_bytes);
      }
    }
  }
  else {
    o.device.set_error("Deep execution did not join before teardown");
    o.quarantined = true;
  }
  state.deep_bounded = {};
  state.path.deep_bounded_slot_kind = nullptr;
  state.path.deep_bounded_anchor_z = nullptr;
  state.shadow_path.deep_bounded_slot_kind = nullptr;
  state.shadow_path.deep_bounded_anchor_z = nullptr;
}

bool DeepBoundedWork::good() const
{
  return !storage_->device.have_error();
}

bool DeepBoundedWork::configure(const BufferParams &logical,
                                const BufferParams &slice,
                                const uint32_t native_states,
                                const uint32_t max_samples,
                                const bool has_holdout,
                                const float projection_scale,
                                IntegratorStateGPU &state,
                                const bool identity_range_fits)
{
  auto &o = *storage_;
  const auto tile = window(logical);
  const auto sw = window(slice);
  if (tile[0] < INT32_MIN || tile[1] < INT32_MIN || tile[2] > INT32_MAX || tile[3] > INT32_MAX ||
      tile[2] <= tile[0] || tile[3] <= tile[1])
  {
    o.device.set_error("Invalid Deep logical Tile window");
    return false;
  }
  const uint64_t pixels = uint64_t(tile[2] - tile[0]) * uint64_t(tile[3] - tile[1]);
  if (pixels >= (uint64_t(1) << 30)) {
    o.device.set_error("Deep logical Tile pixel capacity exceeds schema7");
    return false;
  }
  const bool physical = o.policy == DeepBoundedPolicy::PHYSICAL;
  if (o.tile != tile) {
    o.compact_identity = o.compact_identity_requested && identity_range_fits;
    const auto consumers = o.consumer.bounded_consumers();
    o.families = deep_bounded_family_count(consumers);
    if (!o.families) {
      o.device.set_error("Deep has no frozen encoding consumers");
      return false;
    }
    o.strict.alpha_tolerance = float(consumers.front().config.alpha_tolerance);
    o.strict.depth_tolerance = float(consumers.front().config.depth_tolerance);
    for (const auto &consumer : consumers) {
      o.strict.alpha_tolerance = std::min(o.strict.alpha_tolerance,
                                         float(consumer.config.alpha_tolerance));
      o.strict.depth_tolerance = std::min(o.strict.depth_tolerance,
                                         float(consumer.config.depth_tolerance));
    }
    o.strict.projection_scale = projection_scale;
    const size_t resident = o.resources.source_charge + o.resources.collector_mem.bytes() + o.resources.phase.bytes() +
                            o.resources.encoded_a.bytes() + o.resources.encoded_b.bytes() + o.resources.control.bytes();
    size_t quota, total = 0, free = 0;
    if (!deep_bounded_add(o.budget.quota() - o.budget.used(), resident, quota)) {
      quota = SIZE_MAX;
    }
    o.device.get_device_memory_info(total, free);
    size_t available = SIZE_MAX;
    if (total) {
      deep_bounded_add(free, resident, available);
    }
    DeepBoundedAdmission retained;
    retained.source = o.resources.source_charge;
    retained.collector = o.resources.collector_mem.bytes();
    retained.phase = o.resources.phase.bytes();
    retained.encoded = o.resources.encoded_a.bytes();
    retained.encoded_b = o.resources.encoded_b.bytes();
    retained.control = o.resources.control.bytes();
    o.admission = deep_bounded_choose_packet(uint32_t(pixels), std::min(max_samples, o.admitted_samples), native_states,
                                             o.policy, has_holdout, o.families,
                                             std::min({quota, available, o.admitted_bytes}), retained,
                                             o.exact_admission, o.compact_identity, o.encoding_plan);
    if (!o.admission.roots) {
      o.device.set_error("Deep Budget cannot admit this Tile and one camera sample");
      return false;
    }
    if ((o.admission.encoded > o.resources.encoded_a.bytes() ||
         o.admission.encoded_b > o.resources.encoded_b.bytes()) && !drain()) {
      return false;
    }
    try {
      if (!o.resources.collector_mem.reserve(o.admission.collector) || !o.resources.phase.reserve(o.admission.phase) ||
          !o.resources.encoded_a.reserve(o.admission.encoded) || !o.resources.encoded_b.reserve(o.admission.encoded_b) ||
          !o.resources.control.reserve(o.admission.control))
      {
        return false;
      }
    }
    catch (const std::bad_alloc &) {
      o.device.set_error("Deep device allocation exceeded its checked quota");
      return false;
    }
    if (const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_WORK_TIMINGS");
        trace && std::strcmp(trace, "1") == 0)
    {
      std::fprintf(stderr, "DEEP_TILE_ADMISSION width=%lld height=%lld batch=%u roots=%u\n",
                   static_cast<long long>(tile[2] - tile[0]),
                   static_cast<long long>(tile[3] - tile[1]),
                   o.admission.packet_samples, o.admission.roots);
    }
    o.tile = tile;
    o.prepared = false;
    o.first_tile_packet = true;
    o.collector.x = int(tile[0]);
    o.collector.y = int(tile[1]);
    o.collector.width = uint32_t(tile[2] - tile[0]);
    o.collector.height = uint32_t(tile[3] - tile[1]);
    o.collector.capacity_pixels = uint32_t(pixels);
    DeepBoundedArenaLayout collector_layout;
    const uint64_t collector_base = o.resources.collector_mem.address();
    o.collector.planes = collector_layout.planes(collector_base, size_t(pixels) * DEEP_BOUNDED_K, physical, o.compact_identity);
    o.collector.counts = collector_base + collector_layout.reserve(pixels, 4);
    o.collector.populations = collector_base + collector_layout.reserve(pixels, 4);
    o.capture = {};
    o.capture.policy = uint32_t(o.policy);
    o.capture.has_holdout = has_holdout;
    o.capture.merge = o.strict;
    o.capture.packet_device = o.resources.control.address();
    o.capture.packet.status_ptr = o.resources.control.address() + offsetof(DeepBoundedPacketControl, status);
    o.capture.active_map = collector_base +
                           collector_layout.reserve(size_t(pixels) * 2 + 2, sizeof(uint32_t));
    o.capture.packet.root_capacity = o.admission.roots;
    o.capture.packet.layout_id = (uint64_t(DEEP_BOUNDED_SCHEMA) << 32) | uint32_t(o.policy);
    DeepBoundedArenaLayout raw;
    const uint64_t phase = o.resources.phase.address();
    o.capture.raw = raw.planes(phase, size_t(o.admission.roots) * DEEP_BOUNDED_K, physical, o.compact_identity);
    o.capture.headers = phase + raw.reserve(o.admission.roots, 4);
    if (has_holdout) {
      o.capture.holdout_mq = phase + raw.reserve(o.admission.roots, 8);
    }
    if (physical) {
      o.capture.no_anchor = phase + raw.reserve(o.admission.roots, 12);
    }
  }
  const int64_t x = std::max(tile[0], sw[0]);
  const int64_t y = std::max(tile[1], sw[1]);
  auto &packet = o.capture.packet;
  packet.owned_x = int32_t(x);
  packet.owned_y = int32_t(y);
  packet.owned_w = uint32_t(std::max(int64_t(0), std::min(tile[2], sw[2]) - x));
  packet.owned_h = uint32_t(std::max(int64_t(0), std::min(tile[3], sw[3]) - y));
  packet.owned_pitch = packet.owned_w;
  packet.plane_pitch = packet.owned_w * packet.owned_h;
  o.root_pitch = packet.plane_pitch;
  o.capture.native_active_map = 0;
  if (o.pack_active_pixels && packet.plane_pitch && slice.width > 0 && slice.height > 0) {
    const uint64_t area = uint64_t(slice.width) * uint64_t(slice.height);
    const size_t bytes = size_t(area) * sizeof(uint32_t);
    const size_t extra = bytes - std::min(bytes, o.resources.native_active.bytes());
    size_t total = 0, free = 0;
    if (extra) {
      o.device.get_device_memory_info(total, free);
    }
    if (area <= INT32_MAX && extra <= o.admitted_auxiliary_growth &&
        extra <= o.budget.quota() - o.budget.used() &&
        (!total || extra <= free))
    {
      try {
        if (!o.resources.native_active.reserve(bytes)) {
          return false;
        }
      }
      catch (const std::bad_alloc &) {
        o.device.set_error("Deep adaptive camera list exceeds the checked quota");
        return false;
      }
      o.capture.native_active_map = o.resources.native_active.address();
    }
  }
  state.deep_bounded = packet.plane_pitch ? o.capture : DeepBoundedBinding{};
  /* Only device control owns the dynamic native sampling descriptor. Keep
   * the constant binding stable while preserving every static arena field. */
  state.deep_bounded.packet.start_sample = 0;
  state.deep_bounded.packet.sample_offset = 0;
  state.deep_bounded.packet.num_samples = 0;
  state.deep_bounded.packet.epoch = 0;
  return good();
}

bool DeepBoundedWork::migrate(DeepBoundedWork &destination,
                              const int x,
                              const int y,
                              const uint32_t width,
                              const uint32_t height)
{
  auto &src = *storage_;
  auto &dst = *destination.storage_;
  if (!width || !height) {
    return true;
  }
  if (&src == &dst || src.tile != dst.tile || src.policy != dst.policy ||
      src.first_tile_packet || src.tile[0] != x || width != src.collector.width ||
      y < src.tile[1] || int64_t(y) + height > src.tile[3] ||
      !src.resources.phase.address() || !dst.resources.phase.address())
  {
    src.device.set_error("Invalid bounded Deep migration owner");
    return false;
  }
  DeepBoundedMigrationChunk shape;
  if (!shape.initialize(uint64_t(width) * height, src.policy == DeepBoundedPolicy::PHYSICAL,
                        src.resources.phase.bytes(), dst.resources.phase.bytes()))
  {
    src.device.set_error("Deep Budget cannot stage one collector pixel");
    return false;
  }
  if (dst.first_frame_packet) {
    if (!dst.queue.deep_zero(dst.capture.packet.status_ptr, sizeof(uint32_t))) {
      return false;
    }
    dst.first_frame_packet = false;
  }
  const uint64_t first = uint64_t(y - src.tile[1]) * width;
  uint32_t status = 0;
  if (!src.queue.read_device_region(src.resources.control.memory, sizeof(DeepBoundedPacket), &status,
                                    sizeof(status)) || status)
  {
    src.device.set_error("Bounded Deep cannot migrate a failed collector");
    return false;
  }
  const auto &budget = src.consumer.memory_budget();
  std::vector<uint8_t, DeepBudgetAllocator<uint8_t>> staged{DeepBudgetAllocator<uint8_t>(budget)};
  for (uint64_t offset = 0; offset < shape.pixels; offset += shape.pixels_per_chunk) {
    const uint32_t pixels = uint32_t(std::min(shape.pixels_per_chunk, shape.pixels - offset));
    const size_t bytes = pixels * shape.bytes_per_pixel;
    const uint32_t blocks = blocks_for(pixels * DEEP_BOUNDED_K);
    DeepBoundedDispatch dispatch;
    dispatch.capture = src.capture;
    dispatch.collector = src.collector;
    dispatch.first = uint32_t(first + offset);
    dispatch.size = pixels;
    dispatch.migration = src.resources.phase.address();
    if (!src.queue.enqueue_deep_bounded(DeepBoundedOperation::MIGRATION_PACK, dispatch, blocks) ||
        !src.queue.synchronize_for_state_rebind())
    {
      return false;
    }
    const int peer = dst.queue.deep_copy_peer(dst.resources.phase.address(), src.device, src.resources.phase.address(),
                                              bytes);
    if (peer < 0) {
      return false;
    }
    if (peer == 0) {
      try {
        staged.resize(bytes);
      }
      catch (const std::bad_alloc &) {
        src.device.set_error("Deep migration staging exceeds host quota");
        return false;
      }
      if (!src.queue.read_device_region(src.resources.phase.memory, 0, staged.data(), bytes) ||
          !dst.queue.write_device_region(dst.resources.phase.memory, 0, staged.data(), bytes))
      {
        return false;
      }
    }
    dispatch.capture = dst.capture;
    dispatch.collector = dst.collector;
    dispatch.migration = dst.resources.phase.address();
    if (!dst.queue.enqueue_deep_bounded(DeepBoundedOperation::MIGRATION_UNPACK, dispatch, blocks) ||
        !dst.queue.synchronize_for_state_rebind())
    {
      return false;
    }
  }
  dst.first_tile_packet = false;
  return !src.device.have_error() && !dst.device.have_error();
}

bool DeepBoundedWork::has_owned_pixels() const
{
  return storage_->capture.packet.plane_pitch != 0;
}

bool DeepBoundedWork::prepare_work(const int start,
                                   const uint32_t samples,
                                   const RenderBuffers &buffers,
                                   const int adaptive_pass,
                                   BufferParams &camera_params)
{
  auto &o = *storage_;
  const auto &packet = o.capture.packet;
  o.root_pitch = packet.plane_pitch;
  o.native_columns = 0;
  if (!o.capture.native_active_map || adaptive_pass < 0 || start < 16 || samples < 4) {
    return true;
  }

  /* Use the same native adaptive Film mask for owned pixels and halo pixels. */
  const auto &params = buffers.params;
  const int64_t first = int64_t(camera_params.offset) + camera_params.full_x +
                        int64_t(camera_params.full_y) * camera_params.stride;
  const int64_t last = first + int64_t(camera_params.height - 1) * camera_params.stride +
                       camera_params.width - 1;
  if (first < 0 || camera_params.stride < camera_params.width ||
      adaptive_pass >= params.pass_stride - 3 ||
      uint64_t(last) >= buffers.buffer.size() / uint32_t(params.pass_stride))
  {
    o.device.set_error("Invalid Film window for bounded Deep adaptive mapping");
    return false;
  }
  DeepBoundedDispatch dispatch;
  dispatch.capture = o.capture;
  dispatch.film = buffers.buffer.device_pointer;
  dispatch.film_first = first * params.pass_stride + adaptive_pass + 3;
  dispatch.film_row_stride = camera_params.stride;
  dispatch.film_pass_stride = params.pass_stride;
  dispatch.native_width = uint32_t(camera_params.width);
  dispatch.native_x = camera_params.full_x;
  dispatch.native_y = camera_params.full_y;
  dispatch.size = dispatch.native_width * uint32_t(camera_params.height);
  std::array<uint32_t, 2> counts = {};
  if (!o.queue.deep_zero(o.capture.active_map, sizeof(counts)) ||
      !o.queue.enqueue_deep_bounded(DeepBoundedOperation::MAP_ACTIVE_PIXELS, dispatch,
                                    blocks_for(dispatch.size)) ||
      !o.queue.read_device_region(o.resources.collector_mem.memory,
                                  o.capture.active_map - o.resources.collector_mem.address(),
                                  counts.data(), sizeof(counts)))
  {
    return false;
  }
  const uint32_t owned = counts[0], active = counts[1];
  if (owned > packet.plane_pitch || owned > active || active > dispatch.size) {
    o.device.set_error("Invalid bounded Deep active pixel count");
    return false;
  }

  if (owned) {
    const uint32_t dense = o.admission.roots / packet.plane_pitch;
    const uint32_t packed = o.admission.roots / owned;
    if ((samples - 1) / packed < (samples - 1) / dense) {
      o.root_pitch = owned;
    }
  }
  if (active && uint64_t(active) * 4 <= uint64_t(dispatch.size) * 3) {
    const uint32_t columns = std::min(active, dispatch.native_width);
    o.native_columns = columns;
    o.native_count = active;
    o.native_width = dispatch.native_width;
    o.native_x = dispatch.native_x;
    o.native_y = dispatch.native_y;
    o.native_offset = camera_params.offset;
    o.native_stride = dispatch.film_row_stride;
    camera_params.full_x = 0;
    camera_params.full_y = 0;
    camera_params.width = int(columns);
    camera_params.height = int(1 + (active - 1) / columns);
  }
  return true;
}

uint32_t DeepBoundedWork::packet_samples(const uint32_t remaining) const
{
  const auto &o = *storage_;
  return has_owned_pixels() ? std::min(remaining, o.admission.roots / o.root_pitch) :
                              remaining;
}

bool DeepBoundedWork::begin_packet(const int start,
                                   const uint32_t samples,
                                   const int sample_offset,
                                   IntegratorStateGPU &state,
                                   const bool keep_shadow_queue,
                                   const bool track_camera)
{
  auto &o = *storage_;
  auto &packet = o.capture.packet;
  if (!samples || start < 0 || uint64_t(start) + samples > INT32_MAX ||
      !o.root_pitch || uint64_t(o.root_pitch) * samples > packet.root_capacity || !has_owned_pixels())
  {
    o.device.set_error("Invalid Deep packet admission");
    return false;
  }
  packet.start_sample = start;
  packet.num_samples = samples;
  packet.sample_offset = sample_offset;
  if (o.epoch == UINT64_MAX) {
    o.device.set_error("Bounded Deep camera packet epoch exhausted");
    return false;
  }
  packet.epoch = ++o.epoch;
  DeepBoundedDispatch dispatch;
  dispatch.capture = o.capture;
  dispatch.collector = o.collector;
  dispatch.root_pitch = o.root_pitch;
  dispatch.track_camera = track_camera;
  dispatch.native_width = o.native_width;
  dispatch.native_count = o.native_count;
  dispatch.native_columns = o.native_columns;
  dispatch.native_x = o.native_x;
  dispatch.native_y = o.native_y;
  dispatch.native_offset = o.native_offset;
  dispatch.film_row_stride = o.native_stride;
  dispatch.size = o.root_pitch * samples;
  dispatch.first = (o.first_tile_packet ? DeepBoundedBeginFlags::TILE : 0u) |
                   (o.first_frame_packet ? DeepBoundedBeginFlags::FRAME : 0u);
  if (!keep_shadow_queue) {
    dispatch.next_main_index = reinterpret_cast<uintptr_t>(state.next_main_path_index);
    dispatch.next_shadow_index = reinterpret_cast<uintptr_t>(state.next_shadow_path_index);
  }
  const uint32_t work = o.first_tile_packet ? std::max(dispatch.size, packet.plane_pitch) :
                                              dispatch.size;
  if (!o.queue.enqueue_deep_bounded(DeepBoundedOperation::BEGIN_PACKET, dispatch, blocks_for(work)))
  {
    return false;
  }
  o.first_tile_packet = false;
  o.first_frame_packet = false;
  return true;
}

bool DeepBoundedWork::accumulate_packet()
{
  auto &o = *storage_;
  DeepBoundedDispatch dispatch;
  dispatch.capture = o.capture;
  dispatch.collector = o.collector;
  dispatch.merge = o.strict;
  /* One owner per pixel preserves camera order across packets. Native filter
   * sampling already supplies the pixel reconstruction distribution. */
  dispatch.root_pitch = o.root_pitch;
  return o.queue.enqueue_deep_bounded(DeepBoundedOperation::ACCUMULATE, dispatch,
                                      blocks_for(o.root_pitch));
}

static bool scan_model(DeepBoundedWorkStorage &o, const DeepBoundedModel &model)
{
  struct Level {
    uint64_t input, output, totals;
    uint32_t count;
  };
  std::array<Level, 5> levels;
  uint32_t n = model.pixels;
  if (!n) {
    return true;
  }
  DeepBoundedArenaLayout scratch;
  uint64_t input = model.counts;
  uint64_t output = model.offsets;
  int length = 0;
  for (;;) {
    const uint32_t blocks = blocks_for(n);
    const uint64_t totals = blocks > 1 ? o.scan_scratch + scratch.reserve(blocks, 8, 8) : 0;
    levels[length++] = {input, output, totals, n};
    DeepBoundedDispatch dispatch;
    dispatch.size = n;
    dispatch.scan_input = input;
    dispatch.scan_output = output;
    dispatch.scan_totals = totals;
    dispatch.scan_u64 = length > 1;
    if (!o.queue.enqueue_deep_bounded(DeepBoundedOperation::SCAN, dispatch, blocks)) {
      return false;
    }
    if (blocks == 1) {
      break;
    }
    input = totals;
    output = o.scan_scratch + scratch.reserve(blocks, 8, 8);
    n = blocks;
  }
  for (int i = length - 2; i >= 0; --i) {
    DeepBoundedDispatch dispatch;
    dispatch.scan_input = levels[i + 1].output;
    dispatch.scan_output = levels[i].output;
    dispatch.size = levels[i].count;
    if (!o.queue.enqueue_deep_bounded(DeepBoundedOperation::SCAN_ADD, dispatch,
                                     blocks_for(dispatch.size)))
    {
      return false;
    }
  }
  return true;
}

bool DeepBoundedWork::prepare_tile()
{
  auto &o = *storage_;
  if (o.prepared || !has_owned_pixels()) {
    return true;
  }
  const uint32_t pixels = o.capture.packet.plane_pitch;
  const size_t stride = size_t(pixels) * DEEP_BOUNDED_K;
  const uint32_t model_count = std::min(o.families, 2u);
  const bool physical = o.policy == DeepBoundedPolicy::PHYSICAL;
  const uint64_t base = o.resources.phase.address();
  DeepBoundedArenaLayout layout;
  const auto planes = layout.planes(base, stride * model_count, physical, o.compact_identity);
  const uint64_t counts = base + layout.reserve(size_t(pixels) * model_count, 4);
  const uint64_t offsets = base + layout.reserve(pixels, 8, 8);
  o.scan_scratch = base + layout.reserve(0, 8, 8);
  for (uint32_t i = 0; i < model_count; ++i) {
    const uint64_t offset = i * stride * 4;
    o.models[i] = {{planes.z + offset, planes.z_back + offset,
                    planes.rgba + (physical ? offset * 4 : offset), planes.object + offset,
                    planes.shader ? planes.shader + offset : 0, planes.normal + offset},
                   counts + uint64_t(i) * pixels * 4, offsets, pixels};
  }
  DeepBoundedDispatch dispatch;
  dispatch.capture = o.capture;
  dispatch.collector = o.collector;
  dispatch.model = o.models[0];
  dispatch.merge = o.strict;
  o.prepared = o.queue.enqueue_deep_bounded(DeepBoundedOperation::FINALIZE, dispatch,
                                           blocks_for(pixels));
  return o.prepared;
}

bool DeepBoundedWork::close_tile()
{
  auto &o = *storage_;
  /* close_tile is called by the serial PathTrace owner. */
  if (!publish_ready() || !drain() || !publish_ready()) {
    return false;
  }
  if (!has_owned_pixels()) {
    return true;
  }
  if (!prepare_tile()) {
    return false;
  }
  const auto &packet = o.capture.packet;
  const size_t pixels = packet.plane_pitch;
  const auto configs = o.consumer.bounded_consumers();
  const auto &budget = o.consumer.memory_budget();
  if (o.transport.output_epoch == UINT64_MAX) {
    o.device.set_error("Bounded Deep output epoch exhausted");
    return false;
  }
  ++o.transport.output_epoch;
  o.transport.output = std::make_unique<DeepBoundedHostTile>(budget);
  o.transport.output->window = {packet.owned_x, packet.owned_y, packet.owned_w, packet.owned_h};
  o.transport.output->consumers.resize(configs.size());
  o.transport.output_aliases.resize(configs.size());
  for (size_t i = 0; i < configs.size(); ++i) {
    o.transport.output_aliases[i] = deep_bounded_encoding_owner(configs, i);
  }
  unsigned next = 0;
  /* Each optical family owns the scratch model until all its formats are encoded. */
  for (size_t family = 0; family < configs.size(); ++family) {
    const auto &config = configs[family].config;
    bool first = true;
    for (size_t j = 0; j < family; ++j) {
      first &= !deep_bounded_same_family(configs[j].config, config);
    }
    if (!first) {
      continue;
    }
    DeepBoundedDispatch dispatch;
    dispatch.capture = o.capture;
    dispatch.model = o.models[0];
    dispatch.merge = o.strict;
    if (float(config.alpha_tolerance) != o.strict.alpha_tolerance ||
        float(config.depth_tolerance) != o.strict.depth_tolerance)
    {
      dispatch.input_model = o.models[0];
      dispatch.model = o.models[1];
      dispatch.merge.alpha_tolerance = float(config.alpha_tolerance);
      dispatch.merge.depth_tolerance = float(config.depth_tolerance);
      if (!o.queue.enqueue_deep_bounded(DeepBoundedOperation::COARSEN, dispatch,
                                        blocks_for(uint32_t(pixels))) ||
          !scan_model(o, dispatch.model))
      {
        return false;
      }
    }
    else if (!scan_model(o, dispatch.model)) {
      return false;
    }
    for (size_t i = family; i < configs.size(); ++i) {
      if (!deep_bounded_same_family(config, configs[i].config)) {
        continue;
      }
      const auto &format = configs[i].config;
      if (o.transport.output_aliases[i] != i) {
        continue;
      }
      const unsigned index = next++ & 1;
      auto &slot = o.transport.copies[index];
      if (!deep_bounded_progress_copy(o, index, true)) {
        o.transport.copy_failed = true;
        return false;
      }
      auto &arena = index ? o.resources.encoded_b : o.resources.encoded_a;
      DeepBoundedArenaLayout layout;
      const uint64_t base = arena.address();
      auto &e = dispatch.encoding;
      const bool rgb = o.policy == DeepBoundedPolicy::PHYSICAL && !format.alpha_only;
      e.half = format.precision == DeepCanonicalPrecision::Half;
      e.alpha_only = format.alpha_only;
      e.layout = uint32_t(o.policy) | (e.half << 8) | (e.alpha_only << 9);
      e.capacity_records = pixels * DEEP_BOUNDED_K;
      e.header = base + layout.reserve(1, sizeof(DeepBoundedEncodedHeader));
      e.counts = base + layout.reserve(pixels, 4);
      e.z = base + layout.reserve(e.capacity_records, 4);
      e.z_back = base + layout.reserve(e.capacity_records, 4);
      e.rgba = base + layout.reserve(e.capacity_records, (e.half ? 2 : 4) * (rgb ? 4 : 1));
      e.summary = o.policy == DeepBoundedPolicy::SIMPLE && !e.alpha_only ?
                      base + layout.reserve(pixels, 4) : 0;
      slot.epoch = o.transport.output_epoch;
      slot.submitted_stages = 1u << uint32_t(DeepBoundedCopyStage::LEASED);
      slot.submitted_planes = 0;
      if (!layout.valid || layout.bytes > arena.bytes()) {
        o.device.set_error("Bounded Deep encoded arena does not fit the final model");
        return false;
      }
      slot.submitted_stages |= DeepBoundedCopyPosition::COMPUTE_TOUCHED;
      if (!o.queue.deep_zero(e.header, sizeof(DeepBoundedEncodedHeader)) ||
          !o.queue.enqueue_deep_bounded(DeepBoundedOperation::ENCODE, dispatch,
                                        blocks_for(uint32_t(pixels))))
      {
        return false;
      }
      slot.mark(DeepBoundedCopyStage::COMPACT_SUBMITTED);
      if (!o.queue.deep_mark_compact(index)) {
        return false;
      }
      slot.mark(DeepBoundedCopyStage::COMPACT_EVENT_RECORDED);
      slot.consumer = i;
      slot.encoding = e;
      slot.stage = DeepBoundedCopyPosition::Stage::Compacted;
    }
  }
  return true;
}



CCL_NAMESPACE_END
