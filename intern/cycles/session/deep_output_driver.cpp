/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_output_driver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "device/device.h"
#include "kernel/types.h"
#include "session/deep_camera_sample_set.h"
#include "session/deep_volume_profile.h"

#include "util/log.h"
#include "util/math.h"
#include "util/path.h"
#include "util/string.h"
#include "util/task.h"
#include "util/time.h"

#include <cstring>

CCL_NAMESPACE_BEGIN

namespace {
constexpr float deep_alpha_epsilon = 1e-6f;
constexpr float deep_alpha_linear_fallback = 1e-3f;
constexpr float deep_alpha_log_min_transparency = 1e-5f;
constexpr float deep_surface_depth_epsilon = 1e-6f;
constexpr float deep_opaque_surface_alpha_threshold = 1.0f - 1e-6f;
constexpr float deep_inactive_sample_epsilon = 1e-8f;
constexpr float deep_surface_normal_dot_threshold = 0.94f;

bool deep_capture_pool_reuse_enabled()
{
  const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_CAPTURE_POOL_REUSE");
  return value && std::strcmp(value, "1") == 0;
}

bool deep_volume_profile_enabled()
{
  static const bool enabled = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE") != nullptr;
  return enabled;
}

bool deep_gpu_volume_profile_enabled()
{
  const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PROFILE");
  return value && std::strcmp(value, "1") == 0;
}

struct OpaqueSurfacePrefixInfo {
  int prefix_count = 0;
};

struct OpaqueSurfacePrefixGroup {
  uint32_t surface_object = 0;
  uint32_t surface_prim = 0;
  uint32_t surface_shader = 0;
  float3 normal = make_float3(0.0f, 0.0f, 1.0f);
  float3 color_sum = make_float3(0.0f, 0.0f, 0.0f);
  float output_z = 0.0f;
  int hit_count = 0;
};

enum class DeepPixelLayout {
  empty = 0,
  pure_volume,
  pure_surface,
  safe_surface_front_prefix,
  volume_front_mixed,
  unsupported_mixed,
};

inline bool deep_sample_is_volume(const DeepSampleData &sample)
{
  return sample.z_back > sample.z + deep_surface_depth_epsilon;
}

inline bool deep_sample_is_opaque_surface(const DeepSampleData &sample)
{
  return !deep_sample_is_volume(sample) && sample.a >= deep_opaque_surface_alpha_threshold;
}

inline bool deep_sample_has_hard_surface_metadata(const DeepSampleData &sample)
{
  return (deep_sample_info_flags(sample.flags) & DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA) != 0;
}

inline uint32_t deep_sample_camera_sample(const DeepSampleData &sample)
{
  return deep_sample_info_camera_sample(sample.flags);
}

inline bool deep_sample_is_inactive(const DeepSampleData &sample)
{
  return fabsf(sample.a) <= deep_inactive_sample_epsilon &&
         fabsf(sample.z) <= deep_inactive_sample_epsilon &&
         fabsf(sample.z_back) <= deep_inactive_sample_epsilon;
}

inline bool deep_surface_same_object_shader(const OpaqueSurfacePrefixGroup &group,
                                            const DeepSampleData &sample)
{
  return group.surface_object == sample.surface_object &&
         group.surface_shader == sample.surface_shader;
}

inline float3 deep_sample_rgb(const DeepSampleData &sample)
{
  return make_float3(sample.r, sample.g, sample.b);
}

inline float2 deep_unpack_unorm_16x2(const uint32_t packed)
{
  const float x = float(packed & 0xffffu) * (1.0f / 65535.0f);
  const float y = float((packed >> 16) & 0xffffu) * (1.0f / 65535.0f);
  return make_float2(x, y);
}

inline float3 deep_unpack_packed_geometric_normal(const uint32_t packed)
{
  float2 encoded = deep_unpack_unorm_16x2(packed) * 2.0f - make_float2(1.0f, 1.0f);
  float3 normal = make_float3(
      encoded.x, encoded.y, 1.0f - fabsf(encoded.x) - fabsf(encoded.y));

  if (normal.z < 0.0f) {
    const float2 reflected = make_float2(copysignf(1.0f - fabsf(encoded.y), encoded.x),
                                         copysignf(1.0f - fabsf(encoded.x), encoded.y));
    normal.x = reflected.x;
    normal.y = reflected.y;
    normal.z = 1.0f - fabsf(reflected.x) - fabsf(reflected.y);
  }

  float normal_length = 0.0f;
  return safe_normalize_len(normal, &normal_length);
}

bool deep_find_single_active_sample(const DeepSampleData *sample_data,
                                    const size_t offset,
                                    const int count,
                                    int &active_index)
{
  active_index = -1;
  for (int s = 0; s < count; s++) {
    if (deep_sample_is_inactive(sample_data[offset + s])) {
      continue;
    }
    if (active_index != -1) {
      return false;
    }
    active_index = s;
  }
  return active_index != -1;
}

bool analyze_opaque_surface_prefix(const DeepSampleData *sample_data,
                                   const size_t offset,
                                   const int count,
                                   OpaqueSurfacePrefixInfo &info)
{
  if (count <= 0 || !deep_sample_is_opaque_surface(sample_data[offset])) {
    return false;
  }

  int prefix_count = 0;
  while (prefix_count < count &&
         deep_sample_is_opaque_surface(sample_data[offset + prefix_count]))
  {
    prefix_count++;
  }

  if (prefix_count <= 0) {
    return false;
  }

  for (int s = prefix_count; s < count; s++) {
    if (deep_sample_is_inactive(sample_data[offset + s])) {
      continue;
    }
    if (!deep_sample_is_volume(sample_data[offset + s])) {
      return false;
    }
  }

  info.prefix_count = prefix_count;
  return true;
}

DeepPixelLayout classify_deep_pixel_layout(const DeepSampleData *sample_data,
                                           const size_t offset,
                                           const int count)
{
  int active_count = 0;
  int active_surface_count = 0;
  int active_volume_count = 0;
  bool saw_surface_after_volume = false;
  bool saw_volume_after_surface = false;

  for (int s = 0; s < count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (deep_sample_is_inactive(sample)) {
      continue;
    }

    const bool is_volume = deep_sample_is_volume(sample);
    active_count++;

    if (is_volume) {
      active_volume_count++;
      if (active_surface_count > 0) {
        saw_volume_after_surface = true;
      }
    }
    else {
      active_surface_count++;
      if (active_volume_count > 0) {
        saw_surface_after_volume = true;
      }
    }
  }

  if (active_count == 0) {
    return DeepPixelLayout::empty;
  }
  if (active_volume_count == active_count) {
    return DeepPixelLayout::pure_volume;
  }
  if (active_surface_count == active_count) {
    return DeepPixelLayout::pure_surface;
  }
  if (saw_surface_after_volume) {
    return DeepPixelLayout::volume_front_mixed;
  }

  OpaqueSurfacePrefixInfo prefix_info;
  if (!analyze_opaque_surface_prefix(sample_data, offset, count, prefix_info)) {
    return DeepPixelLayout::unsupported_mixed;
  }

  for (int s = 0; s < prefix_info.prefix_count; s++) {
    if (!deep_sample_has_hard_surface_metadata(sample_data[offset + s])) {
      return DeepPixelLayout::unsupported_mixed;
    }
  }

  if (!saw_volume_after_surface) {
    return DeepPixelLayout::pure_surface;
  }

  return DeepPixelLayout::safe_surface_front_prefix;
}

bool build_opaque_surface_prefix_groups(const DeepSampleData *sample_data,
                                        const size_t offset,
                                        const OpaqueSurfacePrefixInfo &info,
                                        vector<OpaqueSurfacePrefixGroup> &groups)
{
  groups.clear();

  for (int s = 0; s < info.prefix_count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (!deep_sample_has_hard_surface_metadata(sample)) {
      groups.clear();
      return false;
    }
  }

  if (info.prefix_count <= 0) {
    return false;
  }

  DeepCameraSampleSet camera_samples_seen(static_cast<size_t>(info.prefix_count));

  for (int s = 0; s < info.prefix_count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (!camera_samples_seen.add(camera_sample)) {
      continue;
    }

    const float3 sample_normal = deep_unpack_packed_geometric_normal(
        sample.packed_geometric_normal);

    bool merged = false;
    if (!groups.empty()) {
      OpaqueSurfacePrefixGroup &group = groups.back();
      if (deep_surface_same_object_shader(group, sample) &&
          dot(group.normal, sample_normal) >= deep_surface_normal_dot_threshold)
      {
        group.hit_count++;
        group.color_sum += deep_sample_rgb(sample);
        merged = true;
      }
    }

    if (!merged) {
      OpaqueSurfacePrefixGroup &group = groups.emplace_back();
      group.surface_object = sample.surface_object;
      group.surface_prim = sample.surface_prim;
      group.surface_shader = sample.surface_shader;
      group.normal = sample_normal;
      group.color_sum = deep_sample_rgb(sample);
      group.output_z = sample.z;
      group.hit_count = 1;
    }
  }

  return !groups.empty();
}

bool build_opaque_surface_groups(const DeepSampleData *sample_data,
                                 const size_t offset,
                                 const int count,
                                 vector<OpaqueSurfacePrefixGroup> &groups,
                                 int &representative_count,
                                 int &first_active_index)
{
  groups.clear();
  representative_count = 0;
  first_active_index = -1;

  for (int s = 0; s < count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (deep_sample_is_inactive(sample)) {
      continue;
    }
    if (!deep_sample_is_opaque_surface(sample) || !deep_sample_has_hard_surface_metadata(sample)) {
      groups.clear();
      return false;
    }
    if (first_active_index == -1) {
      first_active_index = s;
    }
  }

  if (first_active_index == -1) {
    return false;
  }

  DeepCameraSampleSet camera_samples_seen(static_cast<size_t>(count));

  for (int s = 0; s < count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (deep_sample_is_inactive(sample)) {
      continue;
    }

    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (!camera_samples_seen.add(camera_sample)) {
      continue;
    }

    const float3 sample_normal = deep_unpack_packed_geometric_normal(
        sample.packed_geometric_normal);

    bool merged = false;
    if (!groups.empty()) {
      OpaqueSurfacePrefixGroup &group = groups.back();
      if (deep_surface_same_object_shader(group, sample) &&
          dot(group.normal, sample_normal) >= deep_surface_normal_dot_threshold)
      {
        group.hit_count++;
        group.color_sum += deep_sample_rgb(sample);
        merged = true;
      }
    }

    if (!merged) {
      OpaqueSurfacePrefixGroup &group = groups.emplace_back();
      group.surface_object = sample.surface_object;
      group.surface_prim = sample.surface_prim;
      group.surface_shader = sample.surface_shader;
      group.normal = sample_normal;
      group.color_sum = deep_sample_rgb(sample);
      group.output_z = sample.z;
      group.hit_count = 1;
    }
  }

  for (const OpaqueSurfacePrefixGroup &group : groups) {
    representative_count += group.hit_count;
  }

  return !groups.empty() && representative_count > 0;
}

}  // namespace

