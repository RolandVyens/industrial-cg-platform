/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_output_driver.h"

#include <limits>

#include "device/device.h"
#include "kernel/types.h"

#include "util/log.h"
#include "util/math.h"
#include "util/path.h"
#include "util/string.h"

CCL_NAMESPACE_BEGIN

namespace {
constexpr float deep_alpha_epsilon = 1e-6f;
constexpr float deep_alpha_linear_fallback = 1e-3f;
constexpr float deep_alpha_log_min_transparency = 1e-5f;
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

DeepOutputDriver::DeepOutputDriver(Device *device) : device_(device)
{
}

DeepOutputDriver::~DeepOutputDriver()
{
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
    uint32_t *dst_counts = slice.buffers->get_sample_counts_data();
    DeepSampleData *dst_samples = slice.buffers->get_sample_data_data();
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

  /* Write using provided callback. */
  if (write_callback_) {
    bool success = write_callback_(
        *deep_data, width_, height_, filepath, compression_, use_half_float_);
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

  return processed_cache_.release();
}

void DeepOutputDriver::set_beauty_buffer(const float *rgba_buffer, int width, int height)
{
  if (!pixel_written_.empty()) {
    return;
  }

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

void DeepOutputDriver::accumulate_tile(const float *beauty_pixels,
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

    /* Merge nearby samples based on threshold. */
    if (merge_threshold_ > 0.0f) {
      slice.buffers->set_merge_thresholds(merge_threshold_, alpha_merge_threshold_);
      slice.buffers->merge_nearby_samples();
    }
  }

  deep_buffers_processed_ = true;
  return true;
}

void DeepOutputDriver::init_processed_cache()
{
  processed_cache_ = make_unique<std::vector<std::vector<blender::DeepSample>>>(width_ * height_);
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

      const int count = static_cast<int>(sample_counts[local_idx]);
      const int offset = static_cast<int>(local_idx * static_cast<size_t>(buffer_max_samples));

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
          populate_pixel_samples_with_beauty(
              global_idx, count, sample_data, offset, beauty_r, beauty_g, beauty_b, beauty_a);
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

void DeepOutputDriver::compute_scaled_alphas(const DeepSampleData *sample_data,
                                             int offset,
                                             int count,
                                             float beauty_a,
                                             vector<float> &scaled_alphas,
                                             float &beauty_r,
                                             float &beauty_g,
                                             float &beauty_b)
{
  const float target_alpha = clamp(beauty_a, 0.0f, 1.0f);
  float deep_alpha = 0.0f;

  scaled_alphas.resize(count);
  for (int s = 0; s < count; s++) {
    float a = sample_data[offset + s].a;
    a = clamp(a, 0.0f, 1.0f);
    scaled_alphas[s] = a;
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
        const float alpha_per = 1.0f - powf(target_transparency, 1.0f / float(count));
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
      const float alpha_per = 1.0f - powf(target_transparency, 1.0f / float(count));
      std::fill(scaled_alphas.begin(), scaled_alphas.end(), clamp(alpha_per, 0.0f, 1.0f));
    }
  }

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

void DeepOutputDriver::populate_pixel_samples(size_t global_idx,
                                              int count,
                                              const DeepSampleData *sample_data,
                                              int offset)
{
  (*processed_cache_)[global_idx].resize(count);

  float beauty_r = 0.0f;
  float beauty_g = 0.0f;
  float beauty_b = 0.0f;
  float beauty_a = -1.0f;
  get_beauty_pixel(global_idx, beauty_r, beauty_g, beauty_b, beauty_a);

  vector<float> scaled_alphas;
  if (use_beauty_buffer_ && beauty_a >= 0.0f && count > 0) {
    compute_scaled_alphas(
        sample_data, offset, count, beauty_a, scaled_alphas, beauty_r, beauty_g, beauty_b);
  }

  /* Deep Recolor: Premultiply beauty RGB by per-sample alpha.
   *
   * This matches deep compositing expectations where RGB is associated with alpha. */
  for (int s = 0; s < count; s++) {
    const DeepSampleData &src = sample_data[offset + s];
    blender::DeepSample &dst = (*processed_cache_)[global_idx][s];

    /* Use raw or scaled alpha. */
    const float sample_alpha = scaled_alphas.empty() ? src.a : scaled_alphas[s];

    if (use_beauty_buffer_) {
      dst.r = beauty_r * sample_alpha;
      dst.g = beauty_g * sample_alpha;
      dst.b = beauty_b * sample_alpha;
      dst.a = sample_alpha;
    }
    else {
      /* No beauty data available - fall back to kernel values. */
      dst.r = src.r;
      dst.g = src.g;
      dst.b = src.b;
      dst.a = sample_alpha;
    }

    dst.z = src.z;
    dst.z_back = src.z_back;
  }
}

void DeepOutputDriver::populate_pixel_samples_with_beauty(size_t global_idx,
                                                          int count,
                                                          const DeepSampleData *sample_data,
                                                          int offset,
                                                          float beauty_r,
                                                          float beauty_g,
                                                          float beauty_b,
                                                          float beauty_a)
{
  (*processed_cache_)[global_idx].resize(count);

  vector<float> scaled_alphas;
  if (beauty_a >= 0.0f && count > 0) {
    compute_scaled_alphas(sample_data, offset, count, beauty_a, scaled_alphas, beauty_r, beauty_g,
                          beauty_b);
  }

  const bool use_beauty = (beauty_a >= 0.0f);
  for (int s = 0; s < count; s++) {
    const DeepSampleData &src = sample_data[offset + s];
    blender::DeepSample &dst = (*processed_cache_)[global_idx][s];

    const float sample_alpha = scaled_alphas.empty() ? src.a : scaled_alphas[s];

    if (use_beauty) {
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
      pixel_written.resize(width_ * height_, 0);
    }

    for (const DeepBufferSlice &slice : device_buffers_) {
      merge_slice_into_cache(
          slice, track_overlap, pixel_written, overlap_logged, nullptr, 0, 0, 0, 0);
    }
  }

  return processed_cache_.get();
}

CCL_NAMESPACE_END
