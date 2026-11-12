/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <random>
#include <vector>

#include "testing/testing.h"

#include "device/device.h"
#include "device/queue.h"
#include "session/deep_buffers.h"
#include "integrator/work_tile_scheduler.h"
#include "session/buffers.h"
#include "session/deep_output_driver.h"
#include "kernel/types.h"
#include "session/deep_volume_profile.h"
#include "util/profiling.h"
#include "util/stats.h"

CCL_NAMESPACE_BEGIN

namespace {

float composite_profile_to_depth(const vector<DeepVolumeProfileSample> &profile, const float depth)
{
  float transparency = 1.0f;
  for (const DeepVolumeProfileSample &sample : profile) {
    float alpha = 0.0f;
    if (sample.z_back <= depth) {
      alpha = sample.alpha;
    }
    else if (sample.z < depth && sample.z_back > sample.z) {
      const float fraction = (depth - sample.z) / (sample.z_back - sample.z);
      alpha = 1.0f - std::pow(1.0f - sample.alpha, fraction);
    }
    transparency *= 1.0f - alpha;
  }
  return 1.0f - transparency;
}

float3 composite_profile_rgb_to_depth(const vector<DeepVolumeProfileSample> &profile,
                                      const float depth)
{
  float3 rgb = zero_float3();
  float transparency = 1.0f;
  for (const DeepVolumeProfileSample &sample : profile) {
    float alpha = 0.0f;
    if (sample.z_back <= depth) {
      alpha = sample.alpha;
    }
    else if (sample.z < depth && sample.z_back > sample.z) {
      const float fraction = (depth - sample.z) / (sample.z_back - sample.z);
      alpha = 1.0f - std::pow(1.0f - sample.alpha, fraction);
    }
    const float color_scale = sample.alpha > 1.0e-8f ? alpha / sample.alpha : 0.0f;
    rgb += transparency * sample.rgb * color_scale;
    transparency *= 1.0f - alpha;
  }
  return rgb;
}

DeepSampleData volume_sample(const float alpha,
                             const float z,
                             const float z_back,
                             const uint32_t camera_sample)
{
  return DeepSampleData(0.0f,
                        0.0f,
                        0.0f,
                        alpha,
                        z,
                        z_back,
                        0,
                        0,
                        0,
                        0,
                        deep_sample_info_pack(DEEP_SAMPLE_FLAG_VOLUME_METADATA |
                                                  DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE,
                                              camera_sample));
}

}  // namespace

TEST(DeepSparseCapture, sizes_the_raw_pool_from_camera_samples_and_volume_steps)
{
  EXPECT_EQ(deep_sparse_capture_average_samples(256, 32, 1024), 32800);
  EXPECT_EQ(deep_sparse_capture_average_samples(256, 128, 1024), 131200);
  EXPECT_EQ(deep_sparse_capture_average_samples(256, 128, 8), 1152);
}

TEST(DeepSparseCapture, never_shrinks_below_the_output_capacity)
{
  EXPECT_EQ(deep_sparse_capture_average_samples(4096, 2, 8), 4096);
}

TEST(DeepSparseCapture, saturates_unrepresentable_raw_capacity)
{
  EXPECT_EQ(deep_sparse_capture_average_samples(
                256, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()),
            std::numeric_limits<int>::max());
}

TEST(DeepSparseCapture, rejects_capacity_beyond_kernel_append_index)
{
  size_t bytes = 0;
  EXPECT_TRUE(deep_compute_sparse_buffer_bytes(
      1, 1, std::numeric_limits<int>::max(), bytes));
  EXPECT_FALSE(deep_compute_sparse_buffer_bytes(
      2, 2, std::numeric_limits<int>::max(), bytes));
}

TEST(DeepSparseCapture, bounded_pool_includes_counters_and_never_exceeds_budget)
{
  int samples;
  size_t bytes;
  ASSERT_TRUE(deep_bounded_sparse_capture_samples(
      768, 768, 514, size_t(1024) * 1024 * 1024, samples, bytes));
  EXPECT_EQ(samples, 37);
  EXPECT_LE(bytes, size_t(1024) * 1024 * 1024);
  size_t larger_bytes;
  ASSERT_TRUE(deep_compute_sparse_buffer_bytes(768, 768, samples + 1, larger_bytes));
  EXPECT_GT(larger_bytes, size_t(1024) * 1024 * 1024);
  ASSERT_TRUE(deep_bounded_sparse_capture_samples(4, 1, 64, 212, samples, bytes));
  EXPECT_EQ(samples, 1);
  EXPECT_EQ(bytes, 212);
  EXPECT_FALSE(deep_bounded_sparse_capture_samples(4, 1, 64, 211, samples, bytes));
  EXPECT_EQ(samples, 0);
  EXPECT_EQ(bytes, 0);
}

TEST(DeepSparseCapture, bounded_pool_rejects_invalid_geometry_and_caps_append_indices)
{
  int samples;
  size_t bytes;
  EXPECT_FALSE(deep_bounded_sparse_capture_samples(0, 1, 64, SIZE_MAX, samples, bytes));
  EXPECT_FALSE(deep_bounded_sparse_capture_samples(1, -1, 64, SIZE_MAX, samples, bytes));
  EXPECT_FALSE(
      deep_bounded_sparse_capture_samples(INT_MAX, INT_MAX, 64, SIZE_MAX, samples, bytes));
  ASSERT_TRUE(
      deep_bounded_sparse_capture_samples(65535, 65535, INT_MAX, SIZE_MAX, samples, bytes));
  EXPECT_EQ(samples, 1);
  ASSERT_TRUE(deep_bounded_sparse_capture_samples(2, 2, 64, 1024 * 1024, samples, bytes));
  EXPECT_EQ(samples, 64);
}

TEST(DeepResidentCapture, validates_complete_population_before_pool_reuse)
{
  const uint32_t counts[] = {2, 0, 3, 5};
  size_t captured = 0;
  EXPECT_TRUE(deep_validate_sparse_capture_counts(counts, 3, 5, captured));
  EXPECT_EQ(captured, 5);
  const uint32_t empty[] = {0, 0, 0};
  EXPECT_TRUE(deep_validate_sparse_capture_counts(empty, 2, 5, captured));
  EXPECT_EQ(captured, 0);
}

TEST(DeepResidentCapture, rejects_overflow_and_inconsistent_counts)
{
  size_t captured = 0;
  const uint32_t overflow[] = {1 | DEEP_SAMPLE_COUNT_OVERFLOW, 1};
  EXPECT_FALSE(deep_validate_sparse_capture_counts(overflow, 1, 5, captured));
  const uint32_t over_capacity[] = {6, 6};
  EXPECT_FALSE(deep_validate_sparse_capture_counts(over_capacity, 1, 5, captured));
  const uint32_t missing[] = {1, 2};
  EXPECT_FALSE(deep_validate_sparse_capture_counts(missing, 1, 5, captured));
  const uint32_t excess[] = {2, 1, 2};
  EXPECT_FALSE(deep_validate_sparse_capture_counts(excess, 2, 5, captured));
}

TEST(DeepResidentCapture, bounds_sum_without_integer_wrap)
{
  size_t captured = 0;
  const uint32_t counts[] = {DEEP_SAMPLE_COUNT_MASK, DEEP_SAMPLE_COUNT_MASK, UINT32_MAX};
  EXPECT_FALSE(deep_validate_sparse_capture_counts(counts, 2, UINT32_MAX, captured));
}

TEST(DeepResidentCapture, fits_exact_budget_and_preserves_device_headroom)
{
  EXPECT_TRUE(deep_resident_capture_fits(30, 70, 100, 50, 20));
  EXPECT_FALSE(deep_resident_capture_fits(31, 70, 100, 100, 20));
  EXPECT_FALSE(deep_resident_capture_fits(30, 70, 100, 49, 20));
  EXPECT_FALSE(deep_resident_capture_fits(1, 0, 100, 19, 20));
  EXPECT_FALSE(deep_resident_capture_fits(1, 0, 0, 100, 20));
}

TEST(DeepResidentCapture, rejects_budget_arithmetic_overflow)
{
  const size_t limit = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(deep_resident_capture_fits(20, limit - 10, limit, limit, 0));
  EXPECT_FALSE(deep_resident_capture_fits(0, limit, limit - 1, limit, 0));
  EXPECT_FALSE(deep_resident_capture_fits(limit, 0, limit, limit, 1));
}

TEST(DeepCaptureSorting, preserves_payloads_and_equal_depth_order_across_pixel_ranges)
{
  Stats stats;
  Profiler profiler;
  const vector<DeviceInfo> devices = Device::available_devices(DEVICE_MASK_CPU);
  ASSERT_FALSE(devices.empty());
  unique_ptr<Device> device = Device::create(devices.front(), stats, profiler, true);
  ASSERT_TRUE(device);
  std::mt19937 random(0x108208);
  for (const bool sparse : {false, true}) {
    DeepRenderBuffers buffers(device.get());
    buffers.reset(129, 1, 257, sparse);
    vector<vector<DeepSampleData>> expected(129);
    vector<DeepSampleData> events;
    for (size_t pixel = 0; pixel < 129; pixel++) {
      const size_t count = (pixel * 97) % 258;
      buffers.get_sample_counts_ptr()[pixel] = uint32_t(count);
      for (size_t index = 0; index < count; index++) {
        DeepSampleData event(float(index), float(pixel), -float(index), 0.5f,
                             float((count - index) % 7), float((count - index) % 7) + 0.25f,
                             uint32_t(pixel), uint32_t(index), uint32_t(index * 13),
                             uint32_t(index * 31), DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA,
                             uint32_t(pixel));
        if (sparse) {
          events.push_back(event);
        }
        else {
          buffers.get_sample_data_ptr()[pixel * 257 + index] = event;
          expected[pixel].push_back(event);
        }
      }
    }
    if (sparse) {
      std::shuffle(events.begin(), events.end(), random);
      for (const DeepSampleData &event : events) {
        expected[event.pixel_index].push_back(event);
      }
      std::copy(events.begin(), events.end(), buffers.get_sample_data_ptr());
      buffers.get_sample_counts_ptr()[129] = uint32_t(events.size());
    }
    buffers.copy_to_device();
    ASSERT_TRUE(buffers.copy_from_device());
    buffers.sort_samples_by_depth();
    for (size_t pixel = 0; pixel < 129; pixel++) {
      std::sort(expected[pixel].begin(), expected[pixel].end());
      uint32_t count;
      const DeepSampleData *data;
      size_t offset;
      ASSERT_TRUE(buffers.get_pixel_sample_range(pixel, count, data, offset));
      ASSERT_EQ(count, expected[pixel].size());
      if (count) {
        EXPECT_EQ(std::memcmp(data + offset, expected[pixel].data(), count * sizeof(DeepSampleData)), 0)
            << "sparse=" << sparse << " pixel=" << pixel;
      }
    }
  }
  EXPECT_FALSE(device->have_error()) << device->error_message();
}