struct DeepOutputDriver::DeepBufferSnapshot {
  Device *device = nullptr;
  int full_x = 0;
  int full_y = 0;
  int width = 0;
  int height = 0;
  int window_x = 0;
  int window_y = 0;
  int window_width = 0;
  int window_height = 0;
  int max_samples = 0;
  vector<uint32_t> sample_counts;
  vector<uint32_t> sample_offsets;
  vector<DeepSampleData> sample_data;
};

static void deep_assign_single_sample_with_beauty(std::vector<blender::DeepSample> &pixel_samples,
                                                  const DeepSampleData &src,
                                                  float beauty_r,
                                                  float beauty_g,
                                                  float beauty_b,
                                                  float beauty_a);

DeepOutputDriver::DeepOutputDriver(Device *device) : device_(device)
{
}

DeepOutputDriver::~DeepOutputDriver()
{
  release_temporary_host_caches();
}

void DeepOutputDriver::reset(const int width,
                             const int height,
                             const int max_samples_per_pixel,
                             const int capture_samples_per_pixel)
{
  width_ = width;
  height_ = height;
  max_samples_per_pixel_ = max_samples_per_pixel;
  capture_samples_per_pixel_ = max(capture_samples_per_pixel, max_samples_per_pixel_);
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  processing_error_.store(false, std::memory_order_relaxed);
  pixel_written_.clear();
  beauty_buffer_.clear();
  use_beauty_buffer_ = false;
  sample_count_buffer_.clear();
  use_sample_count_buffer_ = false;
  sample_count_scale_ = 1.0f;
  raw_capture_cache_.reset();
  raw_capture_complete_.clear();
  device_buffers_.clear();
}

void DeepOutputDriver::clear_device_buffers()
{
  processing_error_.store(false, std::memory_order_relaxed);
  for (DeepBufferSlice &slice : device_buffers_) {
    if (slice.buffers) {
      slice.buffers->discard_retained_device_capture();
      slice.buffers->zero();
    }
  }
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  pixel_written_.clear();
  raw_capture_cache_.reset();
  raw_capture_complete_.clear();
}

DeepRenderBuffers *DeepOutputDriver::get_deep_buffers()
{
  if (device_buffers_.size() == 1) {
    return device_buffers_[0].buffers.get();
  }
  return nullptr;
}

bool DeepOutputDriver::layout_matches(const vector<SliceParams> &slices) const
{
  if (device_buffers_.size() != slices.size()) {
    return false;
  }

  for (size_t i = 0; i < slices.size(); ++i) {
    const SliceParams &slice = slices[i];
    const DeepBufferSlice &existing = device_buffers_[i];
    if (existing.device != slice.device || existing.full_x != slice.full_x ||
        existing.full_y != slice.full_y || existing.width != slice.width ||
        existing.height != slice.height || existing.window_x != slice.window_x ||
        existing.window_y != slice.window_y || existing.window_width != slice.window_width ||
        existing.window_height != slice.window_height ||
        (existing.buffers &&
         existing.buffers->get_max_samples_per_pixel() !=
             capture_samples_for_slice(slice, max(capture_samples_per_pixel_, 1))))
    {
      return false;
    }
  }

  return true;
}

int DeepOutputDriver::capture_samples_for_slice(const SliceParams &slice,
                                                const int requested) const
{
  if (!capture_pool_budget_bytes_)
    return requested;
  int samples;
  size_t bytes;
  return deep_bounded_sparse_capture_samples(
             slice.width, slice.height, requested, capture_pool_budget_bytes_, samples, bytes) ?
             samples :
             0;
}

bool DeepOutputDriver::build_device_estimates(const vector<SliceParams> &slices,
                                              int max_samples,
                                              vector<DeviceEstimate> &estimates) const
{
  estimates.clear();
  if (capture_pool_budget_bytes_ && (slices.size() != 1 || !deep_volume_profile_enabled())) {
    device_->set_error("Bounded Deep capture requires a single sparse device slice");
    return false;
  }

  for (size_t i = 0; i < slices.size(); i++) {
    const SliceParams &slice = slices[i];
    if (!slice.device || slice.width <= 0 || slice.height <= 0) {
      continue;
    }

    size_t bytes = 0;
    const int slice_samples = capture_samples_for_slice(slice, max_samples);
    if (slice_samples <= 0) {
      slice.device->set_error("Deep append pool budget cannot represent this render geometry");
      return false;
    }
    const bool sparse_capture = deep_volume_profile_enabled();
    const bool size_valid = sparse_capture ? deep_compute_sparse_buffer_bytes(
                                                 slice.width, slice.height, slice_samples, bytes) :
                                             deep_compute_buffer_bytes(
                                                 slice.width, slice.height, slice_samples, bytes);
    if (!size_valid) {
      slice.device->set_error("Deep EXR buffer size overflow (dimensions too large)");
      return false;
    }

    /* Exact-shape reuse already owns this allocation. Charge only newly required memory. */
    if (deep_capture_pool_reuse_enabled() && sparse_capture && i < device_buffers_.size()) {
      const DeepBufferSlice &existing = device_buffers_[i];
      if (existing.device == slice.device && existing.buffers && existing.width == slice.width &&
          existing.height == slice.height &&
          existing.buffers->get_max_samples_per_pixel() == slice_samples &&
          existing.buffers->uses_sparse_capture())
      {
        bytes = 0;
      }
    }

    bool found = false;
    for (DeviceEstimate &estimate : estimates) {
      if (estimate.device == slice.device) {
        if (estimate.bytes > std::numeric_limits<size_t>::max() - bytes) {
          slice.device->set_error("Deep EXR buffer size overflow (device allocation)");
          return false;
        }
        estimate.bytes += bytes;
        found = true;
        break;
      }
    }
    if (!found) {
      DeviceEstimate estimate;
      estimate.device = slice.device;
      estimate.bytes = bytes;
      estimates.push_back(estimate);
    }
  }

  return true;
}

bool DeepOutputDriver::check_device_memory(const vector<DeviceEstimate> &estimates) const
{
  /* Safety headroom: refuse allocation when less than 32 MB free to avoid OOM. */
  constexpr size_t deep_memory_headroom_bytes = 32 * 1024 * 1024ULL;

  /* NOTE: User tile budget is the primary limiter; this is a safety net for device pressure. */
  for (const DeviceEstimate &estimate : estimates) {
    size_t total = 0;
    size_t free = 0;
    estimate.device->get_device_memory_info(total, free);
    if (total == 0) {
      continue;
    }

    const size_t required = estimate.bytes;
    const bool insufficient = (required > free) ||
                              (free > deep_memory_headroom_bytes &&
                               required + deep_memory_headroom_bytes > free);
    if (insufficient) {
      const string required_str = string_human_readable_size(required);
      const string free_str = string_human_readable_size(free);
      estimate.device->set_error(string_printf(
          "Deep EXR buffers require %s but only %s free on device %s",
          required_str.c_str(),
          free_str.c_str(),
          estimate.device->info.description.c_str()));
      return false;
    }
  }

  return true;
}

vector<DeepOutputDriver::DeepBufferSnapshot> DeepOutputDriver::snapshot_device_buffers()
{
  vector<DeepBufferSnapshot> snapshots;
  if (device_buffers_.empty()) {
    return snapshots;
  }

  snapshots.reserve(device_buffers_.size());
  for (DeepBufferSlice &existing : device_buffers_) {
    if (!existing.buffers) {
      continue;
    }
    if (!existing.buffers->copy_from_device()) {
      LOG_ERROR << "Failed to preserve deep buffers during rebalance";
      snapshots.clear();
      break;
    }

    DeepBufferSnapshot snapshot;
    snapshot.device = existing.device;
    snapshot.full_x = existing.full_x;
    snapshot.full_y = existing.full_y;
    snapshot.width = existing.width;
    snapshot.height = existing.height;
    snapshot.window_x = existing.window_x;
    snapshot.window_y = existing.window_y;
    snapshot.window_width = existing.window_width;
    snapshot.window_height = existing.window_height;
    snapshot.max_samples = existing.buffers->get_max_samples_per_pixel();

    const size_t num_pixels = static_cast<size_t>(max(existing.width, 0)) *
                              max(existing.height, 0);
    snapshot.sample_counts.resize(num_pixels);
    snapshot.sample_offsets.resize(num_pixels + 1, 0);
    for (size_t pixel = 0; pixel < num_pixels; pixel++) {
      uint32_t raw_count;
      const DeepSampleData *samples;
      size_t offset;
      if (!existing.buffers->get_pixel_sample_range(pixel, raw_count, samples, offset)) {
        LOG_ERROR << "Failed to preserve a Deep EXR pixel range during rebalance";
        snapshots.clear();
        return snapshots;
      }
      snapshot.sample_counts[pixel] = raw_count;
      const size_t count = deep_sample_count_retained(raw_count);
      snapshot.sample_data.insert(
          snapshot.sample_data.end(), samples + offset, samples + offset + count);
      snapshot.sample_offsets[pixel + 1] = snapshot.sample_data.size();
    }

    snapshots.push_back(std::move(snapshot));
  }

  return snapshots;
}

void DeepOutputDriver::init_device_buffers(const vector<SliceParams> &slices, int max_samples)
{
  const bool trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
  const double start = trace ? time_dt() : 0.0;
  vector<DeepBufferSlice> reused(slices.size());
  if (deep_capture_pool_reuse_enabled() && deep_volume_profile_enabled()) {
    for (size_t i = 0; i < min(slices.size(), device_buffers_.size()); i++) {
      const SliceParams &slice = slices[i];
      DeepBufferSlice &existing = device_buffers_[i];
      if (existing.device == slice.device && existing.buffers && existing.width == slice.width &&
          existing.height == slice.height &&
          existing.buffers->get_max_samples_per_pixel() ==
              capture_samples_for_slice(slice, max_samples) &&
          existing.buffers->uses_sparse_capture())
      {
        reused[i].buffers = std::move(existing.buffers);
        reused[i].uploaded_kernel_data = std::move(existing.uploaded_kernel_data);
      }
    }
  }
  /* Release unmatched shapes before allocating replacements, preserving the original peak. */
  device_buffers_.clear();
  if (trace) {
    std::fprintf(stderr, "DEEP_BUFFER_FREE total_ms=%.6f\n", (time_dt() - start) * 1000);
  }
  device_buffers_.reserve(slices.size());

  for (size_t i = 0; i < slices.size(); i++) {
    const SliceParams &slice = slices[i];
    DeepBufferSlice entry;
    entry.device = slice.device;
    entry.full_x = slice.full_x;
    entry.full_y = slice.full_y;
    entry.width = slice.width;
    entry.height = slice.height;
    entry.window_x = slice.window_x;
    entry.window_y = slice.window_y;
    entry.window_width = slice.window_width;
    entry.window_height = slice.window_height;

    if (slice.device && slice.width > 0 && slice.height > 0) {
      entry.buffers = std::move(reused[i].buffers);
      entry.uploaded_kernel_data = std::move(reused[i].uploaded_kernel_data);
      if (!entry.buffers) {
        entry.buffers = make_unique<DeepRenderBuffers>(slice.device);
      }
      /* Retained captures were drained before the layout change. Reset counts and host caches;
       * ordinary restore_snapshots remains the sole owner of same-tile rebalance preservation. */
      entry.buffers->reset(slice.width,
                           slice.height,
                           capture_samples_for_slice(slice, max_samples),
                           deep_volume_profile_enabled());
      if (trace && capture_pool_budget_bytes_) {
        std::fprintf(stderr, "DEEP_BOUNDED_POOL width=%d height=%d average=%d capacity=%zu "
                             "bytes=%zu budget=%zu\n", slice.width, slice.height,
                     entry.buffers->get_max_samples_per_pixel(),
                     entry.buffers->get_global_sample_capacity(),
                     entry.buffers->get_sample_data().size() * sizeof(DeepSampleData) +
                         entry.buffers->get_sample_counts().size() * sizeof(uint32_t),
                     capture_pool_budget_bytes_);
      }
    }

    device_buffers_.push_back(std::move(entry));
  }
}

