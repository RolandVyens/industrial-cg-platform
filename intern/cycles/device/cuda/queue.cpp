/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_CUDA

#  include "device/cuda/queue.h"

#  include "device/cuda/device_impl.h"
#  include "device/cuda/graphics_interop.h"
#  include "device/cuda/kernel.h"
#  include "util/path.h"
#  include "util/scoped_defer.h"
#  include "util/time.h"
#  include <cstdlib>
#  include <cerrno>
#  include <cmath>
#  include <limits>
#  include <atomic>
#  include <climits>
#  include <iterator>

#include <iterator>

CCL_NAMESPACE_BEGIN

class CUDADeepCapture {
 public:
  explicit CUDADeepCapture(CUDADevice *device, CUstream stream)
      : device_(device), stream_(stream), scratch_(device, "deep_gpu_raw_scratch")
  {
  }
  ~CUDADeepCapture()
  {
    clear_kernel_timings();
    if (module_) {
      cuModuleUnload(module_);
    }
  }

  bool reserve_scratch(const size_t bytes)
  {
    if (bytes <= scratch_.memory_size()) return true;
    size_t total = 0, free = 0;
    device_->get_device_memory_info(total, free);
    const size_t headroom = max(size_t(256) * 1024 * 1024, total / 16);
    if (free < headroom || bytes > free - headroom) {
      device_->set_error("Deep GPU raw scratch exceeds free device memory headroom");
      return false;
    }
    /* An earlier filter-only batch may still reference this allocation. Drain before
     * replacing it, including when the caller preflights the next processing phase. */
    if (!check(cuStreamSynchronize(stream_), "scratch replacement synchronize")) return false;
    scratch_.alloc_to_device(bytes, false);
    return !device_->have_error() && scratch_.device_pointer;
  }

  bool reserve_fallback_scratch(const vector<device_memory *> &batches,
                               const size_t count_size)
  {
    const char *option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS");
    const bool groups = option && std::strcmp(option, "1") == 0;
    size_t peak = scratch_.memory_size();
    /* The completed profile already validated these immutable batch owners and counts.
     * Use the same conservative scratch bound as the resident capture budget. */
    for (device_memory *batch : batches) {
      peak = max(peak, batch->memory_size() / 48 * (groups ? 82 : 73) +
                       count_size * 20 + ((count_size + 255) / 256) * 4 + 512);
    }
    return reserve_scratch(peak);
  }

  bool process(device_memory &events,
               const vector<uint32_t> &counts,
               vector<uint32_t> &pixel_offsets,
               vector<uint8_t> &storage,
               const vector<uint32_t> *pixel_selection = nullptr,
               const bool filter_only = false)
  {
    const double start = time_dt();
    const size_t count = events.memory_size() / 48;
    if (events.device != device_ || events.memory_size() % 48 || count == 0 ||
        count > size_t(std::numeric_limits<int>::max()) || counts.size() < 2 ||
        counts.size() > size_t(std::numeric_limits<int>::max()))
    {
      device_->set_error("Invalid Deep GPU raw input range or owner");
      return false;
    }
    if (!load()) {
      return false;
    }
    const uint32_t n = uint32_t(count), pixels = uint32_t(counts.size() - 1);
    const uint32_t size = pixels + 1, blocks = (size + 255) / 256;
    const char *group_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS");
    const bool group_heads = group_option && std::strcmp(group_option, "1") == 0;
    group_lanes_ = 1;
    if (group_heads) {
      if (const char *option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_LANES")) {
        char *end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(option, &end, 10);
        if (errno || end == option || *end || value < 1 || value > 32 || (value & (value - 1))) {
          device_->set_error("Invalid Deep GPU group lane spacing"); return false;
        }
        group_lanes_ = uint32_t(value);
      }
    }
    const uint32_t head_chunks = (n + 1023) / 1024, head_size = head_chunks + 1;
    size_t bytes = 0;
    auto reserve = [&](size_t length) {
      const size_t offset = bytes;
      bytes += (length + 15) & ~size_t(15);
      return offset;
    };
    size_t keys_offset[2], ids_offset[2];
    for (int i = 0; i < 2; i++) {
      keys_offset[i] = reserve(size_t(n) * 8);
      ids_offset[i] = reserve(size_t(n) * 4);
    }
    /* Volume summary/difference consume the raw input and keep mask directly. They never gather
     * host output, so reserving a second 48-byte payload here wastes capacity-scaled device memory. */
    const size_t keep_offset = reserve(n);
    const size_t output_offset = filter_only ? 0 : reserve(size_t(n) * 48);
    const size_t counts_offset = reserve(size_t(size) * 4);
    const size_t actual_offset = reserve(size_t(size) * 4);
    const size_t kept_offset = reserve(size_t(size) * 4);
    const size_t offsets_offset = reserve(size_t(size) * 4);
    const size_t totals_offset = reserve(size_t(blocks) * 4), error_offset = reserve(4);
    const size_t selection_offset = reserve(size_t(pixels) * 4);
    const size_t heads_at = group_heads ? reserve(size_t(n + 1) * 8) : 0;
    const size_t head_counts_at = group_heads ? reserve(size_t(head_size) * 4) : 0;
    const size_t head_offsets_at = group_heads ? reserve(size_t(head_size) * 4) : 0;
    const size_t head_totals_at = group_heads ? reserve(size_t((head_size + 255) / 256) * 4) : 0;
    if (!reserve_scratch(bytes)) return false;
    if (device_->have_error() || !scratch_.device_pointer) {
      return false;
    }
    const CUdeviceptr base = CUdeviceptr(scratch_.device_pointer);
    CUdeviceptr keys[2] = {base + keys_offset[0], base + keys_offset[1]};
    CUdeviceptr ids[2] = {base + ids_offset[0], base + ids_offset[1]};
    const CUdeviceptr raw = CUdeviceptr(events.device_pointer), keep = base + keep_offset;
    const CUdeviceptr output = base + output_offset, gpu_counts = base + counts_offset;
    const CUdeviceptr actual = base + actual_offset, kept = base + kept_offset;
    const CUdeviceptr offsets = base + offsets_offset, totals = base + totals_offset;
    const CUdeviceptr error = base + error_offset;
    uint32_t status = 0;
    uint32_t sort_count = n, group_count = 0;
    const char *selected_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_SELECTED_COMPACT");
    const bool selected_compact = pixel_selection && !filter_only && selected_option &&
        std::strcmp(selected_option, "1") == 0;
    bool completed = false;
    /* On a submission/readback failure, drain before the status destination leaves scope.
     * Successful filter-only calls deliberately leave their work ordered on this stream. */
    SCOPED_DEFER(if (!completed) { cuStreamSynchronize(stream_); });
    if (!check(cuMemcpyHtoDAsync(gpu_counts, counts.data(), size_t(size) * 4, stream_), "counts upload") ||
        !check(cuMemsetD8Async(actual, 0, size_t(size) * 4, stream_), "actual counts clear") ||
        !check(cuMemsetD8Async(error, 0, 4, stream_), "error clear") ||
        !launch(Keys, n, raw, n, pixels, keys[0], ids[0], actual, error) ||
        !launch(Validate, pixels, gpu_counts, actual, n, pixels, error))
    {
      return false;
    }
    if (!check(cuMemcpyDtoHAsync(&status, error, 4, stream_), "validation readback") ||
        !check(cuStreamSynchronize(stream_), "validation synchronize"))
    {
      return false;
    }
    if (status) {
      device_->set_error("Deep GPU raw ownership, overflow or cursor mismatch");
      return false;
    }
    if (selected_compact) {
      if (pixel_selection->size() != pixels || !load_selected_functions()) {
        device_->set_error("Invalid Deep GPU selected-pixel layout or module");
        return false;
      }
      const uint32_t chunks = (n + 1023) / 1024, chunk_size = chunks + 1;
      const auto aligned = [](size_t length) { return (length + 15) & ~size_t(15); };
      const CUdeviceptr selected_counts = output;
      const CUdeviceptr selected_offsets = selected_counts + aligned(size_t(chunk_size) * 4);
      const CUdeviceptr selected_totals = selected_offsets + aligned(size_t(chunk_size) * 4);
      /* Reuse the uninitialized gather output until compaction has consumed these offsets. */
      if (aligned(size_t(chunk_size) * 4) * 2 + aligned(size_t((chunk_size + 255) / 256) * 4) > size_t(n) * 48 ||
          !check(cuMemcpyHtoDAsync(base + selection_offset, pixel_selection->data(),
                                  size_t(pixels) * 4, stream_), "compact selection upload") ||
          !check(cuMemsetD8Async(selected_counts, 0, size_t(chunk_size) * 4, stream_),
                 "selected block counts clear") ||
          !launch(SelectedCounts, chunks * 256, raw, base + selection_offset, n, selected_counts) ||
          !launch(Scan, chunk_size, selected_counts, selected_offsets, selected_totals, chunk_size) ||
          !launch(ScanAdd, chunk_size, selected_offsets, selected_totals, chunk_size) ||
          !check(cuMemcpyDtoHAsync(&sort_count, selected_offsets + size_t(chunks) * 4, 4, stream_),
                 "selected population readback") ||
          !check(cuStreamSynchronize(stream_), "selected population synchronize")) return false;
      if (sort_count > n) {
        device_->set_error("Deep GPU selected population exceeds validated input");
        return false;
      }
      if (!sort_count) {
        pixel_offsets.assign(size, 0); storage.clear(); completed = true;
        return true;
      }
      if (!launch(SelectedScatter, chunks * 256, raw, base + selection_offset, keys[0],
                  selected_offsets, n, keys[1], ids[1])) return false;
      std::swap(keys[0], keys[1]); std::swap(ids[0], ids[1]);
    }
    auto sort = [&]() {
      if (!launch(SortBlocks, (sort_count + 1023) / 1024 * 256, keys[0], ids[0], keys[1], ids[1], sort_count))
        return false;
      std::swap(keys[0], keys[1]);
      std::swap(ids[0], ids[1]);
      for (uint32_t width = 1024; width < sort_count; width *= 2) {
        if (!launch(Merge, sort_count, keys[0], ids[0], keys[1], ids[1], sort_count, width)) return false;
        std::swap(keys[0], keys[1]);
        std::swap(ids[0], ids[1]);
      }
      return true;
    };
    if (!sort()) return false;
    const CUdeviceptr heads = group_heads ? base + heads_at : 0;
    if (group_heads) {
      const uint32_t chunks = (sort_count + 1023) / 1024, count_size = chunks + 1;
      const CUdeviceptr head_counts = base + head_counts_at;
      const CUdeviceptr head_offsets = base + head_offsets_at;
      const CUdeviceptr head_totals = base + head_totals_at;
      if (!load_group_functions() ||
          !check(cuMemsetD8Async(head_counts, 0, size_t(count_size) * 4, stream_), "group counts clear") ||
          !launch(GroupCounts, chunks * 256, keys[0], sort_count, head_counts) ||
          !launch(Scan, count_size, head_counts, head_offsets, head_totals, count_size) ||
          !launch(ScanAdd, count_size, head_offsets, head_totals, count_size) ||
          !check(cuMemcpyDtoHAsync(&group_count, head_offsets + size_t(chunks) * 4, 4, stream_),
                 "group population readback") ||
          !check(cuStreamSynchronize(stream_), "group population synchronize")) return false;
      if (!group_count || group_count > sort_count) {
        device_->set_error("Invalid Deep GPU sorted group population"); return false;
      }
      if (!launch(GroupScatter, chunks * 256, keys[0], sort_count, head_offsets, heads, group_count) ||
          !launch(FilterGroups, group_count, raw, ids[0], keep, heads, group_count, group_lanes_)) return false;
    }
    else if (!launch(Filter, sort_count, raw, keys[0], ids[0], keep, sort_count)) return false;
    prepared_raw_ = raw;
    prepared_keys_ = keys[0];
    prepared_ids_ = ids[0];
    prepared_keep_ = keep;
    prepared_count_ = n;
    prepared_heads_ = heads;
    prepared_groups_ = group_count;
    if (filter_only) {
      completed = true;
      return true;
    }
    if (pixel_selection && !selected_compact) {
      if (pixel_selection->size() != pixels || !load_volume_functions() ||
          !check(cuMemcpyHtoDAsync(base + selection_offset, pixel_selection->data(),
                                  size_t(pixels) * 4, stream_), "selection upload") ||
          !launch_volume(8, n, raw, keep, base + selection_offset, n))
      {
        return false;
      }
    }
    if (selected_compact) {
      if (!launch(SelectedSurvivorKeys, sort_count, raw, keep, ids[0], sort_count,
                  keys[1], ids[1])) return false;
      std::swap(keys[0], keys[1]); std::swap(ids[0], ids[1]);
    }
    else if (!launch(SurvivorKeys, n, raw, keep, keys[0], ids[0], n)) return false;
    if (!sort() ||
        !check(cuMemsetD8Async(kept, 0, size_t(size) * 4, stream_), "survivor counts clear") ||
        !launch(Gather, sort_count, raw, keys[0], ids[0], output, kept, sort_count) ||
        !launch(Scan, size, kept, offsets, totals, size) ||
        !launch(ScanAdd, size, offsets, totals, size))
    {
      return false;
    }
    pixel_offsets.resize(size);
    if (!check(cuMemcpyDtoHAsync(pixel_offsets.data(), offsets, size_t(size) * 4, stream_), "offset readback") ||
        !check(cuStreamSynchronize(stream_), "processing synchronize"))
    {
      return false;
    }
    const uint32_t retained = pixel_offsets.back();
    if (retained > n || pixel_offsets.front() != 0) {
      device_->set_error("Invalid Deep GPU raw survivor layout");
      return false;
    }
    const double compute_finish = time_dt();
    storage.resize(size_t(retained) * 48);
    if (retained &&
        (!check(cuMemcpyDtoHAsync(storage.data(), output, storage.size(), stream_), "survivor readback") ||
         !check(cuStreamSynchronize(stream_), "readback synchronize")))
    {
      return false;
    }
    const char *trace = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE");
    if (trace && std::strcmp(trace, "1") == 0) {
      std::fprintf(stderr,
                   "DEEP_GPU_RAW_PROCESS events=%u retained=%u scratch=%zu compute_sync_ms=%.6f "
                   "readback_ms=%.6f total_ms=%.6f\n",
                   n, retained, scratch_.memory_size(), (compute_finish - start) * 1000,
                   (time_dt() - compute_finish) * 1000, (time_dt() - start) * 1000);
    }
    completed = !device_->have_error();
    return completed;
  }