/* Run explicitly with CYCLES_TEST_DEEP_GPU_RESIDENT=CUDA or OPTIX and resident capture enabled.
 * Ordinary host test runs do not initialize a GPU or change process environment variables. */
class DeepResidentCaptureGPU : public testing::Test {
 protected:
  Stats stats;
  Profiler profiler;
  unique_ptr<Device> device;

  void SetUp() override
  {
    const char *backend = std::getenv("CYCLES_TEST_DEEP_GPU_RESIDENT");
    if (!backend) {
      GTEST_SKIP() << "GPU capture lifecycle test requires an explicit backend";
    }
    const bool optix = std::strcmp(backend, "OPTIX") == 0;
    ASSERT_TRUE(optix || std::strcmp(backend, "CUDA") == 0);
    const vector<DeviceInfo> devices = Device::available_devices(optix ? DEVICE_MASK_OPTIX :
                                                                        DEVICE_MASK_CUDA);
    ASSERT_FALSE(devices.empty());
    device = Device::create(devices.front(), stats, profiler, true);
    ASSERT_TRUE(device);
    ASSERT_FALSE(device->have_error()) << device->error_message();
    ASSERT_STREQ(std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE"), "1");
  }
};

TEST_F(DeepResidentCaptureGPU, preserves_batches_and_abort_markers_after_pool_overwrite)
{
  const size_t initial_bytes = stats.mem_used;
  {
    DeepRenderBuffers buffers(device.get());
    buffers.reset(2, 1, 16, true);
    const auto fill = [&](const vector<DeepSampleData> &events, uint32_t first, uint32_t second) {
      std::copy(events.begin(), events.end(), buffers.get_sample_data_ptr());
      uint32_t *counts = buffers.get_sample_counts_ptr();
      counts[0] = first;
      counts[1] = second;
      counts[2] = first + second;
      buffers.copy_to_device();
    };
    DeepSampleData aborted = volume_sample(0.5f, 9.0f, 10.0f, 0);
    aborted.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED;
    DeepSampleData successful = volume_sample(0.5f, 1.0f, 2.0f, 0);
    successful.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE;
    DeepSampleData other_pixel = volume_sample(0.5f, 3.0f, 4.0f, 1);
    other_pixel.pixel_index = 1;
    fill({volume_sample(0.5f, 8.0f, 9.0f, 0), other_pixel, aborted, successful}, 3, 1);
    ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
    buffers.zero();
    DeepSampleData next = volume_sample(0.25f, 5.0f, 6.0f, 2);
    next.pixel_index = 1;
    fill({volume_sample(0.25f, 4.0f, 5.0f, 2), next}, 1, 1);
    ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
    buffers.zero();
    fill({volume_sample(0.75f, 7.0f, 8.0f, 3)}, 1, 0);

    ASSERT_TRUE(buffers.copy_retained_device_capture());
    vector<DeepSampleData> samples;
    buffers.get_pixel_samples(0, 0, samples);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples[0].z, 1.0f);
    EXPECT_EQ(samples[0].flags, successful.flags);
    buffers.get_pixel_samples(1, 0, samples);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples[0].z, 3.0f);
    ASSERT_TRUE(buffers.has_retained_device_capture());
    ASSERT_TRUE(buffers.copy_retained_device_capture());
    buffers.get_pixel_samples(0, 0, samples);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples[0].z, 4.0f);
    EXPECT_FALSE(buffers.has_retained_device_capture());
    /* Draining old chunks must not modify the live device pool during a budget spill. */
    ASSERT_TRUE(buffers.copy_from_device());
    buffers.get_pixel_samples(0, 0, samples);
    ASSERT_EQ(samples.size(), 1);
    EXPECT_EQ(samples[0].z, 7.0f);
    ASSERT_FALSE(device->have_error()) << device->error_message();
  }
  EXPECT_EQ(stats.mem_used, initial_bytes);
}

TEST_F(DeepResidentCaptureGPU, releases_pending_batches_on_cancel_reset_and_destruction)
{
  const size_t initial_bytes = stats.mem_used;
  {
    DeepRenderBuffers buffers(device.get());
    const auto retain_one = [&]() {
      buffers.get_sample_data_ptr()[0] = volume_sample(0.5f, 1.0f, 2.0f, 0);
      uint32_t *counts = buffers.get_sample_counts_ptr();
      counts[0] = counts[1] = 1;
      buffers.copy_to_device();
      ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
      EXPECT_TRUE(buffers.has_retained_device_capture());
    };
    buffers.reset(1, 1, 8, true);
    const size_t pool_bytes = stats.mem_used;
    retain_one();
    buffers.discard_retained_device_capture();
    EXPECT_FALSE(buffers.has_retained_device_capture());
    EXPECT_EQ(stats.mem_used, pool_bytes);
    buffers.zero();
    retain_one();
    buffers.reset(1, 1, 8, true);
    EXPECT_FALSE(buffers.has_retained_device_capture());
    EXPECT_EQ(stats.mem_used, pool_bytes);
    retain_one();
  }
  EXPECT_EQ(stats.mem_used, initial_bytes);
}

TEST_F(DeepResidentCaptureGPU, rejects_per_pixel_ownership_mismatch)
{
  DeepRenderBuffers buffers(device.get());
  buffers.reset(2, 1, 8, true);
  buffers.get_sample_data_ptr()[0] = volume_sample(0.5f, 1.0f, 2.0f, 0);
  buffers.get_sample_data_ptr()[1] = volume_sample(0.5f, 2.0f, 3.0f, 1);
  /* Both raw events belong to pixel zero, but the count stream claims one per pixel.
   * The append cursor and total per-pixel population still agree. */
  uint32_t *counts = buffers.get_sample_counts_ptr();
  counts[0] = counts[1] = 1;
  counts[2] = 2;
  buffers.copy_to_device();
  ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
  EXPECT_FALSE(buffers.copy_retained_device_capture());
}

TEST_F(DeepResidentCaptureGPU, rejects_overflow_before_retaining_a_partial_batch)
{
  DeepRenderBuffers buffers(device.get());
  buffers.reset(1, 1, 8, true);
  uint32_t *counts = buffers.get_sample_counts_ptr();
  counts[0] = 1 | DEEP_SAMPLE_COUNT_OVERFLOW;
  counts[1] = 1;
  buffers.copy_to_device();
  EXPECT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Error);
  EXPECT_FALSE(buffers.has_retained_device_capture());
  EXPECT_TRUE(device->have_error());
}

TEST_F(DeepResidentCaptureGPU, raw_processing_matches_host_across_sort_boundaries)
{
  if (!std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_PROCESS")) {
    GTEST_SKIP() << "Explicit GPU raw processing is required";
  }
  std::mt19937 random(0x810828);
  DeepRenderBuffers buffers(device.get());
  const uint32_t boundaries[] = {1, 255, 256, 257, 1023, 1024, 1025, 2047, 2048, 2049, 8197};
  for (int trial = 0; trial < 120; trial++) {
    const uint32_t pixels = 1 + random() % 33;
    const uint32_t n = trial < 11 ? boundaries[trial] : 1 + random() % 10000;
    vector<DeepSampleData> events;
    vector<uint32_t> owners, counts(pixels + 1, 0), offsets;
    for (uint32_t i = 0; i < n; i++) {
      DeepSampleData event = volume_sample(0.25f, float(i), float(i + 1), random() % (trial % 2 ? 1000000 : 12));
      event.pixel_index = random() % pixels;
      const uint32_t kind = random() % 12;
      if (kind == 0) event.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE;
      if (kind == 1) event.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED;
      if (kind == 2) event.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE |
                                  DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED;
      if (kind == 3) event.flags = DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA;
      event.packed_geometric_normal = i; /* Distinct identities even when other payloads match. */
      owners.push_back(event.pixel_index);
      counts[event.pixel_index]++;
      events.push_back(event);
    }
    counts.back() = n;
    vector<DeepSampleData> expected;
    ASSERT_TRUE(deep_compact_sparse_samples(events.data(), owners.data(), n, pixels, offsets, expected));
    ASSERT_TRUE(deep_filter_aborted_volume_attempts(offsets, expected));
    buffers.reset(int(pixels), 1, int((n + pixels - 1) / pixels), true);
    std::copy(events.begin(), events.end(), buffers.get_sample_data_ptr());
    std::copy(counts.begin(), counts.end(), buffers.get_sample_counts_ptr());
    buffers.copy_to_device();
    ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
    buffers.zero();
    ASSERT_TRUE(buffers.copy_retained_device_capture()) << device->error_message();
    for (uint32_t p = 0; p < pixels; p++) {
      vector<DeepSampleData> actual;
      buffers.get_pixel_samples(int(p), 0, actual);
      const uint32_t count = offsets[p + 1] - offsets[p];
      ASSERT_EQ(actual.size(), count) << "trial=" << trial << ", pixel=" << p;
      if (count) EXPECT_EQ(std::memcmp(actual.data(), expected.data() + offsets[p], count * 48), 0);
    }
    if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_SELECTED_COMPACT")) {
      device_vector<DeepSampleData> raw(device.get(), "deep_selected_fixture", MEM_READ_WRITE);
      raw.alloc(n); std::copy(events.begin(), events.end(), raw.data()); raw.copy_to_device();
      unique_ptr<DeviceQueue> selected_queue = device->gpu_queue_create();
      vector<uint32_t> selection(pixels);
      for (uint32_t p = 0; p < pixels; p++)
        selection[p] = trial % 3 == 0 ? 0 : trial % 3 == 1 ? 1 : random() % 2;
      vector<uint32_t> selected_offsets;
      vector<uint8_t> selected_storage;
      ASSERT_TRUE(selected_queue->process_deep_capture(raw, counts, selected_offsets,
                                                       selected_storage, &selection))
          << device->error_message();
      for (uint32_t p = 0; p < pixels; p++) {
        const uint32_t count = selection[p] ? offsets[p + 1] - offsets[p] : 0;
        ASSERT_EQ(selected_offsets[p + 1] - selected_offsets[p], count)
            << "trial=" << trial << ", selected pixel=" << p;
        if (count)
          EXPECT_EQ(std::memcmp(selected_storage.data() + size_t(selected_offsets[p]) * 48,
                                expected.data() + offsets[p], size_t(count) * 48), 0);
      }
      raw.copy_from_device();
      EXPECT_EQ(std::memcmp(raw.data(), events.data(), size_t(n) * 48), 0);
    }
    const size_t with_scratch = stats.mem_used;
    buffers.discard_retained_device_capture();
    EXPECT_LT(stats.mem_used, with_scratch); /* Queue-owned processing scratch is released. */
  }
}

