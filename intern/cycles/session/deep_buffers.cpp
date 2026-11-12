/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_buffers.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "IMB_deep_sample_merge.hh"

#include "device/device.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "util/algorithm.h"
#include "util/log.h"
#include "util/path.h"
#include "util/task.h"
#include "util/time.h"

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

bool deep_compute_buffer_bytes(int width, int height, int max_samples, size_t &bytes)
{
  if (width <= 0 || height <= 0 || max_samples <= 0) {
    bytes = 0;
    return true;
  }
  const size_t max_size = std::numeric_limits<size_t>::max();
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  if (w > max_size / h) {
    return false;
  }
  const size_t num_pixels = w * h;
  const size_t samples = static_cast<size_t>(max_samples);
  if (samples > 0 && num_pixels > max_size / samples) {
    return false;
  }
  const size_t total_samples = num_pixels * samples;
  if (num_pixels > max_size / sizeof(uint32_t)) {
    return false;
  }
  const size_t count_bytes = num_pixels * sizeof(uint32_t);
  if (total_samples > max_size / sizeof(DeepSampleData)) {
    return false;
  }
  const size_t data_bytes = total_samples * sizeof(DeepSampleData);
  if (count_bytes > max_size - data_bytes) {
    return false;
  }
  bytes = count_bytes + data_bytes;
  return true;
}

int deep_effective_max_samples(const Film *film, const Integrator *integrator)
{
  if (!film) {
    return 0;
  }

  int max_samples = film->get_deep_max_samples();
  if (integrator && integrator->get_volume_ray_marching()) {
    const int max_steps = integrator->get_volume_max_steps();
    const int min_required = min(max_steps, 256);
    if (max_samples < min_required) {
      max_samples = min_required;
    }
  }

  return max_samples;
}

int deep_effective_capture_samples(const Film *film,
                                   const Integrator *integrator,
                                   const int camera_samples)
{
  const int output_samples = deep_effective_max_samples(film, integrator);
  if (getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE") == nullptr || !integrator) {
    return output_samples;
  }

  /* MoonRay retains complete per-camera-ray VolumeInputSegments before building a much smaller
   * common output grid. Cycles' progressive kernel cannot use MoonRay's per-thread current-pixel
   * arena directly, so the uncompressed oracle uses a shared tile pool with a separate average
   * raw-event capacity. Overflow remains explicit. */
  return deep_sparse_capture_average_samples(output_samples,
                                             min(camera_samples,
                                                 deep_volume_capture_batch_samples()),
                                             integrator->get_volume_max_steps());
}

int deep_volume_capture_batch_samples()
{
  /* Progressive batches are flushed to an actual-size host cache before this device pool is
   * reused. Keep the batch small enough for useful tiles while amortizing synchronization. */
  if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_CAPTURE_BATCH_SAMPLES")) {
    return clamp(atoi(value), 1, 1024);
  }
  return 8;
}

int deep_sparse_capture_average_samples(const int output_samples,
                                        const int camera_samples,
                                        const int volume_max_steps)
{
  /* Reserve the full configured ray-marching step budget for every camera ray. The tile memory
   * budget reduces the render tile dimensions when this raw oracle allocation is large. Do not
   * impose an empirical step clamp here: dense VDBs can legitimately retain more than 64 optical
   * intervals per ray, and dropping those records makes every later prefix-alpha result invalid.
   *
   * Allow one additional event per ray for a hard-surface sample. Multiple disjoint volume
   * traversals can still exceed this estimate, so the shared-pool overflow marker remains an
   * explicit validity check rather than silently truncating the profile. */
  const int64_t samples = max(camera_samples, 1);
  const int64_t events_per_ray = int64_t(max(volume_max_steps, 1)) + 1;
  const int64_t raw_capacity = samples * events_per_ray;
  return int(std::min<int64_t>(std::numeric_limits<int>::max(),
                               std::max<int64_t>(max(output_samples, 1), raw_capacity)));
}

namespace {
constexpr float deep_volume_depth_epsilon = 1e-6f;
}  // namespace