  DeepVolumeCaptureResult volume_profile(
      const vector<device_memory *> &batches,
      const vector<vector<uint32_t>> &counts,
      const vector<float> &population,
      const vector<float> &beauty,
      const int max_profile_samples,
      vector<uint32_t> &profile_offsets,
      vector<uint8_t> &profile_storage,
      vector<uint32_t> &fallback_pixels)
  {
    const double start = time_dt();
    kernel_timings_enabled_ = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_KERNEL_TIMINGS") != nullptr;
    SCOPED_DEFER(clear_kernel_timings());
    const size_t pixels_size = population.size();
    if (!pixels_size || pixels_size >= size_t(std::numeric_limits<int>::max()) ||
        batches.empty() || batches.size() != counts.size() || beauty.size() != pixels_size * 4 ||
        max_profile_samples <= 0 || max_profile_samples > 256)
      return DeepVolumeCaptureResult::Unsupported;
    uint32_t samples = 1;
    for (const float value : population) {
      if (!std::isfinite(value) || value < 1 || value > 128 || value != floorf(value))
        return DeepVolumeCaptureResult::Unsupported;
      samples = max(samples, uint32_t(value));
    }
    for (const float value : beauty) {
      if (!std::isfinite(value)) return DeepVolumeCaptureResult::Unsupported;
    }
    const uint32_t pixels = uint32_t(pixels_size), size = pixels + 1;
    const size_t cells = pixels_size * samples;
    if (cells >= size_t(std::numeric_limits<int>::max()))
      return DeepVolumeCaptureResult::Unsupported;
    const char *group_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS");
    const bool group_heads = group_option && std::strcmp(group_option, "1") == 0;
    size_t scratch_peak = scratch_.memory_size();
    for (size_t i = 0; i < batches.size(); i++) {
      if (!batches[i] || batches[i]->device != device_ || counts[i].size() != size ||
          batches[i]->memory_size() % 48)
      {
        device_->set_error("Invalid Deep GPU volume batch range or owner");
        return DeepVolumeCaptureResult::Error;
      }
      /* Both preparation passes use filter_only=true; the raw host-fallback gather runs only
       * after this frame/prefix workspace has been drained and released. */
      scratch_peak = max(scratch_peak, batches[i]->memory_size() / 48 * (group_heads ? 34 : 25) + size_t(size) * 20 +
                                      ((size + 255) / 256) * 4 + 512);
    }
    size_t budget = size_t(4096) * 1024 * 1024;
    if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_BUDGET_MB")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long long mb = std::strtoull(value, &end, 10);
      if (errno || end == value || *end || *value == '-' ||
          mb > std::numeric_limits<size_t>::max() / (1024 * 1024))
      {
        device_->set_error("Invalid Deep GPU volume processing memory budget");
        return DeepVolumeCaptureResult::Error;
      }
      budget = size_t(mb) * 1024 * 1024;
    }
    if (!load_volume_functions() || (group_heads && !load_group_difference()))
      return DeepVolumeCaptureResult::Error;
    device_only_memory<uint8_t> frame(device_, "deep_gpu_volume_frame");
    device_only_memory<uint8_t> prefix(device_, "deep_gpu_volume_prefix");
    device_only_memory<uint8_t> prepared_cache(device_, "deep_gpu_volume_prepared_cache");
    device_only_memory<uint32_t> window_index(device_, "deep_gpu_volume_window_index");
    vector<unique_ptr<device_only_memory<uint8_t>>> compact_caches;
    struct PreparedBatch {
      CUdeviceptr raw, keys, ids, keep, heads;
      uint32_t count, groups;
      vector<uint32_t> window_ranges;
    };
    vector<PreparedBatch> cached_batches;
    const char *cache_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE");
    bool cache_enabled = cache_option && std::strcmp(cache_option, "1") == 0;
    size_t cache_bytes = 0;
    for (device_memory *batch : batches) {
      const size_t n = batch->memory_size() / 48;
      /* Compact heads replace full keys in cached differences. Reserve the worst-case number
       * of distinct groups, then allocate each cache using its actual group population. */
      const size_t grouping = group_heads ? (n + 1) * 8 : n * 8;
      cache_bytes += ((grouping + 15) & ~size_t(15)) +
                     ((n * 4 + 15) & ~size_t(15)) + ((n + 15) & ~size_t(15));
    }
    device_only_memory<uint32_t> debug_hashes(device_, "deep_gpu_volume_debug_hashes");
    CUmodule debug_module = nullptr;
    CUfunction debug_hash_kernel = nullptr;
    SCOPED_DEFER(if (debug_module) { cuModuleUnload(debug_module); });
    vector<uint32_t> host_debug_hashes;
    vector<string> debug_hash_labels;
    /* Declare every asynchronous host-transfer buffer before the drain guard, so error
     * unwinding synchronizes before either host destinations or device allocations die. */
    vector<uint32_t> ignored_offsets, host_ray_offsets, host_grids, host_pixel_prefix_offsets;
    vector<uint32_t> original_ray_offsets, host_selection, window_selection, host_output_counts;
    vector<uint32_t> window_boundaries;
    vector<uint8_t> ignored_storage;
    struct SynchronizeBeforeFree {
      CUstream stream;
      ~SynchronizeBeforeFree() { cuStreamSynchronize(stream); }
    } synchronize_before_free{stream_};
    const char *hash_path = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_HASH_MODULE");
    if (hash_path && pixels == 12544) {
      vector<uint8_t> binary;
      if (!path_read_binary(hash_path, binary) || binary.empty()) {
        device_->set_error("Cannot read diagnostic Deep hash module");
        return DeepVolumeCaptureResult::Error;
      }
      binary.push_back(0);
      if (!check(cuModuleLoadData(&debug_module, binary.data()), "debug hash module") ||
          !check(cuModuleGetFunction(&debug_hash_kernel, debug_module, "deep_debug_hash"),
                 "debug hash kernel")) return DeepVolumeCaptureResult::Error;
      debug_hashes.alloc_to_device((batches.size() * 2 + 8) * 2);
      if (!check(cuMemsetD8Async(CUdeviceptr(debug_hashes.device_pointer), 0,
                                debug_hashes.memory_size(), stream_), "debug hash clear"))
        return DeepVolumeCaptureResult::Error;
    }
    const auto hash = [&](const string &label, CUdeviceptr data, uint64_t length) {
      if (!debug_hash_kernel) return true;
      if (debug_hash_labels.size() >= batches.size() * 2 + 8) {
        device_->set_error("Deep debug hash slot overflow"); return false;
      }
      CUdeviceptr output = CUdeviceptr(debug_hashes.device_pointer) + debug_hash_labels.size() * 8;
      void *arguments[] = {&data, &length, &output};
      debug_hash_labels.push_back(label);
      return check(cuLaunchKernel(debug_hash_kernel, 128, 1, 1, 256, 1, 1, 0, stream_,
                                  arguments, nullptr), "debug payload hash");
    };
    size_t bytes = 0;
    auto reserve = [&](size_t length) {
      const size_t offset = bytes;
      bytes += (length + 15) & ~size_t(15);
      return offset;
    };
    const size_t rays_at = reserve(cells * 16), selection_at = reserve(pixels_size * 4);
    const size_t initial_selection_at = reserve(pixels_size * 4);
    const size_t endpoints_at = reserve(pixels_size * 8), ray_counts_at = reserve(size_t(size) * 4);
    const size_t ray_offsets_at = reserve(size_t(size) * 4), cameras_at = reserve(cells * 4);
    const size_t grids_at = reserve(pixels_size * 16), map_at = reserve(cells * 4);
    const size_t pixel_prefix_offsets_at = reserve(size_t(size) * 4);
    const size_t population_at = reserve(pixels_size * 4), beauty_at = reserve(pixels_size * 16);
    const size_t output_at = reserve(pixels_size * size_t(max_profile_samples) * 24), compact_at = reserve(pixels_size * size_t(max_profile_samples) * 24);
    const size_t counts_at = reserve(size_t(size) * 4), offsets_at = reserve(size_t(size) * 4);
    const size_t blocks = (size + 255) / 256, totals_at = reserve(blocks * 4);
    auto fits = [&](size_t added) {
      size_t total = 0, free = 0;
      device_->get_device_memory_info(total, free);
      const size_t headroom = max(size_t(256) * 1024 * 1024, total / 16);
      return free >= headroom && added <= free - headroom;
    };
    if (bytes > budget || scratch_peak > budget - bytes || !fits(bytes + scratch_peak))
      return DeepVolumeCaptureResult::Unsupported;
    /* Cache only within this immutable volume-profile invocation. Pointer identity cannot
     * establish reuse across calls: callers can overwrite an existing input allocation. */
    cache_enabled = cache_enabled && cache_bytes <= budget - bytes - scratch_peak &&
                    fits(bytes + scratch_peak + cache_bytes);
    const char *scratch_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_SCRATCH_PREFLIGHT");
    if (scratch_option && std::strcmp(scratch_option, "1") == 0 &&
        !reserve_scratch(scratch_peak)) return DeepVolumeCaptureResult::Error;
    if (cache_enabled && !group_heads) prepared_cache.alloc_to_device(cache_bytes, false);
    else if (!cache_enabled) cache_bytes = 0;
    frame.alloc_to_device(bytes, false);
    if (device_->have_error()) return DeepVolumeCaptureResult::Error;
    size_t cache_cursor = 0, compact_cache_bytes = 0;
    const auto save_prepared = [&]() {
      if (!cache_enabled) return true;
      PreparedBatch batch{prepared_raw_, 0, 0, 0, 0, prepared_count_, prepared_groups_};
      CUdeviceptr cache_base = CUdeviceptr(prepared_cache.device_pointer);
      if (group_heads) {
        const auto aligned = [](size_t n) { return (n + 15) & ~size_t(15); };
        const size_t actual_bytes = aligned(size_t(prepared_groups_ + 1) * 8) +
                                    aligned(size_t(prepared_count_) * 4) + aligned(prepared_count_);
        if (actual_bytes > cache_bytes - compact_cache_bytes) {
          device_->set_error("Deep GPU compact cache exceeds reserved budget"); return false;
        }
        auto owner = make_unique<device_only_memory<uint8_t>>(device_, "deep_gpu_group_cache");
        owner->alloc_to_device(actual_bytes, false);
        cache_base = CUdeviceptr(owner->device_pointer);
        compact_caches.push_back(std::move(owner));
        if (device_->have_error() || !cache_base) return false;
        compact_cache_bytes += actual_bytes;
        cache_cursor = 0;
      }
      const auto copy = [&](CUdeviceptr source, size_t length, CUdeviceptr &destination) {
        destination = cache_base + cache_cursor;
        cache_cursor += (length + 15) & ~size_t(15);
        return check(cuMemcpyDtoDAsync(destination, source, length, stream_), "prepared cache copy");
      };
      if (!(group_heads ? copy(prepared_heads_, size_t(prepared_groups_ + 1) * 8, batch.heads) :
                           copy(prepared_keys_, size_t(prepared_count_) * 8, batch.keys)) ||
          !copy(prepared_ids_, size_t(prepared_count_) * 4, batch.ids) ||
          !copy(prepared_keep_, prepared_count_, batch.keep)) return false;
      cached_batches.push_back(batch);
      return true;
    };
    const auto prepare_difference = [&](size_t i) {
      if (!cache_enabled)
        return process(*batches[i], counts[i], ignored_offsets, ignored_storage, nullptr, true);
      const PreparedBatch &batch = cached_batches[i];
      prepared_raw_ = batch.raw;
      prepared_keys_ = batch.keys;
      prepared_ids_ = batch.ids;
      prepared_keep_ = batch.keep;
      prepared_count_ = batch.count;
      prepared_heads_ = batch.heads;
      prepared_groups_ = batch.groups;
      return true;
    };
    const CUdeviceptr base = CUdeviceptr(frame.device_pointer);
    const CUdeviceptr rays = base + rays_at, selection = base + selection_at;
    const CUdeviceptr endpoints = base + endpoints_at, ray_counts = base + ray_counts_at;
    const CUdeviceptr ray_offsets = base + ray_offsets_at, cameras = base + cameras_at;
    const CUdeviceptr grids = base + grids_at, map = base + map_at;
    const CUdeviceptr pop = base + population_at, rgb = base + beauty_at;
    if (!check(cuMemcpyHtoDAsync(pop, population.data(), pixels_size * 4, stream_), "population upload") ||
        !check(cuMemcpyHtoDAsync(rgb, beauty.data(), pixels_size * 16, stream_), "beauty upload") ||
        !launch_volume(0, uint32_t(cells), rays, uint32_t(cells), selection, pixels, endpoints))
      return DeepVolumeCaptureResult::Error;
    for (size_t i = 0; i < batches.size(); i++) {
      if (!process(*batches[i], counts[i], ignored_offsets, ignored_storage, nullptr, true) ||
          !save_prepared() ||
          !hash("summary-raw-" + std::to_string(i), prepared_raw_, batches[i]->memory_size()) ||
          !launch_volume(1, prepared_count_, prepared_raw_, prepared_keep_, prepared_count_,
                         samples, rays, selection, endpoints))
        return DeepVolumeCaptureResult::Error;
    }
    if (cache_enabled && group_heads) cache_bytes = compact_cache_bytes;
    if (!launch_volume(2, pixels, rays, pixels, samples, uint32_t(max_profile_samples), pop,
                       endpoints, selection, ray_counts, cameras, grids))
      return DeepVolumeCaptureResult::Error;
    if (!hash("grids", grids, pixels_size * 16)) return DeepVolumeCaptureResult::Error;
    host_ray_offsets.assign(size, 0);
    host_grids.assign(pixels_size * 4, 0);
    if (!check(cuMemcpyDtoHAsync(host_ray_offsets.data(), ray_counts, size_t(size) * 4, stream_),
               "ray counts readback") ||
        !check(cuMemcpyDtoHAsync(host_grids.data(), grids, pixels_size * 16, stream_),
               "ray grids readback") || !check(cuStreamSynchronize(stream_), "ray layout synchronize"))
      return DeepVolumeCaptureResult::Error;
    for (uint32_t p = 1; p < size; p++) host_ray_offsets[p] += host_ray_offsets[p - 1];
    const uint32_t active_rays = host_ray_offsets.back();
    host_pixel_prefix_offsets.assign(size, 0);
    size_t prefix_elements = 0;
    for (uint32_t p = 0; p < pixels; p++) {
      const uint32_t intervals = host_grids[size_t(p) * 4 + 2];
      if (intervals > uint32_t(max_profile_samples)) {
        device_->set_error("Invalid Deep GPU profile interval count");
        return DeepVolumeCaptureResult::Error;
      }
      prefix_elements += size_t(host_ray_offsets[p + 1] - host_ray_offsets[p]) * (intervals + 2);
      if (prefix_elements > std::numeric_limits<uint32_t>::max())
        return DeepVolumeCaptureResult::Unsupported;
      host_pixel_prefix_offsets[p + 1] = uint32_t(prefix_elements);
    }
    const size_t prefix_bytes = prefix_elements * 32 + size_t(active_rays) * 12;
    const char *chunk_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS");
    const bool prefix_chunks = chunk_option && std::strcmp(chunk_option, "1") == 0;
    if (!prefix_chunks && (scratch_.memory_size() + cache_bytes > budget - bytes ||
        prefix_bytes > budget - bytes - scratch_.memory_size() - cache_bytes || !fits(prefix_bytes)))
      return DeepVolumeCaptureResult::Unsupported;
    const CUdeviceptr output = base + output_at, gpu_counts = base + counts_at;
    const CUdeviceptr offsets = base + offsets_at, totals = base + totals_at;
    double readback_start = 0;
    size_t allocated_prefix_bytes = prefix_bytes;
    if (!prefix_chunks) {
    prefix.alloc_to_device(max(prefix_bytes, size_t(16)), false);
    if (device_->have_error()) return DeepVolumeCaptureResult::Error;
    const CUdeviceptr difference = CUdeviceptr(prefix.device_pointer);
    const CUdeviceptr transmittance = difference + prefix_elements * 28;
    const CUdeviceptr owners = transmittance + prefix_elements * 4;
    const CUdeviceptr last_back = owners + size_t(active_rays) * 4;
    const CUdeviceptr ray_prefix_offsets = last_back + size_t(active_rays) * 4;
    if (!check(cuMemsetD8Async(difference, 0, prefix_bytes, stream_), "volume difference clear") ||
        !check(cuMemcpyHtoDAsync(ray_offsets, host_ray_offsets.data(), size_t(size) * 4, stream_),
               "ray offsets upload") ||
        !check(cuMemcpyHtoDAsync(base + pixel_prefix_offsets_at, host_pixel_prefix_offsets.data(),
                               size_t(size) * 4, stream_), "pixel prefix offsets upload") ||
        !launch_volume(3, pixels, cameras, ray_offsets, pixels, samples, map, owners,
                       base + pixel_prefix_offsets_at, ray_prefix_offsets, grids) ||
        !check(cuMemcpyDtoDAsync(base + initial_selection_at, selection, pixels_size * 4, stream_),
               "initial selection freeze"))
      return DeepVolumeCaptureResult::Error;
    for (size_t i = 0; i < batches.size(); i++) {
      if (!prepare_difference(i) ||
          !hash("difference-raw-" + std::to_string(i), prepared_raw_, batches[i]->memory_size()) ||
          !launch_difference(samples, map, grids, difference, selection,
                             base + initial_selection_at, last_back, ray_prefix_offsets))
        return DeepVolumeCaptureResult::Error;
    }
    if (!hash("differences", difference, prefix_elements * 28) ||
        !hash("ray-offsets", ray_offsets, size_t(size) * 4) ||
        !hash("ray-prefix-offsets", ray_prefix_offsets, size_t(active_rays) * 4))
      return DeepVolumeCaptureResult::Error;
    if (!launch_volume(5, active_rays, difference, owners, grids, active_rays, transmittance,
                       ray_prefix_offsets, selection) ||
        !launch_volume(6, pixels, ray_offsets, transmittance, grids, rgb, pixels, selection,
                       output, gpu_counts, uint32_t(max_profile_samples), ray_prefix_offsets) ||
        !hash("finish-counts", gpu_counts, size_t(size) * 4) ||
        !launch(Scan, size, gpu_counts, offsets, totals, size) ||
        !launch(ScanAdd, size, offsets, totals, size))
      return DeepVolumeCaptureResult::Error;
    if (!hash("transmittance", transmittance, prefix_elements * 4))
      return DeepVolumeCaptureResult::Error;
    profile_offsets.resize(size);
    fallback_pixels.resize(pixels);
    if (!check(cuMemcpyDtoHAsync(profile_offsets.data(), offsets, size_t(size) * 4, stream_),
               "profile offsets readback") ||
        !check(cuMemcpyDtoHAsync(fallback_pixels.data(), selection, pixels_size * 4, stream_),
               "profile selection readback") ||
        !check(cuStreamSynchronize(stream_), "profile layout synchronize"))
      return DeepVolumeCaptureResult::Error;

    }
    else {
      /* Pixels are independent. Bound the ray/prefix workspace without reducing intervals,
       * dropping records or changing either per-ray or per-pixel accumulation order. */
      if (debug_hash_kernel || scratch_.memory_size() + cache_bytes > budget - bytes)
        return DeepVolumeCaptureResult::Unsupported;
      size_t total = 0, free = 0;
      device_->get_device_memory_info(total, free);
      const size_t headroom = max(size_t(256) * 1024 * 1024, total / 16);
      if (free <= headroom) return DeepVolumeCaptureResult::Unsupported;
      const size_t available_prefix = min(budget - bytes - scratch_.memory_size() - cache_bytes,
                                          free - headroom);
      const char *indexed_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_INDEXED_WINDOWS");
      const bool indexed_windows = indexed_option && std::strcmp(indexed_option, "1") == 0;
      size_t prefix_limit = available_prefix;
      if (indexed_windows) {
        if (!cache_enabled || !group_heads) {
          device_->set_error("Indexed Deep windows require cached group heads");
          return DeepVolumeCaptureResult::Error;
        }
        /* Reserve the maximum endpoint metadata before packing. Actual W+1 allocation can
         * be smaller, but a full prefix window must never consume its index reservation. */
        const size_t index_reservation = size_t(pixels + 1) * 8;
        if (index_reservation >= prefix_limit) return DeepVolumeCaptureResult::Unsupported;
        prefix_limit -= index_reservation;
      }
      uint32_t pixel_limit = pixels;
      if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNK_PIXELS")) {
        char *end = nullptr;
        const unsigned long count = std::strtoul(value, &end, 10);
        if (end == value || *end || !count || count > pixels) {
          device_->set_error("Invalid Deep GPU prefix chunk pixel limit");
          return DeepVolumeCaptureResult::Error;
        }
        pixel_limit = uint32_t(count);
      }
      struct PrefixWindow { uint32_t begin, end; size_t bytes; };
      vector<PrefixWindow> windows;
      allocated_prefix_bytes = 16;
      for (uint32_t begin = 0; begin < pixels;) {
        uint32_t end = begin;
        size_t window_bytes = 0;
        while (end < pixels && end - begin < pixel_limit) {
          const size_t rays = host_ray_offsets[end + 1] - host_ray_offsets[end];
          const size_t added = rays * (size_t(host_grids[size_t(end) * 4 + 2] + 2) * 32 + 12);
          if (added > prefix_limit - window_bytes) break;
          window_bytes += added;
          end++;
        }
        if (end == begin) return DeepVolumeCaptureResult::Unsupported;
        windows.push_back({begin, end, window_bytes});
        allocated_prefix_bytes = max(allocated_prefix_bytes, window_bytes);
        begin = end;
      }
      original_ray_offsets = host_ray_offsets;
      if (indexed_windows) {
        if (!cache_enabled || !group_heads) {
          device_->set_error("Indexed Deep windows require cached group heads");
          return DeepVolumeCaptureResult::Error;
        }
        const size_t endpoint_count = windows.size() + 1;
        const size_t index_bytes = endpoint_count * 8;
        if (endpoint_count > std::numeric_limits<uint32_t>::max() ||
            allocated_prefix_bytes > available_prefix ||
            index_bytes > available_prefix - allocated_prefix_bytes || !fits(index_bytes))
        {
          return DeepVolumeCaptureResult::Unsupported;
        }
        if (!check(cuModuleGetFunction(&volume_functions_[10], module_,
                                      "deep_volume_group_window_bounds"), "window bounds lookup"))
          return DeepVolumeCaptureResult::Error;
        window_index.alloc_to_device(endpoint_count * 2);
        if (device_->have_error()) return DeepVolumeCaptureResult::Error;
        window_boundaries.reserve(endpoint_count);
        for (const PrefixWindow &window : windows) window_boundaries.push_back(window.begin);
        window_boundaries.push_back(pixels);
        const CUdeviceptr boundary_input = CUdeviceptr(window_index.device_pointer);
        const CUdeviceptr bounds_output = boundary_input + endpoint_count * 4;
        if (!check(cuMemcpyHtoDAsync(boundary_input, window_boundaries.data(), endpoint_count * 4,
                                    stream_), "window boundary upload"))
          return DeepVolumeCaptureResult::Error;
        for (PreparedBatch &batch : cached_batches) {
          batch.window_ranges.resize(endpoint_count);
          if (!launch_volume(10, uint32_t(endpoint_count), batch.heads, batch.groups, boundary_input,
                             uint32_t(endpoint_count), bounds_output) ||
              !check(cuMemcpyDtoHAsync(batch.window_ranges.data(), bounds_output, endpoint_count * 4,
                                      stream_), "group window ranges readback"))
            return DeepVolumeCaptureResult::Error;
        }
        if (!check(cuStreamSynchronize(stream_), "group window index synchronize"))
          return DeepVolumeCaptureResult::Error;
        for (const PreparedBatch &batch : cached_batches) {
          if (batch.window_ranges.front() != 0 || batch.window_ranges.back() != batch.groups ||
              !std::is_sorted(batch.window_ranges.begin(), batch.window_ranges.end()))
          {
            device_->set_error("Invalid Deep GPU group window index");
            return DeepVolumeCaptureResult::Error;
          }
        }
      }
      host_selection.resize(pixels);
      window_selection.resize(pixels);
      host_output_counts.assign(size, 0);
      if (!check(cuMemcpyDtoHAsync(host_selection.data(), selection, pixels_size * 4, stream_),
                 "chunk selection readback") ||
          !check(cuStreamSynchronize(stream_), "chunk selection synchronize"))
        return DeepVolumeCaptureResult::Error;
      prefix.alloc_to_device(allocated_prefix_bytes, false);
      if (device_->have_error()) return DeepVolumeCaptureResult::Error;
      size_t window_number = 0;
      uint64_t dispatched_groups = 0;
      for (const PrefixWindow &window : windows) {
        size_t elements = 0;
        host_ray_offsets.assign(size, 0);
        host_pixel_prefix_offsets.assign(size, 0);
        for (uint32_t p = 0; p < pixels; p++) {
          const uint32_t count = p >= window.begin && p < window.end ?
              original_ray_offsets[p + 1] - original_ray_offsets[p] : 0;
          host_ray_offsets[p + 1] = host_ray_offsets[p] + count;
          elements += size_t(count) * (host_grids[size_t(p) * 4 + 2] + 2);
          host_pixel_prefix_offsets[p + 1] = uint32_t(elements);
          window_selection[p] = p >= window.begin && p < window.end ? host_selection[p] : 1;
        }
        const uint32_t window_rays = host_ray_offsets.back();
        const CUdeviceptr difference = CUdeviceptr(prefix.device_pointer);
        const CUdeviceptr transmittance = difference + elements * 28;
        const CUdeviceptr owners = transmittance + elements * 4;
        const CUdeviceptr last_back = owners + size_t(window_rays) * 4;
        const CUdeviceptr ray_prefix_offsets = last_back + size_t(window_rays) * 4;
        if (!check(cuMemsetD8Async(difference, 0, max(window.bytes, size_t(16)), stream_),
                   "chunk difference clear") ||
            !check(cuMemcpyHtoDAsync(ray_offsets, host_ray_offsets.data(), size_t(size) * 4, stream_),
                   "chunk ray offsets upload") ||
            !check(cuMemcpyHtoDAsync(base + pixel_prefix_offsets_at, host_pixel_prefix_offsets.data(),
                                    size_t(size) * 4, stream_), "chunk prefix offsets upload") ||
            !check(cuMemcpyHtoDAsync(selection, window_selection.data(), pixels_size * 4, stream_),
                   "chunk selection upload") ||
            !check(cuMemcpyDtoDAsync(base + initial_selection_at, selection, pixels_size * 4, stream_),
                   "chunk initial selection") ||
            !launch_volume(3, pixels, cameras, ray_offsets, pixels, samples, map, owners,
                           base + pixel_prefix_offsets_at, ray_prefix_offsets, grids))
          return DeepVolumeCaptureResult::Error;
        for (size_t i = 0; i < batches.size(); i++) {
          if (!prepare_difference(i)) return DeepVolumeCaptureResult::Error;
          if (indexed_windows) {
            const auto &ranges = cached_batches[i].window_ranges;
            prepared_heads_ += size_t(ranges[window_number]) * 8;
            prepared_groups_ = ranges[window_number + 1] - ranges[window_number];
          }
          dispatched_groups += prepared_groups_;
          if (!launch_difference(samples, map, grids, difference, selection,
                                 base + initial_selection_at, last_back, ray_prefix_offsets))
            return DeepVolumeCaptureResult::Error;
        }
        if (!launch_volume(5, window_rays, difference, owners, grids, window_rays, transmittance,
                           ray_prefix_offsets, selection) ||
            !launch_volume(6, pixels, ray_offsets, transmittance, grids, rgb, pixels, selection,
                           output, gpu_counts, uint32_t(max_profile_samples), ray_prefix_offsets) ||
            !check(cuMemcpyDtoHAsync(host_selection.data() + window.begin,
                                    selection + size_t(window.begin) * 4,
                                    size_t(window.end - window.begin) * 4, stream_),
                   "chunk final selection readback") ||
            !check(cuMemcpyDtoHAsync(host_output_counts.data() + window.begin + 1,
                                    gpu_counts + size_t(window.begin + 1) * 4,
                                    size_t(window.end - window.begin) * 4, stream_),
                   "chunk counts readback") ||
            !check(cuStreamSynchronize(stream_), "chunk completion synchronize"))
          return DeepVolumeCaptureResult::Error;
        window_number++;
      }
      if (!check(cuMemcpyHtoDAsync(gpu_counts, host_output_counts.data(), size_t(size) * 4, stream_),
                 "completed chunk counts upload") ||
          !launch(Scan, size, gpu_counts, offsets, totals, size) ||
          !launch(ScanAdd, size, offsets, totals, size))
        return DeepVolumeCaptureResult::Error;
      profile_offsets.resize(size);
      fallback_pixels = host_selection;
      if (!check(cuMemcpyDtoHAsync(profile_offsets.data(), offsets, size_t(size) * 4, stream_),
                 "chunk profile offsets readback") ||
          !check(cuStreamSynchronize(stream_), "chunk output layout synchronize"))
        return DeepVolumeCaptureResult::Error;
      if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE")) {
        std::fprintf(stderr, "DEEP_GPU_PREFIX_CHUNKS windows=%zu peak=%zu full=%zu indexed=%d "
                             "dispatched_groups=%llu\n", windows.size(), allocated_prefix_bytes,
                     prefix_bytes, int(indexed_windows), (unsigned long long)dispatched_groups);
      }
    }
    const size_t output_bytes = size_t(profile_offsets.back()) * 24;
    if (profile_offsets.front() || output_bytes > pixels_size * size_t(max_profile_samples) * 24) {
      device_->set_error("Invalid Deep GPU volume output range");
      return DeepVolumeCaptureResult::Error;
    }
    readback_start = time_dt();
    profile_storage.resize(output_bytes);
    if (output_bytes &&
        (!launch_volume(7, pixels, output, offsets, base + compact_at, pixels, uint32_t(max_profile_samples)) ||
         !check(cuMemcpyDtoHAsync(profile_storage.data(), base + compact_at, output_bytes, stream_),
                "profile data readback") || !check(cuStreamSynchronize(stream_), "profile synchronize")))
      return DeepVolumeCaptureResult::Error;
    print_kernel_timings();
    const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE");
    if (debug_hash_kernel) {
      host_debug_hashes.resize(debug_hash_labels.size() * 2);
      if (!check(cuMemcpyDtoHAsync(host_debug_hashes.data(), CUdeviceptr(debug_hashes.device_pointer),
                                  host_debug_hashes.size() * 4, stream_), "debug hash readback") ||
          !check(cuStreamSynchronize(stream_), "debug hash synchronize"))
        return DeepVolumeCaptureResult::Error;
      for (size_t i = 0; i < debug_hash_labels.size(); i++)
        std::fprintf(stderr, "DEEP_GPU_HASH pixels=%u stage=%s xor=%08x sum=%08x\n", pixels,
                     debug_hash_labels[i].c_str(), host_debug_hashes[i * 2], host_debug_hashes[i * 2 + 1]);
    }
    if (trace && std::strcmp(trace, "1") == 0) {
      std::fprintf(stderr, "DEEP_GPU_VOLUME_PROFILE pixels=%u rays=%u bytes=%zu workspace=%zu "
                           "compute_sync_ms=%.6f readback_ms=%.6f total_ms=%.6f\n",
                   pixels, active_rays, output_bytes, bytes + allocated_prefix_bytes + scratch_.memory_size() + cache_bytes,
                   (readback_start - start) * 1000, (time_dt() - readback_start) * 1000,
                   (time_dt() - start) * 1000);
    }
    if (trace && std::strcmp(trace, "1") == 0 && cache_enabled) {
      std::fprintf(stderr, "DEEP_GPU_PREPARED_CACHE batches=%zu bytes=%zu\n",
                   cached_batches.size(), cache_bytes);
    }
    /* Opt-in single-frame replay, captured after the ordinary profile readback. Keep the
     * original append order and abort markers so the offline oracle can localize a failure. */
    const char *frame_root = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_FRAME_DUMP_ROOT");
    if (frame_root) {
      static std::atomic<uint32_t> frame_serial{0};
      const uint32_t index = frame_serial.fetch_add(1);
      const char *requested = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_FRAME_DUMP_INDEX");
      if (requested && index == uint32_t(std::strtoul(requested, nullptr, 10))) {
        const std::string stem = std::string(frame_root) + "/frame-" + std::to_string(index);
        const auto save = [&](const std::string &suffix, const void *data, size_t length) {
          FILE *file = std::fopen((stem + suffix + ".bin").c_str(), "wbx");
          if (!file) { device_->set_error("Cannot create exclusive Deep volume frame replay"); return false; }
          const bool written = !length || std::fwrite(data, 1, length, file) == length;
          const bool closed = std::fclose(file) == 0;
          if (!written || !closed) device_->set_error("Cannot write complete Deep volume frame replay");
          return written && closed;
        };
        const uint32_t header[] = {0x44564631u, 1, pixels, samples,
                                   uint32_t(max_profile_samples), uint32_t(batches.size())};
        if (!save("-header", header, sizeof(header)) ||
            !save("-population", population.data(), population.size() * 4) ||
            !save("-beauty", beauty.data(), beauty.size() * 4) ||
            !save("-offsets", profile_offsets.data(), profile_offsets.size() * 4) ||
            !save("-fallback", fallback_pixels.data(), fallback_pixels.size() * 4) ||
            !save("-profile", profile_storage.data(), profile_storage.size()))
          return DeepVolumeCaptureResult::Error;
        for (size_t i = 0; i < batches.size(); i++) {
          vector<uint8_t> raw(batches[i]->memory_size());
          // The profile readback already drains the stream. Synchronous copy has no local
          // asynchronous destination lifetime on an error exit.
          if (!check(cuMemcpyDtoH(raw.data(), CUdeviceptr(batches[i]->device_pointer), raw.size()),
                     "frame replay raw readback") ||
              !save("-batch-" + std::to_string(i) + "-counts", counts[i].data(), counts[i].size() * 4) ||
              !save("-batch-" + std::to_string(i), raw.data(), raw.size()))
            return DeepVolumeCaptureResult::Error;
        }
        std::fprintf(stderr, "DEEP_GPU_FRAME_REPLAY stem=%s pixels=%u batches=%zu\n",
                     stem.c_str(), pixels, batches.size());
      }
    }
    /* Explicit diagnostic fixture capture; it never changes the published profile/selector. */
    const char *dump_root = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_DUMP_ROOT");
    const char *dump_pixel = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_DUMP_PIXEL");
    if (dump_root && dump_pixel) {
      const size_t p = size_t(std::strtoull(dump_pixel, nullptr, 10));
      if (p < pixels) {
        static size_t serial = 0;
        const std::string stem = std::string(dump_root) + "/pixel-" + std::to_string(serial++);
        const float metadata[6] = {population[p], beauty[p * 4], beauty[p * 4 + 1],
                                  beauty[p * 4 + 2], beauty[p * 4 + 3], float(max_profile_samples)};
        FILE *file = std::fopen((stem + "-metadata.bin").c_str(), "wb");
        if (!file) { device_->set_error("Cannot write Deep volume diagnostic metadata"); return DeepVolumeCaptureResult::Error; }
        std::fwrite(metadata, sizeof(metadata), 1, file); std::fclose(file);
        file = std::fopen((stem + "-profile.bin").c_str(), "wb");
        if (!file) { device_->set_error("Cannot write Deep volume diagnostic profile"); return DeepVolumeCaptureResult::Error; }
        const size_t begin = size_t(profile_offsets[p]) * 24;
        const size_t length = size_t(profile_offsets[p + 1] - profile_offsets[p]) * 24;
        if (length) std::fwrite(profile_storage.data() + begin, length, 1, file);
        std::fclose(file);
        vector<uint32_t> selected(pixels, 0); selected[p] = 1;
        for (size_t i = 0; i < batches.size(); i++) {
          if (!process(*batches[i], counts[i], ignored_offsets, ignored_storage, &selected))
            return DeepVolumeCaptureResult::Error;
          file = std::fopen((stem + "-batch-" + std::to_string(i) + ".bin").c_str(), "wb");
          if (!file) { device_->set_error("Cannot write Deep volume diagnostic raw batch"); return DeepVolumeCaptureResult::Error; }
          if (!ignored_storage.empty()) std::fwrite(ignored_storage.data(), ignored_storage.size(), 1, file);
          std::fclose(file);
        }
      }
    }
    return DeepVolumeCaptureResult::Complete;
  }

 private:
  enum Kernel { Keys, Validate, SortBlocks, Merge, Filter, SurvivorKeys, Gather, Scan, ScanAdd,
                SelectedCounts, SelectedScatter, SelectedSurvivorKeys,
                GroupCounts, GroupScatter, FilterGroups };
  CUDADevice *device_;
  CUstream stream_;
  device_only_memory<uint8_t> scratch_;
  CUmodule module_ = nullptr;
  CUfunction functions_[15] = {};
  bool selected_functions_loaded_ = false;
  bool group_functions_loaded_ = false, group_difference_loaded_ = false;
  CUfunction volume_functions_[11] = {};
  bool volume_functions_loaded_ = false;
  CUdeviceptr prepared_raw_ = 0, prepared_keys_ = 0, prepared_ids_ = 0, prepared_keep_ = 0;
  CUdeviceptr prepared_heads_ = 0;
  uint32_t prepared_count_ = 0, prepared_groups_ = 0, group_lanes_ = 1;
  struct KernelTiming {
    int index;
    CUevent begin = nullptr;
    CUevent end = nullptr;
  };
  vector<KernelTiming> kernel_timings_;
  bool kernel_timings_enabled_ = false;

  void clear_kernel_timings()
  {
    for (const KernelTiming &timing : kernel_timings_) {
      if (timing.begin) cuEventDestroy(timing.begin);
      if (timing.end) cuEventDestroy(timing.end);
    }
    kernel_timings_.clear();
    kernel_timings_enabled_ = false;
  }
  bool begin_kernel_timing(const int index)
  {
    if (!kernel_timings_enabled_) return true;
    kernel_timings_.push_back({index});
    KernelTiming &timing = kernel_timings_.back();
    return check(cuEventCreate(&timing.begin, CU_EVENT_DEFAULT), "kernel timer begin create") &&
           check(cuEventCreate(&timing.end, CU_EVENT_DEFAULT), "kernel timer end create") &&
           check(cuEventRecord(timing.begin, stream_), "kernel timer begin record");
  }
  bool end_kernel_timing()
  {
    return !kernel_timings_enabled_ ||
           check(cuEventRecord(kernel_timings_.back().end, stream_), "kernel timer end record");
  }
  void print_kernel_timings()
  {
    /* The ordinary profile readback has already drained this stream. No profiling barrier. */
    float totals[27] = {};
    uint32_t calls[27] = {};
    for (const KernelTiming &timing : kernel_timings_) {
      float ms = 0;
      if (check(cuEventElapsedTime(&ms, timing.begin, timing.end), "kernel timer elapsed")) {
        totals[timing.index] += ms;
        calls[timing.index]++;
      }
    }
    for (int i = 0; i < 27; i++) {
      if (calls[i]) {
        std::fprintf(stderr, "DEEP_GPU_KERNEL_TIMING family=%s index=%d calls=%u total_ms=%.6f\n",
                     i >= 16 ? "volume" : "raw", i >= 16 ? i - 16 : i, calls[i], totals[i]);
      }
    }
  }

  bool load_group_functions()
  {
    if (group_functions_loaded_) return true;
    if (!load()) return false;
    const char *names[] = {"deep_raw_group_counts", "deep_raw_group_scatter_pixels", "deep_raw_filter_group_spread"};
    for (int i = 0; i < 3; i++)
      if (!check(cuModuleGetFunction(&functions_[GroupCounts + i], module_, names[i]),
                 "group kernel lookup")) return false;
    group_functions_loaded_ = true;
    return true;
  }
  bool load_group_difference()
  {
    if (group_difference_loaded_) return true;
    if (!load()) return false;
    if (!check(cuModuleGetFunction(&volume_functions_[9], module_, "deep_volume_difference_group_spread"),
               "group difference lookup")) return false;
    group_difference_loaded_ = true;
    return true;
  }
  bool launch_difference(uint32_t samples, CUdeviceptr map, CUdeviceptr grids,
                         CUdeviceptr difference, CUdeviceptr selection, CUdeviceptr initial_selection,
                         CUdeviceptr last_back, CUdeviceptr ray_prefix_offsets)
  {
    if (prepared_heads_)
      return launch_volume(9, prepared_groups_, prepared_raw_, prepared_ids_, prepared_keep_,
                           prepared_heads_, prepared_groups_, samples, map, grids, difference,
                           selection, initial_selection, last_back, ray_prefix_offsets, group_lanes_);
    return launch_volume(4, prepared_count_, prepared_raw_, prepared_keys_, prepared_ids_,
                         prepared_keep_, prepared_count_, samples, map, grids, difference,
                         selection, initial_selection, last_back, ray_prefix_offsets);
  }

  bool load_selected_functions()
  {
    if (selected_functions_loaded_) return true;
    if (!load()) return false;
    const char *names[] = {"deep_raw_selected_counts", "deep_raw_selected_scatter",
                          "deep_raw_selected_survivor_keys"};
    for (int i = 0; i < 3; i++)
      if (!check(cuModuleGetFunction(&functions_[SelectedCounts + i], module_, names[i]),
                 "selected kernel lookup")) return false;
    selected_functions_loaded_ = true;
    return true;
  }

  bool load_volume_functions()
  {
    if (volume_functions_loaded_) return true;
    if (!load()) return false;
    const char *names[] = {"deep_volume_initialize",
                          getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_SUMMARY_WARP") ?
                              "deep_volume_summarize_warp" : "deep_volume_summarize", "deep_volume_grid",
                          "deep_volume_assign_rays", "deep_volume_difference",
                          "deep_volume_transmittance_checked", getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_COOPERATIVE_FINISH") ?
                              "deep_volume_finish_cooperative" : "deep_volume_finish",
                          "deep_volume_gather", "deep_raw_select"};
    for (int i = 0; i < 9; i++) {
      if (!check(cuModuleGetFunction(&volume_functions_[i], module_, names[i]),
                 "volume kernel lookup")) return false;
    }
    volume_functions_loaded_ = true;
    return true;
  }
  template<typename... T> bool launch_volume(int kernel, uint32_t work, T... arguments)
  {
    if (!work) return true;
    void *parameters[] = {&arguments...};
    const uint32_t blocks = kernel == 6 && getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_COOPERATIVE_FINISH") ?
                                work : uint32_t((uint64_t(work) * (kernel == 9 ? group_lanes_ : 1) + 255) / 256);
    if (!begin_kernel_timing(kernel + 16) ||
        !check(cuLaunchKernel(volume_functions_[kernel], blocks, 1, 1,
                              256, 1, 1, 0, stream_, parameters, nullptr), "volume kernel launch") ||
        !end_kernel_timing()) return false;
    const char *sync = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_SYNC");
    return !sync || (*sync != '1' && *sync != '3') ||
           check(cuStreamSynchronize(stream_), "diagnostic volume synchronize");
  }

  bool check(CUresult result, const char *operation)
  {
    if (result == CUDA_SUCCESS) return true;
    const char *message = "unknown CUDA error";
    cuGetErrorString(result, &message);
    device_->set_error(string("Deep GPU raw ") + operation + ": " + message);
    return false;
  }
  bool load()
  {
    if (module_) return true;
    const char *path = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_MODULE");
    vector<uint8_t> binary;
    if (!path || !path_read_binary(path, binary) || binary.empty()) {
      device_->set_error("Deep GPU raw processing requires a readable experimental module");
      return false;
    }
    binary.push_back(0);
    if (!check(cuModuleLoadData(&module_, binary.data()), "module load")) return false;
    const char *names[] = {"deep_raw_keys", "deep_raw_validate", "deep_raw_sort_blocks",
                          "deep_raw_merge", "deep_raw_filter", "deep_raw_survivor_keys",
                          "deep_raw_gather", "deep_raw_scan", "deep_raw_scan_add"};
    for (int i = 0; i < 9; i++) {
      if (!check(cuModuleGetFunction(&functions_[i], module_, names[i]), "kernel lookup")) return false;
    }
    return true;
  }
  template<typename... T> bool launch(Kernel kernel, uint32_t work, T... arguments)
  {
    void *parameters[] = {&arguments...};
    const uint32_t blocks = uint32_t((uint64_t(work) * (kernel == FilterGroups ? group_lanes_ : 1) + 255) / 256);
    if (!begin_kernel_timing(int(kernel)) ||
        !check(cuLaunchKernel(functions_[kernel], blocks, 1, 1,
                              256, 1, 1, 0, stream_, parameters, nullptr), "kernel launch") ||
        !end_kernel_timing()) return false;
    const char *sync = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_SYNC");
    return !sync || (*sync != '2' && *sync != '3') ||
           check(cuStreamSynchronize(stream_), "diagnostic raw synchronize");
  }
};