TEST_F(DeepResidentCaptureGPU, volume_profile_preserves_prefix_and_selects_complete_host_fallback)
{
  if (!getenv("CYCLES_TEST_DEEP_GPU_VOLUME")) {
    GTEST_SKIP() << "Explicit device volume module test is required";
  }
  const size_t initial_bytes = stats.mem_used;
  {
    const uint32_t pixels = 13;
    vector<DeepSampleData> batches[2], per_pixel[pixels];
    vector<vector<uint32_t>> counts(2, vector<uint32_t>(pixels + 1, 0));
    for (uint32_t p = 0; p < pixels; p++) {
      if (p >= 10) {
        /* The aborted camera has no retained ray-map slot. Pixel 12 additionally retries
         * camera zero successfully; the discarded prefix must not affect that profile. */
        DeepSampleData aborted = volume_sample(0.9f, 9.0f, 10.0f, p == 12 ? 0 : 8);
        aborted.pixel_index = p;
        aborted.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED;
        batches[0].push_back(aborted);
        counts[0][p]++;
        counts[0].back()++;
        if (p == 11) continue;
      }
      const uint32_t rays = p == 1 || p == 2 ? 1 : 8;
      const uint32_t segments = p == 2 ? 4 : 80;
      for (uint32_t ray = 0; ray < rays; ray++) {
        for (uint32_t s = 0; s < segments; s++) {
          const float front = 2.0f + float(ray) * 0.01f;
          DeepSampleData event = volume_sample(0.02f, front + float(s) * 0.025f,
                                              front + float(s + 1) * 0.025f, ray);
          event.pixel_index = p;
          event.flags |= DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
          event.surface_object = __float_as_uint(0.01f);
          event.surface_prim = __float_as_uint(0.02f);
          event.surface_shader = __float_as_uint(0.03f);
          if (p == 12 && s == segments - 1)
            event.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE;
          if (p == 3) event.flags |= DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA;
          if (p == 4) event.r = 0.01f;
          if (p == 5) { event.z = -2; event.z_back = -1; }
          if (p == 6) event.flags &= ~DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE;
          if (p == 7) event.flags = deep_sample_info_pack(deep_sample_info_flags(event.flags), 10000);
          if (p == 8 && s == 1) event.z = front;
          if (p == 9) event.z_back = event.z;
          const uint32_t batch = ray < 4 ? 0 : 1;
          batches[batch].push_back(event);
          counts[batch][p]++;
          counts[batch].back()++;
          per_pixel[p].push_back(event);
        }
      }
    }
    device_vector<DeepSampleData> first(device.get(), "deep_volume_fixture_first", MEM_READ_WRITE);
    device_vector<DeepSampleData> second(device.get(), "deep_volume_fixture_second", MEM_READ_WRITE);
    first.alloc(batches[0].size()); second.alloc(batches[1].size());
    std::copy(batches[0].begin(), batches[0].end(), first.data());
    std::copy(batches[1].begin(), batches[1].end(), second.data());
    first.copy_to_device(); second.copy_to_device();
    unique_ptr<DeviceQueue> queue = device->gpu_queue_create();
    ASSERT_TRUE(queue->supports_deep_volume_capture());
    vector<float> population(pixels, 12.0f), beauty(pixels * 4, 0.2f);
    vector<uint32_t> offsets, fallback;
    vector<uint8_t> storage;
    const vector<device_memory *> views = {&first, &second};
    ASSERT_EQ(queue->process_deep_volume_capture(views, counts, population, beauty, 64,
                                               offsets, storage, fallback),
              DeepVolumeCaptureResult::Complete) << device->error_message();
    ASSERT_EQ(offsets.size(), pixels + 1);
    ASSERT_EQ(fallback.size(), pixels);
    ASSERT_EQ(storage.size(), size_t(offsets.back()) * 24);
    if (getenv("CYCLES_TEST_DEEP_GPU_PREFIX_COMPARE")) {
      /* Force one pixel per chunk, then compare to the unchanged full workspace path. */
      const char *cache_flag = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE");
      const bool compare_cache = cache_flag && std::strcmp(cache_flag, "1") == 0;
      const char *groups_flag = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS");
      const bool compare_groups = groups_flag && std::strcmp(groups_flag, "1") == 0;
      const char *chunk_flag = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS");
      ASSERT_TRUE(chunk_flag && std::strcmp(chunk_flag, "1") == 0);
      ASSERT_STREQ(getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNK_PIXELS"), "1");
#ifdef _WIN32
      ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS", "0"), 0);
      if (compare_cache) ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE", "0"), 0);
      if (compare_groups) ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS", "0"), 0);
#else
      ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS", "0", 1), 0);
      if (compare_cache) ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE", "0", 1), 0);
      if (compare_groups) ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS", "0", 1), 0);
#endif
      vector<uint32_t> original_offsets, original_fallback;
      vector<uint8_t> original_storage;
      const auto result = queue->process_deep_volume_capture(
          views, counts, population, beauty, 64, original_offsets, original_storage, original_fallback);
#ifdef _WIN32
      ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS", "1"), 0);
      if (compare_cache) ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE", "1"), 0);
      if (compare_groups) ASSERT_EQ(_putenv_s("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS", "1"), 0);
#else
      ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREFIX_CHUNKS", "1", 1), 0);
      if (compare_cache) ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PREPARED_CACHE", "1", 1), 0);
      if (compare_groups) ASSERT_EQ(setenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS", "1", 1), 0);
#endif
      ASSERT_EQ(result, DeepVolumeCaptureResult::Complete) << device->error_message();
      EXPECT_EQ(offsets, original_offsets);
      EXPECT_EQ(fallback, original_fallback);
      EXPECT_EQ(storage, original_storage);
    }
    struct PackedProfile { float r, g, b, a, z, z_back; };
    for (uint32_t p = 0; p < pixels; p++) {
      if (p >= 2 && p < 10) {
        EXPECT_EQ(fallback[p], 1u) << p;
        EXPECT_EQ(offsets[p + 1], offsets[p]);
        continue;
      }
      EXPECT_EQ(fallback[p], 0u);
      if (p == 11) {
        EXPECT_EQ(offsets[p + 1], offsets[p]);
        continue;
      }
      std::sort(per_pixel[p].begin(), per_pixel[p].end());
      vector<DeepVolumeProfileSample> expected, actual;
      ASSERT_TRUE(deep_build_volume_profile(per_pixel[p].data(), 0, int(per_pixel[p].size()),
                                            12, 64, make_float3(0.2f), 0.2f, expected));
      for (uint32_t i = offsets[p]; i < offsets[p + 1]; i++) {
        PackedProfile sample;
        std::memcpy(&sample, storage.data() + size_t(i) * 24, 24);
        actual.push_back({make_float3(sample.r, sample.g, sample.b), sample.a, sample.z, sample.z_back});
      }
      ASSERT_FALSE(actual.empty());
      for (int cut = 0; cut < 401; cut++) {
        const float depth = 1.0f + float(cut) * 0.015f;
        EXPECT_NEAR(composite_profile_to_depth(expected, depth),
                    composite_profile_to_depth(actual, depth), 1e-6f);
        const float3 expected_rgb = composite_profile_rgb_to_depth(expected, depth);
        const float3 actual_rgb = composite_profile_rgb_to_depth(actual, depth);
        EXPECT_NEAR(expected_rgb.x, actual_rgb.x, 1e-6f);
        EXPECT_NEAR(expected_rgb.y, actual_rgb.y, 1e-6f);
        EXPECT_NEAR(expected_rgb.z, actual_rgb.z, 1e-6f);
      }
    }
    for (int batch = 0; batch < 2; batch++) {
      ASSERT_TRUE(queue->process_deep_capture(*views[batch], counts[batch], offsets, storage,
                                             &fallback)) << device->error_message();
      for (uint32_t p = 0; p < pixels; p++) {
        vector<DeepSampleData> expected;
        if (fallback[p]) {
          for (const DeepSampleData &event : batches[batch]) {
            if (event.pixel_index == p) expected.push_back(event);
          }
        }
        ASSERT_EQ(offsets[p + 1] - offsets[p], expected.size());
        if (!expected.empty()) {
          EXPECT_EQ(std::memcmp(storage.data() + size_t(offsets[p]) * 48,
                                expected.data(), expected.size() * 48), 0);
        }
      }
    }
    ASSERT_FALSE(device->have_error()) << device->error_message();
  }
  EXPECT_EQ(stats.mem_used, initial_bytes);
}

TEST_F(DeepResidentCaptureGPU, volume_profile_window_preserves_interior_fallback)
{
  if (!getenv("CYCLES_TEST_DEEP_GPU_VOLUME")) {
    GTEST_SKIP() << "Explicit device volume module test is required";
  }
  for (int window_x : {0, 1}) {
    DeepRenderBuffers buffers(device.get());
    buffers.reset(2, 1, 640, true);
    size_t cursor = 0;
    for (uint32_t p = 0; p < 2; p++) {
      for (uint32_t ray = 0; ray < 8; ray++) {
        for (uint32_t step = 0; step < 80; step++) {
          const float front = 2.0f + float(ray) * 0.01f;
          auto event = volume_sample(0.02f, front + float(step) * 0.025f,
                                      front + float(step + 1) * 0.025f, ray);
          event.pixel_index = p;
          event.flags |= DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
          event.surface_object = __float_as_uint(0.01f);
          event.surface_prim = __float_as_uint(0.02f);
          event.surface_shader = __float_as_uint(0.03f);
          if (p == 1) event.flags |= DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA;
          buffers.get_sample_data_ptr()[cursor++] = event;
        }
      }
      buffers.get_sample_counts_ptr()[p] = 640;
    }
    buffers.get_sample_counts_ptr()[2] = uint32_t(cursor);
    buffers.copy_to_device();
    ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
    vector<uint32_t> offsets;
    vector<uint8_t> storage;
    ASSERT_EQ(buffers.profile_retained_device_capture({12.0f, 12.0f},
                                                      vector<float>(8, 0.2f), 64,
                                                      offsets, storage),
              DeepVolumeCaptureResult::Complete) << device->error_message();
    ASSERT_TRUE(buffers.device_profile_pixel(0));
    ASSERT_FALSE(buffers.device_profile_pixel(1));
    ASSERT_TRUE(buffers.device_profile_needs_host_fallback());
    const auto before_offsets = offsets;
    const auto before_storage = storage;
    ASSERT_TRUE(buffers.select_device_profile_window(window_x, 0, 1, 1));
    EXPECT_EQ(buffers.device_profile_needs_host_fallback(), window_x == 1);
    EXPECT_EQ(offsets, before_offsets);
    EXPECT_EQ(storage, before_storage);
    if (window_x == 1) {
      ASSERT_TRUE(buffers.copy_retained_device_capture());
      uint32_t count;
      const DeepSampleData *events;
      size_t offset;
      ASSERT_TRUE(buffers.get_pixel_sample_range(1, count, events, offset));
      EXPECT_EQ(deep_sample_count_retained(count), 640);
    }
    ASSERT_FALSE(device->have_error()) << device->error_message();
  }
}

TEST_F(DeepResidentCaptureGPU, volume_profile_with_only_aborted_groups_has_no_active_rays)
{
  if (!getenv("CYCLES_TEST_DEEP_GPU_VOLUME")) {
    GTEST_SKIP() << "Explicit device volume module test is required";
  }
  const size_t initial_bytes = stats.mem_used;
  {
    vector<DeepSampleData> events;
    vector<vector<uint32_t>> counts(1, vector<uint32_t>{2, 2, 4});
    for (uint32_t p = 0; p < 2; p++) {
      for (uint32_t camera = 0; camera < 2; camera++) {
        DeepSampleData aborted = volume_sample(0.9f, 2.0f, 3.0f, camera);
        aborted.pixel_index = p;
        aborted.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED;
        events.push_back(aborted);
      }
    }
    device_vector<DeepSampleData> raw(device.get(), "deep_volume_aborted_fixture", MEM_READ_WRITE);
    raw.alloc(events.size());
    std::copy(events.begin(), events.end(), raw.data());
    raw.copy_to_device();
    unique_ptr<DeviceQueue> queue = device->gpu_queue_create();
    const vector<device_memory *> batches = {&raw};
    vector<uint32_t> offsets, fallback;
    vector<uint8_t> storage;
    ASSERT_EQ(queue->process_deep_volume_capture(batches, counts, vector<float>(2, 2.0f),
                                               vector<float>(8, 0.0f), 64,
                                               offsets, storage, fallback),
              DeepVolumeCaptureResult::Complete) << device->error_message();
    EXPECT_EQ(offsets, (vector<uint32_t>{0, 0, 0}));
    EXPECT_EQ(fallback, (vector<uint32_t>{0, 0}));
    EXPECT_TRUE(storage.empty());
    ASSERT_FALSE(device->have_error()) << device->error_message();
  }
  EXPECT_EQ(stats.mem_used, initial_bytes);
}

TEST_F(DeepResidentCaptureGPU, volume_profile_covers_one_and_256_interval_limits)
{
  if (!getenv("CYCLES_TEST_DEEP_GPU_VOLUME"))
    GTEST_SKIP() << "Explicit device volume module test is required";
  vector<DeepSampleData> events;
  for (uint32_t ray = 0; ray < 4; ray++) {
    const float front = 2.0f + float(ray) * 0.002f;
    for (uint32_t s = 0; s < 300; s++) {
      auto event = volume_sample(0.001f, front + float(s) * 0.01f,
                                front + float(s + 1) * 0.01f, ray);
      event.flags |= DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
      event.surface_object = event.surface_prim = event.surface_shader = __float_as_uint(0.001f);
      if (s == 299) event.flags |= DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE;
      events.push_back(event);
    }
  }
  device_vector<DeepSampleData> raw(device.get(), "deep_volume_interval_edges", MEM_READ_WRITE);
  raw.alloc(events.size()); std::copy(events.begin(), events.end(), raw.data()); raw.copy_to_device();
  unique_ptr<DeviceQueue> queue = device->gpu_queue_create();
  std::sort(events.begin(), events.end());
  for (const int limit : {1, 256}) {
    vector<DeepVolumeProfileSample> expected, actual;
    ASSERT_TRUE(deep_build_volume_profile(events.data(), 0, int(events.size()), 8, limit,
                                         make_float3(0.2f), 0.2f, expected));
    vector<uint32_t> offsets, fallback;
    vector<uint8_t> storage;
    ASSERT_EQ(queue->process_deep_volume_capture({&raw}, {{1200, 1200}}, {8.0f},
                                               vector<float>(4, 0.2f), limit, offsets,
                                               storage, fallback), DeepVolumeCaptureResult::Complete);
    ASSERT_EQ(fallback, (vector<uint32_t>{0}));
    ASSERT_EQ(offsets, (vector<uint32_t>{0, uint32_t(limit)}));
    struct Packed { float r, g, b, a, z, back; };
    for (uint32_t i = 0; i < offsets.back(); i++) {
      Packed sample; std::memcpy(&sample, storage.data() + size_t(i) * 24, 24);
      actual.push_back({make_float3(sample.r, sample.g, sample.b), sample.a, sample.z, sample.back});
    }
    for (int i = 0; i <= 401; i++) {
      const float cut = 1.0f + float(i) * 0.0125f;
      EXPECT_NEAR(composite_profile_to_depth(expected, cut),
                  composite_profile_to_depth(actual, cut), 1e-6f);
      const auto a = composite_profile_rgb_to_depth(actual, cut);
      const auto b = composite_profile_rgb_to_depth(expected, cut);
      EXPECT_NEAR(a.x, b.x, 1e-6f); EXPECT_NEAR(a.y, b.y, 1e-6f); EXPECT_NEAR(a.z, b.z, 1e-6f);
    }
  }
}

TEST_F(DeepResidentCaptureGPU, captured_volume_profile_preserves_prefix_across_repeated_layouts)
{
  const char *stem = getenv("CYCLES_TEST_DEEP_VOLUME_REPLAY");
  if (!stem || !getenv("CYCLES_TEST_DEEP_GPU_VOLUME")) {
    GTEST_SKIP() << "Explicit captured pixel stem and device volume module are required";
  }
  float metadata[6];
  std::ifstream metadata_file(string(stem) + "-metadata.bin", std::ios::binary);
  metadata_file.read(reinterpret_cast<char *>(metadata), sizeof(metadata));
  ASSERT_TRUE(metadata_file);
  ASSERT_EQ(metadata_file.peek(), std::char_traits<char>::eof());
  vector<vector<DeepSampleData>> input;
  vector<DeepSampleData> ordered;
  for (int batch = 0;; batch++) {
    std::ifstream file(string(stem) + "-batch-" + std::to_string(batch) + ".bin",
                       std::ios::binary | std::ios::ate);
    if (!file) break;
    const auto bytes = file.tellg();
    ASSERT_GE(bytes, 0);
    ASSERT_EQ(size_t(bytes) % sizeof(DeepSampleData), 0);
    vector<DeepSampleData> events(size_t(bytes) / sizeof(DeepSampleData));
    file.seekg(0);
    if (bytes) file.read(reinterpret_cast<char *>(events.data()), bytes);
    ASSERT_TRUE(file);
    if (events.empty()) continue;
    input.push_back(events);
    std::sort(events.begin(), events.end());
    ordered.insert(ordered.end(), events.begin(), events.end());
  }
  ASSERT_FALSE(input.empty());
  std::sort(ordered.begin(), ordered.end());
  vector<DeepVolumeProfileSample> expected;
  ASSERT_TRUE(deep_build_volume_profile(ordered.data(), 0, int(ordered.size()),
                                        int(metadata[0]), int(metadata[5]),
                                        make_float3(metadata[1], metadata[2], metadata[3]),
                                        metadata[4], expected));
  const auto prefix = [](const vector<DeepVolumeProfileSample> &profile, double depth) {
    double transparency = 1.0;
    for (const auto &sample : profile) {
      if (depth <= sample.z) continue;
      double alpha = std::clamp(double(sample.alpha), 0.0, 1.0);
      if (depth < sample.z_back)
        alpha = 1.0 - std::pow(1.0 - alpha,
                              (depth - sample.z) / (sample.z_back - sample.z));
      transparency *= 1.0 - alpha;
    }
    return 1.0 - transparency;
  };
  // Replication changes jagged offsets and sort-block boundaries without another scene render.
  for (const uint32_t pixels : {1u, 3u, 8u}) {
    vector<unique_ptr<device_vector<DeepSampleData>>> owners;
    vector<device_memory *> views;
    vector<vector<uint32_t>> counts;
    for (const auto &batch : input) {
      auto raw = make_unique<device_vector<DeepSampleData>>(device.get(),
                                                         "deep_volume_pixel_replay", MEM_READ_WRITE);
      raw->alloc(batch.size() * pixels);
      size_t cursor = 0;
      for (uint32_t p = 0; p < pixels; p++) {
        for (auto event : batch) {
          event.pixel_index = p;
          raw->data()[cursor++] = event;
        }
      }
      raw->copy_to_device();
      counts.push_back(vector<uint32_t>(pixels + 1, uint32_t(batch.size())));
      counts.back().back() = uint32_t(cursor);
      views.push_back(raw.get());
      owners.push_back(std::move(raw));
    }
    vector<float> population(pixels, metadata[0]), beauty(pixels * 4);
    for (uint32_t p = 0; p < pixels; p++)
      for (int c = 0; c < 4; c++) beauty[p * 4 + c] = metadata[c + 1];
    unique_ptr<DeviceQueue> queue = device->gpu_queue_create();
    for (int repeat = 0; repeat < 8; repeat++) {
      vector<uint32_t> offsets, fallback;
      vector<uint8_t> storage;
      ASSERT_EQ(queue->process_deep_volume_capture(views, counts, population, beauty,
                                                 int(metadata[5]), offsets, storage, fallback),
                DeepVolumeCaptureResult::Complete) << device->error_message();
      ASSERT_EQ(offsets.size(), pixels + 1);
      ASSERT_EQ(storage.size(), size_t(offsets.back()) * 24);
      struct Packed { float r, g, b, a, z, back; };
      for (uint32_t p = 0; p < pixels; p++) {
        ASSERT_EQ(fallback[p], 0u) << "pixels=" << pixels << " repeat=" << repeat;
        vector<DeepVolumeProfileSample> actual;
        for (uint32_t i = offsets[p]; i < offsets[p + 1]; i++) {
          Packed sample;
          std::memcpy(&sample, storage.data() + size_t(i) * 24, 24);
          actual.push_back({make_float3(sample.r, sample.g, sample.b), sample.a,
                            sample.z, sample.back});
        }
        ASSERT_FALSE(actual.empty());
        double maximum = 0;
        for (int cut = 0; cut <= 1000; cut++) {
          const double depth = expected.front().z +
                               (expected.back().z_back - expected.front().z) * (cut / 1000.0);
          maximum = std::max(maximum, std::abs(prefix(actual, depth) - prefix(expected, depth)));
        }
        ASSERT_LE(maximum, 1e-6) << "pixels=" << pixels << " repeat=" << repeat << " pixel=" << p;
      }
    }
  }
}

TEST_F(DeepResidentCaptureGPU, captured_volume_frame_preserves_prefix_with_heterogeneous_grids)
{
  const char *stem = getenv("CYCLES_TEST_DEEP_VOLUME_FRAME_REPLAY");
  if (!stem || !getenv("CYCLES_TEST_DEEP_GPU_VOLUME")) {
    GTEST_SKIP() << "Explicit frame stem and device volume module are required";
  }
  const auto read = [](const string &path, void *data, size_t bytes) {
    std::ifstream file(path, std::ios::binary);
    if (bytes) file.read(reinterpret_cast<char *>(data), bytes);
    return bool(file) && file.peek() == std::char_traits<char>::eof();
  };
  uint32_t header[6];
  ASSERT_TRUE(read(string(stem) + "-header.bin", header, sizeof(header)));
  ASSERT_EQ(header[0], 0x44564631u);
  ASSERT_EQ(header[1], 1u);
  const uint32_t pixels = getenv("CYCLES_TEST_DEEP_VOLUME_FRAME_PIXELS") ?
                              std::min(header[2], uint32_t(std::strtoul(
                                  getenv("CYCLES_TEST_DEEP_VOLUME_FRAME_PIXELS"), nullptr, 10))) :
                              std::min(header[2], 64u);
  ASSERT_GT(pixels, 0u);
  vector<float> population(header[2]), beauty(size_t(header[2]) * 4);
  ASSERT_TRUE(read(string(stem) + "-population.bin", population.data(), population.size() * 4));
  ASSERT_TRUE(read(string(stem) + "-beauty.bin", beauty.data(), beauty.size() * 4));
  population.resize(pixels); beauty.resize(size_t(pixels) * 4);
  vector<unique_ptr<device_vector<DeepSampleData>>> owners;
  vector<device_memory *> views;
  vector<vector<uint32_t>> counts;
  vector<vector<DeepSampleData>> expected_raw(pixels);
  for (uint32_t batch = 0; batch < header[5]; batch++) {
    const string base = string(stem) + "-batch-" + std::to_string(batch);
    vector<uint32_t> original_counts(header[2] + 1);
    ASSERT_TRUE(read(base + "-counts.bin", original_counts.data(), original_counts.size() * 4));
    vector<DeepSampleData> original(original_counts.back()), events;
    ASSERT_TRUE(read(base + ".bin", original.data(), original.size() * sizeof(DeepSampleData)));
    vector<uint32_t> batch_counts(pixels + 1, 0), pixel_owners, offsets;
    for (const auto &event : original) {
      if (event.pixel_index >= pixels) continue;
      events.push_back(event); pixel_owners.push_back(event.pixel_index);
      batch_counts[event.pixel_index]++; batch_counts.back()++;
    }
    if (events.empty()) continue;
    vector<DeepSampleData> compacted;
    ASSERT_TRUE(deep_compact_sparse_samples(events.data(), pixel_owners.data(), events.size(),
                                          pixels, offsets, compacted));
    ASSERT_TRUE(deep_filter_aborted_volume_attempts(offsets, compacted));
    for (uint32_t p = 0; p < pixels; p++) {
      std::sort(compacted.begin() + offsets[p], compacted.begin() + offsets[p + 1]);
      expected_raw[p].insert(expected_raw[p].end(), compacted.begin() + offsets[p],
                             compacted.begin() + offsets[p + 1]);
    }
    auto raw = make_unique<device_vector<DeepSampleData>>(device.get(),
                                                       "deep_volume_frame_replay", MEM_READ_WRITE);
    raw->alloc(events.size());
    std::copy(events.begin(), events.end(), raw->data()); raw->copy_to_device();
    views.push_back(raw.get()); owners.push_back(std::move(raw));
    counts.push_back(std::move(batch_counts));
  }
  unique_ptr<DeviceQueue> queue = device->gpu_queue_create();
  vector<uint32_t> offsets, fallback;
  vector<uint8_t> storage;
  ASSERT_EQ(queue->process_deep_volume_capture(views, counts, population, beauty, int(header[4]),
                                             offsets, storage, fallback),
            DeepVolumeCaptureResult::Complete) << device->error_message();
  ASSERT_EQ(storage.size(), size_t(offsets.back()) * 24);
  const auto prefix = [](const vector<DeepVolumeProfileSample> &profile, double depth) {
    double t = 1;
    for (const auto &sample : profile) {
      if (depth <= sample.z) continue;
      double alpha = std::clamp(double(sample.alpha), 0.0, 1.0);
      if (depth < sample.z_back)
        alpha = 1 - std::pow(1 - alpha, (depth - sample.z) / (sample.z_back - sample.z));
      t *= 1 - alpha;
    }
    return 1 - t;
  };
  uint32_t checked = 0;
  for (uint32_t p = 0; p < pixels; p++) {
    if (fallback[p] || expected_raw[p].empty()) continue;
    std::sort(expected_raw[p].begin(), expected_raw[p].end());
    vector<DeepVolumeProfileSample> expected, actual;
    ASSERT_TRUE(deep_build_volume_profile(expected_raw[p].data(), 0, int(expected_raw[p].size()),
                                         int(population[p]), int(header[4]),
                                         make_float3(beauty[p * 4], beauty[p * 4 + 1], beauty[p * 4 + 2]),
                                         beauty[p * 4 + 3], expected));
    struct Packed { float r, g, b, a, z, back; };
    for (uint32_t i = offsets[p]; i < offsets[p + 1]; i++) {
      Packed sample; std::memcpy(&sample, storage.data() + size_t(i) * 24, 24);
      actual.push_back({make_float3(sample.r, sample.g, sample.b), sample.a, sample.z, sample.back});
    }
    ASSERT_FALSE(actual.empty());
    double maximum = 0;
    for (int cut = 0; cut <= 1000; cut++) {
      const double depth = expected.front().z +
                           (expected.back().z_back - expected.front().z) * (cut / 1000.0);
      maximum = std::max(maximum, std::abs(prefix(actual, depth) - prefix(expected, depth)));
    }
    ASSERT_LE(maximum, 1e-6) << "pixels=" << pixels << " pixel=" << p;
    checked++;
  }
  ASSERT_GT(checked, 0u);
}

TEST_F(DeepResidentCaptureGPU, raw_processing_matches_captured_producer_batch)
{
  const char *path = std::getenv("CYCLES_TEST_DEEP_RAW_REPLAY");
  if (!path || !std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_PROCESS")) {
    GTEST_SKIP() << "Explicit captured raw batch and GPU processing are required";
  }
  std::ifstream file(path, std::ios::binary);
  uint32_t header[6];
  file.read(reinterpret_cast<char *>(header), sizeof(header));
  ASSERT_TRUE(file);
  ASSERT_EQ(header[0], 0x44525031u);
  ASSERT_EQ(header[1], 1);
  ASSERT_EQ(uint64_t(header[2]) * header[3], header[4]);
  ASSERT_GT(header[4], 0);
  ASSERT_LT(header[5], uint32_t(std::numeric_limits<int>::max()));
  vector<uint32_t> counts(header[4] + 1), owners, offsets;
  vector<DeepSampleData> events(header[5]), expected;
  file.read(reinterpret_cast<char *>(counts.data()), counts.size() * 4);
  file.read(reinterpret_cast<char *>(events.data()), events.size() * 48);
  ASSERT_TRUE(file);
  ASSERT_EQ(file.peek(), std::char_traits<char>::eof());
  for (const DeepSampleData &event : events) owners.push_back(event.pixel_index);
  ASSERT_TRUE(deep_compact_sparse_samples(events.data(), owners.data(), events.size(), header[4], offsets, expected));
  ASSERT_TRUE(deep_filter_aborted_volume_attempts(offsets, expected));
  DeepRenderBuffers buffers(device.get());
  /* Shared pool capacity covers the whole append stream without allocating n events per pixel. */
  const uint32_t capacity = (header[5] + header[4] - 1) / header[4];
  buffers.reset(int(header[2]), int(header[3]), int(max(1u, capacity)), true);
  std::copy(events.begin(), events.end(), buffers.get_sample_data_ptr());
  std::copy(counts.begin(), counts.end(), buffers.get_sample_counts_ptr());
  buffers.copy_to_device();
  ASSERT_EQ(buffers.retain_device_capture(), DeepDeviceCaptureResult::Retained);
  buffers.zero();
  ASSERT_TRUE(buffers.copy_retained_device_capture()) << device->error_message();
  for (uint32_t p = 0; p < header[4]; p++) {
    vector<DeepSampleData> actual;
    buffers.get_pixel_samples(int(p % header[2]), int(p / header[2]), actual);
    const uint32_t count = offsets[p + 1] - offsets[p];
    ASSERT_EQ(actual.size(), count) << "pixel=" << p;
    if (count) EXPECT_EQ(std::memcmp(actual.data(), expected.data() + offsets[p], count * 48), 0);
  }
  ASSERT_FALSE(device->have_error()) << device->error_message();
}

TEST(DeepVolumeProfile, preserves_retained_count_and_overflow_state)
{
  constexpr uint32_t retained = 64;
  const uint32_t raw_count = retained | DEEP_SAMPLE_COUNT_OVERFLOW;

  EXPECT_EQ(deep_sample_count_retained(raw_count), retained);
  EXPECT_TRUE(deep_sample_count_overflowed(raw_count));
  EXPECT_FALSE(deep_sample_count_overflowed(retained));
}

TEST(DeepVolumeProfile, averages_camera_transmittance_at_surface_boundary)
{
  /* Camera sample 0 has one dense segment. Camera sample 1 reaches the same back depth through
   * two lighter segments. Their average opacity is 0.375 at depth 2 and 0.59375 at depth 3. */
  const vector<DeepSampleData> samples = {
      volume_sample(0.75f, 1.0f, 3.0f, 0),
      volume_sample(0.25f, 1.0f, 2.0f, 1),
      volume_sample(0.25f, 2.0f, 3.0f, 1),
  };

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.59375f, profile));

  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 0.375f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 3.0f), 0.59375f, 1.0e-5f);
}

