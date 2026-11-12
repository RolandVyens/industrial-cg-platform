/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_OPTIX

#  include "device/cuda/queue.h"

CCL_NAMESPACE_BEGIN

class OptiXDevice;

/* Base class for CUDA queues. */
class OptiXDeviceQueue : public CUDADeviceQueue {
 public:
  OptiXDeviceQueue(OptiXDevice *device);
  ~OptiXDeviceQueue() override;

  void init_execution() override;

  bool enqueue(DeviceKernel kernel,
               const int work_size,
               const DeviceKernelArguments &args) override;
  bool synchronize() override;

 private:
  /* Launch argument snapshots stay pinned and unchanged until this stream drains. */
  bool parameter_staging_ = false;
  bool parameter_staging_requires_deep_policy_ = false;
  std::array<void *, 64> parameter_blocks_ = {};
  size_t parameter_block_count_ = 0;
  size_t parameter_cursor_ = 0;
  const void *stage_parameter(const void *source, size_t bytes);
};

CCL_NAMESPACE_END

#endif /* WITH_OPTIX */
