/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>

#include "integrator/deep_bounded_layout.h"
#include "integrator/deep_frame_capture.h"
#include "integrator/deep_bounded_plan.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceQueue;
class RenderBuffers;
class BufferParams;
struct IntegratorStateGPU;
struct DeepBoundedWorkStorage;

bool deep_bounded_admit_host(DeepFrameCaptureConsumer &consumer,
                             const BufferParams &frame,
                             size_t tile_pixels,
                             size_t devices,
                             size_t available_ram,
                             bool &retain_native_film,
                             size_t cpu_threads = 0);

/* Per-device owner. Raw memory is one packet; collector is one logical Tile.
 * A packet can retire only after the caller closes every native work queue.
 * Final encoded positions have separate lifetimes from the phase arena. */
class DeepBoundedWork {
 public:
  DeepBoundedWork(Device &device, DeviceQueue &queue, DeepFrameCaptureConsumer &consumer);
  ~DeepBoundedWork();
  DeepBoundedWork(const DeepBoundedWork &) = delete;
  DeepBoundedWork &operator=(const DeepBoundedWork &) = delete;

  DeepBoundedDeviceAdmission admission_snapshot(uint32_t native_states, bool compact_range) const;
  void set_admission_limit(uint32_t samples, size_t arena_bytes, size_t auxiliary_growth);
  bool bind_sources(uint32_t native_states, IntegratorStateGPU &state);

  bool configure(const BufferParams &logical,
                 const BufferParams &slice,
                 uint32_t native_states,
                 uint32_t max_samples,
                 bool has_holdout,
                 float projection_scale,
                 IntegratorStateGPU &state,
                 bool identity_range_fits = false);
  bool prepare_work(int start,
                    uint32_t samples,
                    const RenderBuffers &buffers,
                    int adaptive_pass,
                    BufferParams &camera_params);
  uint32_t packet_samples(uint32_t remaining) const;
  bool has_owned_pixels() const;
  bool begin_packet(int start,
                    uint32_t samples,
                    int sample_offset,
                    IntegratorStateGPU &state,
                    bool keep_shadow_queue,
                    bool track_camera);
  bool accumulate_packet();
  bool prepare_tile();
  bool close_tile();
  bool set_tile_beauty(const RenderBuffers &buffers);
  /* Work-local DMA retirement only; never writes shared Production. */
  bool drain();
  /* PathTrace owner only, after Work joins. Does not wait for DMA. */
  bool publish_ready();
  void after_native_init();
  bool migrate(DeepBoundedWork &destination,
               int x,
               int y,
               uint32_t width,
               uint32_t height);
  void discard(IntegratorStateGPU &state);
  bool good() const;

 private:
  std::unique_ptr<DeepBoundedWorkStorage> storage_;
};

CCL_NAMESPACE_END