TEST(DeepVolumeProfile, includes_camera_samples_without_volume_as_clear)
{
  const vector<DeepSampleData> samples = {volume_sample(0.5f, 1.0f, 2.0f, 0)};

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.25f, profile));

  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 0.25f, 1.0e-5f);
}

TEST(DeepVolumeProfile, sparse_camera_ids_do_not_change_the_population)
{
  const vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 3.0f, 0xffffffu),
      volume_sample(0.5f, 1.0f, 2.0f, 7),
      volume_sample(0.5f, 2.0f, 3.0f, 7),
  };
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(
      samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.625f, profile));
  EXPECT_NEAR(composite_profile_to_depth(profile, 3.0f), 0.625f, 1.0e-5f);
}

TEST(DeepVolumeProfile, preserves_epsilon_deduplicated_far_endpoint)
{
  const float near_back = std::nextafter(2.0f, 3.0f);
  const vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, near_back, 0),
      volume_sample(0.5f, 1.0f, 2.0f, 1),
  };
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(
      samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.5f, profile));
  ASSERT_FALSE(profile.empty());
  EXPECT_FLOAT_EQ(profile.back().z_back, 2.0f);
}

TEST(DeepVolumeProfile, aligned_grid_preserves_unordered_ray_spans)
{
  const vector<DeepSampleData> samples = {
      volume_sample(0.5f, 5.0f, 6.0f, 9),
      volume_sample(0.5f, 1.0f, 2.0f, 42),
      volume_sample(0.5f, 2.0f, 5.0f, 42),
      volume_sample(0.5f, 6.0f, 7.0f, 9),
  };
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(
      samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.75f, profile));
  ASSERT_FALSE(profile.empty());
  EXPECT_FLOAT_EQ(profile.front().z, 1.0f);
  EXPECT_FLOAT_EQ(profile.back().z_back, 7.0f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 7.0f), 0.75f, 1.0e-5f);
  const float expected = 1.0f - 0.5f * (1.0f + 0.5f * std::pow(0.5f, 1.0f / 6.0f));
  EXPECT_NEAR(composite_profile_to_depth(profile, 2.5f), expected, 1.0e-5f);
}