void DeepOutputDriver::restore_snapshots(const vector<DeepBufferSnapshot> &snapshots)
{
  if (snapshots.empty()) {
    return;
  }

  auto find_snapshot = [&](int global_x, int global_y) -> const DeepBufferSnapshot * {
    const DeepBufferSnapshot *candidate = nullptr;
    for (const DeepBufferSnapshot &snapshot : snapshots) {
      if (global_x < snapshot.full_x || global_x >= snapshot.full_x + snapshot.width ||
          global_y < snapshot.full_y || global_y >= snapshot.full_y + snapshot.height)
      {
        continue;
      }

      const int window_x_min = snapshot.full_x + snapshot.window_x;
      const int window_y_min = snapshot.full_y + snapshot.window_y;
      const int window_x_max = window_x_min + snapshot.window_width;
      const int window_y_max = window_y_min + snapshot.window_height;
      if (global_x >= window_x_min && global_x < window_x_max && global_y >= window_y_min &&
          global_y < window_y_max)
      {
        return &snapshot;
      }

      if (!candidate) {
        candidate = &snapshot;
      }
    }

    return candidate;
  };

  bool clamp_logged = false;
  for (DeepBufferSlice &slice : device_buffers_) {
    if (!slice.buffers || slice.width <= 0 || slice.height <= 0) {
      continue;
    }

    const int dst_width = slice.width;
    const int dst_height = slice.height;
    const int dst_max_samples = slice.buffers->get_max_samples_per_pixel();
    uint32_t *dst_counts = slice.buffers->get_sample_counts_ptr();
    DeepSampleData *dst_samples = slice.buffers->get_sample_data_ptr();
    if (!dst_counts || !dst_samples) {
      LOG_ERROR << "Deep EXR buffer preservation failed: host memory unavailable";
      continue;
    }

    const size_t dst_pixels = static_cast<size_t>(dst_width) * dst_height;
    const bool sparse_capture = slice.buffers->uses_sparse_capture();
    std::fill(dst_counts, dst_counts + dst_pixels + (sparse_capture ? 1 : 0), 0);
    size_t sparse_write_offset = 0;
    const size_t sparse_capacity = slice.buffers->get_global_sample_capacity();

    for (int y = 0; y < dst_height; ++y) {
      const int global_y = slice.full_y + y;
      for (int x = 0; x < dst_width; ++x) {
        const int global_x = slice.full_x + x;
        const DeepBufferSnapshot *snapshot = find_snapshot(global_x, global_y);
        if (!snapshot) {
          continue;
        }

        const int src_x = global_x - snapshot->full_x;
        const int src_y = global_y - snapshot->full_y;
        if (src_x < 0 || src_x >= snapshot->width || src_y < 0 || src_y >= snapshot->height) {
          continue;
        }

        const size_t src_idx = static_cast<size_t>(src_y) * snapshot->width + src_x;
        const uint32_t raw_count = snapshot->sample_counts[src_idx];
        const bool overflowed = deep_sample_count_overflowed(raw_count);
        uint32_t count = deep_sample_count_retained(raw_count);
        if (count == 0) {
          continue;
        }

        if (!sparse_capture && count > static_cast<uint32_t>(dst_max_samples)) {
          if (!clamp_logged) {
            LOG_WARNING
                << "Deep EXR buffer preservation clamped samples due to max sample mismatch";
            clamp_logged = true;
          }
          count = static_cast<uint32_t>(dst_max_samples);
        }

        const size_t dst_idx = static_cast<size_t>(y) * dst_width + x;
        const size_t src_offset = snapshot->sample_offsets[src_idx];
        size_t dst_offset = dst_idx * static_cast<size_t>(dst_max_samples);
        if (sparse_capture) {
          dst_offset = sparse_write_offset;
          const size_t available = sparse_capacity - min(sparse_write_offset, sparse_capacity);
          if (count > available) {
            count = static_cast<uint32_t>(available);
            if (!clamp_logged) {
              LOG_WARNING << "Deep EXR sparse buffer preservation reached the shared pool capacity";
              clamp_logged = true;
            }
          }
          sparse_write_offset += count;
        }
        dst_counts[dst_idx] = count |
                              ((overflowed ||
                                (sparse_capture && count <
                                                       deep_sample_count_retained(raw_count))) ?
                                   DEEP_SAMPLE_COUNT_OVERFLOW :
                                   0u);
        memcpy(dst_samples + dst_offset,
               snapshot->sample_data.data() + src_offset,
               sizeof(DeepSampleData) * count);
        if (sparse_capture) {
          for (uint32_t sample = 0; sample < count; sample++) {
            dst_samples[dst_offset + sample].pixel_index = static_cast<uint32_t>(dst_idx);
          }
        }
      }
    }

    if (sparse_capture) {
      dst_counts[dst_pixels] = static_cast<uint32_t>(sparse_write_offset);
    }

    slice.buffers->copy_to_device();
  }
}

void DeepOutputDriver::sync_device_buffers(const vector<SliceParams> &slices)
{
  if (!enabled_) {
    return;
  }

  if (layout_matches(slices)) {
    return;
  }

  /* Retained chunks belong to the old slice's pixel coordinates. Drain them before a layout
   * change destroys their allocations or changes the mapping to the full-image cache. */
  for (const DeepBufferSlice &slice : device_buffers_) {
    if (slice.buffers && slice.buffers->has_retained_device_capture()) {
      if (!flush_device_capture(true)) {
        return;
      }
      break;
    }
  }

  const int max_samples = max(capture_samples_per_pixel_, 1);
  vector<DeviceEstimate> estimates;
  if (!build_device_estimates(slices, max_samples, estimates)) {
    return;
  }
  if (!check_device_memory(estimates)) {
    return;
  }

  /* Preserve device data only when work is rebalanced inside the same logical render tile.
   * Adjacent tiles have overlapping render-buffer borders, but those border samples have not been
   * committed to the output window. Restoring them into the next tile and rendering them again
   * duplicates optical events in narrow tile-periodic bands. */
  int old_min_x = std::numeric_limits<int>::max();
  int old_min_y = std::numeric_limits<int>::max();
  int old_max_x = std::numeric_limits<int>::min();
  int old_max_y = std::numeric_limits<int>::min();
  bool old_window_valid = false;
  for (const DeepBufferSlice &slice : device_buffers_) {
    if (slice.window_width <= 0 || slice.window_height <= 0) {
      continue;
    }
    old_min_x = min(old_min_x, slice.full_x + slice.window_x);
    old_min_y = min(old_min_y, slice.full_y + slice.window_y);
    old_max_x = max(old_max_x, slice.full_x + slice.window_x + slice.window_width);
    old_max_y = max(old_max_y, slice.full_y + slice.window_y + slice.window_height);
    old_window_valid = true;
  }

  int new_min_x = std::numeric_limits<int>::max();
  int new_min_y = std::numeric_limits<int>::max();
  int new_max_x = std::numeric_limits<int>::min();
  int new_max_y = std::numeric_limits<int>::min();
  bool new_window_valid = false;
  for (const SliceParams &slice : slices) {
    if (slice.window_width <= 0 || slice.window_height <= 0) {
      continue;
    }
    new_min_x = min(new_min_x, slice.full_x + slice.window_x);
    new_min_y = min(new_min_y, slice.full_y + slice.window_y);
    new_max_x = max(new_max_x, slice.full_x + slice.window_x + slice.window_width);
    new_max_y = max(new_max_y, slice.full_y + slice.window_y + slice.window_height);
    new_window_valid = true;
  }

  const bool preserve_same_tile = old_window_valid && new_window_valid &&
                                  old_min_x == new_min_x && old_min_y == new_min_y &&
                                  old_max_x == new_max_x && old_max_y == new_max_y;
  vector<DeepBufferSnapshot> snapshots = preserve_same_tile ? snapshot_device_buffers() :
                                                              vector<DeepBufferSnapshot>();
  init_device_buffers(slices, max_samples);
  if (preserve_same_tile) {
    restore_snapshots(snapshots);
  }

  if (pixel_written_.empty()) {
    processed_cache_.reset();
  }
  deep_buffers_processed_ = false;
}

void DeepOutputDriver::invalidate_device_kernel_data()
{
  for (DeepBufferSlice &slice : device_buffers_) {
    slice.uploaded_kernel_data.clear();
  }
}

