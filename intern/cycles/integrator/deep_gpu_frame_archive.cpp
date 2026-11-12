/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_gpu_frame_work.h"
#include "device/device.h"
#include "session/buffers.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

CCL_NAMESPACE_BEGIN

namespace {
uint32_t archive_blocks(const uint64_t size)
{
  return uint32_t((size + 255) / 256);
}

/* These are opaque-run range descriptors, never curve or colour values. */
DeepGPUFrameRange archive_span(const std::span<const DeepGPUFrameRange> ranges,
                              const uint32_t first, const uint32_t count)
{
  DeepGPUFrameRange span{};
  bool found = false;
  for (uint32_t i = first; i < first + count; i++) {
    if (ranges[i].count) {
      if (!found) { span.offset = ranges[i].offset; found = true; }
      span.count += ranges[i].count;
    }
  }
  return span;
}
}  // namespace

bool DeepGPUFrameWork::spill_native_primitives()
{
  auto &buffer = *native_[size_t(DeepGPUResidentKind::Primitive)];
  if (!buffer.used()) return true;
  auto raw = std::allocate_shared<RawBytes>(DeepBudgetAllocator<RawBytes>(host_budget_), host_budget_);
  raw->bytes.resize(buffer.used());
  if (!queue_->read_device_region(buffer.memory(), 0, raw->bytes.data(), raw->bytes.size())) return false;
  primitive_spill_.push_back(std::move(raw));
  /* The producer page already joined at the dispatch boundary. Only its owned
   * immutable D2D copy is retired; native live continuations remain resident. */
  buffer.clear();
  return true;
}

bool DeepGPUFrameWork::archive_records(const DeepGPUResidentKind kind, device_memory &source,
                                        const uint32_t records, Buffer &camera_map,
                                        Buffer &camera_keys, ArchivedPagePtr &page)
{
  const uint32_t pixels = uint32_t(uint64_t(archive_width_) * archive_height_);
  const uint32_t stride = kind == DeepGPUResidentKind::Primitive ?
      sizeof(DeepCompactPrimitive) : sizeof(DeepCompactColourSource);
  const uint32_t camera_field = kind == DeepGPUResidentKind::Primitive ?
      offsetof(DeepCompactPrimitive, camera) : offsetof(DeepCompactColourSource, camera);
  auto keys = make_buffer(), ids = make_buffer(), ranges = make_buffer(), packed = make_buffer();
  if (!keys->reserve(*queue_, size_t(records) * 8, false) ||
      !ids->reserve(*queue_, size_t(records) * 4, false)) return false;
  DeepGPUFrameDispatch d;
  d.a = source.device_pointer; d.b = keys->pointer(); d.c = ids->pointer();
  d.d = camera_map->pointer(); d.e = camera_keys->pointer(); d.h = controls_.device_pointer;
  d.n = records; d.m = stride; d.k = camera_field; d.width = uint32_t(camera_map->capacity() / 4);
  if (!operation(DeepGPUFrameOperation::CompactArchiveKeys, d, archive_blocks(records)) ||
      !sort(*keys, *ids, records) || !group(*keys, records, pixels, *ranges) || !good()) return false;
  page = std::allocate_shared<ArchivedPage>(DeepBudgetAllocator<ArchivedPage>(host_budget_), host_budget_);
  page->ranges.resize(pixels);
  if (!queue_->read_device_region(ranges->memory(), 0, page->ranges.data(),
                                    size_t(pixels) * sizeof(DeepGPUFrameRange))) return false;
  const auto retained = archive_span(page->ranges, 0, pixels);
  if (retained.offset != 0 || retained.count > records)
    return fail("Deep archive contains invalid device-produced range metadata");
  if (!packed->reserve(*queue_, size_t(records) * stride, false)) return false;
  d.b = ids->pointer(); d.c = packed->pointer(); d.f = keys->pointer();
  if (!operation(DeepGPUFrameOperation::CompactArchiveGather, d, archive_blocks(uint64_t(records) * stride)) ||
      !good()) return false;
  if (const char *verify = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
      verify && std::strcmp(verify, "1") == 0) {
    auto camera_ranges = make_buffer();
    if (!group(*camera_keys, uint32_t(camera_map->capacity() / 4), pixels, *camera_ranges)) return false;
    d.a = packed->pointer(); d.b = camera_ranges->pointer(); d.c = ranges->pointer(); d.n = pixels; d.kind = 1;
    if (!operation(DeepGPUFrameOperation::CompactArchiveRebase, d, archive_blocks(pixels)) || !good()) return false;
  }
  page->bytes.resize(size_t(retained.count) * stride);
  return !retained.count || queue_->read_device_region(packed->memory(), 0,
                                                        page->bytes.data(), page->bytes.size());
}