TEST(DeepVolumeProfile, combines_overlapping_segments_before_averaging_clear_rays)
{
  const vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 3.0f, 0),
      volume_sample(0.36f, 2.0f, 4.0f, 0),
  };

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.34f, profile));

  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 0.1464466f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 3.0f), 0.3f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 4.0f), 0.34f, 1.0e-5f);
}

TEST(DeepVolumeProfile, aligns_jittered_ray_boundaries_to_a_common_optical_grid)
{
  const float alpha_04 = 1.0f - std::exp(-0.4f);
  const float alpha_06 = 1.0f - std::exp(-0.6f);
  const vector<DeepSampleData> samples = {
      volume_sample(alpha_04, 1.0f, 1.4f, 0),
      volume_sample(alpha_06, 1.4f, 2.0f, 0),
      volume_sample(alpha_06, 1.0f, 1.6f, 1),
      volume_sample(alpha_04, 1.6f, 2.0f, 1),
  };

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(
      samples.data(), 0, int(samples.size()), 2, 64, zero_float3(), 0.0f, profile));

  EXPECT_LE(profile.size(), 2);
  EXPECT_NEAR(composite_profile_to_depth(profile, 1.5f), 1.0f - std::exp(-0.5f), 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 1.0f - std::exp(-1.0f), 1.0e-5f);
}

