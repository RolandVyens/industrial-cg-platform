/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "IMB_deep_sample.hh" /* Blender deep sample type (for IMB_exr_save_deep). */

#include "session/deep_buffers.h"
#include "session/output_driver.h"

#include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

class Scene;
class Device;
struct KernelData;

/* Callback type for writing deep EXR files.
 * This allows Blender to inject its own EXR writing implementation. */
using DeepExrWriteCallback = std::function<bool(
    const std::vector<std::vector<blender::DeepSample>> &deep_data,
    int width,
    int height,
    const std::string &filepath,
    int compression,
    bool use_half_float)>;

/* Deep output driver for writing deep EXR files.
 *
 * This driver wraps around the standard output driver and additionally
 * manages deep sample buffers. After rendering completes, it processes
 * the deep samples and writes them to a deep EXR file.
 */
class DeepOutputDriver {
 public:
  struct SliceParams {
    Device *device = nullptr;
    int full_x = 0;
    int full_y = 0;
    int width = 0;
    int height = 0;
    int window_x = 0;
    int window_y = 0;
    int window_width = 0;
    int window_height = 0;
  };

  explicit DeepOutputDriver(Device *device);
  ~DeepOutputDriver();

  /* Initialize for a new render with given dimensions and settings. */
  void reset(int width, int height, int max_samples_per_pixel);

  /* Clear deep sample buffers between renders without reallocating. */
  void clear_device_buffers();

  /* Get the deep buffers for kernel access. */
  DeepRenderBuffers *get_deep_buffers();

  /* Called after rendering completes to finalize and save deep data. */
  void finalize_deep_output(const std::string &filepath);

  /* Check if deep output is enabled. */
  bool is_enabled() const;

  /* Enable/disable deep output. */
  void set_enabled(bool enabled);

  /* Set the depth merge threshold for combining nearby samples. */
  void set_merge_threshold(float threshold);

  /* Set the alpha merge threshold for combining nearby samples. */
  void set_alpha_merge_threshold(float threshold);

  /* Set EXR compression type (from Blender scene settings). */
  void set_compression(int compression);

  /* Set whether to use half-float for RGBA channels. */
  void set_use_half_float(bool use_half);

  /* Set the callback function for writing deep EXR files. */
  void set_write_callback(DeepExrWriteCallback callback);

  /* Set beauty buffer for uniform RGB and alpha normalization.
   * The buffer should contain RGBA floats, size = width * height * 4. */
  void set_beauty_buffer(const float *rgba_buffer, int width, int height);

  /* Get processed deep data for compositor storage.
   * Uses std::vector to match Blender render/imbuf API expectations.
   * Returns a newly allocated vector that becomes owned by the caller.
   * Must call set_beauty_buffer first for Deep Recolor. */
  std::vector<std::vector<blender::DeepSample>> *get_processed_deep_data();

  /* Get deep output dimensions. */
  int get_width() const { return width_; }
  int get_height() const { return height_; }
  int get_max_samples_per_pixel() const { return max_samples_per_pixel_; }

  /* Sync per-device deep buffers and update kernel data with device-specific pointers. */
  void sync_device_buffers(const vector<SliceParams> &slices);
  void update_device_kernel_data(const KernelData &base_data);

 private:
  bool enabled_ = false;
  float merge_threshold_ = 0.001f;
  float alpha_merge_threshold_ = 0.01f;
  int width_ = 0;
  int height_ = 0;
  int max_samples_per_pixel_ = 0;
  int compression_ = 1; /* ZIPS by default */
  bool use_half_float_ = false;

  struct DeepBufferSlice {
    Device *device = nullptr;
    unique_ptr<DeepRenderBuffers> buffers;
    int full_x = 0;
    int full_y = 0;
    int width = 0;
    int height = 0;
    int window_x = 0;
    int window_y = 0;
    int window_width = 0;
    int window_height = 0;
  };

  struct DeviceEstimate {
    Device *device = nullptr;
    size_t bytes = 0;
  };

  struct DeepBufferSnapshot {
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

  vector<DeepBufferSlice> device_buffers_;
  Device *device_ = nullptr;

  DeepExrWriteCallback write_callback_;

  /* Beauty buffer for uniform RGB and alpha normalization. */
  vector<float> beauty_buffer_;
  bool use_beauty_buffer_ = false;

  /* Stored as std::vector to match Blender's IMB_exr_save_deep API. */
  unique_ptr<std::vector<std::vector<blender::DeepSample>>> processed_cache_;
  bool deep_buffers_processed_ = false;

  bool process_device_buffers();
  bool layout_matches(const vector<SliceParams> &slices) const;
  bool compute_deep_bytes(int width, int height, int max_samples, size_t &bytes) const;
  bool build_device_estimates(const vector<SliceParams> &slices,
                              int max_samples,
                              vector<DeviceEstimate> &estimates) const;
  bool check_device_memory(const vector<DeviceEstimate> &estimates) const;
  vector<DeepBufferSnapshot> snapshot_device_buffers();
  void init_device_buffers(const vector<SliceParams> &slices, int max_samples);
  void restore_snapshots(const vector<DeepBufferSnapshot> &snapshots);
  void init_processed_cache();
  void merge_slice_into_cache(const DeepBufferSlice &slice,
                              bool track_overlap,
                              vector<uint8_t> &pixel_written,
                              bool &overlap_logged);
  void populate_pixel_samples(size_t global_idx,
                              int count,
                              const DeepSampleData *sample_data,
                              int offset);
  void get_beauty_pixel(size_t global_idx,
                        float &beauty_r,
                        float &beauty_g,
                        float &beauty_b,
                        float &beauty_a) const;
  void compute_scaled_alphas(const DeepSampleData *sample_data,
                             int offset,
                             int count,
                             float beauty_a,
                             vector<float> &scaled_alphas,
                             float &beauty_r,
                             float &beauty_g,
                             float &beauty_b);

  std::vector<std::vector<blender::DeepSample>> *ensure_processed_cache();
};

CCL_NAMESPACE_END