bool DeepGPUFrameWork::archive_native()
{
  using K = DeepGPUResidentKind;
  const int x = domain_.core_width ? domain_.core_x : domain_.x;
  const int y = domain_.core_height ? domain_.core_y : domain_.y;
  const int width = domain_.core_width ? domain_.core_width : domain_.width;
  const int height = domain_.core_height ? domain_.core_height : domain_.height;
  if (width <= 0 || height <= 0 || uint64_t(width) * height > UINT32_MAX)
    return fail("Deep archive has an invalid native tile extent");
  if (archives_.empty()) {
    archive_x_ = x; archive_y_ = y; archive_width_ = width; archive_height_ = height;
  }
  else if (x != archive_x_ || y != archive_y_ || width != archive_width_ || height != archive_height_)
    return fail("Deep archive tile identity changed before publication");
  const uint32_t count = native_count(K::CompactCamera), pixels = uint32_t(width * height);
  auto camera_keys = make_buffer(), camera_ids = make_buffer(), camera_map = make_buffer();
  auto camera_ranges = make_buffer(), packed = make_buffer();
  if (!camera_keys->reserve(*queue_, size_t(count) * 8, false) ||
      !camera_ids->reserve(*queue_, size_t(count) * 4, false) ||
      !camera_map->reserve(*queue_, size_t(count) * 4, false)) return false;
  DeepGPUFrameDispatch d;
  d.a = native_pointer(K::CompactCamera); d.b = camera_keys->pointer(); d.c = camera_ids->pointer();
  d.n = count; d.m = width; d.k = height; d.base = uint32_t(x); d.pixel = uint32_t(y);
  if (!operation(DeepGPUFrameOperation::CompactCameraKeys, d, archive_blocks(count)) ||
      !sort(*camera_keys, *camera_ids, count) ||
      !group(*camera_keys, count, pixels, *camera_ranges)) return false;
  d = {}; d.b = camera_keys->pointer(); d.c = camera_ids->pointer(); d.d = camera_map->pointer();
  d.h = controls_.device_pointer; d.n = count;
  if (!operation(DeepGPUFrameOperation::BuildRays, d, archive_blocks(count)) || !good()) return false;
  auto domain = std::allocate_shared<ArchivedDomain>(DeepBudgetAllocator<ArchivedDomain>(host_budget_), host_budget_);
  domain->cameras = std::allocate_shared<ArchivedPage>(DeepBudgetAllocator<ArchivedPage>(host_budget_), host_budget_);
  domain->cameras->ranges.resize(pixels);
  if (!queue_->read_device_region(camera_ranges->memory(), 0, domain->cameras->ranges.data(),
                                    size_t(pixels) * sizeof(DeepGPUFrameRange))) return false;
  const auto retained = archive_span(domain->cameras->ranges, 0, pixels);
  if (retained.offset || retained.count > count) return fail("Deep archive camera population range is invalid");
  camera_ranges.reset();
  if (!packed->reserve(*queue_, size_t(count) * sizeof(DeepCompactCamera), false)) return false;
  d = {}; d.a = native_pointer(K::CompactCamera); d.b = camera_ids->pointer(); d.c = packed->pointer();
  d.d = controls_.device_pointer; d.n = count; d.m = sizeof(DeepCompactCamera);
  if (!operation(DeepGPUFrameOperation::Gather, d, archive_blocks(uint64_t(count) * d.m))) return false;
  domain->cameras->bytes.resize(size_t(retained.count) * sizeof(DeepCompactCamera));
  if (retained.count && !queue_->read_device_region(packed->memory(), 0, domain->cameras->bytes.data(),
                                                     domain->cameras->bytes.size())) return false;
  packed.reset(); camera_ids.reset(); native_[size_t(K::CompactCamera)]->clear();
  if (!spill_native_primitives()) return false;
  auto reload = make_buffer();
  for (auto &raw : primitive_spill_) {
    if (!reload->append_raw(*queue_, raw->bytes.data(), raw->bytes.size())) return false;
    ArchivedPagePtr page;
    if (!archive_records(K::Primitive, reload->memory(),
                            uint32_t(raw->bytes.size() / sizeof(DeepCompactPrimitive)),
                            camera_map, camera_keys, page)) return false;
    domain->primitives.push_back(std::move(page));
    reload->clear(); raw.reset();
  }
  primitive_spill_.clear();
  auto &colours = *native_[size_t(K::ColourSource)];
  const size_t colour_count = colours.used() / sizeof(DeepCompactColourSource);
  for (size_t at = 0; at < colour_count;) {
    const uint32_t n = uint32_t(std::min(size_t(16384), colour_count - at));
    if (!reload->append(*queue_, colours.memory(), size_t(n) * sizeof(DeepCompactColourSource),
                         at * sizeof(DeepCompactColourSource))) return false;
    ArchivedPagePtr page;
    if (!archive_records(K::ColourSource, reload->memory(), n, camera_map, camera_keys, page)) return false;
    domain->colours.push_back(std::move(page)); reload->clear(); at += n;
  }
  colours.clear(); archives_.push_back(std::move(domain));
  return good();
}