TEST(DeepVolumeProfile, saturation_keeps_rendered_camera_sample_population)
{
  EXPECT_EQ(deep_volume_profile_camera_population(64, 64, 64), 64);
  EXPECT_EQ(deep_volume_profile_camera_population(64, 128, 64), 64);
  EXPECT_EQ(deep_volume_profile_camera_population(0, 64, 64), 1);
}

TEST(DeepVolumeProfile, beauty_alpha_does_not_override_optical_profile)
{
  const vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 2.0f, 0),
      volume_sample(0.5f, 2.0f, 3.0f, 0),
  };

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 1, 64, zero_float3(), 1.0f, profile));

  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 0.5f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 3.0f), 0.75f, 1.0e-5f);
}

TEST(DeepVolumeProfile, rejects_legacy_samples_without_volume_metadata)
{
  vector<DeepSampleData> samples = {volume_sample(0.5f, 1.0f, 2.0f, 0)};
  samples[0].flags = 0;

  vector<DeepVolumeProfileSample> profile;
  EXPECT_FALSE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 1, 64, zero_float3(), 0.5f, profile));
  EXPECT_TRUE(profile.empty());
}

TEST(DeepVolumeProfile, respects_profile_sample_limit_without_beauty_calibration)
{
  vector<DeepSampleData> samples;
  for (int i = 0; i < 8; i++) {
    samples.push_back(volume_sample(0.1f, 1.0f + float(i), 2.0f + float(i), 0));
  }

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(
      deep_build_volume_profile(
          samples.data(), 0, int(samples.size()), 1, 3, zero_float3(), 0.6f, profile));

  EXPECT_LE(profile.size(), 3);
  EXPECT_NEAR(composite_profile_to_depth(profile, 9.0f), 1.0f - std::pow(0.9f, 8.0f), 1.0e-5f);
}

TEST(DeepVolumeProfile, preserves_depth_dependent_radiance)
{
  vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 2.0f, 0),
      volume_sample(0.5f, 2.0f, 3.0f, 0),
  };
  /* Captured RGB is the camera-space contribution accumulated at each depth. */
  samples[0].r = 1.0f;
  samples[1].b = 0.5f;

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(samples.data(),
                                        0,
                                        int(samples.size()),
                                        1,
                                        64,
                                        make_float3(1.0f, 0.0f, 0.5f),
                                        0.75f,
                                        profile));

  const float3 front = composite_profile_rgb_to_depth(profile, 2.0f);
  EXPECT_NEAR(front.x, 1.0f, 1.0e-5f);
  EXPECT_NEAR(front.y, 0.0f, 1.0e-5f);
  EXPECT_NEAR(front.z, 0.0f, 1.0e-5f);

  const float3 full = composite_profile_rgb_to_depth(profile, 3.0f);
  EXPECT_NEAR(full.x, 1.0f, 1.0e-5f);
  EXPECT_NEAR(full.y, 0.0f, 1.0e-5f);
  EXPECT_NEAR(full.z, 0.5f, 1.0e-5f);
}

TEST(DeepVolumeProfile, zero_radiance_segments_preserve_color_and_optical_depth)
{
  vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 2.0f, 0),
      volume_sample(0.5f, 2.0f, 3.0f, 0),
      volume_sample(0.5f, 3.0f, 4.0f, 0),
  };
  samples[0].r = 1.0f;
  samples[2].b = 0.5f;

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(samples.data(),
                                        0,
                                        int(samples.size()),
                                        1,
                                        64,
                                        make_float3(1.0f, 0.0f, 0.5f),
                                        0.875f,
                                        profile));
  const float3 middle = composite_profile_rgb_to_depth(profile, 3.0f);
  EXPECT_NEAR(middle.x, 1.0f, 1.0e-5f);
  EXPECT_FLOAT_EQ(middle.z, 0.0f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 3.0f), 0.75f, 1.0e-5f);
  const float3 full = composite_profile_rgb_to_depth(profile, 4.0f);
  EXPECT_NEAR(full.x, 1.0f, 1.0e-5f);
  EXPECT_NEAR(full.z, 0.5f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 4.0f), 0.875f, 1.0e-5f);
}

TEST(DeepVolumeProfile, tiny_nonzero_radiance_keeps_its_depth_distribution)
{
  vector<DeepSampleData> samples = {
      volume_sample(0.5f, 1.0f, 2.0f, 0),
      volume_sample(0.5f, 2.0f, 3.0f, 0),
  };
  samples[1].r = 5.0e-8f;
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(samples.data(),
                                        0,
                                        int(samples.size()),
                                        1,
                                        64,
                                        make_float3(5.0e-8f, 0.0f, 0.0f),
                                        0.75f,
                                        profile));
  EXPECT_FLOAT_EQ(composite_profile_rgb_to_depth(profile, 2.0f).x, 0.0f);
  EXPECT_NEAR(composite_profile_rgb_to_depth(profile, 3.0f).x, 5.0e-8f, 1.0e-12f);
}

TEST(DeepVolumeProfile, stochastic_termination_does_not_censor_optical_profile)
{
  vector<DeepSampleData> samples;
  for (int camera_sample = 0; camera_sample < 4; camera_sample++) {
    samples.push_back(volume_sample(0.75f, 1.0f, 2.0f, camera_sample));
    if (camera_sample < 2) {
      samples.back().flags |= DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED;
    }
    else {
      samples.push_back(volume_sample(0.75f, 3.0f, 4.0f, camera_sample));
    }
  }

  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(samples.data(),
                                        0,
                                        int(samples.size()),
                                        4,
                                        64,
                                        zero_float3(),
                                        0.9375f,
                                        profile));

  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 0.75f, 1.0e-5f);
  EXPECT_NEAR(composite_profile_to_depth(profile, 4.0f), 0.84375f, 1.0e-5f);
}

TEST(DeepVolumeProfile, rejects_incomplete_optical_ray)
{
  vector<DeepSampleData> samples = {volume_sample(0.5f, 1.0f, 2.0f, 0)};
  samples[0].flags &= ~DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE;

  vector<DeepVolumeProfileSample> profile;
  EXPECT_FALSE(deep_build_volume_profile(samples.data(),
                                         0,
                                         int(samples.size()),
                                         1,
                                         64,
                                         zero_float3(),
                                         0.5f,
                                         profile));
  EXPECT_TRUE(profile.empty());
}

TEST(DeepVolumeProfile, finite_opaque_endpoint_preserves_partial_transmission)
{
  vector<DeepSampleData> samples = {volume_sample(1.0f, 1.0f, 101.0f, 0),
                                    volume_sample(0.0f, 2.0f, 3.0f, 0)};
  samples[0].flags |= DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
  samples[0].surface_object = samples[0].surface_prim = samples[0].surface_shader =
      __float_as_uint(30.0f);
  samples[0].r = 1.0f;
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(samples.data(), 0, 2, 1, 64,
                                        zero_float3(), 0.0f, profile));
  EXPECT_NEAR(composite_profile_to_depth(profile, 2.0f), 1.0f - std::exp(-0.3f), 1.0e-6f);
  EXPECT_NEAR(composite_profile_rgb_to_depth(profile, 2.0f).x,
              1.0f - std::exp(-0.3f), 1.0e-6f);
}