bool deep_compact_sparse_samples(const DeepSampleData *events,
                                 const uint32_t *event_pixels,
                                 const size_t event_count,
                                 const size_t pixel_count,
                                 vector<uint32_t> &pixel_offsets,
                                 vector<DeepSampleData> &compacted_events)
{
  pixel_offsets.clear();
  compacted_events.clear();
  if ((event_count > 0 && (events == nullptr || event_pixels == nullptr)) ||
      event_count > uint64_t(std::numeric_limits<uint32_t>::max()) ||
      pixel_count > uint64_t(std::numeric_limits<uint32_t>::max()))
  {
    return false;
  }

  pixel_offsets.assign(pixel_count + 1, 0);
  for (size_t event = 0; event < event_count; event++) {
    const uint32_t pixel = event_pixels[event];
    if (pixel >= pixel_count) {
      pixel_offsets.clear();
      return false;
    }
    pixel_offsets[pixel + 1]++;
  }
  for (size_t pixel = 0; pixel < pixel_count; pixel++) {
    pixel_offsets[pixel + 1] += pixel_offsets[pixel];
  }

  compacted_events.resize(event_count);
  vector<uint32_t> write_offsets(pixel_offsets.begin(), pixel_offsets.end() - 1);
  for (size_t event = 0; event < event_count; event++) {
    compacted_events[write_offsets[event_pixels[event]]++] = events[event];
  }
  return true;
}

bool deep_filter_aborted_volume_attempts(vector<uint32_t> &pixel_offsets,
                                         vector<DeepSampleData> &compacted_events)
{
  if (pixel_offsets.empty() || pixel_offsets.front() != 0 ||
      pixel_offsets.back() != compacted_events.size())
  {
    return false;
  }
  for (size_t pixel = 0; pixel + 1 < pixel_offsets.size(); pixel++) {
    if (pixel_offsets[pixel] > pixel_offsets[pixel + 1]) {
      return false;
    }
  }

  const vector<uint32_t> source_offsets = pixel_offsets;
  vector<DeepSampleData> filtered_events;
  filtered_events.reserve(compacted_events.size());
  pixel_offsets.assign(source_offsets.size(), 0);

  for (size_t pixel = 0; pixel + 1 < source_offsets.size(); pixel++) {
    const size_t begin = source_offsets[pixel];
    const size_t end = source_offsets[pixel + 1];
    vector<uint8_t> keep(end - begin, 1);
    std::unordered_map<uint32_t, vector<size_t>> open_segments;

    for (size_t index = begin; index < end; index++) {
      const DeepSampleData &sample = compacted_events[index];
      const uint32_t flags = deep_sample_info_flags(sample.flags);
      if (!(flags & DEEP_SAMPLE_FLAG_VOLUME_METADATA)) {
        continue;
      }

      const uint32_t camera_sample = deep_sample_info_camera_sample(sample.flags);
      vector<size_t> &open_segment = open_segments[camera_sample];
      open_segment.push_back(index - begin);
      if (flags & DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED) {
        for (const size_t local_index : open_segment) {
          keep[local_index] = 0;
        }
        open_segments.erase(camera_sample);
      }
      else if (flags & DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE) {
        open_segments.erase(camera_sample);
      }
    }

    for (size_t index = begin; index < end; index++) {
      if (keep[index - begin]) {
        filtered_events.push_back(compacted_events[index]);
      }
    }
    pixel_offsets[pixel + 1] = uint32_t(filtered_events.size());
  }

  compacted_events.swap(filtered_events);
  return true;
}

bool deep_compute_sparse_buffer_bytes(const int width,
                                      const int height,
                                      const int average_samples,
                                      size_t &bytes)
{
  if (!deep_compute_buffer_bytes(width, height, average_samples, bytes)) {
    return false;
  }
  if (width <= 0 || height <= 0 || average_samples <= 0) {
    return true;
  }
  const size_t num_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t samples = static_cast<size_t>(average_samples);
  /* The device append cursor and returned sample index are uint32_t. Reject an allocation whose
   * addressable event population would wrap even when size_t and device memory could represent it. */
  if (num_pixels > std::numeric_limits<uint32_t>::max() / samples ||
      bytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t))
  {
    return false;
  }
  /* Sparse capture reuses DeepSampleData's alignment tail for pixel ownership and needs one
   * additional count word for the append cursor. */
  bytes += sizeof(uint32_t);
  return true;
}