/* CUDADeviceQueue */

CUDADeviceQueue::CUDADeviceQueue(CUDADevice *device, const size_t extra_auxiliary_capacity)
    : DeviceQueue(device), cuda_device_(device), cuda_stream_(nullptr),
      deep_auxiliary_budget_(4096 + 16 * sizeof(SmallReadback) + extra_auxiliary_capacity)
{
  const CUDAContextScope scope(cuda_device_);
  cuda_device_assert(cuda_device_, cuStreamCreate(&cuda_stream_, CU_STREAM_NON_BLOCKING));
  const char *option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_NATIVE_KERNEL_TIMINGS");
  native_timings_enabled_ = option && std::strcmp(option, "1") == 0;
  const char *frame = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_PIPELINE");
  const bool legacy_route = frame && std::strcmp(frame, "1") == 0 &&
                            device->info.type == DEVICE_OPTIX;
  /* Frame demand is frozen after queue construction. Keep defaults ready here,
   * but use them only while the device has a real bounded Deep policy. */
  const char *defaults = getenv("CYCLES_TEST_DEEP_DEMAND_DEFAULTS");
  const bool demand_defaults = device->info.type == DEVICE_OPTIX &&
                               (!defaults || std::strcmp(defaults, "0") != 0);
  const bool bounded_route = legacy_route || demand_defaults;
  bounded_shadow_block_ = bounded_route ? 64 : 0;
  bounded_physical_shadow_block_ = bounded_route ? 128 : 0;
  bounded_numeric_block_ = bounded_route ? 128 : 0;
  const char *adaptive_blocks = getenv("CYCLES_TEST_DEEP_ADAPTIVE_BLOCKS");
  bounded_adaptive_blocks_ = bounded_route &&
                             (!adaptive_blocks || std::strcmp(adaptive_blocks, "0") != 0);
  bounded_adaptive_all_modes_ = adaptive_blocks && std::strcmp(adaptive_blocks, "1") == 0;
  if (const char *shadow = getenv("CYCLES_TEST_DEEP_SHADOW_BLOCK")) {
    const int requested = std::atoi(shadow);
    if (requested == 32 || requested == 64 || requested == 128 || requested == 384) {
      bounded_shadow_block_ = bounded_physical_shadow_block_ = requested;
      bounded_shadow_block_override_ = true;
    }
  }
  if (const char *numeric_block = getenv("CYCLES_TEST_DEEP_NUMERIC_BLOCK")) {
    const int requested = std::atoi(numeric_block);
    if (requested == 64 || requested == 128) {
      bounded_numeric_block_ = requested;
    }
  }
  const char *context_scope = getenv("CYCLES_TEST_DEEP_CONTEXT_SCOPE");
  bounded_context_scope_ = !context_scope || std::strcmp(context_scope, "0") != 0;
  const char *shader_prefix = getenv("CYCLES_TEST_DEEP_PARALLEL_SHADER_PREFIX");
  bounded_parallel_shader_prefix_ = !shader_prefix || std::strcmp(shader_prefix, "0") != 0;
  const char *readbacks = getenv("CYCLES_TEST_DEEP_PINNED_QUEUE_READBACK");
  small_readbacks_enabled_ = readbacks ? std::strcmp(readbacks, "1") == 0 : bounded_route;
  small_readbacks_require_deep_policy_ = !readbacks && !legacy_route;
}