void DeepOutputDriver::update_device_kernel_data(const KernelData &base_data)
{
  if (!enabled_) {
    return;
  }

  for (DeepBufferSlice &slice : device_buffers_) {
    if (!slice.device || !slice.buffers) {
      continue;
    }

    KernelData data = base_data;
    /* Keep the kernel-side addressing mode tied to the buffer layout allocated for this device
     * slice. The scene KernelData copy can predate per-tile Deep buffer synchronization on GPU
     * backends; leaving its boolean-style value intact makes the kernel write dense per-pixel
     * slots while the host interprets the allocation as an append-only sparse pool. */
    data.film.use_deep_output = slice.buffers->uses_sparse_capture() ? 2 : 1;
    const char *optical_only = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_ONLY_KERNEL");
    data.film.deep_optical_only_kernel =
        data.film.use_deep_output == 2 && optical_only && std::strcmp(optical_only, "1") == 0 &&
        !(base_data.kernel_features & KERNEL_FEATURE_OSL_SHADING) &&
        (slice.device->info.type == DEVICE_CUDA || slice.device->info.type == DEVICE_OPTIX);
    const char *optical_warp = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WARP_KERNEL");
    if (data.film.deep_optical_only_kernel && optical_warp &&
        std::strcmp(optical_warp, "1") == 0)
    {
      data.film.deep_optical_only_kernel = 2;
    }
    const char *optical_wavefront = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WAVEFRONT");
    if (data.film.deep_optical_only_kernel == 2 && optical_wavefront &&
        std::strcmp(optical_wavefront, "1") == 0)
    {
      data.film.deep_optical_only_kernel = 3;
    }
    const char *warp_append = getenv("CYCLES_EXPERIMENTAL_DEEP_WARP_APPEND");
    if (data.film.use_deep_output == 2 && warp_append && std::strcmp(warp_append, "1") == 0 &&
        (slice.device->info.type == DEVICE_CUDA || slice.device->info.type == DEVICE_OPTIX))
    {
      data.film.use_deep_output = 3;
    }
    data.film.deep_width = slice.width;
    data.film.deep_height = slice.height;
    data.film.deep_max_samples = slice.buffers->get_max_samples_per_pixel();
    data.film.deep_samples_ptr = slice.buffers->get_sample_data_device_ptr();
    data.film.deep_sample_counts_ptr = slice.buffers->get_sample_counts_device_ptr();

    const char *cache = getenv("CYCLES_EXPERIMENTAL_DEEP_KERNEL_DATA_CACHE");
    const bool use_cache = cache && std::strcmp(cache, "1") == 0 &&
        (slice.device->info.type == DEVICE_CUDA || slice.device->info.type == DEVICE_OPTIX);
    if (use_cache && slice.uploaded_kernel_data.size() == sizeof(data) &&
        std::memcmp(slice.uploaded_kernel_data.data(), &data, sizeof(data)) == 0)
    {
      continue;
    }
    slice.device->const_copy_to("data", &data, sizeof(data));
    if (use_cache && !slice.device->have_error()) {
      slice.uploaded_kernel_data.resize(sizeof(data));
      std::memcpy(slice.uploaded_kernel_data.data(), &data, sizeof(data));
    }
    else {
      slice.uploaded_kernel_data.clear();
    }
  }
}

bool DeepOutputDriver::finalize_deep_output(const std::string &filepath)
{
  std::vector<std::vector<blender::DeepSample>> *deep_data = ensure_processed_cache();
  if (!deep_data) {
    return false;
  }

  /* Write using provided callback. */
  if (write_callback_) {
    bool success = write_callback_(
        *deep_data,
        width_,
        height_,
        filepath,
        compression_,
        use_half_float_,
        merge_threshold_,
        alpha_merge_threshold_,
        has_display_window_,
        display_width_,
        display_height_,
        display_offset_x_,
        display_offset_y_,
        data_offset_x_,
        data_offset_y_);
    if (success) {
      LOG_INFO << "Deep EXR saved successfully: " << filepath;
    }
    else {
      LOG_ERROR << "Failed to write deep EXR: " << filepath;
    }
    return success;
  }
  else {
    LOG_WARNING << "No deep EXR write callback set - skipping file output";
    return false;
  }
}

std::vector<std::vector<blender::DeepSample>> *
DeepOutputDriver::get_unmerged_processed_deep_data()
{
  std::vector<std::vector<blender::DeepSample>> *deep_data = ensure_processed_cache();
  if (!deep_data) {
    return nullptr;
  }
  if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_DUMP_ROOT") && width_ > 204 && height_ > 127) {
    for (const size_t pixel : {size_t(127) * width_ + 204, size_t(136) * width_ + 165}) {
      std::fprintf(stderr, "DEEP_GPU_PROFILE_EXPORT pixel=%zu samples=%zu\n", pixel, (*deep_data)[pixel].size());
      for (const auto &sample : (*deep_data)[pixel])
        std::fprintf(stderr, "DEEP_GPU_PROFILE_EXPORT_VALUE %.9g %.9g %.9g\n", sample.z, sample.z_back, sample.a);
    }
  }

  return processed_cache_.release();
}

void DeepOutputDriver::release_temporary_host_caches()
{
  processed_cache_.reset();
  raw_capture_cache_.reset();
  raw_capture_complete_.free_memory();
  pixel_written_.free_memory();
  sample_count_buffer_.free_memory();
  beauty_buffer_.free_memory();
  use_sample_count_buffer_ = false;
  use_beauty_buffer_ = false;
  sample_count_scale_ = 1.0f;
}

void DeepOutputDriver::set_beauty_buffer(const float *rgba_buffer, int width, int height)
{
  if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_DUMP_ROOT"))
    std::fprintf(stderr, "DEEP_GPU_PROFILE_ROUTE beauty_set cache=%d size=%dx%d\n", int(processed_cache_ != nullptr), width, height);
  if (!rgba_buffer || width != width_ || height != height_) {
    use_beauty_buffer_ = false;
    beauty_buffer_.clear();
    processed_cache_.reset();
    return;
  }

  const size_t size = static_cast<size_t>(width) * height * 4;
  beauty_buffer_.resize(size);
  memcpy(beauty_buffer_.data(), rgba_buffer, size * sizeof(float));
  use_beauty_buffer_ = true;
  processed_cache_.reset();
}

void DeepOutputDriver::set_sample_count_buffer(const float *sample_count_buffer,
                                               int width,
                                               int height,
                                               float sample_count_scale)
{
  if (!sample_count_buffer || width != width_ || height != height_) {
    use_sample_count_buffer_ = false;
    sample_count_buffer_.clear();
    sample_count_scale_ = 1.0f;
    processed_cache_.reset();
    return;
  }

  const size_t size = static_cast<size_t>(width) * height;
  sample_count_buffer_.resize(size);
  memcpy(sample_count_buffer_.data(), sample_count_buffer, size * sizeof(float));
  use_sample_count_buffer_ = true;
  sample_count_scale_ = max(sample_count_scale, 0.0f);
  processed_cache_.reset();
}

void DeepOutputDriver::accumulate_tile(const float *beauty_pixels,
                                       const float *sample_count_pixels,
                                       float sample_count_scale,
                                       int tile_width,
                                       int tile_height,
                                       int tile_offset_x,
                                       int tile_offset_y)
{
  if (!enabled_ || device_buffers_.empty()) {
    return;
  }

  const bool trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
  const double start = trace ? time_dt() : 0.0;
  const bool batched_capture = deep_volume_profile_enabled();
  if (batched_capture) {
    if (!flush_device_capture(true, deep_gpu_volume_profile_enabled())) {
      processing_error_.store(true, std::memory_order_relaxed);
      return;
    }
  }
  else if (!process_device_buffers()) {
    processing_error_.store(true, std::memory_order_relaxed);
    return;
  }

  const double flushed = trace ? time_dt() : 0.0;
  if (!processed_cache_) {
    init_processed_cache();
  }

  if (pixel_written_.empty()) {
    pixel_written_.assign(static_cast<size_t>(width_) * height_, 0);
  }

  if (sample_count_pixels && tile_width > 0 && tile_height > 0) {
    if (sample_count_buffer_.size() != static_cast<size_t>(width_) * height_) {
      sample_count_buffer_.assign(static_cast<size_t>(width_) * height_, 0.0f);
    }
    use_sample_count_buffer_ = true;
    sample_count_scale_ = max(sample_count_scale, 0.0f);

    for (int y = 0; y < tile_height; y++) {
      const int global_y = tile_offset_y + y;
      if (global_y < 0 || global_y >= height_) {
        continue;
      }
      for (int x = 0; x < tile_width; x++) {
        const int global_x = tile_offset_x + x;
        if (global_x < 0 || global_x >= width_) {
          continue;
        }

        const size_t src_idx = static_cast<size_t>(y) * tile_width + x;
        const size_t global_idx = static_cast<size_t>(global_y) * width_ + global_x;
        sample_count_buffer_[global_idx] = sample_count_pixels[src_idx];
      }
    }
  }

  if (batched_capture) {
    if (deep_gpu_volume_profile_enabled() &&
        !merge_device_volume_capture(beauty_pixels, tile_width, tile_height,
                                     tile_offset_x, tile_offset_y))
    {
      processing_error_.store(true, std::memory_order_relaxed);
      return;
    }
    merge_raw_capture_region_into_cache(beauty_pixels,
                                        tile_width,
                                        tile_height,
                                        tile_offset_x,
                                        tile_offset_y);
  }
  else {
    const bool track_overlap = true;
    bool overlap_logged = false;
    for (const DeepBufferSlice &slice : device_buffers_) {
      merge_slice_into_cache(slice,
                             track_overlap,
                             pixel_written_,
                             overlap_logged,
                             beauty_pixels,
                             tile_width,
                             tile_height,
                             tile_offset_x,
                             tile_offset_y);
    }
  }

  if (trace) {
    std::fprintf(stderr, "DEEP_TILE_MERGE flush_ms=%.6f merge_ms=%.6f\n",
                 (flushed - start) * 1000, (time_dt() - flushed) * 1000);
  }
  /* Next tile will have different device buffers; reprocess on demand. */
  deep_buffers_processed_ = false;
}

bool DeepOutputDriver::is_enabled() const
{
  return enabled_;
}

void DeepOutputDriver::set_enabled(bool enabled)
{
  enabled_ = enabled;
  if (!enabled_) {
    processed_cache_.reset();
    deep_buffers_processed_ = false;
    pixel_written_.clear();
    raw_capture_cache_.reset();
    raw_capture_complete_.clear();
    device_buffers_.clear();
  }
}

void DeepOutputDriver::set_merge_threshold(float threshold)
{
  merge_threshold_ = threshold;
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  pixel_written_.clear();
}

void DeepOutputDriver::set_alpha_merge_threshold(float threshold)
{
  alpha_merge_threshold_ = threshold;
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  pixel_written_.clear();
}

void DeepOutputDriver::set_compression(int compression)
{
  compression_ = compression;
}

void DeepOutputDriver::set_use_half_float(bool use_half)
{
  use_half_float_ = use_half;
}

void DeepOutputDriver::set_display_window(bool has_display_window,
                                          int display_width,
                                          int display_height,
                                          int display_offset_x,
                                          int display_offset_y,
                                          int data_offset_x,
                                          int data_offset_y)
{
  has_display_window_ = has_display_window;
  display_width_ = display_width;
  display_height_ = display_height;
  display_offset_x_ = display_offset_x;
  display_offset_y_ = display_offset_y;
  data_offset_x_ = data_offset_x;
  data_offset_y_ = data_offset_y;
}

void DeepOutputDriver::set_write_callback(DeepExrWriteCallback callback)
{
  write_callback_ = std::move(callback);
}

bool DeepOutputDriver::process_device_buffers()
{
  if (deep_buffers_processed_) {
    return true;
  }

  for (DeepBufferSlice &slice : device_buffers_) {
    if (!slice.buffers) {
      continue;
    }

    /* Copy data from device to host. */
    if (!slice.buffers->copy_from_device()) {
      LOG_ERROR << "Failed to copy deep buffers from device";
      processing_error_.store(true, std::memory_order_relaxed);
      return false;
    }

    /* Sort samples by depth (front to back). */
    slice.buffers->sort_samples_by_depth();

  }

  deep_buffers_processed_ = true;
  return true;
}

