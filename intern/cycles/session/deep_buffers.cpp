/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_buffers.h"

#include "device/device.h"
#include "IMB_deep_sample_merge.hh"
#include "util/algorithm.h"
#include "util/log.h"

namespace blender::imbuf::deep_merge {

template<> struct DeepSampleTraits<ccl::DeepSampleData> {
  static float r(const ccl::DeepSampleData &sample)
  {
    return sample.r;
  }

  static float g(const ccl::DeepSampleData &sample)
  {
    return sample.g;
  }

  static float b(const ccl::DeepSampleData &sample)
  {
    return sample.b;
  }

  static float a(const ccl::DeepSampleData &sample)
  {
    return sample.a;
  }

  static float z(const ccl::DeepSampleData &sample)
  {
    return sample.z;
  }

  static float z_back(const ccl::DeepSampleData &sample)
  {
    return sample.z_back;
  }

  static void set_r(ccl::DeepSampleData &sample, const float value)
  {
    sample.r = value;
  }

  static void set_g(ccl::DeepSampleData &sample, const float value)
  {
    sample.g = value;
  }

  static void set_b(ccl::DeepSampleData &sample, const float value)
  {
    sample.b = value;
  }

  static void set_a(ccl::DeepSampleData &sample, const float value)
  {
    sample.a = value;
  }

  static void set_z_back(ccl::DeepSampleData &sample, const float value)
  {
    sample.z_back = value;
  }
};

}  // namespace blender::imbuf::deep_merge

CCL_NAMESPACE_BEGIN

namespace {
constexpr float deep_volume_depth_epsilon = 1e-6f;
}  // namespace

/* -------------------------------------------------------------------- */
/** \name Deep Render Buffers Implementation
 * \{ */

DeepRenderBuffers::DeepRenderBuffers(Device *device)
    : sample_counts_(device, "deep_sample_counts", MEM_READ_WRITE),
      sample_data_(device, "deep_sample_data", MEM_READ_WRITE),
      device_(device)
{
}

DeepRenderBuffers::~DeepRenderBuffers()
{
  sample_counts_.free();
  sample_data_.free();
}

void DeepRenderBuffers::reset(int new_width, int new_height, int new_max_samples)
{
  width_ = new_width;
  height_ = new_height;
  max_samples_per_pixel_ = new_max_samples;

  const size_t num_pixels = static_cast<size_t>(width_) * height_;

  /* Allocate sample counts (one per pixel). */
  sample_counts_.alloc(num_pixels);

  /* Allocate sample data with worst-case size:
   * Every pixel could have max_samples_per_pixel samples.
   * This is memory intensive but simplifies GPU access patterns. */
  const size_t max_total_samples = num_pixels * max_samples_per_pixel_;
  sample_data_.alloc(max_total_samples);

  /* Initialize to zero. */
  zero();

  /* Cache device pointers for kernel access immediately after allocation. */
  d_sample_counts_ = sample_counts_.device_pointer;
  d_sample_data_ = sample_data_.device_pointer;

  LOG_DEBUG << "Deep buffers allocated: " << width_ << "x" << height_ << " with max "
            << max_samples_per_pixel_ << " samples/pixel ("
            << (max_total_samples * sizeof(DeepSampleData)) / (1024 * 1024) << " MB)";
}

void DeepRenderBuffers::zero()
{
  sample_counts_.zero_to_device();
  sample_data_.zero_to_device();
}

bool DeepRenderBuffers::copy_from_device()
{
  if (!is_allocated()) {
    return false;
  }

  sample_counts_.copy_from_device();
  sample_data_.copy_from_device();

  return true;
}

void DeepRenderBuffers::copy_to_device()
{
  if (!is_allocated()) {
    return;
  }

  sample_counts_.copy_to_device();
  sample_data_.copy_to_device();

  /* Cache device pointers for kernel access. */
  d_sample_counts_ = sample_counts_.device_pointer;
  d_sample_data_ = sample_data_.device_pointer;
}

void DeepRenderBuffers::get_pixel_samples(int x, int y, vector<DeepSampleData> &out_samples) const
{
  out_samples.clear();

  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    return;
  }

  const size_t pixel_index = static_cast<size_t>(y) * width_ + x;
  const uint32_t num_samples = sample_counts_.data()[pixel_index];

  if (num_samples == 0) {
    return;
  }

  /* Calculate offset into sample_data for this pixel. */
  const size_t sample_offset = pixel_index * max_samples_per_pixel_;

  out_samples.reserve(num_samples);
  for (uint32_t i = 0; i < num_samples && i < uint32_t(max_samples_per_pixel_); i++) {
    out_samples.push_back(sample_data_.data()[sample_offset + i]);
  }
}

size_t DeepRenderBuffers::get_total_sample_count() const
{
  if (!is_allocated()) {
    return 0;
  }

  size_t total = 0;
  const uint32_t *counts = sample_counts_.data();
  const size_t num_pixels = static_cast<size_t>(width_) * height_;

  for (size_t i = 0; i < num_pixels; i++) {
    total += counts[i];
  }

  return total;
}

void DeepRenderBuffers::sort_samples_by_depth()
{
  if (!is_allocated()) {
    return;
  }

  const size_t num_pixels = static_cast<size_t>(width_) * height_;
  const uint32_t *counts = sample_counts_.data();
  DeepSampleData *data = sample_data_.data();

  for (size_t pixel = 0; pixel < num_pixels; pixel++) {
    const uint32_t num_samples = counts[pixel];
    if (num_samples <= 1) {
      continue;  /* Nothing to sort. */
    }

    const size_t offset = pixel * max_samples_per_pixel_;
    sort(data + offset, data + offset + num_samples);
  }
}

void DeepRenderBuffers::merge_nearby_samples()
{
  if (!is_allocated() || depth_merge_threshold_ <= 0.0f) {
    return;
  }

  const size_t num_pixels = static_cast<size_t>(width_) * height_;
  uint32_t *counts = sample_counts_.data();
  DeepSampleData *data = sample_data_.data();

  for (size_t pixel = 0; pixel < num_pixels; pixel++) {
    uint32_t num_samples = counts[pixel];
    if (num_samples <= 1) {
      continue;
    }

    const size_t offset = pixel * max_samples_per_pixel_;

    counts[pixel] = blender::imbuf::deep_merge::merge_sorted_deep_samples(
        data + offset,
        num_samples,
        depth_merge_threshold_,
        alpha_merge_threshold_,
        deep_volume_depth_epsilon);
  }
}

void DeepRenderBuffers::compute_sample_offsets()
{
  if (!is_allocated()) {
    return;
  }

  const size_t num_pixels = static_cast<size_t>(width_) * height_;
  sample_offsets_.resize(num_pixels);

  /* Compute prefix sum of sample counts.
   * Note: For deep buffers we use a fixed layout where each pixel has
   * max_samples_per_pixel slots, so offset = pixel_index * max_samples_per_pixel. */
  for (size_t i = 0; i < num_pixels; i++) {
    sample_offsets_[i] = i * max_samples_per_pixel_;
  }
}

/** \} */

CCL_NAMESPACE_END