CUDADeviceQueue::~CUDADeviceQueue()
{
  const CUDAContextScope scope(cuda_device_);
  finish_native_timings();
  if (deep_capture_) {
    cuStreamSynchronize(cuda_stream_);
    deep_capture_.reset();
  }
  if (deep_frame_module_) {
    cuStreamSynchronize(cuda_stream_);
    cuModuleUnload(deep_frame_module_);
  }
  if (deep_bounded_module_) {
    cuStreamSynchronize(cuda_stream_);
    cuModuleUnload(deep_bounded_module_);
  }
  if (deep_copy_stream_) {
    cuStreamSynchronize(deep_copy_stream_);
    cuStreamDestroy(deep_copy_stream_);
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (deep_compact_done_[i]) {
      cuEventDestroy(deep_compact_done_[i]);
    }
    if (deep_header_done_[i]) {
      cuEventDestroy(deep_header_done_[i]);
    }
    if (deep_copy_done_[i]) {
      cuEventDestroy(deep_copy_done_[i]);
    }
  }
  if (small_readback_buffer_) {
    /* After an unsuccessful join the driver retains the 4 KiB allocation.
     * No deferred write targets a host view during owner destruction. */
    if (cuStreamSynchronize(cuda_stream_) == CUDA_SUCCESS) {
      free_deep_auxiliary(small_readback_buffer_, 4096);
    }
    small_readback_buffer_ = nullptr;
  }
  cuStreamDestroy(cuda_stream_);
}