TEST(DeepVolumeProfile, retains_zero_alpha_emission_without_beauty_rescaling)
{
  DeepSampleData sample = volume_sample(0.0f, 1.0f, 2.0f, 0);
  sample.r = 2.0f;
  vector<DeepVolumeProfileSample> profile;
  ASSERT_TRUE(deep_build_volume_profile(&sample, 0, 1, 1, 64,
                                        make_float3(10.0f), 0.0f, profile));
  ASSERT_EQ(profile.size(), 1u);
  EXPECT_EQ(profile[0].alpha, 0.0f);
  EXPECT_EQ(profile[0].rgb.x, 2.0f);
  EXPECT_EQ(profile[0].rgb.y, 0.0f);
}

TEST(DeepVolumeProfile, rejects_invalid_tau_and_population_identity)
{
  vector<DeepVolumeProfileSample> profile;
  for (const float tau : {-1.0f, std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()})
  {
    DeepSampleData sample = volume_sample(0.5f, 1.0f, 2.0f, 0);
    sample.flags |= DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
    sample.surface_object = __float_as_uint(tau);
    sample.surface_prim = sample.surface_shader = __float_as_uint(1.0f);
    EXPECT_FALSE(deep_build_volume_profile(&sample, 0, 1, 1, 64,
                                           zero_float3(), 0.0f, profile));
  }
  const DeepSampleData samples[] = {volume_sample(0.5f, 1.0f, 2.0f, 10),
                                    volume_sample(0.5f, 1.0f, 2.0f, 20)};
  EXPECT_FALSE(deep_build_volume_profile(samples, 0, 2, 1, 64,
                                         zero_float3(), 0.0f, profile));
}

TEST(DeepSparseCapture, compacts_skewed_tile_events_without_a_per_pixel_cap)
{
  const DeepSampleData events[] = {
      DeepSampleData(0.0f, 0.0f, 0.0f, 0.1f, 3.0f, 3.1f),
      DeepSampleData(0.0f, 0.0f, 0.0f, 0.2f, 1.0f, 1.1f),
      DeepSampleData(0.0f, 0.0f, 0.0f, 0.3f, 2.0f, 2.1f),
      DeepSampleData(0.0f, 0.0f, 0.0f, 0.4f, 4.0f, 4.1f),
  };
  const uint32_t pixels[] = {1, 0, 1, 1};
  vector<uint32_t> offsets;
  vector<DeepSampleData> compacted;

  ASSERT_TRUE(deep_compact_sparse_samples(events, pixels, 4, 3, offsets, compacted));
  ASSERT_EQ(offsets, (vector<uint32_t>{0, 1, 4, 4}));
  ASSERT_EQ(compacted.size(), 4);
  EXPECT_FLOAT_EQ(compacted[0].z, 1.0f);
  EXPECT_FLOAT_EQ(compacted[1].z, 3.0f);
  EXPECT_FLOAT_EQ(compacted[2].z, 2.0f);
  EXPECT_FLOAT_EQ(compacted[3].z, 4.0f);
}

TEST(DeepSparseCapture, rejects_an_event_outside_the_tile)
{
  const DeepSampleData event(0.0f, 0.0f, 0.0f, 0.1f, 1.0f, 1.1f);
  const uint32_t pixel = 2;
  vector<uint32_t> offsets;
  vector<DeepSampleData> compacted;

  EXPECT_FALSE(deep_compact_sparse_samples(&event, &pixel, 1, 2, offsets, compacted));
  EXPECT_TRUE(offsets.empty());
  EXPECT_TRUE(compacted.empty());
}

TEST(DeepSparseCapture, discards_only_the_aborted_part_of_a_volume_camera_ray)
{
  const auto volume_event = [](const uint32_t camera_sample,
                               const float z,
                               const uint32_t flags) {
    return DeepSampleData(0.0f,
                          0.0f,
                          0.0f,
                          0.1f,
                          z,
                          z + 0.1f,
                          0,
                          0,
                          0,
                          0,
                          deep_sample_info_pack(DEEP_SAMPLE_FLAG_VOLUME_METADATA | flags,
                                                camera_sample));
  };
  vector<DeepSampleData> events = {
      volume_event(0, 1.0f, DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE),
      volume_event(1, 1.5f, 0),
      volume_event(0, 2.0f, 0),
      volume_event(1, 2.5f, DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE),
      volume_event(0, 3.0f, DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED),
      volume_event(0, 2.0f, 0),
      volume_event(0, 3.0f, DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE),
  };
  vector<uint32_t> offsets = {0, uint32_t(events.size())};

  ASSERT_TRUE(deep_filter_aborted_volume_attempts(offsets, events));
  ASSERT_EQ(offsets, (vector<uint32_t>{0, 5}));
  ASSERT_EQ(events.size(), 5);
  EXPECT_FLOAT_EQ(events[0].z, 1.0f);
  EXPECT_FLOAT_EQ(events[1].z, 1.5f);
  EXPECT_FLOAT_EQ(events[2].z, 2.5f);
  EXPECT_FLOAT_EQ(events[3].z, 2.0f);
  EXPECT_FLOAT_EQ(events[4].z, 3.0f);
}

TEST(DeepSparseCapture, rejects_invalid_aborted_attempt_offsets)
{
  vector<uint32_t> offsets = {0, 2};
  vector<DeepSampleData> events(1);
  EXPECT_FALSE(deep_filter_aborted_volume_attempts(offsets, events));
}


namespace {

class DeepConstantRecordingDevice : public Device {
 public:
  DeepConstantRecordingDevice(Stats &stats, Profiler &profiler)
      : Device(DeviceInfo(), stats, profiler, true)
  {
    info.type = DEVICE_OPTIX; /* Host-only recorder exercises the GPU upload policy. */
  }
  size_t uploads = 0;
  size_t allocations = 0;
  KernelData uploaded = {};
  BVHLayoutMask get_bvh_layout_mask(uint) const override { return BVH_LAYOUT_NONE; }
  void const_copy_to(const char *name, void *host, size_t size) override
  {
    EXPECT_STREQ(name, "data");
    EXPECT_EQ(size, sizeof(uploaded));
    std::memcpy(&uploaded, host, sizeof(uploaded));
    uploads++;
  }
 protected:
  void mem_alloc(device_memory &mem) override
  {
    if (!mem.device_pointer) allocations++;
    mem.device_pointer = 1;
  }
  void mem_copy_to(device_memory &mem) override { mem_alloc(mem); }
  void mem_move_to_host(device_memory &) override {}
  void mem_copy_from(device_memory &, size_t, size_t, size_t, size_t) override {}
  void mem_zero(device_memory &mem) override
  {
    mem_alloc(mem);
    if (mem.host_pointer) std::memset(mem.host_pointer, 0, mem.memory_size());
  }
  void mem_free(device_memory &mem) override { mem.device_pointer = 0; }
};

}  // namespace

TEST(DeepKernelDataCache, invalidates_on_external_upload_layout_and_render_reset)
{
  const char *enabled = std::getenv("CYCLES_EXPERIMENTAL_DEEP_KERNEL_DATA_CACHE");
  if (!enabled || std::strcmp(enabled, "1") != 0) {
    GTEST_SKIP() << "Opt-in host-only constant upload cache probe";
  }
  Stats stats;
  Profiler profiler;
  DeepConstantRecordingDevice device(stats, profiler);
  DeepOutputDriver driver(&device);
  driver.set_enabled(true);
  driver.reset(2, 1, 2);
  DeepOutputDriver::SliceParams slice;
  slice.device = &device;
  slice.width = slice.window_width = 1;
  slice.height = slice.window_height = 1;
  driver.sync_device_buffers({slice});
  KernelData data = {};
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, 1);
  const auto first = device.uploaded;
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, 1);
  /* A scene upload can overwrite the slice data with byte-identical base data. */
  device.const_copy_to("data", &data, sizeof(data));
  driver.invalidate_device_kernel_data();
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, 3);
  EXPECT_EQ(std::memcmp(&first, &device.uploaded, sizeof(first)), 0);
  data.integrator.seed = 123;
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, 4);
  EXPECT_EQ(device.uploaded.integrator.seed, 123);
  slice.full_x = 1;
  driver.sync_device_buffers({slice});
  driver.update_device_kernel_data(data);
  const char *reuse = std::getenv("CYCLES_EXPERIMENTAL_DEEP_CAPTURE_POOL_REUSE");
  const size_t layout_uploads = reuse && std::strcmp(reuse, "1") == 0 ? 4 : 5;
  EXPECT_EQ(device.uploads, layout_uploads);
  driver.reset(2, 1, 2);
  driver.sync_device_buffers({slice});
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, layout_uploads + 1);
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploads, layout_uploads + 1);
}

