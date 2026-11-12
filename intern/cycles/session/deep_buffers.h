/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/device.h"
#include "device/queue.h"
#include "util/types.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Film;
class Integrator;

/* Compute deep buffer bytes for the given dimensions and max samples.
 * Returns false on overflow. */
bool deep_compute_buffer_bytes(int width, int height, int max_samples, size_t &bytes);
bool deep_compute_sparse_buffer_bytes(int width, int height, int average_samples, size_t &bytes);
/* Bound a reusable append pool independently of render geometry. Includes counts/cursor storage;

 * * this is a global pool capacity, never a per-pixel sample truncation rule. */
bool deep_bounded_sparse_capture_samples(
    int width, int height, int requested_samples, size_t budget, int &samples, size_t &bytes);
bool deep_bounded_pool_geometry_requested();

/* Compute effective deep max samples, accounting for volume ray marching settings. */
int deep_effective_max_samples(const Film *film, const Integrator *integrator);
/* Raw event capacity can exceed the final output interval limit for optical profiles. */
int deep_effective_capture_samples(const Film *film,
                                   const Integrator *integrator,
                                   int camera_samples);
int deep_sparse_capture_average_samples(int output_samples,
                                         int camera_samples,
                                         int volume_max_steps);
int deep_volume_capture_batch_samples();

/* -------------------------------------------------------------------- */
/** \name Deep Sample Data Structure
 *
 * Stores a single deep sample with RGBA color and depth information.
 * Each pixel can have multiple samples at different depths.
 * Keep the flags and pack/unpack helpers below in sync with kernel/film/deep_write.h.
 * \{ */

constexpr uint32_t DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA = (1u << 0);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_METADATA = (1u << 1);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED = (1u << 2);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE = (1u << 3);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_PROFILE = (1u << 4);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH = (1u << 5);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE = (1u << 6);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED = (1u << 7);
constexpr uint32_t DEEP_SAMPLE_INFO_FLAG_MASK = 0xffu;
constexpr uint32_t DEEP_SAMPLE_INFO_CAMERA_SAMPLE_SHIFT = 8u;
constexpr uint32_t DEEP_SAMPLE_INFO_CAMERA_SAMPLE_MASK =
    0xffffffffu & ~DEEP_SAMPLE_INFO_FLAG_MASK;
constexpr uint32_t DEEP_SAMPLE_COUNT_OVERFLOW = (1u << 31);
constexpr uint32_t DEEP_SAMPLE_COUNT_MASK = ~DEEP_SAMPLE_COUNT_OVERFLOW;

inline uint32_t deep_sample_count_retained(const uint32_t count)
{
  return count & DEEP_SAMPLE_COUNT_MASK;
}

inline bool deep_sample_count_overflowed(const uint32_t count)
{
  return (count & DEEP_SAMPLE_COUNT_OVERFLOW) != 0;
}

inline uint32_t deep_sample_info_pack(const uint32_t flags, const uint32_t camera_sample)
{
  return (flags & DEEP_SAMPLE_INFO_FLAG_MASK) |
         ((camera_sample << DEEP_SAMPLE_INFO_CAMERA_SAMPLE_SHIFT) &
          DEEP_SAMPLE_INFO_CAMERA_SAMPLE_MASK);
}

inline uint32_t deep_sample_info_flags(const uint32_t info)
{
  return info & DEEP_SAMPLE_INFO_FLAG_MASK;
}

inline uint32_t deep_sample_info_camera_sample(const uint32_t info)
{
  return (info & DEEP_SAMPLE_INFO_CAMERA_SAMPLE_MASK) >> DEEP_SAMPLE_INFO_CAMERA_SAMPLE_SHIFT;
}

/* Must match KernelDeepSample alignment and layout in kernel/film/deep_write.h. */
struct alignas(16) DeepSampleData {
  /* Color channels. */
  float r, g, b, a;
  /* Front depth (ray hit distance from camera). */
  float z;
  /* Back depth (for volumetric samples, equals z for surfaces). */
  float z_back;
  /* Hard-surface metadata used for export-side compaction. */
  uint32_t surface_object;
  uint32_t surface_prim;
  uint32_t surface_shader;
  uint32_t packed_geometric_normal;
  uint32_t flags;
  /* Sparse tile capture owner. Occupies the former alignment tail, so the ABI stays 48 bytes. */
  uint32_t pixel_index;

  DeepSampleData()
      : r(0),
        g(0),
        b(0),
        a(0),
        z(0),
        z_back(0),
        surface_object(0),
        surface_prim(0),
        surface_shader(0),
        packed_geometric_normal(0),
        flags(0),
        pixel_index(0)
  {
  }