void *CUDADeviceQueue::deep_pin(const size_t bytes)
{
  const CUDAContextScope scope(cuda_device_);
  void *pointer = nullptr;
  assert_success(cuMemHostAlloc(&pointer, bytes, CU_MEMHOSTALLOC_PORTABLE), "Deep pinned lease");
  return pointer;
}

void CUDADeviceQueue::deep_unpin(void *pointer)
{
  if (pointer) {
    const CUDAContextScope scope(cuda_device_);
    assert_success(cuMemFreeHost(pointer), "Deep pinned release");
  }
}

bool CUDADeviceQueue::deep_zero(const uint64_t dst, const size_t bytes)
{
  const CUDAContextScope scope(cuda_device_);
  assert_success(cuMemsetD8Async(CUdeviceptr(dst), 0, bytes, cuda_stream_), "Deep control reset");
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::deep_mark_compact(const unsigned slot)
{
  const CUDAContextScope scope(cuda_device_);
  if (!deep_copy_stream_) {
    assert_success(cuStreamCreate(&deep_copy_stream_, CU_STREAM_NON_BLOCKING), "Deep copy stream");
  }
  if (!deep_compact_done_[slot]) {
    assert_success(cuEventCreate(&deep_compact_done_[slot], CU_EVENT_DISABLE_TIMING),
                   "Deep compact event");
  }
  if (!deep_header_done_[slot]) {
    assert_success(cuEventCreate(&deep_header_done_[slot], CU_EVENT_DISABLE_TIMING),
                   "Deep header event");
  }
  if (!deep_copy_done_[slot]) {
    assert_success(cuEventCreate(&deep_copy_done_[slot], CU_EVENT_DISABLE_TIMING),
                   "Deep copy event");
  }
  if (cuda_device_->have_error()) {
    return false;
  }
  assert_success(cuEventRecord(deep_compact_done_[slot], cuda_stream_), "Deep compact record");
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::deep_copy_begin(const unsigned slot)
{
  const CUDAContextScope scope(cuda_device_);
  assert_success(cuStreamWaitEvent(deep_copy_stream_, deep_compact_done_[slot], 0),
                 "Deep copy waits for compact");
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::deep_copy_plane(void *dst, const uint64_t src, const size_t bytes)
{
  if (bytes) {
    const CUDAContextScope scope(cuda_device_);
    assert_success(cuMemcpyDtoHAsync(dst, CUdeviceptr(src), bytes, deep_copy_stream_),
                   "Deep plane readback");
  }
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::deep_copy_end(const unsigned slot, const bool payload)
{
  const CUDAContextScope scope(cuda_device_);
  const CUevent event = payload ? deep_copy_done_[slot] : deep_header_done_[slot];
  assert_success(cuEventRecord(event, deep_copy_stream_), "Deep copy record");
  return !cuda_device_->have_error();
}

int CUDADeviceQueue::deep_copy_ready(const unsigned slot, const bool payload, const bool wait)
{
  const CUDAContextScope scope(cuda_device_);
  const CUevent event = payload ? deep_copy_done_[slot] : deep_header_done_[slot];
  const CUresult result = wait ? cuEventSynchronize(event) : cuEventQuery(event);
  if (result == CUDA_ERROR_NOT_READY) {
    return 0;
  }
  assert_success(result, "Deep copy completion");
  return result == CUDA_SUCCESS ? 1 : -1;
}

int CUDADeviceQueue::deep_copy_peer(const uint64_t dst,
                                      Device &source,
                                      const uint64_t src,
                                      const size_t bytes)
{
  if (source.info.type != DEVICE_OPTIX && source.info.type != DEVICE_CUDA) {
    return 0;
  }
  auto &peer = static_cast<CUDADevice &>(source);
  const CUDAContextScope scope(cuda_device_);
  int accessible = 0;
  assert_success(cuDeviceCanAccessPeer(&accessible, cuda_device_->cuDevice, peer.cuDevice),
                 "Deep migration peer capability");
  if (cuda_device_->have_error()) {
    return -1;
  }
  if (!accessible) {
    return 0;
  }
  assert_success(cuMemcpyPeerAsync(CUdeviceptr(dst), cuda_device_->cuContext, CUdeviceptr(src),
                                  peer.cuContext, bytes, cuda_stream_),
                 "Deep migration peer copy");
  return cuda_device_->have_error() ? -1 : 1;
}

bool CUDADeviceQueue::deep_copy_join()
{
  const CUDAContextScope scope(cuda_device_);
  const CUresult result = deep_copy_stream_ ? cuStreamSynchronize(deep_copy_stream_) : CUDA_SUCCESS;
  assert_success(result, "Deep copy join");
  return result == CUDA_SUCCESS;
}

bool CUDADeviceQueue::load_deep_bounded_module()
{
  if (!deep_bounded_module_) {
    int major = 0, minor = 0;
    assert_success(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                                        cuda_device_->cuDevice), "Deep module architecture major");
    assert_success(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                                        cuda_device_->cuDevice), "Deep module architecture minor");
    if (cuda_device_->have_error()) return false;
    const string cubin = path_get(string_printf("lib/deep_bounded_7_sm_%d%d.cubin", major, minor));
    const bool compiled = path_exists(cubin);
    const string path = compiled ? cubin : path_get("lib/deep_bounded_7.ptx");
    assert_success(cuModuleLoad(&deep_bounded_module_, path.c_str()), "load bounded Deep module");
    std::fprintf(stderr, "DEEP_BOUNDED_MODULE format=%s path=%s\n",
                 compiled ? "cubin" : "ptx", path.c_str());
    if (!deep_bounded_module_) {
      return false;
    }
  }
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::enqueue_deep_bounded(const DeepBoundedOperation operation,
                                          const DeepBoundedDispatch &parameters,
                                          const uint32_t blocks,
                                          const uint32_t threads)
{
  if (blocks == 0) {
    return true;
  }
  const uint32_t policy = cuda_device_->deep_bounded_policy();
  const size_t index = size_t(operation);
  if (!policy || policy > 3 || index >= size_t(DeepBoundedOperation::COUNT) || !threads ||
      cuda_device_->have_error())
  {
    return false;
  }
  const CUDAContextScope scope(cuda_device_);
  if (!load_deep_bounded_module()) return false;
  CUfunction &function = deep_bounded_functions_[policy - 1][index];
  static const char *const modes[] = {"alpha", "simple", "physical"};
  if (!function) {
    static const char *const operations[] = {"begin_packet", "accumulate", "finalize", "coarsen",
                                            "scan", "scan_add", "encode", "migration_pack",
                                            "migration_unpack", "map_active_pixels"};
    static_assert(std::size(operations) == size_t(DeepBoundedOperation::COUNT));
    const string name = string("kernel_deep_bounded_") + modes[policy - 1] + "_" +
                        operations[index];
    assert_success(cuModuleGetFunction(&function, deep_bounded_module_, name.c_str()), name.c_str());
    if (!function) {
      return false;
    }
  }
  void *args[] = {const_cast<DeepBoundedDispatch *>(&parameters)};
  if (native_timings_enabled_) {
    if (!begin_native_timing(DEVICE_KERNEL_NUM,
                            int(std::min(uint64_t(blocks) * threads, uint64_t(INT_MAX)))))
      return false;
    native_timings_.back().bounded_operation = uint32_t(operation);
  }
  uint32_t launch_blocks = blocks, launch_threads = threads;
  if (bounded_numeric_block_ &&
      (operation == DeepBoundedOperation::ACCUMULATE ||
       operation == DeepBoundedOperation::FINALIZE ||
       operation == DeepBoundedOperation::COARSEN))
  {
    const uint64_t work = uint64_t(blocks) * threads;
    launch_threads = uint32_t(bounded_numeric_block_);
    launch_blocks = uint32_t((work + launch_threads - 1) / launch_threads);
  }
  const CUresult result = cuLaunchKernel(function, launch_blocks, 1, 1,
                                         launch_threads, 1, 1, 0, stream(), args, nullptr);
  assert_success(result, "enqueue bounded Deep operation");
  return (!native_timings_enabled_ || end_native_timing()) && result == CUDA_SUCCESS;
}

bool CUDADeviceQueue::begin_native_timing(const DeviceKernel kernel, const int work)
{
  if (!native_timings_enabled_) return true;
  native_timings_.push_back({kernel, work});
  NativeKernelTiming &timer = native_timings_.back();
  assert_success(cuEventCreate(&timer.begin, CU_EVENT_DEFAULT), "native timer begin create");
  assert_success(cuEventCreate(&timer.end, CU_EVENT_DEFAULT), "native timer end create");
  if (cuda_device_->have_error()) return false;
  assert_success(cuEventRecord(timer.begin, cuda_stream_), "native timer begin record");
  return !cuda_device_->have_error();
}

bool CUDADeviceQueue::end_native_timing()
{
  if (!native_timings_enabled_) return true;
  assert_success(cuEventRecord(native_timings_.back().end, cuda_stream_), "native timer end record");
  return !cuda_device_->have_error();
}

void CUDADeviceQueue::finish_native_timings()
{
  if (native_timings_.empty()) return;
  /* Drain once at queue destruction; enqueue instrumentation adds no per-kernel barrier. */
  assert_success(cuStreamSynchronize(cuda_stream_), "native timers drain");
  constexpr int timing_slots = DEVICE_KERNEL_NUM + int(DeepBoundedOperation::COUNT);
  double totals[timing_slots] = {};
  uint32_t calls[timing_slots] = {};
  uint64_t work[timing_slots] = {};
  for (const NativeKernelTiming &timer : native_timings_) {
    if (timer.begin && timer.end && !cuda_device_->have_error()) {
      float ms = 0;
      assert_success(cuEventElapsedTime(&ms, timer.begin, timer.end), "native timer elapsed");
      if (!cuda_device_->have_error()) {
        const int slot = timer.kernel == DEVICE_KERNEL_NUM ?
                             DEVICE_KERNEL_NUM + int(timer.bounded_operation) : int(timer.kernel);
        totals[slot] += ms;
        calls[slot]++;
        work[slot] += timer.work;
      }
    }
    if (timer.begin) cuEventDestroy(timer.begin);
    if (timer.end) cuEventDestroy(timer.end);
  }
  for (int kernel = 0; kernel < DEVICE_KERNEL_NUM; kernel++) {
    if (calls[kernel])
      std::fprintf(stderr, "DEEP_GPU_NATIVE_KERNEL_TIMING kernel=%s calls=%u work=%llu total_ms=%.6f\n",
                   device_kernel_as_string(DeviceKernel(kernel)), calls[kernel],
                   static_cast<unsigned long long>(work[kernel]), totals[kernel]);
  }
  static const char *names[] = {"begin_packet", "accumulate", "finalize", "coarsen", "scan",
                               "scan_add", "encode", "migration_pack", "migration_unpack",
                               "map_active_pixels"};
  static_assert(std::size(names) == size_t(DeepBoundedOperation::COUNT));
  for (int i = 0; i < int(DeepBoundedOperation::COUNT); ++i) {
    const int slot = DEVICE_KERNEL_NUM + i;
    if (calls[slot])
      std::fprintf(stderr,
                   "DEEP_GPU_BOUNDED_KERNEL_TIMING operation=%s calls=%u work=%llu total_ms=%.6f\n",
                   names[i], calls[slot], static_cast<unsigned long long>(work[slot]), totals[slot]);
  }
  native_timings_.clear();
}

int CUDADeviceQueue::num_concurrent_states(const size_t state_size) const
{
  const int max_num_threads = cuda_device_->get_num_multiprocessors() *
                              cuda_device_->get_max_num_threads_per_multiprocessor();
  int num_states = max(max_num_threads, 65536) * 16;

  const char *factor_str = getenv("CYCLES_CONCURRENT_STATES_FACTOR");
  if (factor_str) {
    const float factor = (float)atof(factor_str);
    if (factor != 0.0f) {
      num_states = max((int)(num_states * factor), 1024);
    }
    else {
      LOG_TRACE << "CYCLES_CONCURRENT_STATES_FACTOR evaluated to 0";
    }
  }

  LOG_TRACE << "GPU queue concurrent states: " << num_states << ", using up to "
            << string_human_readable_size(num_states * state_size);

  return num_states;
}

int CUDADeviceQueue::num_concurrent_busy_states(const size_t /*state_size*/) const
{
  const int max_num_threads = cuda_device_->get_num_multiprocessors() *
                              cuda_device_->get_max_num_threads_per_multiprocessor();

  if (max_num_threads == 0) {
    return 65536;
  }

  return 4 * max_num_threads;
}

void CUDADeviceQueue::run_with_deep_context(ContextCallback callback, void *data)
{
  if (!bounded_context_scope_ || cuda_device_->have_error()) {
    callback(data);
    return;
  }
  const CUDAContextScope scope(cuda_device_, true);
  callback(data);
}

void CUDADeviceQueue::init_execution()
{
  /* Synchronize all textures and memory copies before executing task.
   * Use default stream (nullptr) since that's what we will synchronize
   * here to ensure all scene data is copied. */
  CUDAContextScope scope(cuda_device_);
  cuda_device_->load_image_info(nullptr);
  cuda_device_assert(cuda_device_, cuCtxSynchronize());

  debug_init_execution();
}

void CUDADeviceQueue::load_image_info()
{
  CUDAContextScope scope(cuda_device_);
  cuda_device_->load_image_info(this);
}

bool CUDADeviceQueue::enqueue(DeviceKernel kernel,
                              const int work_size,
                              const DeviceKernelArguments &args)
{
  if (cuda_device_->have_error()) {
    return false;
  }
  if (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL_WARP &&
      work_size > std::numeric_limits<int>::max() / 32)
  {
    cuda_device_->set_error("Deep optical warp launch exceeds integer thread-index range");
    return false;
  }

  debug_enqueue_begin(kernel, work_size);

  const CUDAContextScope scope(cuda_device_);

  /* Update image info in case integrator memory alloc caused texture to move to host. */
  if (cuda_device_->load_image_info(nullptr)) {
    cuda_device_assert(cuda_device_, cuCtxSynchronize());
    if (cuda_device_->have_error()) {
      return false;
    }
  }

  /* The R6 shader histogram has the same integer prefix/clear contract.
   * Keep the native DeviceKernel ID and the following sorted index pass. */
  if (kernel == DEVICE_KERNEL_PREFIX_SUM && cuda_device_->deep_bounded_policy() &&
      bounded_parallel_shader_prefix_)
  {
    if (!load_deep_bounded_module()) return false;
    if (!deep_shader_prefix_sum_) {
      assert_success(cuModuleGetFunction(&deep_shader_prefix_sum_, deep_bounded_module_,
                                         "kernel_deep_shader_prefix_sum"), "shader prefix lookup");
      if (!deep_shader_prefix_sum_) return false;
    }
    if (!begin_native_timing(kernel, work_size)) return false;
    assert_success(cuLaunchKernel(deep_shader_prefix_sum_, 1, 1, 1, 256, 1, 1, 0,
                                  cuda_stream_, const_cast<void **>(args.values), nullptr),
                   "parallel shader prefix");
    if (!end_native_timing()) return false;
    debug_enqueue_end();
    return !cuda_device_->have_error();
  }

  /* Compute kernel launch parameters. */
  const CUDADeviceKernel &cuda_kernel = cuda_device_->kernels.get(kernel);
  int num_threads_per_block = cuda_kernel.num_threads_per_block;
  if (cuda_device_->deep_bounded_policy() && num_threads_per_block == 384 &&
      (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME ||
       kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING ||
       kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW))
  {
    /* The bounded Tile produces short waves. Smaller blocks let the same
     * register-heavy shading code spread those waves across more SMs. */
    num_threads_per_block = 128;
  }
  const uint32_t bounded_policy = cuda_device_->deep_bounded_policy();
  const int bounded_block = bounded_policy == 3 ? bounded_physical_shadow_block_ :
                                                 bounded_shadow_block_;
  if (bounded_policy && bounded_block &&
      (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW ||
       kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
       kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND) &&
      bounded_block <= cuda_kernel.num_threads_per_block)
  {
    num_threads_per_block = bounded_block;
  }
  if (bounded_policy && bounded_adaptive_blocks_ &&
      (bounded_policy == 3 || bounded_adaptive_all_modes_))
  {
    /* Keep Flat and the retained Simple configuration free of new device
     * queries. A queue belongs to one Device for its entire lifetime. */
    if (!bounded_native_scale_ready_) {
      bounded_native_scale_.initialize(cuda_device_->get_num_multiprocessors());
      bounded_native_scale_ready_ = true;
    }
    switch (kernel) {
      case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME:
      case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING:
        num_threads_per_block = bounded_native_scale_.select(work_size, num_threads_per_block);
        break;
      case DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW:
      case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE:
      case DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND:
        if (!bounded_shadow_block_override_) {
          num_threads_per_block = bounded_native_scale_.select(work_size, num_threads_per_block);
        }
        break;
      default:
        break;
    }
  }
  const uint64_t launch_size = uint64_t(work_size) *
                               (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL_WARP ?
                                    32 :
                                    1);
  const int num_blocks = int((launch_size + num_threads_per_block - 1) / num_threads_per_block);

  int shared_mem_bytes = 0;

  switch (kernel) {
    case DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_QUEUED_SHADOW_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_ACTIVE_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_COMPACT_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_TERMINATED_SHADOW_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_PATHS_ARRAY:
      /* See parall_active_index.h for why this amount of shared memory is needed. */
      shared_mem_bytes = (num_threads_per_block + 1) * sizeof(int);
      break;

    default:
      break;
  }

  /* Launch kernel. */
  if (!begin_native_timing(kernel, work_size)) return false;
  assert_success(cuLaunchKernel(cuda_kernel.function,
                                num_blocks,
                                1,
                                1,
                                num_threads_per_block,
                                1,
                                1,
                                shared_mem_bytes,
                                cuda_stream_,
                                const_cast<void **>(args.values),
                                nullptr),
                 "enqueue");
  if (!end_native_timing()) return false;

  debug_enqueue_end();

  return !(cuda_device_->have_error());
}

void CUDADeviceQueue::publish_small_readbacks()
{
  for (size_t i = 0; i < small_readback_count_; ++i) {
    const SmallReadback &readback = small_readbacks_[i];
    std::memcpy(readback.destination,
                static_cast<const char *>(small_readback_buffer_) + readback.offset,
                readback.bytes);
  }
  small_readback_count_ = 0;
  small_readback_cursor_ = 0;
}

bool CUDADeviceQueue::synchronize()
{
  if (cuda_device_->have_error()) {
    return false;
  }

  const CUDAContextScope scope(cuda_device_);
  const CUresult joined = cuStreamSynchronize(cuda_stream_);
  assert_success(joined, "synchronize");
  if (joined == CUDA_SUCCESS) {
    publish_small_readbacks();
  }

  debug_synchronize();

  return !(cuda_device_->have_error());
}

bool CUDADeviceQueue::synchronize_for_state_rebind()
{
  const CUDAContextScope scope(cuda_device_);
  const CUresult result = cuStreamSynchronize(cuda_stream_);
  assert_success(result, "drain_before_state_rebind");
  if (result == CUDA_SUCCESS) {
    publish_small_readbacks();
  }
  /* A logical diagnostic failure must not bypass stream draining. It remains latched and
   * rejects completion, but does not imply the CUDA stream itself failed to drain. */
  return result == CUDA_SUCCESS;
}

void CUDADeviceQueue::zero_to_device(device_memory &mem)
{
  assert(mem.type != MEM_IMAGE_TEXTURE);

  if (mem.memory_size() == 0) {
    return;
  }

  /* Allocate on demand. */
  if (mem.device_pointer == 0) {
    if (mem.type == MEM_GLOBAL) {
      cuda_device_->global_alloc(mem);
    }
    else {
      cuda_device_->mem_alloc(mem);
    }
  }

  /* Zero memory on device. */
  device_ptr d_ptr = mem.device->mem_device_ptr(mem, cuda_device_);
  assert(d_ptr != 0);

  const CUDAContextScope scope(cuda_device_);
  assert_success(cuMemsetD8Async((CUdeviceptr)d_ptr, 0, mem.memory_size(), cuda_stream_),
                 "zero_to_device");
}

void CUDADeviceQueue::copy_to_device(device_memory &mem)
{
  assert(mem.type != MEM_IMAGE_TEXTURE);

  if (mem.memory_size() == 0) {
    return;
  }

  /* Allocate on demand. */
  if (mem.device_pointer == 0) {
    if (mem.type == MEM_GLOBAL) {
      cuda_device_->global_alloc(mem);
    }
    else {
      cuda_device_->mem_alloc(mem);
    }
  }

  device_ptr d_ptr = mem.device->mem_device_ptr(mem, cuda_device_);
  assert(d_ptr != 0);
  assert(mem.host_pointer != nullptr);

  /* Copy memory to device. */
  const CUDAContextScope scope(cuda_device_);
  assert_success(
      cuMemcpyHtoDAsync((CUdeviceptr)d_ptr, mem.host_pointer, mem.memory_size(), cuda_stream_),
      "copy_to_device");
}

void *CUDADeviceQueue::allocate_deep_auxiliary(const size_t bytes)
{
  bool attempted = false;
  CUresult result = CUDA_SUCCESS;
  void *pointer = deep_auxiliary_budget_.allocate_with(bytes, [&]() -> void * {
    attempted = true;
    void *allocated = nullptr;
    result = cuMemHostAlloc(&allocated, bytes, CU_MEMHOSTALLOC_PORTABLE);
    if (result != CUDA_SUCCESS) {
      if (allocated) cuMemFreeHost(allocated);
      return nullptr;
    }
    return allocated;
  });
  if (!pointer) {
    if (!attempted)
      cuda_device_->set_error("Deep queue auxiliary capacity exceeds available host credit");
    else if (result == CUDA_SUCCESS)
      cuda_device_->set_error("Deep queue auxiliary allocation returned no storage");
    else
      assert_success(result, "allocate Deep queue auxiliary storage");
  }
  return pointer;
}

void CUDADeviceQueue::free_deep_auxiliary(void *pointer, const size_t bytes)
{
  const CUresult result = cuMemFreeHost(pointer);
  assert_success(result, "free Deep queue auxiliary storage");
  if (result == CUDA_SUCCESS) deep_auxiliary_budget_.relinquish(bytes);
}

void CUDADeviceQueue::copy_from_device(device_memory &mem)
{
  assert(mem.type != MEM_GLOBAL && mem.type != MEM_IMAGE_TEXTURE);

  if (mem.memory_size() == 0) {
    return;
  }

  assert(mem.device_pointer != 0);
  assert(mem.host_pointer != nullptr);

  /* Keep the native counter join, while avoiding pageable D2H staging
   * synchronizing inside each individual small transfer. */
  const CUDAContextScope scope(cuda_device_);
  if (small_readbacks_enabled_ &&
      (!small_readbacks_require_deep_policy_ || cuda_device_->deep_bounded_policy()) &&
      mem.memory_size() <= 1024)
  {
    constexpr size_t capacity = 4096;
    if (!small_readback_buffer_) {
      small_readback_buffer_ = allocate_deep_auxiliary(capacity);
      if (!small_readback_buffer_ || cuda_device_->have_error()) {
        return;
      }
    }
    const size_t bytes = mem.memory_size();
    const size_t aligned = (bytes + 15) & ~size_t(15);
    if (small_readback_cursor_ + aligned > capacity || small_readback_count_ == small_readbacks_.size()) {
      if (!synchronize()) {
        return;
      }
    }
    const size_t offset = small_readback_cursor_;
    const CUresult copied = cuMemcpyDtoHAsync(
        static_cast<char *>(small_readback_buffer_) + offset,
        (CUdeviceptr)mem.device_pointer, bytes, cuda_stream_);
    assert_success(copied, "copy small queue readback");
    if (copied == CUDA_SUCCESS) {
      small_readbacks_[small_readback_count_++] = {mem.host_pointer, offset, bytes};
      small_readback_cursor_ += aligned;
    }
    return;
  }
  assert_success(
      cuMemcpyDtoHAsync(
          mem.host_pointer, (CUdeviceptr)mem.device_pointer, mem.memory_size(), cuda_stream_),
      "copy_from_device");
}

void *CUDADeviceQueue::copy_from_device_synchronized(device_memory &mem, vector<uint8_t> &storage)
{
  if (mem.memory_size() == 0) {
    return nullptr;
  }

  storage.resize(mem.memory_size());

  device_ptr d_ptr = mem.device->mem_device_ptr(mem, cuda_device_);
  assert(d_ptr != 0);

  const CUDAContextScope scope(cuda_device_);
  assert_success(
      cuMemcpyDtoHAsync(storage.data(), (CUdeviceptr)d_ptr, mem.memory_size(), cuda_stream_),
      "copy_from_device_synchronized");

  synchronize();
  return storage.data();
}

void CUDADeviceQueue::assert_success(CUresult result, const char *operation)
{
  if (result != CUDA_SUCCESS) {
    const char *name = cuewErrorString(result);
    cuda_device_->set_error(string_printf(
        "%s in CUDA queue %s (%s)", name, operation, debug_active_kernels().c_str()));
  }
}

bool CUDADeviceQueue::copy_device_to_device(device_memory &destination,
                                           device_memory &source,
                                           const size_t bytes)
{
  if (source.device != cuda_device_ || destination.device != cuda_device_ ||
      bytes > source.memory_size() || bytes > destination.memory_size() ||
      (bytes != 0 && (source.device_pointer == 0 || destination.device_pointer == 0)))
  {
    cuda_device_->set_error("Invalid same-device copy range or memory owner");
    return false;
  }
  if (bytes == 0) {
    return true;
  }
  const CUDAContextScope scope(cuda_device_);
  const char *sync = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_SYNC");
  if (sync && *sync == '4') {
    cuda_device_->set_error("Context-wide Deep diagnostic synchronization is quarantined after "
                            "the 2026-10-08 GPU stability incident");
    return false;
  }
  assert_success(cuMemcpyDtoDAsync((CUdeviceptr)destination.device_pointer,
                                   (CUdeviceptr)source.device_pointer,
                                   bytes,
                                   cuda_stream_),
                 "copy_device_to_device");
  return !cuda_device_->have_error();
}

unique_ptr<DeviceGraphicsInterop> CUDADeviceQueue::graphics_interop_create()
{
  return make_unique<CUDADeviceGraphicsInterop>(this);
}

bool CUDADeviceQueue::process_deep_capture(device_memory &events,
                                          const vector<uint32_t> &counts,
                                          vector<uint32_t> &pixel_offsets,
                                          vector<uint8_t> &event_storage,
                                          const vector<uint32_t> *pixel_selection)
{
  const CUDAContextScope scope(cuda_device_);
  if (!deep_capture_) {
    deep_capture_ = make_unique<CUDADeepCapture>(cuda_device_, cuda_stream_);
  }
  return deep_capture_->process(events, counts, pixel_offsets, event_storage, pixel_selection);
}

DeepVolumeCaptureResult CUDADeviceQueue::process_deep_volume_capture(
    const vector<device_memory *> &batches,
    const vector<vector<uint32_t>> &counts,
    const vector<float> &population,
    const vector<float> &beauty,
    const int max_profile_samples,
    vector<uint32_t> &profile_offsets,
    vector<uint8_t> &profile_storage,
    vector<uint32_t> &fallback_pixels)
{
  const CUDAContextScope scope(cuda_device_);
  const char *sync = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_SYNC");
  if (sync && *sync == '5') {
    cuda_device_->set_error("Context-wide Deep diagnostic synchronization is quarantined after "
                            "the 2026-10-08 GPU stability incident");
    return DeepVolumeCaptureResult::Error;
  }
  if (!deep_capture_) deep_capture_ = make_unique<CUDADeepCapture>(cuda_device_, cuda_stream_);
  const bool trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
  const double start = trace ? time_dt() : 0.0;
  const auto result = deep_capture_->volume_profile(batches, counts, population, beauty,
                                                    max_profile_samples, profile_offsets,
                                                    profile_storage, fallback_pixels);
  if (trace) {
    /* The callee's own timer ends before its local device workspace destructors. */
    std::fprintf(stderr, "DEEP_GPU_PROFILE_RETURN total_ms=%.6f\n", (time_dt() - start) * 1000);
  }
  const char *scratch_option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_SCRATCH_PREFLIGHT");
  if (result == DeepVolumeCaptureResult::Complete && scratch_option &&
      std::strcmp(scratch_option, "1") == 0 &&
      std::any_of(fallback_pixels.begin(), fallback_pixels.end(),
                  [](const uint32_t value) { return value != 0; }) &&
      !deep_capture_->reserve_fallback_scratch(batches, population.size() + 1))
    return DeepVolumeCaptureResult::Error;
  if (result == DeepVolumeCaptureResult::Complete &&
      getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_REPEAT_CHECK")) {
    vector<uint32_t> repeated_offsets, repeated_fallback;
    vector<uint8_t> repeated_storage;
    const auto repeated = deep_capture_->volume_profile(batches, counts, population, beauty,
                                                        max_profile_samples, repeated_offsets,
                                                        repeated_storage, repeated_fallback);
    if (repeated != DeepVolumeCaptureResult::Complete) return repeated;
    const bool same = profile_offsets == repeated_offsets &&
                      fallback_pixels == repeated_fallback && profile_storage == repeated_storage;
    std::fprintf(stderr, "DEEP_GPU_PROFILE_REPEAT pixels=%zu identical=%d first_bytes=%zu second_bytes=%zu\n",
                 population.size(), int(same), profile_storage.size(), repeated_storage.size());
    if (!same) {
      cuda_device_->set_error("Deep volume diagnostic repeated immutable input changed output");
      return DeepVolumeCaptureResult::Error;
    }
  }
  return result;
}

bool CUDADeviceQueue::supports_deep_frame_reduction() const
{
#ifdef WITH_CYCLES_DEEP_REFERENCE_GPU
  return cuda_device_->info.type == DEVICE_OPTIX;
#else
  return false;
#endif
}

bool CUDADeviceQueue::enqueue_deep_frame_reduction(const DeepGPUFrameReduceParameters &parameters)
{
#ifndef WITH_CYCLES_DEEP_REFERENCE_GPU
  cuda_device_->set_error("Deep GPU reference module is disabled in this build");
  return false;
#endif
  if (!supports_deep_frame_reduction() || cuda_device_->have_error())
    return false;
  const CUDAContextScope scope(cuda_device_);
  const auto check = [&](const CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS)
      return true;
    cuda_device_->set_error(string("Deep GPU frame: ") + operation);
    return false;
  };
  if (parameters.abi != DEEP_GPU_FRAME_ABI || !parameters.rays || !parameters.pixels ||
      !parameters.cuts || (!parameters.transmission && !parameters.values) || !parameters.status ||
      !parameters.ray_count || !parameters.pixel_count || !parameters.cut_count ||
      (parameters.optical_count && (!parameters.optical || !parameters.optical_prefix)) ||
      (parameters.visibility_count && !parameters.visibility) ||
      (parameters.radiance_count && !parameters.radiance) ||
      parameters.ray_count > uint32_t(INT_MAX) || parameters.pixel_count > uint32_t(INT_MAX) ||
      parameters.optical_count > uint32_t(INT_MAX) ||
      parameters.visibility_count > uint32_t(INT_MAX) || parameters.cut_count > uint32_t(INT_MAX) ||
      parameters.radiance_count > uint32_t(INT_MAX) || !std::isfinite(parameters.exposure) ||
      (parameters.flags & ~1u))
  {
    cuda_device_->set_error("Deep GPU frame: invalid device buffer description");
    return false;
  }
  if (!load_deep_frame_module())
    return false;
  DeepGPUFrameReduceParameters arguments = parameters;
  void *args[] = {&arguments};
  /* Both kernels share the caller's queue and owners. There is no per-ray host
   * decision, raw readback or synchronization between these launches. */
  return check(cuLaunchKernel(deep_frame_prefix_,
                              parameters.ray_count, 1, 1, 128, 1, 1, 0, cuda_stream_, args, nullptr),
               "prefix launch") &&
         check(cuLaunchKernel(deep_frame_transmission_,
                              parameters.pixel_count, 8, 1, 32, 1, 1, 0, cuda_stream_, args, nullptr),
               "transmission launch");
}

bool CUDADeviceQueue::load_deep_frame_module()
{
#ifndef WITH_CYCLES_DEEP_REFERENCE_GPU
  cuda_device_->set_error("Deep GPU reference module is disabled in this build");
  return false;
#else
  if (deep_frame_module_)
    return true;
  const auto check = [&](const CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS)
      return true;
    cuda_device_->set_error(string("Deep GPU frame: ") + operation);
    return false;
  };
  {
    const char *path = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_FRAME_MODULE");
    const string installed = path_get("lib/deep_frame_reduce_44474603.ptx");
    if (!path || !*path) path = installed.c_str();
    CUmodule module = nullptr;
    if (!check(cuModuleLoad(&module, path), "module load"))
      return false;
    CUdeviceptr address = 0;
    size_t bytes = 0;
    uint32_t abi = 0;
    const bool loaded =
        check(cuModuleGetGlobal(&address, &bytes, module, "deep_gpu_frame_module_abi"), "module ABI") &&
        bytes == sizeof(abi) && check(cuMemcpyDtoH(&abi, address, bytes), "module ABI readback") &&
        abi == DEEP_GPU_FRAME_ABI &&
        check(cuModuleGetFunction(&deep_frame_prefix_, module, "deep_gpu_frame_optical_prefix"),
              "prefix entry") &&
        check(cuModuleGetFunction(&deep_frame_transmission_, module, "deep_gpu_frame_transmission"),
              "transmission entry");
    if (!loaded) {
      cuModuleUnload(module);
      if (!cuda_device_->have_error())
        cuda_device_->set_error("Deep GPU frame: incompatible device module ABI");
      return false;
    }
    deep_frame_module_ = module;
  }
  return true;
#endif
}

bool CUDADeviceQueue::enqueue_deep_frame_operation(const DeepGPUFrameOperation operation,
                                                  const DeepGPUFrameDispatch &parameters,
                                                  const uint32_t blocks,
                                                  const uint32_t threads)
{
  if (!supports_deep_frame_reduction() || cuda_device_->have_error())
    return false;
  if (!blocks)
    return true;
  const CUDAContextScope scope(cuda_device_);
  const char *names[] = {"deep_gpu_frame_fit", "deep_gpu_frame_scan",
                         "deep_gpu_frame_scan_add", "deep_gpu_frame_pack",
                         "deep_gpu_frame_native_seed", "deep_gpu_frame_native_reverse",
                         "deep_gpu_frame_native_radiance", "deep_gpu_frame_native_camera_input",
                         "deep_gpu_frame_owned_optical", "deep_gpu_frame_owned_visibility",
                         "deep_gpu_frame_make_keys", "deep_gpu_frame_sort_blocks",
                         "deep_gpu_frame_merge", "deep_gpu_frame_gather",
                         "deep_gpu_frame_ranges", "deep_gpu_frame_camera_keys",
                         "deep_gpu_frame_build_rays", "deep_gpu_frame_grid_count",
                         "deep_gpu_frame_grid_fill", "deep_gpu_frame_unique_flags",
                         "deep_gpu_frame_unique_scatter", "deep_gpu_frame_grid_publish",
                         "deep_gpu_frame_midpoints", "deep_gpu_frame_refine_flags",
                         "deep_gpu_frame_refine_scatter", "deep_gpu_frame_build_pixels",
                         "deep_gpu_frame_validate_optical", "deep_gpu_frame_validate_optical_finish",
                         "deep_gpu_frame_coalesce_radiance", "deep_gpu_frame_pack_radiance",
                         "deep_gpu_frame_compact_rebase", "deep_gpu_frame_compact_camera_pack",
                         "deep_gpu_frame_compact_camera_keys", "deep_gpu_frame_compact_primitive_keys",
                         "deep_gpu_frame_compact_events", "deep_gpu_frame_compact_event_keys",
                         "deep_gpu_frame_compact_sweep", "deep_gpu_frame_compact_tail",
                         "deep_gpu_frame_compact_reverse", "deep_gpu_frame_compact_colour_init",
                         "deep_gpu_frame_compact_root_radiance",
                         "deep_gpu_frame_compact_colour_hash", "deep_gpu_frame_compact_colour_page",
                         "deep_gpu_frame_compact_join_events", "deep_gpu_frame_compact_join_keys",
                         "deep_gpu_frame_compact_join_sweep", "deep_gpu_frame_compact_simple_colour",
                         "deep_gpu_frame_compact_encode_init", "deep_gpu_frame_compact_encode_page",
                         "deep_gpu_frame_compact_final_ranges", "deep_gpu_frame_compact_payload_ids",
                         "deep_gpu_frame_compact_sort_pack", "deep_gpu_frame_compact_sort_unpack",
                         "deep_gpu_frame_compact_sort_merge", "deep_gpu_frame_compact_archive_keys",
                         "deep_gpu_frame_compact_archive_gather", "deep_gpu_frame_compact_archive_rebase",
                         "deep_gpu_frame_compact_witness_init", "deep_gpu_frame_compact_witness_seal",
                         "deep_gpu_frame_compact_witness_sources", "deep_gpu_frame_compact_witness_output",
                         "deep_gpu_frame_compact_needs_extinction", "deep_gpu_frame_compact_pending_ranges",
                         "deep_gpu_frame_compact_qualification_cases"};
  static_assert(std::size(names) == size_t(DeepGPUFrameOperation::Count));
  const size_t index = size_t(operation);
  if (index >= size_t(DeepGPUFrameOperation::Count) || parameters.abi != DEEP_GPU_FRAME_ABI ||
      threads == 0 || threads > 1024 || !load_deep_frame_module())
  {
    if (!cuda_device_->have_error())
      cuda_device_->set_error("Deep GPU frame: invalid operation dispatch");
    return false;
  }
  CUfunction *function = &deep_frame_operations_[index];
  const char *name = names[index];
  if (operation == DeepGPUFrameOperation::CompactEncodePage && parameters.kind) {
    static const char *encode_names[] = {"deep_gpu_frame_compact_encode_simple",
        "deep_gpu_frame_compact_encode_physical", "deep_gpu_frame_compact_encode_simple_trace",
        "deep_gpu_frame_compact_encode_physical_trace"};
    if (parameters.kind > std::size(encode_names)) {
      cuda_device_->set_error("Deep GPU frame: invalid encode specialization"); return false;
    }
    function = &deep_frame_encode_operations_[parameters.kind - 1];
    name = encode_names[parameters.kind - 1];
  }
  const bool first_dispatch = !*function;
  if (first_dispatch &&
      cuModuleGetFunction(function, deep_frame_module_, name) != CUDA_SUCCESS)
  {
    cuda_device_->set_error(string("Deep GPU frame: missing entry ") + name);
    return false;
  }
  if (first_dispatch) {
    const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_CHAIN_TRACE");
    if (trace && strcmp(trace, "1") == 0) {
      const CUfunction_attribute attributes[] = {CU_FUNC_ATTRIBUTE_NUM_REGS,
          CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
          CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK};
      int values[] = {-1, -1, -1, -1};
      for (size_t i = 0; i < std::size(attributes); i++)
        cuFuncGetAttribute(&values[i], attributes[i], *function);
      std::fprintf(stderr, "DEEP_GPU_KERNEL_RESOURCES operation=%u entry=%s registers=%d local_bytes=%d shared_bytes=%d max_threads=%d\n",
                   uint32_t(index), name, values[0], values[1], values[2], values[3]);
    }
  }
  DeepGPUFrameDispatch arguments = parameters;
  void *args[] = {&arguments};
  if (cuLaunchKernel(*function, blocks, 1, 1, threads, 1, 1,
                     0, cuda_stream_, args, nullptr) != CUDA_SUCCESS)
  {
    cuda_device_->set_error(string("Deep GPU frame: launch failed ") + name);
    return false;
  }
  return true;
}

bool CUDADeviceQueue::copy_device_region(device_memory &destination,
                                         device_memory &source,
                                         const size_t destination_offset,
                                         const size_t source_offset,
                                         const size_t bytes)
{
  const CUDAContextScope scope(cuda_device_);
  if (cuda_device_->have_error())
    return false;
  if (destination.device != cuda_device_ || source.device != cuda_device_ ||
      destination_offset > destination.memory_size() ||
      bytes > destination.memory_size() - destination_offset ||
      source_offset > source.memory_size() || bytes > source.memory_size() - source_offset ||
      (bytes && (!destination.device_pointer || !source.device_pointer ||
                  destination.shared_pointer || source.shared_pointer)))
  {
    cuda_device_->set_error("Deep GPU frame: invalid resident copy range or owner");
    return false;
  }
  if (bytes && cuMemcpyDtoDAsync(destination.device_pointer + destination_offset,
                               source.device_pointer + source_offset,
                               bytes, cuda_stream_) != CUDA_SUCCESS)
  {
    cuda_device_->set_error("Deep GPU frame: resident region copy failed");
    return false;
  }
  return true;
}

bool CUDADeviceQueue::read_device_region(device_memory &source, const size_t source_offset,
                                          void *destination, const size_t bytes)
{
  const CUDAContextScope scope(cuda_device_);
  if (cuda_device_->have_error()) return false;
  if (source.device != cuda_device_ || source_offset > source.memory_size() ||
      bytes > source.memory_size() - source_offset ||
      (bytes && (!destination || !source.device_pointer || source.shared_pointer)))
  {
    cuda_device_->set_error("Deep GPU frame: invalid raw-byte readback range"); return false;
  }
  if (!synchronize()) return false;
  if (bytes && cuMemcpyDtoH(destination, source.device_pointer + source_offset,
                           bytes) != CUDA_SUCCESS)
  {
    cuda_device_->set_error("Deep GPU frame: raw-byte readback failed"); return false;
  }
  return true;
}

bool CUDADeviceQueue::write_device_region(device_memory &destination,
                                           const size_t destination_offset,
                                           const void *source, const size_t bytes)
{
  const CUDAContextScope scope(cuda_device_);
  if (cuda_device_->have_error()) return false;
  if (destination.device != cuda_device_ || destination_offset > destination.memory_size() ||
      bytes > destination.memory_size() - destination_offset ||
      (bytes && (!source || !destination.device_pointer || destination.shared_pointer))) {
    cuda_device_->set_error("Deep GPU frame: invalid raw-byte reload range"); return false;
  }
  /* The consumer stream is non-blocking. A pageable H2D copy submitted to the
   * default stream can return after staging but before DMA completes, and does
   * not order later kernels in this stream. Keep both on the owning queue and
   * join before the caller can retire or reuse the immutable host page. */
  if (bytes && cuMemcpyHtoDAsync(destination.device_pointer + destination_offset,
                                source, bytes, cuda_stream_) != CUDA_SUCCESS) {
    cuda_device_->set_error("Deep GPU frame: raw-byte reload failed"); return false;
  }
  return synchronize();
}

CCL_NAMESPACE_END

#endif /* WITH_CUDA */