void DeepOutputDriver::init_raw_capture_cache()
{
  const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  raw_capture_cache_ = make_unique<vector<vector<DeepSampleData>>>(pixel_count);
  raw_capture_complete_.assign(pixel_count, 1);
}

bool DeepOutputDriver::flush_device_capture(const bool finalize, const bool allow_device_profile)
{
  if (!enabled_ || !deep_volume_profile_enabled()) {
    return true;
  }
  if (!raw_capture_cache_) {
    init_raw_capture_cache();
  }

  const char *parallel = getenv("CYCLES_EXPERIMENTAL_DEEP_PARALLEL_CAPTURE");
  const bool use_parallel = parallel && std::strcmp(parallel, "1") == 0;
  const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE");
  const bool trace_enabled = trace && std::strcmp(trace, "1") == 0;
  if (trace_enabled && finalize) {
    std::fprintf(stderr, "DEEP_GPU_PROFILE_ROUTE final_flush allow=%d slices=%zu\n",
                 int(allow_device_profile), device_buffers_.size());
  }
  const auto append_batch = [&](DeepBufferSlice &slice) {
    const double start = trace_enabled ? time_dt() : 0.0;
    slice.buffers->sort_samples_by_depth();
    const double append_start = trace_enabled ? time_dt() : 0.0;
    std::atomic<bool> success(true);
    std::atomic<bool> overflow(false);
    const auto append_rows = [&](const blocked_range<int> &rows) {
      for (int y = rows.begin(); y < rows.end(); y++) {
        const int local_y = slice.window_y + y;
        const int global_y = slice.full_y + local_y;
        if (local_y < 0 || local_y >= slice.height || global_y < 0 || global_y >= height_) {
          continue;
        }
        for (int x = 0; x < slice.window_width; x++) {
          const int local_x = slice.window_x + x;
          const int global_x = slice.full_x + local_x;
          if (local_x < 0 || local_x >= slice.width || global_x < 0 || global_x >= width_) {
            continue;
          }

          const size_t local_idx = static_cast<size_t>(local_y) * slice.width + local_x;
          const size_t global_idx = static_cast<size_t>(global_y) * width_ + global_x;
          uint32_t raw_count;
          const DeepSampleData *samples;
          size_t offset;
          if (!slice.buffers->get_pixel_sample_range(local_idx, raw_count, samples, offset)) {
            success.store(false);
            return;
          }

          if (deep_sample_count_overflowed(raw_count)) {
            overflow.store(true);
            success.store(false);
            return;
          }
          const size_t count = deep_sample_count_retained(raw_count);
          if (count > 0) {
            vector<DeepSampleData> &destination = (*raw_capture_cache_)[global_idx];
            destination.insert(destination.end(), samples + offset, samples + offset + count);
          }
        }
      }
    };
    const blocked_range<int> rows(0, max(0, slice.window_height), 1);
    if (use_parallel) {
      tbb::task_group_context context;
      context.capture_fp_settings();
      parallel_for(rows, append_rows, context);
    }
    else {
      append_rows(rows);
    }
    if (overflow.load()) {
      slice.device->set_error(
          "Deep EXR optical capture overflow: the bounded capture batch was too large");
    }
    if (trace_enabled) {
      std::fprintf(stderr,
                   "DEEP_CAPTURE_APPEND parallel=%d sort_ms=%.6f append_ms=%.6f\n",
                   int(use_parallel), (append_start - start) * 1000,
                   (time_dt() - append_start) * 1000);
    }
    return success.load();
  };

  for (DeepBufferSlice &slice : device_buffers_) {
    if (!slice.buffers) {
      continue;
    }
    const DeepDeviceCaptureResult capture = slice.buffers->retain_device_capture();
    if (capture == DeepDeviceCaptureResult::Error) {
      processing_error_.store(true, std::memory_order_relaxed);
      return false;
    }
    if (capture == DeepDeviceCaptureResult::Retained) {
      slice.buffers->zero();
      if (!finalize || allow_device_profile) {
        continue;
      }
    }
    /* Preserve original batch order, aborted-attempt filtering, and per-batch depth sorting on
     * both finalization and a budget spill. A spill never drops events or camera samples. */
    while (slice.buffers->has_retained_device_capture()) {
      if (!slice.buffers->copy_retained_device_capture() || !append_batch(slice)) {
        processing_error_.store(true, std::memory_order_relaxed);
        return false;
      }
    }
    if (capture == DeepDeviceCaptureResult::Readback) {
      if (!slice.buffers->copy_from_device() || !append_batch(slice)) {
        processing_error_.store(true, std::memory_order_relaxed);
        return false;
      }
      slice.buffers->zero();
    }
  }

  deep_buffers_processed_ = false;
  return true;
}

bool DeepOutputDriver::merge_device_volume_capture(const float *beauty_pixels,
                                                   const int beauty_width,
                                                   const int beauty_height,
                                                   const int beauty_offset_x,
                                                   const int beauty_offset_y)
{
  if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE")) {
    std::fprintf(stderr, "DEEP_GPU_PROFILE_ROUTE merge slices=%zu beauty=%d size=%dx%d max=%d\n",
                 device_buffers_.size(), int(beauty_pixels != nullptr), beauty_width, beauty_height,
                 max_samples_per_pixel_);
  }
  /* Multiple device slices and missing Beauty metadata retain the complete established path.
   * Layout changes also use the ordinary non-deferred flush before changing pixel ownership. */
  if (device_buffers_.size() != 1 || !beauty_pixels || beauty_width <= 0 || beauty_height <= 0)
    return flush_device_capture(true);
  DeepBufferSlice &slice = device_buffers_.front();
  if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE")) {
    std::fprintf(stderr, "DEEP_GPU_PROFILE_ROUTE slice full=%d,%d size=%dx%d window=%d,%d %dx%d\n",
                 slice.full_x, slice.full_y, slice.width, slice.height, slice.window_x,
                 slice.window_y, slice.window_width, slice.window_height);
  }
  if (!slice.buffers || !slice.buffers->has_retained_device_capture()) return true;
  for (int y = 0; y < slice.window_height; y++) {
    const int gy = slice.full_y + slice.window_y + y;
    if (gy < 0 || gy >= height_) continue;
    for (int x = 0; x < slice.window_width; x++) {
      const int gx = slice.full_x + slice.window_x + x;
      if (gx < 0 || gx >= width_) continue;
      if (!(*raw_capture_cache_)[size_t(gy) * width_ + gx].empty())
        return flush_device_capture(true); /* A prior budget spill already belongs to this pixel. */
    }
  }
  const size_t pixels = size_t(slice.width) * slice.height;
  vector<float> population(pixels, 1.0f), beauty(pixels * 4, 0.0f);
  for (int y = 0; y < slice.height; y++) {
    const int gy = slice.full_y + y;
    for (int x = 0; x < slice.width; x++) {
      const int gx = slice.full_x + x;
      const size_t local = size_t(y) * slice.width + x;
      if (gx < 0 || gx >= width_ || gy < 0 || gy >= height_) continue;
      population[local] = float(max(1, int(get_sample_count_pixel(size_t(gy) * width_ + gx) + 0.5f)));
      const int bx = gx - beauty_offset_x, by = gy - beauty_offset_y;
      if (bx >= 0 && bx < beauty_width && by >= 0 && by < beauty_height)
        std::copy_n(beauty_pixels + (size_t(by) * beauty_width + bx) * 4, 4,
                    beauty.data() + local * 4);
    }
  }
  vector<uint32_t> offsets;
  vector<uint8_t> storage;
  const bool trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
  const double profile_start = trace ? time_dt() : 0.0;
  const DeepVolumeCaptureResult result = slice.buffers->profile_retained_device_capture(
      population, beauty, max_samples_per_pixel_, offsets, storage);
  const double install_start = trace ? time_dt() : 0.0;
  if (result == DeepVolumeCaptureResult::Error) return false;
  if (result == DeepVolumeCaptureResult::Unsupported) return flush_device_capture(true);
  if (offsets.size() != pixels + 1 || storage.size() != size_t(offsets.back()) * 24) {
    slice.device->set_error("Invalid Deep GPU profile host layout");
    return false;
  }
  static_assert(sizeof(blender::DeepSample) == 24);
  const bool dump_install = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_DUMP_ROOT") != nullptr;
  std::atomic<bool> invalid_range(false), invalid_sample(false);
  const auto install_rows = [&](const blocked_range<int> &rows) {
    for (int y = rows.begin(); y < rows.end(); y++) {
      const int ly = slice.window_y + y, gy = slice.full_y + ly;
      if (ly < 0 || ly >= slice.height || gy < 0 || gy >= height_) continue;
      for (int x = 0; x < slice.window_width; x++) {
        const int lx = slice.window_x + x, gx = slice.full_x + lx;
        if (lx < 0 || lx >= slice.width || gx < 0 || gx >= width_) continue;
        const size_t local = size_t(ly) * slice.width + lx, global = size_t(gy) * width_ + gx;
        if (!slice.buffers->device_profile_pixel(local)) continue;
        if (offsets[local] > offsets[local + 1] || offsets[local + 1] > offsets.back()) {
          invalid_range.store(true, std::memory_order_relaxed);
          return;
        }
        if (pixel_written_[global]) continue;
        auto &destination = (*processed_cache_)[global];
        destination.resize(offsets[local + 1] - offsets[local]);
        if (!destination.empty())
          std::memcpy(destination.data(), storage.data() + size_t(offsets[local]) * 24,
                       destination.size() * 24);
        for (const blender::DeepSample &sample : destination) {
          if (!std::isfinite(sample.r) || !std::isfinite(sample.g) || !std::isfinite(sample.b) ||
              !std::isfinite(sample.a) || sample.a < 0 || sample.a > 1 ||
              !std::isfinite(sample.z) || !std::isfinite(sample.z_back) || sample.z_back <= sample.z)
          {
            invalid_sample.store(true, std::memory_order_relaxed);
            return;
          }
        }
        pixel_written_[global] = 1;
        if (dump_install &&
            ((gx == 204 && gy == 127) || (gx == 165 && gy == 136))) {
          std::fprintf(stderr, "DEEP_GPU_PROFILE_INSTALL pixel=%zu samples=%zu\n", global, destination.size());
          for (const auto &sample : destination)
            std::fprintf(stderr, "DEEP_GPU_PROFILE_INSTALL_VALUE %.9g %.9g %.9g\n", sample.z, sample.z_back, sample.a);
        }
      }
    }
  };
  const char *parallel_install = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_PARALLEL_INSTALL");
  const bool use_parallel_install = parallel_install && std::strcmp(parallel_install, "1") == 0;
  const blocked_range<int> rows(0, max(0, slice.window_height), 1);
  if (use_parallel_install) {
    /* Each row owns distinct sample vectors and byte-valued pixel status. Join before
     * checking errors, selecting fallback pixels, or releasing retained device batches. */
    tbb::task_group_context context;
    context.capture_fp_settings();
    parallel_for(rows, install_rows, context);
  }
  else {
    install_rows(rows);
  }
  if (invalid_range.load(std::memory_order_relaxed)) {
    slice.device->set_error("Invalid Deep GPU profile pixel range");
    return false;
  }
  if (invalid_sample.load(std::memory_order_relaxed)) {
    slice.device->set_error("Non-finite or invalid Deep GPU profile sample");
    return false;
  }
  const double install_finish = trace ? time_dt() : 0.0;
  const char *profile_window = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_PROFILE_WINDOW");
  if (profile_window && std::strcmp(profile_window, "1") == 0) {
    const int x = max(slice.window_x, max(0, -slice.full_x));
    const int y = max(slice.window_y, max(0, -slice.full_y));
    const int right = min(slice.window_x + slice.window_width, min(slice.width, width_ - slice.full_x));
    const int bottom = min(slice.window_y + slice.window_height, min(slice.height, height_ - slice.full_y));
    if (!slice.buffers->select_device_profile_window(x, y, max(0, right - x), max(0, bottom - y))) {
      return false;
    }
  }
  if (!slice.buffers->device_profile_needs_host_fallback())
    slice.buffers->discard_retained_device_capture();
  /* The selector keeps complete chronological raw payloads for every unsupported pixel. */
  const bool success = flush_device_capture(true);
  if (trace) {
    std::fprintf(stderr,
                 "DEEP_GPU_PROFILE_INSTALL parallel=%d profile_ms=%.6f install_ms=%.6f fallback_ms=%.6f\n",
                 int(use_parallel_install), (install_start - profile_start) * 1000,
                 (install_finish - install_start) * 1000,
                 (time_dt() - install_finish) * 1000);
  }
  return success;
}

