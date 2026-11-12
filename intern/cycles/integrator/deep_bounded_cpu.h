/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>

#include "integrator/deep_frame_capture.h"
#include "kernel/integrator/deep_bounded_types.h"

CCL_NAMESPACE_BEGIN

class Device;
class RenderBuffers;
class BufferParams;
struct DeepBoundedCPUStorage;

struct DeepCPUAdmission {
  size_t arena = 0, scratch = 0, output_credit = 0, peak = 0;
};

/* No allocations or native work: checked bounds for the actual CPU layout. */
bool deep_cpu_admission(const DeepFrameCaptureConsumer &consumer,
                        uint32_t pixels,
                        uint32_t threads,
                        DeepCPUAdmission &result);

/* Synchronous CPU adapter. Each native pixel owns its collector; one thread
 * borrows one raw root until main/shadow/AO/MNEE have all returned. */
class DeepBoundedCPUWork {
 public:
  DeepBoundedCPUWork(Device &device, DeepFrameCaptureConsumer &consumer);
  ~DeepBoundedCPUWork();
  DeepBoundedCPUWork(const DeepBoundedCPUWork &) = delete;
  DeepBoundedCPUWork &operator=(const DeepBoundedCPUWork &) = delete;

  bool configure(const BufferParams &logical,
                 const BufferParams &slice,
                 uint32_t threads,
                 bool holdout,
                 float projection_scale);
  bool owns(int x, int y) const;
  const DeepBoundedBinding *begin_sample(uint32_t worker, int x, int y, int sample);
  bool end_sample(uint32_t worker);
  bool prepare_tile();
  bool close_tile();
  bool set_tile_beauty(const RenderBuffers &buffers);
  bool drain();
  bool good() const;
  uint32_t error_flags() const;
  uint32_t policy() const;

 private:
  std::unique_ptr<DeepBoundedCPUStorage> storage_;
};

CCL_NAMESPACE_END
