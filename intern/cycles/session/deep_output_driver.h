/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstddef>
#include <atomic>
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
    bool use_half_float,
    float deep_merge_tolerance,
    float deep_alpha_merge_tolerance,
    bool has_display_window,
    int display_width,
    int display_height,
    int display_offset_x,
    int display_offset_y,
    int data_offset_x,
    int data_offset_y)>;

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
  void reset(int width,
             int height,
             int max_samples_per_pixel,
             int capture_samples_per_pixel = 0);

  /* Clear deep sample buffers between renders without reallocating. */
  void clear_device_buffers();

  /* Get the deep buffers for kernel access. */
  /* Only valid for single-device renders. */
  DeepRenderBuffers *get_deep_buffers();

  /* Called after rendering completes to finalize and save deep data. */
  bool finalize_deep_output(const std::string &filepath);

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

  /* Set EXR display/data window semantics for overscan delivery. */
  void set_display_window(bool has_display_window,
                          int display_width,
                          int display_height,
                          int display_offset_x,
                          int display_offset_y,
                          int data_offset_x,
                          int data_offset_y);

  /* Set beauty buffer for uniform RGB and alpha normalization.
   * The buffer should contain RGBA floats, size = width * height * 4. */
  void set_beauty_buffer(const float *rgba_buffer, int width, int height);

  /* Set per-pixel sample count buffer for hard-surface deep coverage reconstruction.
   * Blender's Debug Sample Count pass is normalized by the render sample limit, so
   * `sample_count_scale` converts stored values back to absolute per-pixel sample counts. */
  void set_sample_count_buffer(
      const float *sample_count_buffer, int width, int height, float sample_count_scale);

  /* Accumulate deep samples for the current tile into the processed cache. */
  void accumulate_tile(const float *beauty_pixels,
                       const float *sample_count_pixels,
                       float sample_count_scale,
                       int tile_width,
                       int tile_height,
                       int tile_offset_x,
                       int tile_offset_y);

  /* Completed tiles have already consumed their raw samples with resolved Beauty and counts.
   * Late Blender pass capture must not invalidate this cache. */
  bool has_accumulated_tiles() const { return !pixel_written_.empty(); }

  /* Get unmerged processed deep data for compositor storage.
   * Uses std::vector to match Blender render/imbuf API expectations.
   * Returns a newly allocated vector that becomes owned by the caller.
   * Must call set_beauty_buffer first for Deep Recolor. Compositor File Output nodes own their
   * local merge settings, so scene-output merge thresholds must not be applied here. */
  std::vector<std::vector<blender::DeepSample>> *get_unmerged_processed_deep_data();

  /* Release large temporary host-side caches after final deep output has been written or copied
   * into Blender-owned RenderResult structures. Keeps the driver object and device buffers alive
   * until the session/frame teardown finishes. */
  void release_temporary_host_caches();

  /* Get deep output dimensions. */
  int get_width() const { return width_; }
  int get_height() const { return height_; }
  int get_max_samples_per_pixel() const { return max_samples_per_pixel_; }
  int get_capture_samples_per_pixel() const { return capture_samples_per_pixel_; }
  void set_capture_pool_budget(size_t bytes)
  {
    capture_pool_budget_bytes_ = bytes;
  }
  size_t get_capture_pool_budget() const
  {
    return capture_pool_budget_bytes_;
  }

  /* Sync per-device deep buffers and update kernel data with device-specific pointers. */
  void sync_device_buffers(const vector<SliceParams> &slices);
  void update_device_kernel_data(const KernelData &base_data);
  /* Scene/camera uploads overwrite slice-specific constants, even if their values match. */
  void invalidate_device_kernel_data();

  /* Preserve the current progressive sparse-capture batch on the host and reuse the device pool. */
  bool flush_device_capture(bool finalize = false, bool allow_device_profile = false);

 private:
  bool enabled_ = false;
  float merge_threshold_ = 0.001f;
  float alpha_merge_threshold_ = 0.01f;
  int width_ = 0;
  int height_ = 0;
  int max_samples_per_pixel_ = 0;
  int capture_samples_per_pixel_ = 0;
  size_t capture_pool_budget_bytes_ = 0;
  int compression_ = 1; /* ZIPS by default */
  bool use_half_float_ = false;
  bool has_display_window_ = false;
  int display_width_ = 0;
  int display_height_ = 0;
  int display_offset_x_ = 0;
  int display_offset_y_ = 0;
  int data_offset_x_ = 0;
  int data_offset_y_ = 0;

  struct DeepBufferSlice {
    Device *device = nullptr;
    unique_ptr<DeepRenderBuffers> buffers;
    vector<uint8_t> uploaded_kernel_data;
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

  struct DeepBufferSnapshot;
  int capture_samples_for_slice(const SliceParams &slice, int requested) const;

  vector<DeepBufferSlice> device_buffers_;
  Device *device_ = nullptr;

  DeepExrWriteCallback write_callback_;

  /* Beauty buffer for uniform RGB and alpha normalization. */
  vector<float> beauty_buffer_;
  bool use_beauty_buffer_ = false;

  /* Sample count buffer for hard-surface edge coverage reconstruction. */
  vector<float> sample_count_buffer_;
  bool use_sample_count_buffer_ = false;
  float sample_count_scale_ = 1.0f;

  /* Stored as std::vector to match Blender's IMB_exr_save_deep API. */
  unique_ptr<std::vector<std::vector<blender::DeepSample>>> processed_cache_;
  bool deep_buffers_processed_ = false;
  std::atomic<bool> processing_error_{false};
  /* Populated only when accumulate_tile() is used to track written pixels. */
  vector<uint8_t> pixel_written_;

  /* Actual-size raw optical events accumulated across progressive sample batches. */
  unique_ptr<vector<vector<DeepSampleData>>> raw_capture_cache_;
  vector<uint8_t> raw_capture_complete_;

  bool process_device_buffers();
  bool layout_matches(const vector<SliceParams> &slices) const;
  bool build_device_estimates(const vector<SliceParams> &slices,
                              int max_samples,
                              vector<DeviceEstimate> &estimates) const;
  bool check_device_memory(const vector<DeviceEstimate> &estimates) const;
  vector<DeepBufferSnapshot> snapshot_device_buffers();
  void init_device_buffers(const vector<SliceParams> &slices, int max_samples);
  void restore_snapshots(const vector<DeepBufferSnapshot> &snapshots);
  void init_processed_cache();
  void init_raw_capture_cache();
  void merge_raw_capture_region_into_cache(const float *beauty_pixels,
                                           int beauty_width,
                                           int beauty_height,
                                           int beauty_offset_x,
                                           int beauty_offset_y);
  bool merge_device_volume_capture(const float *beauty_pixels,
                                   int beauty_width,
                                   int beauty_height,
                                   int beauty_offset_x,
                                   int beauty_offset_y);
  void merge_slice_into_cache(const DeepBufferSlice &slice,
                              bool track_overlap,
                              vector<uint8_t> &pixel_written,
                              bool &overlap_logged,
                              /* Beauty pass window. */
                              const float *beauty_pixels,
                              int beauty_width,
                              int beauty_height,
                              int beauty_offset_x,
                              int beauty_offset_y);
  void populate_pixel_samples(size_t global_idx,
                              int count,
                              const DeepSampleData *sample_data,
                              size_t offset,
                              bool optical_data_complete);
  void populate_pixel_samples_with_resolved_beauty(size_t global_idx,
                                                   int count,
                                                   const DeepSampleData *sample_data,
                                                   size_t offset,
                                                   bool has_beauty,
                                                   float beauty_r,
                                                   float beauty_g,
                                                   float beauty_b,
                                                   float beauty_a,
                                                   bool optical_data_complete);
  void get_beauty_pixel(size_t global_idx,
                        float &beauty_r,
                        float &beauty_g,
                        float &beauty_b,
                        float &beauty_a) const;
  float get_sample_count_pixel(size_t global_idx) const;
  void compute_scaled_alphas(const DeepSampleData *sample_data,
                             size_t offset,
                             int count,
                             float beauty_a,
                             vector<float> &scaled_alphas,
                             float &beauty_r,
                             float &beauty_g,
                             float &beauty_b);
  bool populate_pure_surface_grouped_samples(size_t global_idx,
                                             int count,
                                             const DeepSampleData *sample_data,
                                             size_t offset,
                                             float beauty_a);
  bool populate_opaque_surface_prefix_samples(size_t global_idx,
                                              int count,
                                              const DeepSampleData *sample_data,
                                              size_t offset,
                                              float beauty_r,
                                              float beauty_g,
                                              float beauty_b,
                                              float beauty_a);

  std::vector<std::vector<blender::DeepSample>> *ensure_processed_cache();
};

CCL_NAMESPACE_END
