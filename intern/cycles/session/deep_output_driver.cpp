/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_output_driver.h"

#include <limits>

#include "IMB_deep_sample_merge.hh"

#include "device/device.h"
#include "kernel/types.h"

#include "util/log.h"
#include "util/math.h"
#include "util/path.h"
#include "util/string.h"

#include <cstring>

namespace blender::imbuf::deep_merge {

template<> struct DeepSampleTraits<blender::DeepSample> {
  static float r(const blender::DeepSample &sample)
  {
    return sample.r;
  }

  static float g(const blender::DeepSample &sample)
  {
    return sample.g;
  }

  static float b(const blender::DeepSample &sample)
  {
    return sample.b;
  }

  static float a(const blender::DeepSample &sample)
  {
    return sample.a;
  }

  static float z(const blender::DeepSample &sample)
  {
    return sample.z;
  }

  static float z_back(const blender::DeepSample &sample)
  {
    return sample.z_back;
  }

  static void set_r(blender::DeepSample &sample, const float value)
  {
    sample.r = value;
  }

  static void set_g(blender::DeepSample &sample, const float value)
  {
    sample.g = value;
  }

  static void set_b(blender::DeepSample &sample, const float value)
  {
    sample.b = value;
  }

  static void set_a(blender::DeepSample &sample, const float value)
  {
    sample.a = value;
  }

  static void set_z_back(blender::DeepSample &sample, const float value)
  {
    sample.z_back = value;
  }
};

}  // namespace blender::imbuf::deep_merge

CCL_NAMESPACE_BEGIN

namespace {
constexpr float deep_alpha_epsilon = 1e-6f;
constexpr float deep_alpha_linear_fallback = 1e-3f;
constexpr float deep_alpha_log_min_transparency = 1e-5f;
constexpr float deep_surface_depth_epsilon = 1e-6f;
constexpr float deep_volume_depth_epsilon = 1e-6f;
constexpr float deep_opaque_surface_alpha_threshold = 1.0f - 1e-6f;
constexpr float deep_inactive_sample_epsilon = 1e-8f;
constexpr float deep_surface_normal_dot_threshold = 0.94f;

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

  uint32_t max_camera_sample = 0;
  bool have_camera_sample = false;
  for (int s = 0; s < info.prefix_count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (!deep_sample_has_hard_surface_metadata(sample)) {
      groups.clear();
      return false;
    }

    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (!have_camera_sample || camera_sample > max_camera_sample) {
      max_camera_sample = camera_sample;
      have_camera_sample = true;
    }
  }

  if (!have_camera_sample) {
    return false;
  }

  vector<uint8_t> camera_sample_seen(static_cast<size_t>(max_camera_sample) + 1, 0);