size_t DeepGPUFrameWork::archive_window_cost(const uint32_t first, const uint32_t pixels) const
{
  uint64_t cameras = 0, primitives = 0, colours = 0;
  for (const auto &domain : archives_) {
    cameras += archive_span(domain->cameras->ranges, first, pixels).count;
    for (const auto &page : domain->primitives) primitives += archive_span(page->ranges, first, pixels).count;
    for (const auto &page : domain->colours) colours += archive_span(page->ranges, first, pixels).count;
  }
  /* Admission guidance includes immutable inputs, two event endpoints, FP64
   * scans, fits and sorting replacement peaks. It is not a sample/quality cap;
   * any one-pixel minimum still uses the same algorithm and explicit errors. */
  const uint64_t multiplier = domain_.demand.capture_rgb() ? 4 : 1;
  if (cameras > SIZE_MAX / (256 * multiplier) || primitives > SIZE_MAX / (640 * multiplier) ||
      colours > SIZE_MAX / 2048 || pixels > SIZE_MAX / 512) return SIZE_MAX;
  size_t cost = 0;
  for (const uint64_t term : {cameras * 256 * multiplier, primitives * 640 * multiplier,
                              colours * 2048, uint64_t(pixels) * 512}) {
    if (term > SIZE_MAX - cost) return SIZE_MAX;
    cost += size_t(term);
  }
  return cost;
}