void DeepOutputDriver::init_processed_cache()
{
  const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  processed_cache_ = make_unique<std::vector<std::vector<blender::DeepSample>>>(pixel_count);
}

void DeepOutputDriver::merge_raw_capture_region_into_cache(const float *beauty_pixels,
                                                           const int beauty_width,
                                                           const int beauty_height,
                                                           const int beauty_offset_x,
                                                           const int beauty_offset_y)
{
  if (!processed_cache_ || !raw_capture_cache_) {
    return;
  }

  const int region_x = beauty_pixels && beauty_width > 0 ? beauty_offset_x : 0;
  const int region_y = beauty_pixels && beauty_height > 0 ? beauty_offset_y : 0;
  const int region_width = beauty_pixels && beauty_width > 0 ? beauty_width : width_;
  const int region_height = beauty_pixels && beauty_height > 0 ? beauty_height : height_;
  std::atomic<bool> overlap_logged(false);
  const char *parallel = getenv("CYCLES_EXPERIMENTAL_DEEP_PARALLEL_PROFILE");
  const bool use_parallel = parallel && std::strcmp(parallel, "1") == 0;
  const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE");
  const bool trace_enabled = trace && std::strcmp(trace, "1") == 0;
  const char *surface_ties = getenv("CYCLES_EXPERIMENTAL_DEEP_SURFACE_TIE_ORDER");
  const bool deterministic_surface_ties = surface_ties && std::strcmp(surface_ties, "1") == 0;
  const double start = trace_enabled ? time_dt() : 0.0;

  /* Each row owns distinct vectors and byte-valued status entries. Keep every pixel's sort,
   * ray encounter order and floating-point operations unchanged; join before writing the tile. */
  const auto process_rows = [&](const blocked_range<int> &rows) {
    for (int y = rows.begin(); y < rows.end(); y++) {
      const int global_y = region_y + y;
      if (global_y < 0 || global_y >= height_) {
        continue;
      }
      for (int x = 0; x < region_width; x++) {
        const int global_x = region_x + x;
        if (global_x < 0 || global_x >= width_) {
          continue;
        }
        const size_t global_idx = static_cast<size_t>(global_y) * width_ + global_x;
        vector<DeepSampleData> &samples = (*raw_capture_cache_)[global_idx];
        if (samples.empty()) {
          continue;
        }

        if (!pixel_written_.empty() && pixel_written_[global_idx]) {
          if (!overlap_logged.exchange(true)) {
            LOG_WARNING << "Deep EXR raw capture overlap detected while merging tiles";
          }
          samples.clear();
          raw_capture_complete_[global_idx] = 1;
          continue;
        }
        if (!pixel_written_.empty()) {
          pixel_written_[global_idx] = 1;
        }

        if (deterministic_surface_ties) {
          /* Equal-depth hard surfaces otherwise reach normal-based grouping in append order.
           * A different GPU batch topology can move a whole camera's coverage between groups.
           * Break only these exact depth ties; retain every record and every depth. */
          std::sort(samples.begin(), samples.end(), [](const DeepSampleData &a,
                                                       const DeepSampleData &b) {
            if (a.z != b.z) return a.z < b.z;
            const bool a_surface = deep_sample_has_hard_surface_metadata(a);
            const bool b_surface = deep_sample_has_hard_surface_metadata(b);
            if (a_surface != b_surface) return a_surface < b_surface;
            if (!a_surface) return false;
            if (a.flags != b.flags) return a.flags < b.flags;
            if (a.surface_object != b.surface_object) return a.surface_object < b.surface_object;
            if (a.surface_prim != b.surface_prim) return a.surface_prim < b.surface_prim;
            if (a.surface_shader != b.surface_shader) return a.surface_shader < b.surface_shader;
            return a.packed_geometric_normal < b.packed_geometric_normal;
          });
        }
        else {
          std::sort(samples.begin(), samples.end());
        }
        if (beauty_pixels && beauty_width > 0 && beauty_height > 0) {
          const size_t beauty_idx = (static_cast<size_t>(y) * beauty_width + x) * 4;
          populate_pixel_samples_with_resolved_beauty(global_idx,
                                                      int(samples.size()),
                                                      samples.data(),
                                                      0,
                                                      true,
                                                      beauty_pixels[beauty_idx + 0],
                                                      beauty_pixels[beauty_idx + 1],
                                                      beauty_pixels[beauty_idx + 2],
                                                      beauty_pixels[beauty_idx + 3],
                                                      raw_capture_complete_[global_idx] != 0);
        }
        else {
          populate_pixel_samples(global_idx,
                                 int(samples.size()),
                                 samples.data(),
                                 0,
                                 raw_capture_complete_[global_idx] != 0);
        }
        samples.clear();
        raw_capture_complete_[global_idx] = 1;
      }
    }
  };
  const blocked_range<int> rows(0, max(0, region_height), 1);
  if (use_parallel) {
    tbb::task_group_context context;
    context.capture_fp_settings();
    parallel_for(rows, process_rows, context);
  }
  else {
    process_rows(rows);
  }
  if (trace_enabled) {
    std::fprintf(stderr,
                 "DEEP_PROFILE_REGION parallel=%d width=%d height=%d total_ms=%.6f\n",
                 int(use_parallel), region_width, region_height, (time_dt() - start) * 1000);
  }
}

void DeepOutputDriver::merge_slice_into_cache(const DeepBufferSlice &slice,
                                              bool track_overlap,
                                              vector<uint8_t> &pixel_written,
                                              bool &overlap_logged,
                                              /* Beauty pass window. */
                                              const float *beauty_pixels,
                                              int beauty_width,
                                              int beauty_height,
                                              int beauty_offset_x,
                                              int beauty_offset_y)
{
  if (!slice.buffers) {
    return;
  }

  const int buffer_width = slice.width;
  const int buffer_height = slice.height;

  for (int y = 0; y < slice.window_height; y++) {
    const int local_y = slice.window_y + y;
    const int global_y = slice.full_y + slice.window_y + y;
    if (local_y < 0 || local_y >= buffer_height || global_y < 0 || global_y >= height_) {
      continue;
    }

    for (int x = 0; x < slice.window_width; x++) {
      const int local_x = slice.window_x + x;
      const int global_x = slice.full_x + slice.window_x + x;
      if (local_x < 0 || local_x >= buffer_width || global_x < 0 || global_x >= width_) {
        continue;
      }

      const size_t local_idx = static_cast<size_t>(local_y) * buffer_width + local_x;
      const size_t global_idx = static_cast<size_t>(global_y) * width_ + global_x;

      uint32_t raw_count;
      const DeepSampleData *sample_data;
      size_t offset;
      if (!slice.buffers->get_pixel_sample_range(local_idx, raw_count, sample_data, offset)) {
        continue;
      }
      const int count = static_cast<int>(deep_sample_count_retained(raw_count));
      const bool optical_data_complete = !deep_sample_count_overflowed(raw_count);
      if (count <= 0) {
        continue;
      }

      if (track_overlap) {
        if (pixel_written[global_idx]) {
          if (!overlap_logged) {
            LOG_WARNING << "Deep EXR slice overlap detected while merging slices";
            overlap_logged = true;
          }
          continue;
        }
        pixel_written[global_idx] = 1;
      }

      if (beauty_pixels && beauty_width > 0 && beauty_height > 0) {
        const int beauty_x = global_x - beauty_offset_x;
        const int beauty_y = global_y - beauty_offset_y;
        if (beauty_x >= 0 && beauty_x < beauty_width && beauty_y >= 0 &&
            beauty_y < beauty_height)
        {
          const size_t beauty_idx =
              (static_cast<size_t>(beauty_y) * beauty_width + beauty_x) * 4;
          const float beauty_r = beauty_pixels[beauty_idx + 0];
          const float beauty_g = beauty_pixels[beauty_idx + 1];
          const float beauty_b = beauty_pixels[beauty_idx + 2];
          const float beauty_a = beauty_pixels[beauty_idx + 3];
          populate_pixel_samples_with_resolved_beauty(global_idx,
                                                      count,
                                                      sample_data,
                                                      offset,
                                                      true,
                                                      beauty_r,
                                                      beauty_g,
                                                      beauty_b,
                                                      beauty_a,
                                                      optical_data_complete);
          continue;
        }
      }

      populate_pixel_samples(global_idx, count, sample_data, offset, optical_data_complete);
    }
  }
}

void DeepOutputDriver::get_beauty_pixel(size_t global_idx,
                                        float &beauty_r,
                                        float &beauty_g,
                                        float &beauty_b,
                                        float &beauty_a) const
{
  beauty_r = 0.0f;
  beauty_g = 0.0f;
  beauty_b = 0.0f;
  beauty_a = -1.0f;

  if (use_beauty_buffer_ && global_idx * 4 + 3 < beauty_buffer_.size()) {
    const size_t beauty_offset = global_idx * 4;
    beauty_r = beauty_buffer_[beauty_offset + 0];
    beauty_g = beauty_buffer_[beauty_offset + 1];
    beauty_b = beauty_buffer_[beauty_offset + 2];
    beauty_a = beauty_buffer_[beauty_offset + 3];
  }
}