  DeepSampleData(float r_,
                 float g_,
                 float b_,
                 float a_,
                 float z_,
                 float z_back_,
                 uint32_t surface_object_ = 0,
                 uint32_t surface_prim_ = 0,
                 uint32_t surface_shader_ = 0,
                 uint32_t packed_geometric_normal_ = 0,
                 uint32_t flags_ = 0,
                 uint32_t pixel_index_ = 0)
      : r(r_),
        g(g_),
        b(b_),
        a(a_),
        z(z_),
        z_back(z_back_),
        surface_object(surface_object_),
        surface_prim(surface_prim_),
        surface_shader(surface_shader_),
        packed_geometric_normal(packed_geometric_normal_),
        flags(flags_),
        pixel_index(pixel_index_)
  {
  }

  /* Compare by depth for sorting. */
  bool operator<(const DeepSampleData &other) const
  {
    return z < other.z;
  }
};

static_assert(sizeof(DeepSampleData) == 48, "DeepSampleData must stay tightly packed.");
static_assert(alignof(DeepSampleData) == 16, "DeepSampleData alignment must match kernel.");

/* Convert an append-only tile event stream into contiguous per-pixel ranges.
 * The sparse stream has no per-pixel capacity: pixel skew only consumes shared tile capacity. */
bool deep_compact_sparse_samples(const DeepSampleData *events,
                                 const uint32_t *event_pixels,
                                 size_t event_count,
                                 size_t pixel_count,
                                 vector<uint32_t> &pixel_offsets,
                                 vector<DeepSampleData> &compacted_events);

/* Remove intervals appended by a GPU shader attempt that stopped on a tiled-image cache miss.
 * Input order must still be the append order; call this before depth sorting. */
bool deep_filter_aborted_volume_attempts(vector<uint32_t> &pixel_offsets,
                                         vector<DeepSampleData> &compacted_events);

/* Validate the complete shared append population before resetting or retaining a capture pool. */
bool deep_validate_sparse_capture_counts(const uint32_t *counts,
                                         size_t pixel_count,
                                         size_t capacity,
                                         size_t &captured);
bool deep_resident_capture_fits(size_t bytes,
                                size_t retained_bytes,
                                size_t budget,
                                size_t free_bytes,
                                size_t headroom);

enum class DeepDeviceCaptureResult { Retained, Readback, Error };

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
  void reset(int width, int height, int max_samples, bool use_sparse_capture = false);

  /* Clear all sample data. */
  void zero();

  /* Copy data from device to host for output. */
  bool copy_from_device();

  /* Retain a complete batch without host event readback. Pool reuse requires a successful result.
   * Readback means ordinary complete host processing is required by device/budget constraints. */
  DeepDeviceCaptureResult retain_device_capture();
  bool copy_retained_device_capture();
  DeepVolumeCaptureResult profile_retained_device_capture(
      const vector<float> &population,
      const vector<float> &beauty,
      int max_profile_samples,
      vector<uint32_t> &profile_offsets,
      vector<uint8_t> &profile_storage);
  bool device_profile_pixel(size_t pixel) const
  {
    return pixel < retained_pixel_selection_.size() && retained_pixel_selection_[pixel] == 0;
  }
  bool device_profile_needs_host_fallback() const;
  bool select_device_profile_window(int x, int y, int width, int height);
  bool has_retained_device_capture() const
  {
    return !retained_device_batches_.empty();
  }
  void discard_retained_device_capture();

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
    return use_sparse_capture_ ? sparse_sample_data_.data() : sample_data_.data();
  }

  bool get_pixel_sample_range(size_t pixel_index,
                              uint32_t &raw_count,
                              const DeepSampleData *&sample_data,
                              size_t &offset) const;

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

  size_t get_global_sample_capacity() const
  {
    return sample_data_.size();
  }

  bool uses_sparse_capture() const
  {
    return use_sparse_capture_;
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
  bool use_sparse_capture_ = false;
  vector<uint32_t> sparse_pixel_offsets_;
  vector<DeepSampleData> sparse_sample_data_;

  struct RetainedDeviceBatch {
    unique_ptr<device_only_memory<DeepSampleData>> events;
    vector<uint32_t> counts;
  };
  unique_ptr<DeviceQueue> retained_queue_;
  vector<RetainedDeviceBatch> retained_device_batches_;
  size_t retained_device_bytes_ = 0;
  size_t retained_processing_reservation_ = 0;
  vector<uint32_t> retained_pixel_selection_;

  bool compact_sparse_capture(size_t retained);

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
