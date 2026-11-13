/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_CUDA

#  include "device/memory.h"
#  include "device/deep_queue_memory.h"
#  include <array>
#  include "device/queue.h"

#  include "device/cuda/deep_launch.h"
#  include "device/cuda/util.h"

CCL_NAMESPACE_BEGIN

class CUDADevice;
class device_memory;
class CUDADeepCapture;

/* Base class for CUDA queues. */
class CUDADeviceQueue : public DeviceQueue {
 public:
  CUDADeviceQueue(CUDADevice *device, size_t extra_auxiliary_capacity = 0);
  ~CUDADeviceQueue() override;

  int num_concurrent_states(const size_t state_size) const override;
  int num_concurrent_busy_states(const size_t state_size) const override;

  void init_execution() override;
  void run_with_deep_context(ContextCallback callback, void *data) override;
  void load_image_info() override;

  bool enqueue(DeviceKernel kernel,
               const int work_size,
               const DeviceKernelArguments &args) override;

  bool synchronize() override;
  bool synchronize_for_state_rebind() override;

  void zero_to_device(device_memory &mem) override;
  void copy_to_device(device_memory &mem) override;
  void copy_from_device(device_memory &mem) override;
  void *copy_from_device_synchronized(device_memory &mem, vector<uint8_t> &storage) override;

  bool supports_device_to_device_copy() const override
  {
    return true;
  }
  bool copy_device_to_device(device_memory &destination,
                             device_memory &source,
                             size_t bytes) override;

  virtual CUstream stream()
  {
    return cuda_stream_;
  }

  bool supports_deep_frame_reduction() const override;
  bool enqueue_deep_frame_reduction(const DeepGPUFrameReduceParameters &parameters) override;
  bool enqueue_deep_frame_operation(DeepGPUFrameOperation operation,
                                    const DeepGPUFrameDispatch &parameters,
                                    uint32_t blocks,
                                    uint32_t threads) override;
  bool enqueue_deep_bounded(DeepBoundedOperation operation,
                            const DeepBoundedDispatch &parameters,
                            uint32_t blocks,
                            uint32_t threads) override;
  void *deep_pin(size_t bytes) override;
  void deep_unpin(void *pointer) override;
  bool deep_zero(uint64_t dst, size_t bytes) override;
  bool deep_mark_compact(unsigned slot) override;
  bool deep_copy_begin(unsigned slot) override;
  bool deep_copy_plane(void *dst, uint64_t src, size_t bytes) override;
  bool deep_copy_end(unsigned slot, bool payload) override;
  int deep_copy_ready(unsigned slot, bool payload, bool wait) override;
  bool deep_copy_join() override;
  int deep_copy_peer(uint64_t dst, Device &source, uint64_t src, size_t bytes) override;
  bool copy_device_region(device_memory &destination,
                           device_memory &source,
                           size_t destination_offset,
                           size_t source_offset,
                           size_t bytes) override;
  bool read_device_region(device_memory &source, size_t source_offset,
                           void *destination, size_t bytes) override;
  bool write_device_region(device_memory &destination, size_t destination_offset,
                             const void *source, size_t bytes) override;

  bool supports_deep_capture_processing() const override
  {
    return true;
  }
  bool process_deep_capture(device_memory &events,
                            const vector<uint32_t> &counts,
                            vector<uint32_t> &pixel_offsets,
                            vector<uint8_t> &event_storage,
                            const vector<uint32_t> *pixel_selection = nullptr) override;
  bool supports_deep_volume_capture() const override
  {
    return true;
  }
  DeepVolumeCaptureResult process_deep_volume_capture(
      const vector<device_memory *> &batches,
      const vector<vector<uint32_t>> &counts,
      const vector<float> &population,
      const vector<float> &beauty,
      int max_profile_samples,
      vector<uint32_t> &profile_offsets,
      vector<uint8_t> &profile_storage,
      vector<uint32_t> &fallback_pixels) override;

  unique_ptr<DeviceGraphicsInterop> graphics_interop_create() override;

 protected:
  CUDADevice *cuda_device_;
  CUstream cuda_stream_;
  unique_ptr<CUDADeepCapture> deep_capture_;
  CUmodule deep_frame_module_ = nullptr;
  CUfunction deep_frame_prefix_ = nullptr, deep_frame_transmission_ = nullptr;
  CUfunction deep_frame_operations_[size_t(DeepGPUFrameOperation::Count)] = {};
  CUfunction deep_frame_encode_operations_[4] = {};
  CUstream deep_copy_stream_ = nullptr;
  CUevent deep_compact_done_[2] = {}, deep_header_done_[2] = {}, deep_copy_done_[2] = {};
  CUmodule deep_bounded_module_ = nullptr;
  CUfunction deep_shader_prefix_sum_ = nullptr;
  bool bounded_parallel_shader_prefix_ = true;
  bool load_deep_bounded_module();
  CUfunction deep_bounded_functions_[3][size_t(DeepBoundedOperation::COUNT)] = {};
  bool load_deep_frame_module();

  struct NativeKernelTiming {
    DeviceKernel kernel;
    int work;
    CUevent begin = nullptr, end = nullptr;
    uint32_t bounded_operation = 0;
  };
  bool native_timings_enabled_ = false;
  vector<NativeKernelTiming> native_timings_;
  bool begin_native_timing(DeviceKernel kernel, int work);
  bool end_native_timing();
  void finish_native_timings();

  struct SmallReadback {
    void *destination;
    size_t offset;
    size_t bytes;
  };
  bool small_readbacks_enabled_ = false;
  bool small_readbacks_require_deep_policy_ = false;
  bool bounded_context_scope_ = true;
  int bounded_shadow_block_ = 0, bounded_physical_shadow_block_ = 0;
  int bounded_numeric_block_ = 0;
  DeepNativeLaunchScale bounded_native_scale_;
  bool bounded_adaptive_blocks_ = false;
  bool bounded_adaptive_all_modes_ = false;
  bool bounded_native_scale_ready_ = false;
  bool bounded_shadow_block_override_ = false;
  void *small_readback_buffer_ = nullptr;
  size_t small_readback_cursor_ = 0;
  std::array<SmallReadback, 16> small_readbacks_;
  size_t small_readback_count_ = 0;
  void publish_small_readbacks();

 protected:
  DeepQueueAuxiliaryBudget deep_auxiliary_budget_;
  void *allocate_deep_auxiliary(size_t bytes);
  void free_deep_auxiliary(void *pointer, size_t bytes);

  void assert_success(CUresult result, const char *operation);
};

CCL_NAMESPACE_END

#endif /* WITH_CUDA */