float DeepOutputDriver::get_sample_count_pixel(size_t global_idx) const
{
  if (!use_sample_count_buffer_ || global_idx >= sample_count_buffer_.size()) {
    return 0.0f;
  }

  return sample_count_buffer_[global_idx] * sample_count_scale_;
}

static void deep_scale_alpha_values(vector<float> &scaled_alphas, const float target_alpha)
{
  float deep_alpha = 0.0f;
  const float sample_count = float(scaled_alphas.size());

  for (float &a : scaled_alphas) {
    a = clamp(a, 0.0f, 1.0f);
    deep_alpha = deep_alpha + a * (1.0f - deep_alpha);
  }

  if (fabsf(deep_alpha - target_alpha) > deep_alpha_epsilon) {
    const float target_transparency = 1.0f - target_alpha;
    const float deep_transparency = 1.0f - deep_alpha;
    if (target_alpha <= 0.0f) {
      std::fill(scaled_alphas.begin(), scaled_alphas.end(), 0.0f);
    }
    else if (target_alpha >= 1.0f) {
      std::fill(scaled_alphas.begin(), scaled_alphas.end(), 1.0f);
    }
    else if (deep_alpha < deep_alpha_linear_fallback ||
             target_alpha < deep_alpha_linear_fallback)
    {
      if (deep_alpha > deep_alpha_epsilon) {
        const float scale = target_alpha / deep_alpha;
        for (float &a : scaled_alphas) {
          a = clamp(a * scale, 0.0f, 1.0f);
        }
      }
      else {
        const float alpha_per = 1.0f - powf(target_transparency, 1.0f / sample_count);
        std::fill(scaled_alphas.begin(), scaled_alphas.end(), clamp(alpha_per, 0.0f, 1.0f));
      }
    }
    else if (deep_transparency > deep_alpha_log_min_transparency &&
             deep_transparency < 1.0f &&
             target_transparency > 0.0f)
    {
      const float k = logf(target_transparency) / logf(deep_transparency);
      for (float &a : scaled_alphas) {
        const float t = 1.0f - a;
        const float t_scaled = powf(t, k);
        a = clamp(1.0f - t_scaled, 0.0f, 1.0f);
      }
    }
    else {
      const float alpha_per = 1.0f - powf(target_transparency, 1.0f / sample_count);
      std::fill(scaled_alphas.begin(), scaled_alphas.end(), clamp(alpha_per, 0.0f, 1.0f));
    }
  }
}

void DeepOutputDriver::compute_scaled_alphas(const DeepSampleData *sample_data,
                                             size_t offset,
                                             int count,
                                             float beauty_a,
                                             vector<float> &scaled_alphas,
                                             float &beauty_r,
                                             float &beauty_g,
                                             float &beauty_b)
{
  const float target_alpha = clamp(beauty_a, 0.0f, 1.0f);

  scaled_alphas.resize(static_cast<size_t>(count));
  for (int s = 0; s < count; s++) {
    scaled_alphas[s] = sample_data[offset + s].a;
  }

  deep_scale_alpha_values(scaled_alphas, target_alpha);

  if (beauty_a > deep_alpha_epsilon) {
    const float inv_alpha = 1.0f / beauty_a;
    beauty_r *= inv_alpha;
    beauty_g *= inv_alpha;
    beauty_b *= inv_alpha;
  }
  else {
    beauty_r = 0.0f;
    beauty_g = 0.0f;
    beauty_b = 0.0f;
  }
}

static bool deep_assign_volume_profile_with_beauty(
    std::vector<blender::DeepSample> &pixel_samples,
    const DeepSampleData *sample_data,
    const size_t offset,
    const int count,
    const int total_camera_samples,
    const int max_profile_samples,
    float beauty_r,
    float beauty_g,
    float beauty_b,
    const float beauty_a)
{
  vector<DeepVolumeProfileSample> profile;
  if (!deep_build_volume_profile(
          sample_data,
          offset,
          count,
          total_camera_samples,
          max_profile_samples,
          make_float3(beauty_r, beauty_g, beauty_b),
          beauty_a,
          profile))
  {
    return false;
  }

  pixel_samples.clear();
  pixel_samples.reserve(profile.size());
  for (const DeepVolumeProfileSample &src : profile) {
    blender::DeepSample &dst = pixel_samples.emplace_back();
    dst.r = src.rgb.x;
    dst.g = src.rgb.y;
    dst.b = src.rgb.z;
    dst.a = src.alpha;
    dst.z = src.z;
    dst.z_back = src.z_back;
  }
  return true;
}

static void deep_assign_single_sample_with_beauty(std::vector<blender::DeepSample> &pixel_samples,
                                                  const DeepSampleData &src,
                                                  float beauty_r,
                                                  float beauty_g,
                                                  float beauty_b,
                                                  float beauty_a)
{
  const float sample_alpha = clamp(beauty_a, 0.0f, 1.0f);
  if (beauty_a > deep_alpha_epsilon) {
    const float inv_alpha = 1.0f / beauty_a;
    beauty_r *= inv_alpha;
    beauty_g *= inv_alpha;
    beauty_b *= inv_alpha;
  }
  else {
    beauty_r = 0.0f;
    beauty_g = 0.0f;
    beauty_b = 0.0f;
  }

  pixel_samples.clear();
  blender::DeepSample &dst = pixel_samples.emplace_back();
  dst.r = beauty_r * sample_alpha;
  dst.g = beauty_g * sample_alpha;
  dst.b = beauty_b * sample_alpha;
  dst.a = sample_alpha;
  dst.z = src.z;
  dst.z_back = src.z_back;
}

static void deep_assign_single_sample_with_rgb(std::vector<blender::DeepSample> &pixel_samples,
                                               const DeepSampleData &src,
                                               const float3 rgb,
                                               const float alpha)
{
  const float sample_alpha = clamp(alpha, 0.0f, 1.0f);
  pixel_samples.clear();
  blender::DeepSample &dst = pixel_samples.emplace_back();
  dst.r = rgb.x * sample_alpha;
  dst.g = rgb.y * sample_alpha;
  dst.b = rgb.z * sample_alpha;
  dst.a = sample_alpha;
  dst.z = src.z;
  dst.z_back = src.z_back;
}

bool DeepOutputDriver::populate_pure_surface_grouped_samples(size_t global_idx,
                                                             int count,
                                                             const DeepSampleData *sample_data,
                                                             size_t offset,
                                                             float beauty_a)
{
  if (count <= 0) {
    return false;
  }

  vector<OpaqueSurfacePrefixGroup> surface_groups;
  int representative_count = 0;
  int first_active_index = -1;
  if (!build_opaque_surface_groups(
          sample_data, offset, count, surface_groups, representative_count, first_active_index))
  {
    return false;
  }

  const float target_coverage = clamp(beauty_a, 0.0f, 1.0f);
  std::vector<blender::DeepSample> &pixel_samples = (*processed_cache_)[global_idx];
  pixel_samples.clear();

  if (target_coverage <= deep_alpha_epsilon) {
    return true;
  }

  if (surface_groups.size() == 1) {
    const OpaqueSurfacePrefixGroup &group = surface_groups.front();
    const float3 avg_rgb = (group.hit_count > 0) ? (group.color_sum / float(group.hit_count)) :
                                                   make_float3(0.0f, 0.0f, 0.0f);
    deep_assign_single_sample_with_rgb(
        pixel_samples, sample_data[offset + first_active_index], avg_rgb, target_coverage);
    return true;
  }

  pixel_samples.reserve(surface_groups.size());

  float remaining_target_coverage = target_coverage;
  /* Convert per-group coverage back into front-to-back deep sample alpha. */
  float remaining_transparency = 1.0f;
  for (const OpaqueSurfacePrefixGroup &group : surface_groups) {
    if (remaining_target_coverage <= deep_alpha_epsilon ||
        remaining_transparency <= deep_alpha_epsilon)
    {
      break;
    }

    const float coverage_fraction = float(group.hit_count) / float(representative_count);
    const float group_coverage = clamp(
        target_coverage * coverage_fraction, 0.0f, remaining_target_coverage);
    const float sample_alpha = (remaining_transparency > deep_alpha_epsilon) ?
                                   clamp(group_coverage / remaining_transparency, 0.0f, 1.0f) :
                                   0.0f;
    const float3 avg_rgb = (group.hit_count > 0) ? (group.color_sum / float(group.hit_count)) :
                                                   make_float3(0.0f, 0.0f, 0.0f);

    blender::DeepSample &dst = pixel_samples.emplace_back();
    dst.r = avg_rgb.x * sample_alpha;
    dst.g = avg_rgb.y * sample_alpha;
    dst.b = avg_rgb.z * sample_alpha;
    dst.a = sample_alpha;
    dst.z = group.output_z;
    dst.z_back = group.output_z;

    remaining_target_coverage = max(0.0f, remaining_target_coverage - group_coverage);
    remaining_transparency = max(0.0f, remaining_transparency - group_coverage);
  }

  return !pixel_samples.empty();
}