bool deep_bounded_sparse_capture_samples(const int width,
                                         const int height,
                                         const int requested_samples,
                                         const size_t budget,
                                         int &samples,
                                         size_t &bytes)
{
  samples = 0;
  bytes = 0;
  if (width <= 0 || height <= 0 || requested_samples <= 0) {
    return false;
  }
  const uint64_t pixels = uint64_t(width) * uint64_t(height);
  if (pixels > UINT32_MAX || pixels > (std::numeric_limits<size_t>::max() - 4) / 4) {
    return false;
  }
  const size_t counts_bytes = size_t(pixels) * sizeof(uint32_t) + sizeof(uint32_t);
  if (counts_bytes >= budget ||
      pixels > std::numeric_limits<size_t>::max() / sizeof(DeepSampleData))
  {
    return false;
  }
  const size_t samples_per_pixel = min(
      size_t(requested_samples),
      min((budget - counts_bytes) / (size_t(pixels) * sizeof(DeepSampleData)),
          size_t(UINT32_MAX / pixels)));
  if (samples_per_pixel == 0) {
    return false;
  }
  samples = int(samples_per_pixel);
  return deep_compute_sparse_buffer_bytes(width, height, samples, bytes) && bytes <= budget;
}

bool deep_bounded_pool_geometry_requested()
{
  for (const char *name : {"CYCLES_EXPERIMENTAL_DEEP_GPU_BOUNDED_POOL_GEOMETRY",
                           "CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE",
                           "CYCLES_EXPERIMENTAL_DEEP_GPU_CAPTURE_GROUPS",
                           "CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_ONLY_KERNEL",
                           "CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WARP_KERNEL",
                           "CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WAVEFRONT"})
  {
    const char *value = std::getenv(name);
    if (!value || std::strcmp(value, "1") != 0)
      return false;
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Deep Render Buffers Implementation
 * \{ */

bool deep_validate_sparse_capture_counts(const uint32_t *counts,
                                         const size_t pixel_count,
                                         const size_t capacity,
                                         size_t &captured)
{
  captured = counts[pixel_count];
  if (captured > capacity) {
    return false;
  }
  size_t total = 0;
  for (size_t pixel = 0; pixel < pixel_count; pixel++) {
    const uint32_t count = counts[pixel];
    if (deep_sample_count_overflowed(count) || size_t(count) > captured - total) {
      return false;
    }
    total += count;
  }
  return total == captured;
}

bool deep_resident_capture_fits(const size_t bytes,
                                const size_t retained_bytes,
                                const size_t budget,
                                const size_t free_bytes,
                                const size_t headroom)
{
  return retained_bytes <= budget && bytes <= budget - retained_bytes &&
         headroom <= free_bytes && bytes <= free_bytes - headroom;
}

static bool deep_resident_capture_requested()
{
  const char *value = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE");
  return value && std::strcmp(value, "1") == 0;
}

static bool deep_resident_capture_trace()
{
  return std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
}

DeepRenderBuffers::DeepRenderBuffers(Device *device)
    : device_(device),
      sample_counts_(device, "deep_sample_counts", MEM_READ_WRITE),
      sample_data_(device, "deep_sample_data", MEM_READ_WRITE)
{
}

DeepRenderBuffers::~DeepRenderBuffers()
{
  sample_counts_.free();
  sample_data_.free();
}

void DeepRenderBuffers::reset(const int new_width,
                              const int new_height,
                              const int new_max_samples,
                              const bool use_sparse_capture)
{
  const bool trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") != nullptr;
  const double reset_start = trace ? time_dt() : 0.0;
  discard_retained_device_capture();
  width_ = new_width;
  height_ = new_height;
  max_samples_per_pixel_ = new_max_samples;
  use_sparse_capture_ = use_sparse_capture;
  sparse_pixel_offsets_.clear();
  sparse_sample_data_.clear();

  const size_t num_pixels = static_cast<size_t>(width_) * height_;

  /* Allocate sample counts (one per pixel). */
  sample_counts_.alloc(num_pixels + (use_sparse_capture_ ? 1 : 0));

  /* Allocate sample data with worst-case size:
   * Every pixel could have max_samples_per_pixel samples.
   * This is memory intensive but simplifies GPU access patterns. */
  const size_t max_total_samples = num_pixels * max_samples_per_pixel_;
  sample_data_.alloc(max_total_samples);

  const double allocated = trace ? time_dt() : 0.0;
  /* Initialize to zero. */
  zero();
  if (trace) {
    std::fprintf(stderr, "DEEP_BUFFER_RESET width=%d height=%d host_alloc_ms=%.6f device_init_ms=%.6f\n",
                 width_, height_, (allocated - reset_start) * 1000,
                 (time_dt() - allocated) * 1000);
  }

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
  /* Sparse capture addresses only the contiguous prefix claimed by the append cursor, and every
   * claimed event is initialized field-by-field by the kernel. Clearing the unused worst-case
   * payload adds capacity-scaled traffic without establishing any observable state. */
  if (use_sparse_capture_) {
    sample_data_.alloc_to_device();
  }
  else {
    sample_data_.zero_to_device();
  }
}

bool DeepRenderBuffers::copy_from_device()
{
  if (!is_allocated()) {
    return false;
  }

  sample_counts_.copy_from_device();
  size_t retained = sample_data_.size();
  if (use_sparse_capture_) {
    const size_t pixel_count = static_cast<size_t>(width_) * height_;
    const size_t captured = size_t(sample_counts_.data()[pixel_count]);
    retained = min(captured, sample_data_.size());
    if (retained > 0) {
      /* Sparse events are appended contiguously. Read the cursor/counts first, then transfer only
       * the initialized prefix. A cursor beyond capacity is still rejected below by comparing the
       * retained prefix with the per-pixel population and overflow flags. */
      sample_data_.copy_from_device(0, retained, 1);
    }
  }
  else {
    sample_data_.copy_from_device();
  }
  return !use_sparse_capture_ || compact_sparse_capture(retained);
}

bool DeepRenderBuffers::compact_sparse_capture(const size_t retained)
{
  {
    const size_t pixel_count = static_cast<size_t>(width_) * height_;
    size_t per_pixel_total = 0;
    for (size_t pixel = 0; pixel < pixel_count; pixel++) {
      const uint32_t count = deep_sample_count_retained(sample_counts_.data()[pixel]);
      per_pixel_total += count;
    }
    vector<uint32_t> event_pixels(retained);
    for (size_t event = 0; event < retained; event++) {
      event_pixels[event] = sample_data_[event].pixel_index;
    }
    if (!deep_compact_sparse_samples(sample_data_.data(),
                                     event_pixels.data(),
                                     retained,
                                     pixel_count,
                                     sparse_pixel_offsets_,
                                     sparse_sample_data_))
    {
      return false;
    }
    /* A sparse kernel increments both the shared append cursor and the owning pixel count for
     * every retained event. Reject a layout mismatch instead of letting later sorting trust a
     * dense count against an empty or shorter compacted array. */
    if (per_pixel_total != retained || sparse_pixel_offsets_.back() != retained) {
      LOG_ERROR << "Deep EXR sparse capture layout mismatch (cursor retained " << retained
                << ", per-pixel retained " << per_pixel_total << ")";
      sparse_pixel_offsets_.clear();
      sparse_sample_data_.clear();
      return false;
    }
    for (size_t pixel = 0; pixel < pixel_count; pixel++) {
      const uint32_t actual = sparse_pixel_offsets_[pixel + 1] - sparse_pixel_offsets_[pixel];
      if (actual != deep_sample_count_retained(sample_counts_[pixel])) {
        LOG_ERROR << "Deep EXR sparse pixel ownership/count mismatch at pixel " << pixel;
        sparse_pixel_offsets_.clear();
        sparse_sample_data_.clear();
        return false;
      }
    }
    /* Opt-in replay evidence is the original append stream, before attempt filtering or sorting.
     * Capture one complete, count-validated batch without changing its 48-byte event ABI. */
    static const char *replay_path = std::getenv("CYCLES_EXPERIMENTAL_DEEP_RAW_REPLAY");
    static std::atomic<bool> replay_written(false);
    size_t replay_minimum = 0;
    if (replay_path && replay_path[0]) {
      if (const char *minimum = std::getenv("CYCLES_EXPERIMENTAL_DEEP_RAW_REPLAY_MIN_EVENTS")) {
        char *end;
        errno = 0;
        const unsigned long long count = std::strtoull(minimum, &end, 10);
        if (errno || end == minimum || *end != '\0' || *minimum == '-' ||
            count > std::numeric_limits<uint32_t>::max())
        {
          LOG_ERROR << "Invalid Deep EXR raw replay minimum event count";
          return false;
        }
        replay_minimum = size_t(count);
      }
    }
    if (replay_path && replay_path[0] && retained >= replay_minimum &&
        !replay_written.exchange(true))
    {
      FILE *file = path_fopen(replay_path, "wbx");
      if (!file) {
        LOG_ERROR << "Unable to create exclusive Deep EXR raw replay: " << replay_path;
        return false;
      }
      const uint32_t header[] = {
          0x44525031u, 1u, uint32_t(width_), uint32_t(height_), uint32_t(pixel_count), uint32_t(retained)};
      const bool written = std::fwrite(header, sizeof(header), 1, file) == 1 &&
                           std::fwrite(sample_counts_.data(), sizeof(uint32_t), pixel_count + 1, file) ==
                               pixel_count + 1 &&
                           std::fwrite(sample_data_.data(), sizeof(DeepSampleData), retained, file) ==
                               retained;
      const bool closed = std::fclose(file) == 0;
      if (!written || !closed) {
        LOG_ERROR << "Unable to write complete Deep EXR raw replay: " << replay_path;
        return false;
      }
      LOG_INFO << "Deep EXR raw replay: " << replay_path << ", events=" << retained;
    }
    if (!deep_filter_aborted_volume_attempts(sparse_pixel_offsets_, sparse_sample_data_)) {
      LOG_ERROR << "Failed to discard aborted Deep EXR volume shader attempts";
      sparse_pixel_offsets_.clear();
      sparse_sample_data_.clear();
      return false;
    }
  }

  return true;
}

DeepDeviceCaptureResult DeepRenderBuffers::retain_device_capture()
{
  if (!is_allocated() || !use_sparse_capture_ || !deep_resident_capture_requested() ||
      (device_->info.type != DEVICE_CUDA && device_->info.type != DEVICE_OPTIX))
  {
    return DeepDeviceCaptureResult::Readback;
  }

  const double start = time_dt();
  /* The same synchronous count read used by host capture also completes the producer before the
   * separate copy queue accesses its append pool. Keep this ordering explicit. */
  sample_counts_.copy_from_device();
  size_t captured;
  const size_t pixel_count = static_cast<size_t>(width_) * height_;
  if (device_->have_error() ||
      !deep_validate_sparse_capture_counts(
          sample_counts_.data(), pixel_count, sample_data_.size(), captured))
  {
    device_->set_error("Deep EXR optical capture overflow or sparse count mismatch");
    return DeepDeviceCaptureResult::Error;
  }
  if (captured == 0) {
    return DeepDeviceCaptureResult::Retained;
  }

  size_t budget = size_t(4096) * 1024 * 1024;
  if (const char *value = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_BUDGET_MB")) {
    char *end;
    errno = 0;
    const unsigned long long mb = std::strtoull(value, &end, 10);
    if (errno || end == value || *end != '\0' || *value == '-' ||
        mb > std::numeric_limits<size_t>::max() / (1024 * 1024))
    {
      device_->set_error("Invalid Deep EXR resident capture memory budget");
      return DeepDeviceCaptureResult::Error;
    }
    budget = size_t(mb) * 1024 * 1024;
  }
  size_t total = 0, free = 0;
  device_->get_device_memory_info(total, free);
  const size_t headroom = max(size_t(256) * 1024 * 1024, total / 16);
  const size_t bytes = captured * sizeof(DeepSampleData);
  size_t processing_reservation = retained_processing_reservation_;
  const char *process = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_PROCESS");
  if (process && std::strcmp(process, "1") == 0) {
    if (captured > size_t(std::numeric_limits<int>::max()) ||
        pixel_count >= size_t(std::numeric_limits<int>::max()))
    {
      return DeepDeviceCaptureResult::Readback;
    }
    /* Reserve the next batch's complete processing scratch as well as retained raw payloads.
     * This is conservative when an existing queue allocation is already counted in free memory. */
    const char *groups = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_GROUP_HEADS");
    const bool group_heads = groups && std::strcmp(groups, "1") == 0;
    const size_t scratch = captured * (group_heads ? 82 : 73) + (pixel_count + 1) * 20 +
                           ((pixel_count + 256) / 256) * 4 + 256;
    processing_reservation = max(processing_reservation, scratch);
  }
  if (!deep_resident_capture_fits(bytes + processing_reservation,
                                 retained_device_bytes_, budget, free, headroom))
  {
    if (deep_resident_capture_trace()) {
      std::fprintf(stderr,
                   "DEEP_GPU_CAPTURE spill bytes=%zu resident=%zu budget=%zu free=%zu headroom=%zu "
                   "processing_reserve=%zu\n",
                   bytes, retained_device_bytes_, budget, free, headroom, processing_reservation);
    }
    return DeepDeviceCaptureResult::Readback;
  }
  if (!retained_queue_) {
    retained_queue_ = device_->gpu_queue_create();
  }
  if (!retained_queue_ || !retained_queue_->supports_device_to_device_copy()) {
    return DeepDeviceCaptureResult::Readback;
  }

  RetainedDeviceBatch batch;
  batch.events = make_unique<device_only_memory<DeepSampleData>>(device_, "deep_retained_events");
  batch.counts.assign(sample_counts_.data(), sample_counts_.data() + pixel_count + 1);
  const double allocate_start = time_dt();
  batch.events->alloc_to_device(captured);
  const double copy_start = time_dt();
  if (device_->have_error() ||
      !retained_queue_->copy_device_to_device(*batch.events, sample_data_, bytes) ||
      !retained_queue_->synchronize())
  {
    return DeepDeviceCaptureResult::Error;
  }
  /* Only publish a chunk after the D2D copy has completed. The caller can now reset/reuse the
   * append pool without aliasing this batch, including on cancellation or a layout change. */
  retained_device_bytes_ += bytes;
  retained_processing_reservation_ = processing_reservation;
  retained_device_batches_.push_back(std::move(batch));
  if (deep_resident_capture_trace()) {
    const double finish = time_dt();
    std::fprintf(stderr,
                 "DEEP_GPU_CAPTURE retain events=%zu bytes=%zu resident=%zu batches=%zu "
                 "allocate_ms=%.6f copy_sync_ms=%.6f total_ms=%.6f\n",
                 captured, bytes, retained_device_bytes_, retained_device_batches_.size(),
                 (copy_start - allocate_start) * 1000, (finish - copy_start) * 1000,
                 (finish - start) * 1000);
  }
  return DeepDeviceCaptureResult::Retained;
}

bool DeepRenderBuffers::copy_retained_device_capture()
{
  if (retained_device_batches_.empty()) {
    return false;
  }
  RetainedDeviceBatch &batch = retained_device_batches_.front();
  vector<uint8_t> storage;
  const double start = time_dt();
  const char *process = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_PROCESS");
  if (process && std::strcmp(process, "1") == 0 &&
      retained_queue_->supports_deep_capture_processing())
  {
    if (!retained_queue_->process_deep_capture(
            *batch.events, batch.counts, sparse_pixel_offsets_, storage,
            retained_pixel_selection_.empty() ? nullptr : &retained_pixel_selection_))
    {
      return false;
    }
    sparse_sample_data_.resize(storage.size() / sizeof(DeepSampleData));
    if (!storage.empty()) {
      std::memcpy(sparse_sample_data_.data(), storage.data(), storage.size());
    }
    std::copy(batch.counts.begin(), batch.counts.end(), sample_counts_.data());
    const size_t bytes = batch.events->memory_size();
    retained_device_batches_.erase(retained_device_batches_.begin());
    retained_device_bytes_ -= bytes;
    if (retained_device_batches_.empty()) retained_pixel_selection_.clear();
    if (deep_resident_capture_trace()) {
      std::fprintf(stderr, "DEEP_GPU_CAPTURE host_finish bytes=%zu total_ms=%.6f\n",
                   bytes, (time_dt() - start) * 1000);
    }
    return true;
  }
  void *data = retained_queue_->copy_from_device_synchronized(*batch.events, storage);
  const size_t bytes = batch.events->memory_size();
  if (!data || device_->have_error() || bytes > sample_data_.memory_size()) {
    return false;
  }
  std::memcpy(sample_data_.data(), data, bytes);
  std::copy(batch.counts.begin(), batch.counts.end(), sample_counts_.data());
  const double compact_start = time_dt();
  if (!compact_sparse_capture(bytes / sizeof(DeepSampleData))) {
    return false;
  }
  retained_device_batches_.erase(retained_device_batches_.begin());
  retained_device_bytes_ -= bytes;
  if (deep_resident_capture_trace()) {
    const double finish = time_dt();
    std::fprintf(stderr,
                 "DEEP_GPU_CAPTURE drain bytes=%zu resident=%zu readback_ms=%.6f compact_ms=%.6f\n",
                 bytes, retained_device_bytes_, (compact_start - start) * 1000,
                 (finish - compact_start) * 1000);
  }
  return true;
}

DeepVolumeCaptureResult DeepRenderBuffers::profile_retained_device_capture(
    const vector<float> &population,
    const vector<float> &beauty,
    const int max_profile_samples,
    vector<uint32_t> &profile_offsets,
    vector<uint8_t> &profile_storage)
{
  const char *raw_processing = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RAW_PROCESS");
  if (retained_device_batches_.empty() || !retained_queue_ ||
      !retained_queue_->supports_deep_volume_capture() || !raw_processing ||
      std::strcmp(raw_processing, "1") != 0 || !retained_pixel_selection_.empty())
    return DeepVolumeCaptureResult::Unsupported;
  vector<device_memory *> batches;
  vector<vector<uint32_t>> counts;
  for (RetainedDeviceBatch &batch : retained_device_batches_) {
    batches.push_back(batch.events.get());
    counts.push_back(batch.counts);
  }
  vector<uint32_t> selection;
  const DeepVolumeCaptureResult result = retained_queue_->process_deep_volume_capture(
      batches, counts, population, beauty, max_profile_samples, profile_offsets,
      profile_storage, selection);
  if (result == DeepVolumeCaptureResult::Complete) retained_pixel_selection_ = std::move(selection);
  return result;
}

bool DeepRenderBuffers::device_profile_needs_host_fallback() const
{
  return std::any_of(retained_pixel_selection_.begin(), retained_pixel_selection_.end(),
                     [](const uint32_t value) { return value != 0; });
}

bool DeepRenderBuffers::select_device_profile_window(const int x,
                                                      const int y,
                                                      const int width,
                                                      const int height)
{
  if (x < 0 || y < 0 || width < 0 || height < 0 || x > width_ || y > height_ ||
      width > width_ - x || height > height_ - y ||
      retained_pixel_selection_.size() != size_t(width_) * height_)
  {
    device_->set_error("Invalid Deep GPU profile output window");
    return false;
  }
  /* Border payloads are captured for render-buffer filtering but never merged to this tile's
   * output window. They must not trigger host fallback for pixels owned by adjacent tiles. */
  for (int py = 0; py < height_; py++) {
    for (int px = 0; px < width_; px++) {
      if (px < x || px >= x + width || py < y || py >= y + height) {
        retained_pixel_selection_[size_t(py) * width_ + px] = 0;
      }
    }
  }
  return true;
}

void DeepRenderBuffers::discard_retained_device_capture()
{
  retained_device_batches_.clear();
  retained_device_bytes_ = 0;
  retained_processing_reservation_ = 0;
  retained_pixel_selection_.free_memory();
  retained_queue_.reset();
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

bool DeepRenderBuffers::get_pixel_sample_range(const size_t pixel_index,
                                               uint32_t &raw_count,
                                               const DeepSampleData *&sample_data,
                                               size_t &offset) const
{
  const size_t pixel_count = static_cast<size_t>(width_) * height_;
  if (pixel_index >= pixel_count) {
    return false;
  }
  raw_count = sample_counts_.data()[pixel_index];
  if (use_sparse_capture_) {
    if (sparse_pixel_offsets_.size() != pixel_count + 1) {
      return false;
    }
    offset = sparse_pixel_offsets_[pixel_index];
    const uint32_t available = sparse_pixel_offsets_[pixel_index + 1] -
                               sparse_pixel_offsets_[pixel_index];
    const uint32_t retained = min(deep_sample_count_retained(raw_count), available);
    raw_count = retained | (raw_count & DEEP_SAMPLE_COUNT_OVERFLOW);
    sample_data = sparse_sample_data_.data();
  }
  else {
    offset = pixel_index * static_cast<size_t>(max_samples_per_pixel_);
    sample_data = sample_data_.data();
  }
  return true;
}

void DeepRenderBuffers::get_pixel_samples(int x, int y, vector<DeepSampleData> &out_samples) const
{
  out_samples.clear();

  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    return;
  }

  const size_t pixel_index = static_cast<size_t>(y) * width_ + x;
  const uint32_t num_samples = deep_sample_count_retained(sample_counts_.data()[pixel_index]);

  if (num_samples == 0) {
    return;
  }

  /* Calculate offset into sample_data for this pixel. */
  uint32_t raw_count;
  const DeepSampleData *data;
  size_t sample_offset;
  if (!get_pixel_sample_range(pixel_index, raw_count, data, sample_offset)) {
    return;
  }
  const uint32_t retained_count = deep_sample_count_retained(raw_count);

  out_samples.reserve(retained_count);
  for (uint32_t i = 0; i < retained_count; i++) {
    out_samples.push_back(data[sample_offset + i]);
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
    total += deep_sample_count_retained(counts[i]);
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
  DeepSampleData *data = use_sparse_capture_ ? sparse_sample_data_.data() : sample_data_.data();

  const auto sort_pixel = [&](const size_t pixel) {
    const uint32_t num_samples = use_sparse_capture_ ?
                                     sparse_pixel_offsets_[pixel + 1] -
                                         sparse_pixel_offsets_[pixel] :
                                     deep_sample_count_retained(counts[pixel]);
    if (num_samples <= 1) {
      return;  /* Nothing to sort. */
    }

    const size_t offset = use_sparse_capture_ ? sparse_pixel_offsets_[pixel] :
                                                pixel * max_samples_per_pixel_;
    sort(data + offset, data + offset + num_samples);
  };
  const char *parallel = std::getenv("CYCLES_EXPERIMENTAL_DEEP_PARALLEL_CAPTURE");
  if (parallel && std::strcmp(parallel, "1") == 0) {
    tbb::task_group_context context;
    context.capture_fp_settings();
    parallel_for(blocked_range<size_t>(0, num_pixels, 64),
                 [&](const blocked_range<size_t> &range) {
                   for (size_t pixel = range.begin(); pixel < range.end(); pixel++) {
                     sort_pixel(pixel);
                   }
                 },
                 context);
  }
  else {
    for (size_t pixel = 0; pixel < num_pixels; pixel++) {
      sort_pixel(pixel);
    }
  }
}

void DeepRenderBuffers::merge_nearby_samples()
{
  if (!is_allocated() || depth_merge_threshold_ <= 0.0f) {
    return;
  }

  const size_t num_pixels = static_cast<size_t>(width_) * height_;
  uint32_t *counts = sample_counts_.data();
  DeepSampleData *data = use_sparse_capture_ ? sparse_sample_data_.data() : sample_data_.data();

  for (size_t pixel = 0; pixel < num_pixels; pixel++) {
    const bool overflowed = deep_sample_count_overflowed(counts[pixel]);
    uint32_t num_samples = deep_sample_count_retained(counts[pixel]);
    if (num_samples <= 1) {
      continue;
    }

    const size_t offset = use_sparse_capture_ ? sparse_pixel_offsets_[pixel] :
                                                pixel * max_samples_per_pixel_;

    counts[pixel] = blender::imbuf::deep_merge::merge_sorted_deep_samples(
        data + offset,
        num_samples,
        depth_merge_threshold_,
        alpha_merge_threshold_,
        deep_volume_depth_epsilon,
        true);
    if (overflowed) {
      counts[pixel] |= DEEP_SAMPLE_COUNT_OVERFLOW;
    }
  }
}

/** \} */

CCL_NAMESPACE_END