bool DeepGPUFrameWork::load_archive_window(const uint32_t first, const uint32_t pixels)
{
  using K = DeepGPUResidentKind;
  const char *trace = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_CHAIN_TRACE");
  if (trace && std::strcmp(trace, "1") == 0)
    std::fprintf(stderr, "DEEP_GPU_ARCHIVE_WINDOW first=%u pixels=%u domains=%llu\n",
                 first, pixels, static_cast<unsigned long long>(archives_.size()));
  for (auto &buffer : tile_) buffer->clear();
  tile_cameras_ = 0;
  for (const auto &domain : archives_) {
    const auto cameras = archive_span(domain->cameras->ranges, first, pixels);
    if (!cameras.count) continue;
    if (uint64_t(tile_cameras_) + cameras.count > UINT32_MAX)
      return fail("Deep reloaded camera window index overflow");
    if (!tile_[size_t(K::CompactCamera)]->append_raw(*queue_,
          domain->cameras->bytes.data() + size_t(cameras.offset) * sizeof(DeepCompactCamera),
          size_t(cameras.count) * sizeof(DeepCompactCamera))) return false;
    const auto load = [&](const ArchivedPagePtr &page, const K kind) {
      const auto span = archive_span(page->ranges, first, pixels);
      if (!span.count) return true;
      const uint32_t stride = kind == K::Primitive ? sizeof(DeepCompactPrimitive) : sizeof(DeepCompactColourSource);
      const uint32_t field = kind == K::Primitive ? offsetof(DeepCompactPrimitive, camera) :
                                                     offsetof(DeepCompactColourSource, camera);
      if (uint64_t(span.offset) + span.count > page->bytes.size() / stride)
        return fail("Deep archived record window exceeds its immutable byte page");
      const char *verify = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
      if (verify && std::strcmp(verify, "1") == 0) {
        /* Camera ownership is integer page metadata, not host curve evaluation. */
        for (uint32_t i = 0; i < span.count; i++) {
          uint32_t camera;
          std::memcpy(&camera, page->bytes.data() + size_t(span.offset + i) * stride + field, 4);
          if (camera < cameras.offset || uint64_t(camera) >= uint64_t(cameras.offset) + cameras.count) {
            std::fprintf(stderr, "DEEP_ARCHIVE_HOST_INVALID first=%u pixels=%u offset=%u record=%u camera=%u base=%u count=%u\n",
                         first, pixels, span.offset, i, camera, cameras.offset, cameras.count);
            return fail("Deep immutable archive page has invalid camera ownership");
          }
        }
      }
      auto &buffer = *tile_[size_t(kind)];
      const size_t old = buffer.used();
      if (!buffer.append_raw(*queue_, page->bytes.data() + size_t(span.offset) * stride,
                                size_t(span.count) * stride)) return false;
      DeepGPUFrameDispatch d;
      d.a = buffer.pointer() + old; d.h = controls_.device_pointer;
      d.n = span.count; d.m = stride; d.k = field; d.base = cameras.offset;
      d.width = cameras.count; d.pixel = tile_cameras_;
      return operation(DeepGPUFrameOperation::CompactArchiveRebase, d, archive_blocks(span.count));
    };
    for (const auto &page : domain->primitives) if (!load(page, K::Primitive)) return false;
    for (const auto &page : domain->colours) if (!load(page, K::ColourSource)) return false;
    tile_cameras_ += cameras.count;
  }
  return good();
}

