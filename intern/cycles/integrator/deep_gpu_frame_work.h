/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/memory.h"
#include "device/queue.h"
#include "integrator/deep_frame_capture.h"
#include "session/deep_frame_production.h"

#include <array>
#include <functional>
#include <string>

CCL_NAMESPACE_BEGIN

class DeviceScene;
class RenderBuffers;
struct IntegratorStateGPU;
struct BufferParams;

/* Host owns allocation/control metadata and opaque committed spill bytes.
 * All numerical operations, including reloaded-run merges, execute on GPU. */
class DeepGPUResidentBuffer {
 public:
  DeepGPUResidentBuffer(Device *device, const DeepMemoryBudget &host_budget,
                         const DeepMemoryBudget &device_budget);
  ~DeepGPUResidentBuffer();
  DeepGPUResidentBuffer(const DeepGPUResidentBuffer &) = delete;
  DeepGPUResidentBuffer &operator=(const DeepGPUResidentBuffer &) = delete;
  bool reserve(DeviceQueue &queue, size_t bytes, bool preserve = true);
  bool append(DeviceQueue &queue, device_memory &source, size_t bytes,
              size_t source_offset = 0);
  bool append_raw(DeviceQueue &queue, const void *source, size_t bytes);
  bool zero(DeviceQueue &queue, size_t bytes);
  void clear();
  device_ptr pointer() const;
  device_memory &memory();
  size_t used() const { return used_; }
  size_t capacity() const { return capacity_; }
  void set_used(size_t bytes) { used_ = bytes; }

 private:
  Device *device_;
  DeviceQueue *last_queue_ = nullptr;
  DeepMemoryBudget budget_;
  DeepMemoryBudget host_budget_;
  unique_ptr<device_only_memory<uint8_t>> memory_;
  size_t capacity_ = 0, used_ = 0;
};

enum class DeepGPUResidentKind : uint32_t {
  Camera, Root, Homogeneous, Point, Source, Node, Call,
  Optical, OpticalOwner, Visibility, VisibilityOwner, Radiance, RadianceOwner,
  Primitive, ScalarState, Stack, CompactCamera, RootColour, ColourSource, Count
};

struct DeepGPUFinalTile {
  explicit DeepGPUFinalTile(const DeepMemoryBudget &budget)
      : ranges(DeepBudgetAllocator<DeepGPUFrameFinalRange>(budget)),
        samples(DeepBudgetAllocator<DeepEncodedPayload>(budget))
  {
  }
  int x = 0, y = 0, width = 0, height = 0;
  uint32_t consumers = 0;
  DeepNativeCaptureVector<DeepGPUFrameFinalRange> ranges;
  DeepNativeCaptureVector<DeepEncodedPayload> samples;
};

class DeepGPUFrameWork {
 public:
  DeepGPUFrameWork(Device *device, DeviceQueue *queue,
                   const DeepMemoryBudget &host_budget, const DeepMemoryBudget &device_budget);
  ~DeepGPUFrameWork();
  bool begin(const DeepNativeCaptureDomain &domain);
  bool append_native(DeepGPUResidentKind kind, device_memory &source, uint32_t records);
  uint32_t native_count(DeepGPUResidentKind kind) const;
  device_ptr native_pointer(DeepGPUResidentKind kind) const;
  bool reserve_native_root_colours(uint32_t count);
  bool bind_witness(IntegratorStateGPU &state);
  bool finish_native(DeviceScene *scene, const IntegratorStateGPU &beauty,
                      const bool *cancel, RenderBuffers *buffers,
                      const std::function<bool()> &cancel_poll);
  bool append_optical(device_memory &pieces, uint32_t count,
                       device_memory &visibility, uint32_t visibility_count,
                       uint32_t camera_offset, uint32_t chunk_cameras);
  bool close_tile(RenderBuffers &buffers, std::span<const DeepFrozenConsumer> consumers,
                   DeepGPUFinalTile &output);
  bool close_tiles(RenderBuffers &buffers, std::span<const DeepFrozenConsumer> consumers,
                     const std::function<bool(DeepGPUFinalTile &&)> &publish,
                     const std::function<bool()> &cancel_poll);
  bool prepare_tiles(RenderBuffers &buffers, std::span<const DeepFrozenConsumer> consumers,
                       const std::function<bool()> &cancel_poll);
  void set_beauty_denoised(const bool value) { beauty_denoised_ = value; }
  void discard(bool archives = true);
  const DeepNativeCaptureDomain &domain() const { return domain_; }
  DeviceQueue &queue() const { return *queue_; }
  device_ptr status_pointer() const { return controls_.device_pointer; }
  uint32_t camera_base() const { return tile_cameras_; }
  bool good();