  for (int s = 0; s < info.prefix_count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (camera_sample >= camera_sample_seen.size()) {
      groups.clear();
      return false;
    }
    if (camera_sample_seen[camera_sample]) {
      continue;
    }
    camera_sample_seen[camera_sample] = 1;

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

  uint32_t max_camera_sample = 0;
  bool have_camera_sample = false;
  for (int s = 0; s < count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (deep_sample_is_inactive(sample)) {
      continue;
    }
    if (!deep_sample_is_opaque_surface(sample) || !deep_sample_has_hard_surface_metadata(sample)) {
      groups.clear();
      return false;
    }

    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (!have_camera_sample || camera_sample > max_camera_sample) {
      max_camera_sample = camera_sample;
      have_camera_sample = true;
    }
    if (first_active_index == -1) {
      first_active_index = s;
    }
  }

  if (!have_camera_sample || first_active_index == -1) {
    return false;
  }

  vector<uint8_t> camera_sample_seen(static_cast<size_t>(max_camera_sample) + 1, 0);

  for (int s = 0; s < count; s++) {
    const DeepSampleData &sample = sample_data[offset + s];
    if (deep_sample_is_inactive(sample)) {
      continue;
    }

    const uint32_t camera_sample = deep_sample_camera_sample(sample);
    if (camera_sample >= camera_sample_seen.size()) {
      groups.clear();
      return false;
    }
    if (camera_sample_seen[camera_sample]) {
      continue;
    }
    camera_sample_seen[camera_sample] = 1;

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

std::vector<std::vector<blender::DeepSample>> deep_copy_merged_samples(
    const std::vector<std::vector<blender::DeepSample>> &deep_data,
    const float merge_threshold,
    const float alpha_merge_threshold)
{
  std::vector<std::vector<blender::DeepSample>> merged_samples = deep_data;

  if (merge_threshold <= 0.0f) {
    return merged_samples;
  }

  for (std::vector<blender::DeepSample> &pixel_samples : merged_samples) {
    if (pixel_samples.size() <= 1) {
      continue;
    }

    std::sort(pixel_samples.begin(),
              pixel_samples.end(),
              [](const blender::DeepSample &a, const blender::DeepSample &b) {
                return a.z < b.z;
              });
    const size_t merged_count = blender::imbuf::deep_merge::merge_sorted_deep_samples(
        pixel_samples.data(),
        pixel_samples.size(),
        merge_threshold,
        alpha_merge_threshold,
        deep_volume_depth_epsilon);
    pixel_samples.resize(merged_count);
  }

  return merged_samples;
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

void DeepOutputDriver::reset(int width, int height, int max_samples_per_pixel)
{
  width_ = width;
  height_ = height;
  max_samples_per_pixel_ = max_samples_per_pixel;
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  pixel_written_.clear();
  beauty_buffer_.clear();
  use_beauty_buffer_ = false;
  sample_count_buffer_.clear();
  use_sample_count_buffer_ = false;
  sample_count_scale_ = 1.0f;
  device_buffers_.clear();
}

void DeepOutputDriver::clear_device_buffers()
{
  for (DeepBufferSlice &slice : device_buffers_) {
    if (slice.buffers) {
      slice.buffers->zero();
    }
  }
  processed_cache_.reset();
  deep_buffers_processed_ = false;
  pixel_written_.clear();
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
        existing.window_y != slice.window_y ||
        existing.window_width != slice.window_width ||
        existing.window_height != slice.window_height)
    {
      return false;
    }
  }

  return true;
}

bool DeepOutputDriver::build_device_estimates(const vector<SliceParams> &slices,
                                              int max_samples,
                                              vector<DeviceEstimate> &estimates) const
{
  estimates.clear();

  for (const SliceParams &slice : slices) {
    if (!slice.device || slice.width <= 0 || slice.height <= 0) {
      continue;
    }

    size_t bytes = 0;
    if (!deep_compute_buffer_bytes(slice.width, slice.height, max_samples, bytes)) {
      slice.device->set_error("Deep EXR buffer size overflow (dimensions too large)");
      return false;
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
    const size_t total_samples = num_pixels * static_cast<size_t>(snapshot.max_samples);
    snapshot.sample_counts.assign(existing.buffers->get_sample_counts().data(),
                                  existing.buffers->get_sample_counts().data() + num_pixels);
    snapshot.sample_data.assign(existing.buffers->get_sample_data().data(),
                                existing.buffers->get_sample_data().data() + total_samples);

    snapshots.push_back(std::move(snapshot));
  }

  return snapshots;
}

void DeepOutputDriver::init_device_buffers(const vector<SliceParams> &slices, int max_samples)
{
  device_buffers_.clear();
  device_buffers_.reserve(slices.size());

  for (const SliceParams &slice : slices) {
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
      entry.buffers = make_unique<DeepRenderBuffers>(slice.device);
      entry.buffers->reset(slice.width, slice.height, max_samples);
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
    std::fill(dst_counts, dst_counts + dst_pixels, 0);

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
        uint32_t count = snapshot->sample_counts[src_idx];
        if (count == 0) {
          continue;
        }

        if (count > static_cast<uint32_t>(dst_max_samples)) {
          if (!clamp_logged) {
            LOG_WARNING
                << "Deep EXR buffer preservation clamped samples due to max sample mismatch";
            clamp_logged = true;
          }
          count = static_cast<uint32_t>(dst_max_samples);
        }

        const size_t dst_idx = static_cast<size_t>(y) * dst_width + x;
        dst_counts[dst_idx] = count;

        const size_t src_offset = src_idx * static_cast<size_t>(snapshot->max_samples);
        const size_t dst_offset = dst_idx * static_cast<size_t>(dst_max_samples);
        memcpy(dst_samples + dst_offset,
               snapshot->sample_data.data() + src_offset,
               sizeof(DeepSampleData) * count);
      }
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

  const int max_samples = max(max_samples_per_pixel_, 1);
  vector<DeviceEstimate> estimates;
  if (!build_device_estimates(slices, max_samples, estimates)) {
    return;
  }
  if (!check_device_memory(estimates)) {
    return;
  }

  vector<DeepBufferSnapshot> snapshots = snapshot_device_buffers();
  init_device_buffers(slices, max_samples);
  restore_snapshots(snapshots);

  if (pixel_written_.empty()) {
    processed_cache_.reset();
  }
  deep_buffers_processed_ = false;
}

void DeepOutputDriver::update_device_kernel_data(const KernelData &base_data)
{
  if (!enabled_) {
    return;
  }

  for (const DeepBufferSlice &slice : device_buffers_) {
    if (!slice.device || !slice.buffers) {
      continue;
    }

    KernelData data = base_data;
    data.film.deep_width = slice.width;
    data.film.deep_height = slice.height;
    data.film.deep_max_samples = max_samples_per_pixel_;
    data.film.deep_samples_ptr = slice.buffers->get_sample_data_device_ptr();
    data.film.deep_sample_counts_ptr = slice.buffers->get_sample_counts_device_ptr();

    slice.device->const_copy_to("data", &data, sizeof(data));
  }
}

void DeepOutputDriver::finalize_deep_output(const std::string &filepath)
{
  std::vector<std::vector<blender::DeepSample>> *deep_data = ensure_processed_cache();
  if (!deep_data) {
    return;
  }

  const std::vector<std::vector<blender::DeepSample>> *deep_data_to_write = deep_data;
  std::vector<std::vector<blender::DeepSample>> merged_samples_storage;
  if (merge_threshold_ > 0.0f) {
    merged_samples_storage = deep_copy_merged_samples(
        *deep_data, merge_threshold_, alpha_merge_threshold_);
    deep_data_to_write = &merged_samples_storage;
  }

  /* Write using provided callback. */
  if (write_callback_) {
    bool success = write_callback_(
        *deep_data_to_write,
        width_,
        height_,
        filepath,
        compression_,
        use_half_float_,
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
  }
  else {
    LOG_WARNING << "No deep EXR write callback set - skipping file output";
    LOG_INFO << "Deep output finalization complete (callback not configured)";
  }
}

std::vector<std::vector<blender::DeepSample>> *DeepOutputDriver::get_processed_deep_data()
{
  std::vector<std::vector<blender::DeepSample>> *deep_data = ensure_processed_cache();
  if (!deep_data) {
    return nullptr;
  }

  if (merge_threshold_ > 0.0f) {
    return new std::vector<std::vector<blender::DeepSample>>(
        deep_copy_merged_samples(*deep_data, merge_threshold_, alpha_merge_threshold_));
  }

  return processed_cache_.release();
}

void DeepOutputDriver::release_temporary_host_caches()
{
  processed_cache_.reset();
  pixel_written_.free_memory();
  sample_count_buffer_.free_memory();
  beauty_buffer_.free_memory();
  use_sample_count_buffer_ = false;
  use_beauty_buffer_ = false;
  sample_count_scale_ = 1.0f;
}

void DeepOutputDriver::set_beauty_buffer(const float *rgba_buffer, int width, int height)
{
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

  if (!process_device_buffers()) {
    return;
  }

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
      return false;
    }

    /* Sort samples by depth (front to back). */
    slice.buffers->sort_samples_by_depth();

  }

  deep_buffers_processed_ = true;
  return true;
}

void DeepOutputDriver::init_processed_cache()
{
  const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  processed_cache_ = make_unique<std::vector<std::vector<blender::DeepSample>>>(pixel_count);
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

  const uint32_t *sample_counts = slice.buffers->get_sample_counts_host();
  const DeepSampleData *sample_data = slice.buffers->get_sample_data_host();

  const int buffer_width = slice.width;
  const int buffer_height = slice.height;
  const int buffer_max_samples = slice.buffers->get_max_samples_per_pixel();

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

      const int count = static_cast<int>(sample_counts[local_idx]);
      const size_t offset = local_idx * static_cast<size_t>(buffer_max_samples);
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
          populate_pixel_samples_with_resolved_beauty(
              global_idx, count, sample_data, offset, true, beauty_r, beauty_g, beauty_b, beauty_a);
          continue;
        }
      }

      populate_pixel_samples(global_idx, count, sample_data, offset);
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
    dst.r = beauty_r * sample_alpha;
    dst.g = beauty_g * sample_alpha;
    dst.b = beauty_b * sample_alpha;
    dst.a = sample_alpha;
    dst.z = src.z;
    dst.z_back = src.z_back;
  }

  return !pixel_samples.empty();
}

void DeepOutputDriver::populate_pixel_samples(size_t global_idx,
                                              int count,
                                              const DeepSampleData *sample_data,
                                              size_t offset)
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
      beauty_a);
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
    float beauty_a)
{
  vector<float> scaled_alphas;
  if (has_beauty && count > 0) {
    const DeepPixelLayout pixel_layout = classify_deep_pixel_layout(sample_data, offset, count);

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

  return processed_cache_.get();
}

CCL_NAMESPACE_END