bool DeepGPUFrameWork::close_tiles(RenderBuffers &buffers,
                                    const std::span<const DeepFrozenConsumer> consumers,
                                    const std::function<bool(DeepGPUFinalTile &&)> &publish,
                                    const std::function<bool()> &cancel_poll)
{
  cancel_poll_ = cancel_poll;
  struct CancelScope {
    std::function<bool()> &callback;
    ~CancelScope() { callback = {}; }
  } cancel_scope{cancel_poll_};
  progress_reserve_->clear();
  if (!preparing_simple_ && !pending_simple_.empty())
    return finish_pending_tiles(buffers, consumers, publish);
  if (!dump_witness(buffers)) return false;
  if (!active_ && archives_.empty()) {
    DeepGPUFinalTile output(host_budget_);
    return close_tile(buffers, consumers, output) && publish(std::move(output)) && dump_trace();
  }
  if (active_ || archives_.empty() || archive_x_ != buffers.params.full_x + buffers.params.window_x ||
      archive_y_ != buffers.params.full_y + buffers.params.window_y ||
      archive_width_ != buffers.params.window_width || archive_height_ != buffers.params.window_height)
    return fail("Deep archived inputs do not match the final native Film window");
  struct WindowGuard {
    BufferParams &p;
    int x, y, width, height;
    explicit WindowGuard(BufferParams &params)
        : p(params), x(p.window_x), y(p.window_y), width(p.window_width), height(p.window_height) {}
    ~WindowGuard() { p.window_x = x; p.window_y = y; p.window_width = width; p.window_height = height; }
  } restore(buffers.params);
  const uint32_t width = uint32_t(archive_width_), height = uint32_t(archive_height_);
  for (uint32_t y = 0; y < height;) {
    if (cancel_poll_ && cancel_poll_()) return false;
    uint32_t x = 0, rows = 1;
    size_t total = 0, free = 0;
    device_->get_device_memory_info(total, free);
    const size_t remaining = device_budget_.quota() - device_budget_.used();
    const size_t available = (total ? std::min(remaining, free) : remaining) / 2;
    if (archive_window_cost(y * width, width) <= available) {
      uint32_t lo = 1, hi = height - y;
      while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        if (archive_window_cost(y * width, mid * width) <= available) lo = mid;
        else hi = mid - 1;
      }
      rows = lo;
    }
    do {
      uint32_t columns = width;
      if (rows == 1) {
        uint32_t lo = 1, hi = width - x;
        while (lo < hi) {
          const uint32_t mid = lo + (hi - lo + 1) / 2;
          if (archive_window_cost(y * width + x, mid) <= available) lo = mid;
          else hi = mid - 1;
        }
        columns = lo;
      }
      if (!load_archive_window(y * width + x, columns * rows)) return false;
      buffers.params.window_x = restore.x + x; buffers.params.window_y = restore.y + y;
      buffers.params.window_width = columns; buffers.params.window_height = rows;
      DeepGPUFinalTile output(host_budget_);
      if (!close_tile(buffers, consumers, output) || !publish(std::move(output))) return false;
      x += columns;
    } while (x < width);
    y += rows;
  }
  if (!dump_trace()) return false;
  discard();
  return true;
}

bool DeepGPUFrameWork::prepare_tiles(RenderBuffers &buffers,
                                      const std::span<const DeepFrozenConsumer> consumers,
                                      const std::function<bool()> &cancel_poll)
{
  if (!domain_.demand.simple_mode || !domain_.demand.needs_rgb || !pending_simple_.empty())
    return fail("Deep pending Simple alpha has an invalid mode, demand or repeated preparation");
  preparing_simple_ = true;
  struct PrepareScope { bool &flag; ~PrepareScope() { flag = false; } } scope{preparing_simple_};
  return close_tiles(buffers, consumers, [&](DeepGPUFinalTile &&output) {
    auto pending = std::allocate_shared<PendingSimpleTile>(
        DeepBudgetAllocator<PendingSimpleTile>(host_budget_), host_budget_);
    pending->output = std::move(output);
    pending->summaries.swap(preparing_summaries_);
    pending_simple_.push_back(std::move(pending));
    return true;
  }, cancel_poll);
}