bool DeepOutputDriver::populate_opaque_surface_prefix_samples(size_t global_idx,
                                                              int count,
                                                              const DeepSampleData *sample_data,
                                                              size_t offset,
                                                              float beauty_r,
                                                              float beauty_g,
                                                              float beauty_b,
                                                              float beauty_a)
{
  if (count <= 0) {
    return false;
  }

  OpaqueSurfacePrefixInfo prefix_info;
  if (!analyze_opaque_surface_prefix(sample_data, offset, count, prefix_info)) {
    return false;
  }

  vector<OpaqueSurfacePrefixGroup> surface_groups;
  if (!build_opaque_surface_prefix_groups(sample_data, offset, prefix_info, surface_groups)) {
    return false;
  }

  if (surface_groups.size() == 1 && prefix_info.prefix_count == count) {
    const OpaqueSurfacePrefixGroup &group = surface_groups.front();
    const float3 avg_rgb = (group.hit_count > 0) ? (group.color_sum / float(group.hit_count)) :
                                                   make_float3(0.0f, 0.0f, 0.0f);
    deep_assign_single_sample_with_rgb(
        (*processed_cache_)[global_idx], sample_data[offset], avg_rgb, beauty_a);
    return true;
  }

  int representative_count = 0;
  for (const OpaqueSurfacePrefixGroup &group : surface_groups) {
    representative_count += group.hit_count;
  }

  const float sample_count = get_sample_count_pixel(global_idx);
  const float total_sample_count = (sample_count > 0.0f) ?
                                       max(sample_count, float(representative_count)) :
                                       float(representative_count);
  float remaining_coverage = 1.0f;
  float prefix_r = 0.0f;
  float prefix_g = 0.0f;
  float prefix_b = 0.0f;

  std::vector<blender::DeepSample> &pixel_samples = (*processed_cache_)[global_idx];
  pixel_samples.clear();
  pixel_samples.reserve(surface_groups.size() +
                        static_cast<size_t>(count - prefix_info.prefix_count));

  for (const OpaqueSurfacePrefixGroup &group : surface_groups) {
    if (remaining_coverage <= deep_alpha_epsilon) {
      break;
    }

    const float group_coverage = clamp(float(group.hit_count) / total_sample_count,
                                       0.0f,
                                       remaining_coverage);
    const float sample_alpha = (remaining_coverage > deep_alpha_epsilon) ?
                                   clamp(group_coverage / remaining_coverage, 0.0f, 1.0f) :
                                   0.0f;
    const float3 avg_rgb = (group.hit_count > 0) ? (group.color_sum / float(group.hit_count)) :
                                                   make_float3(0.0f, 0.0f, 0.0f);
    blender::DeepSample &dst = pixel_samples.emplace_back();
    dst.r = avg_rgb.x * sample_alpha;
    dst.g = avg_rgb.y * sample_alpha;
    dst.b = avg_rgb.z * sample_alpha;
    dst.a = sample_alpha;
    dst.z = group.output_z;
    dst.z_back = group.output_z;

    prefix_r += remaining_coverage * dst.r;
    prefix_g += remaining_coverage * dst.g;
    prefix_b += remaining_coverage * dst.b;
    remaining_coverage = max(0.0f, remaining_coverage - group_coverage);
  }

  const float prefix_coverage = 1.0f - remaining_coverage;

  vector<float> tail_alphas;
  tail_alphas.reserve(static_cast<size_t>(count - prefix_info.prefix_count));
  for (int s = prefix_info.prefix_count; s < count; s++) {
    const DeepSampleData &src = sample_data[offset + s];
    if (deep_sample_is_inactive(src)) {
      continue;
    }
    tail_alphas.push_back(src.a);
  }

  if (!tail_alphas.empty()) {
    /* The opaque prefix already accounts for part of the flat pixel coverage.
     * Rescale the trailing non-prefix samples so the tail consumes only the remaining target alpha.
     */
    const float target_tail_alpha = (remaining_coverage > deep_alpha_epsilon) ?
                                        clamp((clamp(beauty_a, 0.0f, 1.0f) - prefix_coverage) /
                                                  remaining_coverage,
                                              0.0f,
                                              1.0f) :
                                        0.0f;
    deep_scale_alpha_values(tail_alphas, target_tail_alpha);
  }

  float tail_transparency = 1.0f;
  for (const float alpha : tail_alphas) {
    tail_transparency *= 1.0f - clamp(alpha, 0.0f, 1.0f);
  }

  const float tail_coverage = 1.0f - tail_transparency;
  const float tail_denominator = remaining_coverage * tail_coverage;
  float tail_r = 0.0f;
  float tail_g = 0.0f;
  float tail_b = 0.0f;
  if (tail_denominator > deep_alpha_epsilon) {
    tail_r = (beauty_r - prefix_r) / tail_denominator;
    tail_g = (beauty_g - prefix_g) / tail_denominator;
    tail_b = (beauty_b - prefix_b) / tail_denominator;
  }

  size_t tail_alpha_index = 0;
  for (int s = prefix_info.prefix_count; s < count; s++) {
    const DeepSampleData &src = sample_data[offset + s];
    if (deep_sample_is_inactive(src)) {
      continue;
    }

    const float sample_alpha = (tail_alpha_index < tail_alphas.size()) ?
                                   clamp(tail_alphas[tail_alpha_index], 0.0f, 1.0f) :
                                   clamp(src.a, 0.0f, 1.0f);
    tail_alpha_index++;
    blender::DeepSample &dst = pixel_samples.emplace_back();
    dst.r = tail_r * sample_alpha;
    dst.g = tail_g * sample_alpha;
    dst.b = tail_b * sample_alpha;
    dst.a = sample_alpha;
    dst.z = src.z;
    dst.z_back = src.z_back;
  }

  return !pixel_samples.empty();
}

void DeepOutputDriver::populate_pixel_samples(size_t global_idx,
                                               int count,
                                               const DeepSampleData *sample_data,
                                               size_t offset,
                                               const bool optical_data_complete)
{
  float beauty_r = 0.0f;
  float beauty_g = 0.0f;
  float beauty_b = 0.0f;
  float beauty_a = -1.0f;
  get_beauty_pixel(global_idx, beauty_r, beauty_g, beauty_b, beauty_a);

  populate_pixel_samples_with_resolved_beauty(
      global_idx,
      count,
      sample_data,
      offset,
      use_beauty_buffer_ && beauty_a >= 0.0f,
      beauty_r,
      beauty_g,
      beauty_b,
      beauty_a,
      optical_data_complete);
}

void DeepOutputDriver::populate_pixel_samples_with_resolved_beauty(
    size_t global_idx,
    int count,
    const DeepSampleData *sample_data,
    size_t offset,
    bool has_beauty,
    float beauty_r,
    float beauty_g,
    float beauty_b,
    float beauty_a,
    const bool optical_data_complete)
{
  vector<float> scaled_alphas;
  if (has_beauty && count > 0) {
    const DeepPixelLayout pixel_layout = classify_deep_pixel_layout(sample_data, offset, count);

    if (deep_volume_profile_enabled() && pixel_layout == DeepPixelLayout::pure_volume &&
        !optical_data_complete)
    {
      processing_error_.store(true, std::memory_order_relaxed);
      static std::atomic<bool> logged_volume_profile_overflow(false);
      if (!logged_volume_profile_overflow.exchange(true)) {
        LOG_WARNING << "Experimental Deep volume profile rejected incomplete optical data: "
                       "the raw per-pixel event buffer overflowed (retained samples="
                    << count << ").";
      }
      return;
    }

    if (deep_volume_profile_enabled() && pixel_layout == DeepPixelLayout::pure_volume &&
        optical_data_complete)
    {
      const int camera_samples = max(1, int(get_sample_count_pixel(global_idx) + 0.5f));
      const int profile_camera_samples = deep_volume_profile_camera_population(
          camera_samples, count, max_samples_per_pixel_);
      const bool assigned = deep_assign_volume_profile_with_beauty((*processed_cache_)[global_idx],
                                                                   sample_data,
                                                                   offset,
                                                                   count,
                                                                   profile_camera_samples,
                                                                   max_samples_per_pixel_,
                                                                   beauty_r,
                                                                   beauty_g,
                                                                   beauty_b,
                                                                   beauty_a);
      static std::atomic<bool> logged_volume_profile_status(false);
      if (!logged_volume_profile_status.exchange(true)) {
        LOG_INFO << "Experimental Deep volume profile: "
                 << (assigned ? "active" : "metadata unavailable") << ", captured samples="
                 << count << ", camera samples=" << camera_samples
                 << ", first flags=" << deep_sample_info_flags(sample_data[offset].flags);
      }
      if (assigned) {
        return;
      }
      /* A profile rejection is not a license to replace missing/invalid optical input with
       * terminal Beauty normalization. Do not publish any processed result for this frame. */
      processing_error_.store(true, std::memory_order_relaxed);
      return;
    }

    if (count == 1) {
      const DeepSampleData &src = sample_data[offset];
      if (deep_sample_has_hard_surface_metadata(src)) {
        deep_assign_single_sample_with_rgb(
            (*processed_cache_)[global_idx], src, deep_sample_rgb(src), beauty_a);
      }
      else {
        deep_assign_single_sample_with_beauty(
            (*processed_cache_)[global_idx], src, beauty_r, beauty_g, beauty_b, beauty_a);
      }
      return;
    }

    int single_active_index = -1;
    if (deep_find_single_active_sample(sample_data, offset, count, single_active_index)) {
      const DeepSampleData &src = sample_data[offset + single_active_index];
      if (deep_sample_has_hard_surface_metadata(src)) {
        deep_assign_single_sample_with_rgb(
            (*processed_cache_)[global_idx], src, deep_sample_rgb(src), beauty_a);
      }
      else {
        deep_assign_single_sample_with_beauty(
            (*processed_cache_)[global_idx], src, beauty_r, beauty_g, beauty_b, beauty_a);
      }
      return;
    }

    if (pixel_layout == DeepPixelLayout::pure_surface &&
        populate_pure_surface_grouped_samples(global_idx, count, sample_data, offset, beauty_a))
    {
      return;
    }

    if (pixel_layout == DeepPixelLayout::safe_surface_front_prefix &&
        populate_opaque_surface_prefix_samples(
            global_idx, count, sample_data, offset, beauty_r, beauty_g, beauty_b, beauty_a))
    {
      return;
    }

    compute_scaled_alphas(sample_data, offset, count, beauty_a, scaled_alphas, beauty_r, beauty_g,
                          beauty_b);
  }

  (*processed_cache_)[global_idx].resize(static_cast<size_t>(count));

  for (int s = 0; s < count; s++) {
    const DeepSampleData &src = sample_data[offset + s];
    blender::DeepSample &dst = (*processed_cache_)[global_idx][s];

    const float sample_alpha = scaled_alphas.empty() ? src.a : scaled_alphas[s];

    if (has_beauty) {
      dst.r = beauty_r * sample_alpha;
      dst.g = beauty_g * sample_alpha;
      dst.b = beauty_b * sample_alpha;
      dst.a = sample_alpha;
    }
    else {
      dst.r = src.r;
      dst.g = src.g;
      dst.b = src.b;
      dst.a = sample_alpha;
    }

    dst.z = src.z;
    dst.z_back = src.z_back;
  }
}

std::vector<std::vector<blender::DeepSample>> *DeepOutputDriver::ensure_processed_cache()
{
  if (!enabled_ || device_buffers_.empty()) {
    return nullptr;
  }

  if (!processed_cache_) {
    if (deep_volume_profile_enabled()) {
      if (!flush_device_capture(true)) {
        return nullptr;
      }
      init_processed_cache();
      merge_raw_capture_region_into_cache(nullptr, 0, 0, 0, 0);
    }
    else {
      if (!process_device_buffers()) {
        return nullptr;
      }

      init_processed_cache();

      const bool track_overlap = (device_buffers_.size() > 1);
      bool overlap_logged = false;
      vector<uint8_t> pixel_written;
      if (track_overlap) {
        const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
        pixel_written.resize(pixel_count, 0);
      }

      for (const DeepBufferSlice &slice : device_buffers_) {
        merge_slice_into_cache(
            slice, track_overlap, pixel_written, overlap_logged, nullptr, 0, 0, 0, 0);
      }
    }
  }

  if (processing_error_.load(std::memory_order_relaxed)) {
    if (device_) device_->set_error("Deep EXR rejected invalid or incomplete optical profile");
    return nullptr;
  }
  for (const DeepBufferSlice &slice : device_buffers_) {
    if (slice.device && slice.device->have_error()) return nullptr;
  }
  return processed_cache_.get();
}

CCL_NAMESPACE_END
