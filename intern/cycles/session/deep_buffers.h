/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/device.h"
#include "util/types.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Film;
class Integrator;

/* Compute deep buffer bytes for the given dimensions and max samples.
 * Returns false on overflow. */
bool deep_compute_buffer_bytes(int width, int height, int max_samples, size_t &bytes);

/* Compute effective deep max samples, accounting for volume ray marching settings. */
int deep_effective_max_samples(const Film *film, const Integrator *integrator);

/* -------------------------------------------------------------------- */
/** \name Deep Sample Data Structure
 *
 * Stores a single deep sample with RGBA color and depth information.
 * Each pixel can have multiple samples at different depths.
 * \{ */

/* Must match KernelDeepSample alignment in kernel/film/deep_write.h. */
struct alignas(32) DeepSampleData {
  /* Color channels. */
  float r, g, b, a;
  /* Front depth (ray hit distance from camera). */
  float z;
  /* Back depth (for volumetric samples, equals z for surfaces). */
  float z_back;

  DeepSampleData() : r(0), g(0), b(0), a(0), z(0), z_back(0) {}

  DeepSampleData(float r_, float g_, float b_, float a_, float z_, float z_back_)
      : r(r_), g(g_), b(b_), a(a_), z(z_), z_back(z_back_)
  {
  }

  /* Compare by depth for sorting. */
  bool operator<(const DeepSampleData &other) const
  {
    return z < other.z;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deep Render Buffers
 *
 * Manages per-pixel deep sample storage during rendering.
 * Uses a fixed maximum samples per pixel with atomic counting.
 * \{ */

class DeepRenderBuffers {
 public:
  explicit DeepRenderBuffers(Device *device);
  ~DeepRenderBuffers();

  /* Reset buffers for new render. */
  void reset(int width, int height, int max_samples);

  /* Clear all sample data. */
  void zero();

  /* Copy data from device to host for output. */
  bool copy_from_device();

  /* Copy configuration to device before render. */
  void copy_to_device();

  /* Get samples for a single pixel (host-side, after copy_from_device). */
  void get_pixel_samples(int x, int y, vector<DeepSampleData> &out_samples) const;

  /* Get total number of samples across all pixels. */
  size_t get_total_sample_count() const;

  /* Sort all samples by depth (call before output). */
  void sort_samples_by_depth();

  /* Merge nearby samples within threshold (reduces output size). */
  void merge_nearby_samples();

  /* Host-side accessors for reading sample data after copy_from_device(). */
  const uint32_t *get_sample_counts_host() const
  {
    return sample_counts_.data();
  }

  const DeepSampleData *get_sample_data_host() const
  {
    return sample_data_.data();
  }

  int get_width() const
  {
    return width_;
  }

  int get_height() const
  {
    return height_;
  }

  int get_max_samples_per_pixel() const
  {
    return max_samples_per_pixel_;
  }

  float get_depth_merge_threshold() const
  {
    return depth_merge_threshold_;
  }

  float get_alpha_merge_threshold() const
  {
    return alpha_merge_threshold_;
  }

  void set_merge_thresholds(const float depth_threshold, const float alpha_threshold)
  {
    depth_merge_threshold_ = depth_threshold;
    alpha_merge_threshold_ = alpha_threshold;
  }

  const device_vector<uint32_t> &get_sample_counts() const
  {
    return sample_counts_;
  }

  uint32_t *get_sample_counts_ptr()
  {
    return sample_counts_.data();
  }

  const device_vector<DeepSampleData> &get_sample_data() const
  {
    return sample_data_;
  }

  DeepSampleData *get_sample_data_ptr()
  {
    return sample_data_.data();
  }

  device_ptr get_sample_counts_device_ptr() const
  {
    return d_sample_counts_;
  }

  device_ptr get_sample_data_device_ptr() const
  {
    return d_sample_data_;
  }

 private:
  Device *device_;

  /* Buffer parameters. */
  int width_ = 0;
  int height_ = 0;

  /* Configuration from scene settings. */
  int max_samples_per_pixel_ = 64;
  float depth_merge_threshold_ = 0.01f;
  float alpha_merge_threshold_ = 0.01f;

  /* Per-pixel sample count (atomic during render). */
  device_vector<uint32_t> sample_counts_;

  /* Sample data: width * height * max_samples_per_pixel entries
   * Organized as: pixel[0].samples[0..n], pixel[1].samples[0..n], ... */
  device_vector<DeepSampleData> sample_data_;

  /* Device pointer for kernel access. */
  device_ptr d_sample_counts_ = 0;
  device_ptr d_sample_data_ = 0;

  /* Check if buffers are allocated. */
  bool is_allocated() const
  {
    return width_ > 0 && height_ > 0;
  }
};

/** \} */

CCL_NAMESPACE_END