bool DeepGPUFrameWork::finish_pending_tiles(RenderBuffers &buffers,
                                             const std::span<const DeepFrozenConsumer> consumers,
                                             const std::function<bool(DeepGPUFinalTile &&)> &publish)
{
  if (!domain_.demand.simple_mode) return fail("Pending Simple payload belongs to a different mode");
  const auto &film = buffers.params;
  const int combined = film.get_pass_offset(PASS_COMBINED,
      beauty_denoised_ ? PassMode::DENOISED : PassMode::NOISY);
  if (combined == PASS_UNUSED || combined < 0 || combined + 3 > film.pass_stride)
    return fail("Pending Simple output has no completed native Beauty source");
  for (auto &pending : pending_simple_) {
    if (cancel_poll_ && cancel_poll_()) return false;
    auto &output = pending->output;
    const uint64_t pixels64 = uint64_t(output.width) * output.height;
    const int64_t x = int64_t(output.x) - film.full_x, y = int64_t(output.y) - film.full_y;
    if (output.consumers != consumers.size() || pending->summaries.size() != consumers.size() ||
        pixels64 > UINT32_MAX || output.ranges.size() != pixels64 * consumers.size() ||
        x < 0 || y < 0 || x + output.width > film.width || y + output.height > film.height)
      return fail("Pending Simple output window or consumer identity changed");
    const uint32_t pixels_count = uint32_t(pixels64);
    for (uint32_t consumer = 0; consumer < consumers.size(); consumer++) {
      const auto &summary = pending->summaries[consumer];
      if (!summary) continue; /* Alpha-only or a previously encoded identical family. */
      uint32_t first = UINT32_MAX;
      uint64_t last = 0, count = 0;
      const auto *ranges = output.ranges.data() + size_t(consumer) * pixels_count;
      for (uint32_t i = 0; i < pixels_count; i++) {
        if (ranges[i].count) {
          first = std::min(first, ranges[i].offset);
          last = std::max(last, uint64_t(ranges[i].offset) + ranges[i].count);
          count += ranges[i].count;
        }
      }
      if (!count) first = 0;
      if (last < first || last - first != count || last > output.samples.size() || count > UINT32_MAX ||
          summary->bytes.size() != size_t(pixels_count) * sizeof(DeepCompactOutputSummary))
        return fail("Pending Simple raw bytes have inconsistent family ownership");
      auto samples = make_buffer(), input_ranges = make_buffer(), local_ranges = make_buffer();
      auto population = make_buffer(), denominator = make_buffer();
      if ((count && !samples->append_raw(*queue_, output.samples.data() + first,
                                           size_t(count) * sizeof(DeepGPUFrameSample))) ||
          !input_ranges->append_raw(*queue_, ranges, size_t(pixels_count) * sizeof(DeepGPUFrameFinalRange)) ||
          !local_ranges->reserve(*queue_, size_t(pixels_count) * sizeof(DeepGPUFrameFinalRange), false) ||
          !population->reserve(*queue_, size_t(pixels_count) * sizeof(DeepGPUFramePixel), false) ||
          !denominator->append_raw(*queue_, summary->bytes.data(), summary->bytes.size())) return false;
      DeepGPUFrameDispatch d;
      d.a = input_ranges->pointer(); d.b = local_ranges->pointer(); d.c = population->pointer();
      d.d = controls_.device_pointer; d.n = pixels_count; d.base = first; d.m = uint32_t(count);
      if (!operation(DeepGPUFrameOperation::CompactPendingRanges, d, archive_blocks(pixels_count))) return false;
      d = {}; d.a = denominator->pointer(); d.b = samples->pointer(); d.c = local_ranges->pointer();
      d.d = buffers.buffer.device_pointer; d.e = population->pointer(); d.f = controls_.device_pointer;
      d.g = uint64_t(combined); d.h = (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
      d.n = pixels_count; d.m = output.width; d.k = film.pass_stride; d.width = film.stride;
      d.base = uint32_t(count); d.kind = consumers[consumer].config.precision == DeepCanonicalPrecision::Half;
      d.p = film.exposure;
      if (!operation(DeepGPUFrameOperation::CompactSimpleColour, d, pixels_count, 128) || !good() ||
          (count && !read_final(*samples, size_t(count) * sizeof(DeepGPUFrameSample),
                                 output.samples.data() + first))) return false;
    }
    if (!publish(std::move(output))) return false;
    pending->summaries.clear();
  }
  pending_simple_.clear();
  discard();
  return true;
}

CCL_NAMESPACE_END