TEST(DeepCapturePoolReuse, reuses_allocation_without_restoring_previous_tile_counts)
{
  const char *reuse = std::getenv("CYCLES_EXPERIMENTAL_DEEP_CAPTURE_POOL_REUSE");
  if (!reuse || std::strcmp(reuse, "1") != 0 ||
      !std::getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE"))
  {
    GTEST_SKIP() << "Opt-in host-only capture pool reuse probe";
  }
  Stats stats;
  Profiler profiler;
  DeepConstantRecordingDevice device(stats, profiler);
  DeepOutputDriver driver(&device);
  driver.set_enabled(true);
  driver.reset(2, 1, 2);
  DeepOutputDriver::SliceParams slice;
  slice.device = &device;
  slice.width = slice.window_width = 1;
  slice.height = slice.window_height = 1;
  driver.sync_device_buffers({slice});
  auto *first = driver.get_deep_buffers();
  ASSERT_NE(first, nullptr);
  const size_t first_allocations = device.allocations;
  EXPECT_EQ(first_allocations, 2);
  first->get_sample_counts_ptr()[0] = 1;
  first->get_sample_counts_ptr()[1] = 1;
  slice.full_x = 1;
  driver.sync_device_buffers({slice});
  EXPECT_EQ(driver.get_deep_buffers(), first);
  EXPECT_EQ(device.allocations, first_allocations);
  EXPECT_EQ(first->get_sample_counts_ptr()[0], 0);
  EXPECT_EQ(first->get_sample_counts_ptr()[1], 0);
  slice.width = slice.window_width = 2;
  driver.sync_device_buffers({slice});
  EXPECT_EQ(device.allocations, first_allocations + 2);
}

TEST(DeepCapturePoolReuse, bounded_pool_preserves_skew_and_rebuilds_changed_capacity)
{
  if (!std::getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE")) {
    GTEST_SKIP() << "Opt-in host-only bounded sparse pool probe";
  }
  Stats stats;
  Profiler profiler;
  DeepConstantRecordingDevice device(stats, profiler);
  DeepOutputDriver driver(&device);
  driver.set_enabled(true);
  driver.reset(4, 1, 64, 514);
  driver.set_capture_pool_budget(400);
  DeepOutputDriver::SliceParams slice;
  slice.device = &device;
  slice.width = slice.window_width = 4;
  slice.height = slice.window_height = 1;
  driver.sync_device_buffers({slice});
  ASSERT_FALSE(device.have_error());
  auto *buffers = driver.get_deep_buffers();
  ASSERT_NE(buffers, nullptr);
  EXPECT_EQ(buffers->get_max_samples_per_pixel(), 1);
  EXPECT_EQ(buffers->get_global_sample_capacity(), 4);
  EXPECT_EQ(driver.get_max_samples_per_pixel(), 64);
  auto *counts = buffers->get_sample_counts_ptr();
  counts[0] = counts[4] = 3;
  auto *events = buffers->get_sample_data_ptr();
  for (int i = 0; i < 3; i++) {
    events[i] = volume_sample(0.1f, float(i + 1), float(i + 2), 0);
    events[i].pixel_index = 0;
  }
  ASSERT_TRUE(buffers->copy_from_device());
  vector<DeepSampleData> pixel;
  buffers->get_pixel_samples(0, 0, pixel);
  EXPECT_EQ(pixel.size(), 3); /* The average pool capacity is not a per-pixel cap. */
  driver.set_capture_pool_budget(600);
  driver.sync_device_buffers({slice});
  ASSERT_FALSE(device.have_error());
  buffers = driver.get_deep_buffers();
  ASSERT_NE(buffers, nullptr);
  EXPECT_EQ(buffers->get_max_samples_per_pixel(), 3);
  EXPECT_EQ(buffers->get_global_sample_capacity(), 12);
  EXPECT_EQ(buffers->get_sample_counts_ptr()[0], 3);
  KernelData data = {};
  driver.update_device_kernel_data(data);
  EXPECT_EQ(device.uploaded.film.deep_max_samples, 3);
  EXPECT_EQ(device.uploaded.film.use_deep_output, 2);
  driver.set_capture_pool_budget(1);
  driver.sync_device_buffers({slice});
  EXPECT_TRUE(device.have_error());
}

TEST(DeepOutputTiles, single_tile_keeps_actual_population_and_consumed_cache)
{
  if (!std::getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE")) {
    GTEST_SKIP() << "Opt-in optical profile output path";
  }
  Stats stats;
  Profiler profiler;
  const vector<DeviceInfo> devices = Device::available_devices(DEVICE_MASK_CPU);
  ASSERT_FALSE(devices.empty());
  unique_ptr<Device> device = Device::create(devices.front(), stats, profiler, true);
  ASSERT_TRUE(device);
  DeepOutputDriver driver(device.get());
  driver.set_enabled(true);
  driver.reset(2, 1, 8);
  DeepOutputDriver::SliceParams slice;
  slice.device = device.get();
  slice.width = slice.window_width = 2;
  slice.height = slice.window_height = 1;
  driver.sync_device_buffers({slice});
  EXPECT_FALSE(driver.has_accumulated_tiles());
  DeepRenderBuffers *buffers = driver.get_deep_buffers();
  ASSERT_TRUE(buffers);
  ASSERT_TRUE(buffers->uses_sparse_capture());
  for (uint32_t pixel = 0; pixel < 2; pixel++) {
    DeepSampleData event = volume_sample(0.5f, 1.0f, 2.0f, 0);
    event.pixel_index = pixel;
    buffers->get_sample_data_ptr()[pixel] = event;
    buffers->get_sample_counts_ptr()[pixel] = 1;
  }
  buffers->get_sample_counts_ptr()[2] = 2;
  buffers->copy_to_device();
  /* Only one ray produces an event, but all four/eight rays count, including empty rays.
   * These are the raw internal pass values, independent of an exposed Debug pass. */
  const float counts[] = {0.5f, 1.0f};
  const float beauty[] = {0.125f, 0.0f, 0.0f, 0.125f, 0.0625f, 0.0f, 0.0f, 0.0625f};
  driver.accumulate_tile(beauty, counts, 8.0f, 2, 1, 0, 0);
  ASSERT_FALSE(device->have_error()) << device->error_message();
  ASSERT_TRUE(driver.has_accumulated_tiles());
  unique_ptr<std::vector<std::vector<blender::DeepSample>>> output(
      driver.get_unmerged_processed_deep_data());
  ASSERT_TRUE(output);
  ASSERT_EQ(output->size(), 2);
  for (size_t pixel = 0; pixel < 2; pixel++) {
    ASSERT_FALSE((*output)[pixel].empty());
    float transparency = 1.0f;
    for (const blender::DeepSample &sample : (*output)[pixel]) transparency *= 1.0f - sample.a;
    EXPECT_NEAR(1.0f - transparency, beauty[pixel * 4 + 3], 1.0e-6f);
    EXPECT_NEAR((*output)[pixel].front().a, beauty[pixel * 4 + 3], 1.0e-6f);
  }
  driver.reset(2, 1, 8);
  EXPECT_FALSE(driver.has_accumulated_tiles());
}

TEST(DeepCaptureGroups, bounded_work_units_cover_every_pixel_and_sample)
{
  for (const int2 size : {make_int2(106, 64), make_int2(528, 318)}) {
    for (const int samples : {1, 2, 4, 8, 16, 32}) {
      const int group_limit = size.x * size.y * 2;
      WorkTileScheduler scheduler;
      scheduler.set_accelerated_rt(true);
      scheduler.set_max_num_path_states(min(1966080 / 8, group_limit));
      BufferParams params;
      params.width = params.stride = size.x;
      params.height = size.y;
      scheduler.reset(params, 0, samples, 0, 1.0f);
      vector<uint8_t> covered(size_t(size.x) * size.y * samples, 0);
      int available = group_limit;
      while (scheduler.has_remaining_work()) {
        KernelWorkTile work;
        if (!scheduler.get_work(&work, available)) {
          ASSERT_LT(available, group_limit); /* A fresh group must always make progress. */
          available = group_limit;
          continue;
        }
        const int cameras = work.w * work.h * work.num_samples;
        ASSERT_LE(cameras, available);
        available -= cameras;
        for (int s = work.start_sample; s < work.start_sample + work.num_samples; s++)
          for (int y = work.y; y < work.y + work.h; y++)
            for (int x = work.x; x < work.x + work.w; x++) {
              const size_t index = (size_t(s) * size.y + y) * size.x + x;
              ASSERT_LT(index, covered.size());
              ASSERT_EQ(covered[index], 0);
              covered[index] = 1;
            }
        if (!available) available = group_limit;
      }
      EXPECT_EQ(std::count(covered.begin(), covered.end(), uint8_t(1)), covered.size());
    }
  }
}



TEST(DeepSurfaceCapture, equal_depth_coverage_is_independent_of_gpu_append_order)
{
  if (!std::getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE") ||
      !std::getenv("CYCLES_EXPERIMENTAL_DEEP_SURFACE_TIE_ORDER")) {
    GTEST_SKIP() << "Opt-in deterministic optical-capture surface ordering";
  }
  Stats stats;
  Profiler profiler;
  const vector<DeviceInfo> devices = Device::available_devices(DEVICE_MASK_CPU);
  ASSERT_FALSE(devices.empty());
  unique_ptr<Device> device = Device::create(devices.front(), stats, profiler, true);
  ASSERT_TRUE(device);
  vector<DeepSampleData> events;
  for (uint32_t camera = 0; camera < 3; camera++) {
    const float angle = (float(camera) - 1.0f) * 0.2617993878f;
    const float octahedral_x = sinf(angle) / (fabsf(sinf(angle)) + cosf(angle));
    const uint32_t packed_normal = uint32_t((octahedral_x * 0.5f + 0.5f) * 65535.0f + 0.5f) |
                                   (uint32_t(0.5f * 65535.0f + 0.5f) << 16);
    events.emplace_back(0.2f, 0.3f, 0.4f, 1.0f, 10.0f, 10.0f,
                        1, camera, 3, packed_normal,
                        deep_sample_info_pack(DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA, camera));
  }
  vector<int> order = {0, 1, 2};
  do {
    DeepOutputDriver driver(device.get());
    driver.set_enabled(true);
    driver.reset(1, 1, 64);
    DeepOutputDriver::SliceParams slice;
    slice.device = device.get();
    slice.width = slice.height = slice.window_width = slice.window_height = 1;
    driver.sync_device_buffers({slice});
    DeepRenderBuffers *buffers = driver.get_deep_buffers();
    ASSERT_TRUE(buffers);
    ASSERT_TRUE(buffers->uses_sparse_capture());
    for (size_t i = 0; i < events.size(); i++) buffers->get_sample_data_ptr()[i] = events[order[i]];
    buffers->get_sample_counts_ptr()[0] = buffers->get_sample_counts_ptr()[1] = 3;
    buffers->copy_to_device();
    const float count[] = {1.0f};
    const float beauty[] = {0.2f, 0.3f, 0.4f, 1.0f};
    driver.accumulate_tile(beauty, count, 3.0f, 1, 1, 0, 0);
    ASSERT_FALSE(device->have_error()) << device->error_message();
    unique_ptr<std::vector<std::vector<blender::DeepSample>>> output(
        driver.get_unmerged_processed_deep_data());
    ASSERT_TRUE(output);
    ASSERT_EQ(output->size(), 1);
    const auto &samples = (*output)[0];
    ASSERT_EQ(samples.size(), 2) << order[0] << order[1] << order[2];
    EXPECT_FLOAT_EQ(samples[0].a, 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(samples[1].a, 1.0f);
    EXPECT_FLOAT_EQ(samples[0].z, 10.0f);
    EXPECT_FLOAT_EQ(samples[1].z, 10.0f);
  } while (std::next_permutation(order.begin(), order.end()));
}

CCL_NAMESPACE_END