 private:
  using Buffer = std::shared_ptr<DeepGPUResidentBuffer>;
  struct RawBytes {
    explicit RawBytes(const DeepMemoryBudget &budget) : bytes(DeepBudgetAllocator<uint8_t>(budget)) {}
    DeepNativeCaptureVector<uint8_t> bytes;
  };
  using RawBytesPtr = std::shared_ptr<RawBytes>;
  struct PendingSimpleTile {
    explicit PendingSimpleTile(const DeepMemoryBudget &budget)
        : output(budget), summaries(DeepBudgetAllocator<RawBytesPtr>(budget)) {}
    DeepGPUFinalTile output;
    DeepNativeCaptureVector<RawBytesPtr> summaries;
  };
  bool finish_pending_tiles(RenderBuffers &buffers, std::span<const DeepFrozenConsumer> consumers,
                              const std::function<bool(DeepGPUFinalTile &&)> &publish);
  struct ArchivedPage : RawBytes {
    explicit ArchivedPage(const DeepMemoryBudget &budget)
        : RawBytes(budget), ranges(DeepBudgetAllocator<DeepGPUFrameRange>(budget)) {}
    DeepNativeCaptureVector<DeepGPUFrameRange> ranges;
  };
  using ArchivedPagePtr = std::shared_ptr<ArchivedPage>;
  struct ArchivedDomain {
    explicit ArchivedDomain(const DeepMemoryBudget &budget)
        : primitives(DeepBudgetAllocator<ArchivedPagePtr>(budget)),
          colours(DeepBudgetAllocator<ArchivedPagePtr>(budget)) {}
    ArchivedPagePtr cameras;
    DeepNativeCaptureVector<ArchivedPagePtr> primitives, colours;
  };
  bool spill_native_primitives();
  bool archive_native();
  bool archive_records(DeepGPUResidentKind kind, device_memory &source, uint32_t records,
                         Buffer &camera_map, Buffer &camera_keys, ArchivedPagePtr &page);
  bool load_archive_window(uint32_t first_pixel, uint32_t pixels);
  size_t archive_window_cost(uint32_t first_pixel, uint32_t pixels) const;
  struct CompactTile {
    Buffer camera_keys, camera_ids, camera_map, camera_ranges, pixels;
    Buffer primitive_keys, primitive_ids, events, event_keys, event_ids, event_ranges;
    Buffer areas, knots, tail, projection;
    DeepCompactParameters parameters = {};
    uint32_t pixel_count = 0, camera_count = 0, primitive_count = 0, event_count = 0;
  };
  bool build_compact_base(RenderBuffers &buffers, CompactTile &tile);
  bool begin_witness();
  bool finish_witness_native();
  bool dump_witness(RenderBuffers &buffers);
  bool dump_trace();
  bool build_compact_colour(CompactTile &tile, Buffer &joined, Buffer &ranges,
                            uint32_t &joined_events, float exposure, bool alpha_half);
  Buffer make_buffer();
  bool fail(const char *message);
  bool operation(DeepGPUFrameOperation op, const DeepGPUFrameDispatch &p,
                  uint32_t blocks, uint32_t threads = 256);
  bool scan(DeepGPUResidentBuffer &counts, uint32_t size, DeepGPUResidentBuffer &offsets);
  bool sort(DeepGPUResidentBuffer &keys, DeepGPUResidentBuffer &ids, uint32_t count);
  bool sort_spilled(DeepGPUResidentBuffer &keys, DeepGPUResidentBuffer &ids, uint32_t count);
  bool group(DeepGPUResidentBuffer &keys, uint32_t count, uint32_t owners,
              DeepGPUResidentBuffer &ranges);
  uint32_t total(DeepGPUResidentBuffer &counts, DeepGPUResidentBuffer &offsets, uint32_t size);
  bool read_final(DeepGPUResidentBuffer &source, size_t bytes, void *destination);
  bool upload_metadata(const void *source, size_t bytes, device_vector<uint8_t> &destination);
  bool resolve_native_radiance();
  Device *device_;
  DeviceQueue *queue_;
  DeepMemoryBudget host_budget_, device_budget_;
  DeepNativeCaptureDomain domain_;
  DeepNativeCaptureVector<std::shared_ptr<RawBytes>> primitive_spill_;
  DeepNativeCaptureVector<std::shared_ptr<ArchivedDomain>> archives_;
  int archive_x_ = 0, archive_y_ = 0, archive_width_ = 0, archive_height_ = 0;
  std::array<Buffer, size_t(DeepGPUResidentKind::Count)> native_, tile_;
  Buffer progress_reserve_;
  Buffer witness_cuts_device_, witness_log_, witness_rgb_, witness_rays_;
  Buffer trace_records_, trace_counter_, trace_probes_device_;
  Buffer trace_raw_, trace_raw_counter_, trace_colour_, trace_colour_counter_, capture_case_;
  DeepCompactTraceParameters trace_ = {};
  DeepNativeCaptureVector<uint64_t> trace_probes_;
  DeepNativeCaptureVector<double> witness_cuts_;
  DeepCompactWitnessParameters witness_ = {};
  bool witness_configured_ = false;
  std::string witness_root_;
  DeepNativeCaptureVector<std::shared_ptr<PendingSimpleTile>> pending_simple_;
  DeepNativeCaptureVector<RawBytesPtr> preparing_summaries_;
  bool preparing_simple_ = false, beauty_denoised_ = false;
  uint32_t tile_cameras_ = 0;
  DeepGPUFrameOperation last_operation_ = DeepGPUFrameOperation::Count;
  bool active_ = false;
  std::function<bool()> cancel_poll_;
  device_vector<uint32_t> controls_;
  device_vector<uint8_t> native_parameters_, reduce_parameters_, grid_parameters_, encode_parameters_;
  device_vector<uint8_t> consumer_parameters_;
  device_vector<uint8_t> witness_parameters_;
  device_vector<uint8_t> trace_parameters_;
  std::array<size_t, 7> metadata_charges_ = {};
};

CCL_NAMESPACE_END
