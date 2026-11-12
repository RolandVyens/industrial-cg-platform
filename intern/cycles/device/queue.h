/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/kernel.h"

#include "kernel/integrator/deep_gpu_frame.h"
#include "kernel/integrator/deep_bounded_types.h"

#include "device/graphics_interop.h"
#include "util/log.h"
#include "util/map.h"
#include "util/string.h"
#include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

class Device;
class device_memory;

enum class DeepVolumeCaptureResult { Complete, Unsupported, Error };

struct KernelWorkTile;

/* Container for device kernel arguments with type correctness ensured by API. */
struct DeviceKernelArguments {

  enum Type {
    POINTER,
    INT32,
    FLOAT32,
    KERNEL_FILM_CONVERT,
    HIPRT_GLOBAL_STACK,
  };

  static const int MAX_ARGS = 19;
  Type types[MAX_ARGS];
  void *values[MAX_ARGS];
  size_t sizes[MAX_ARGS];
  size_t count = 0;

  DeviceKernelArguments() = default;

  template<class T> DeviceKernelArguments(const T *arg)
  {
    add(arg);
  }

  template<class T, class... Args> DeviceKernelArguments(const T *first, Args... args)
  {
    add(first);
    add(args...);
  }

  void add(const KernelFilmConvert *value)
  {
    add(KERNEL_FILM_CONVERT, value, sizeof(KernelFilmConvert));
  }
  void add(const device_ptr *value)
  {
    add(POINTER, value, sizeof(device_ptr));
  }
  void add(const int32_t *value)
  {
    add(INT32, value, sizeof(int32_t));
  }
  void add(const float *value)
  {
    add(FLOAT32, value, sizeof(float));
  }
  void add(const Type type, const void *value, const size_t size)
  {
    assert(count < MAX_ARGS);

    types[count] = type;
    values[count] = (void *)value;
    sizes[count] = size;
    count++;
  }
  template<typename T, typename... Args> void add(const T *first, Args... args)
  {
    add(first);
    add(args...);
  }
};

/* Abstraction of a command queue for a device.
 * Provides API to schedule kernel execution in a specific queue with minimal possible overhead
 * from driver side.
 *
 * This class encapsulates all properties needed for commands execution. */
class DeviceQueue {
 public:
  virtual bool enqueue_deep_bounded(DeepBoundedOperation,
                                    const DeepBoundedDispatch &,
                                    uint32_t,
                                    uint32_t = 256)
  {
    return false;
  }
  virtual ~DeviceQueue();

  /* Number of concurrent states to process for integrator,
   * based on number of cores and/or available memory. */
  virtual int num_concurrent_states(const size_t state_size) const = 0;

  /* Number of states which keeps the device occupied with work without losing performance.
   * The renderer will add more work (when available) when number of active paths falls below this
   * value. */
  virtual int num_concurrent_busy_states(const size_t state_size) const = 0;

  /* Number of partitions of sorted shaders, that improves memory locality of
   * integrator state fetch at the cost of decreased coherence for shader kernel execution. */
  virtual int num_sort_partitions(int max_num_paths, uint max_scene_shaders) const
  {
    /* Sort partitioning becomes less effective when more shaders are in the wavefront. In lieu of
     * a more sophisticated heuristic we simply disable sort partitioning if the shader count is
     * high.
     */
    if (max_scene_shaders < 300) {
      return max(max_num_paths / 65536, 1);
    }
    else {
      return 1;
    }
  }

  /* Does device support local atomic sorting kernels (INTEGRATOR_SORT_BUCKET_PASS and
   * INTEGRATOR_SORT_WRITE_PASS)? */
  virtual bool supports_local_atomic_sort() const
  {
    return false;
  }

  /* Initialize execution of kernels on this queue.
   *
   * Will, for example, load all data required by the kernels from Device to global or path state.
   *
   * Use this method after device synchronization has finished before enqueueing any kernels. */
  virtual void init_execution() = 0;

  /* Invoke synchronously on this host thread. The callback and its stack
   * capture are never retained; backends may hold their context around it. */
  using ContextCallback = void (*)(void *);
  virtual void run_with_deep_context(ContextCallback callback, void *data)
  {
    callback(data);
  }

  /* Update device-specific image state after allocating device_image. */
  virtual void load_image_info() = 0;

  /* Enqueue kernel execution.
   *
   * Execute the kernel work_size times on the device.
   * Supported arguments types:
   * - int: pass pointer to the int
   * - device memory: pass pointer to device_memory.device_pointer
   * Return false if there was an error executing this or a previous kernel. */
  virtual bool enqueue(DeviceKernel kernel,
                       const int work_size,
                       const DeviceKernelArguments &args) = 0;

  /* Wait unit all enqueued kernels have finished execution.
   * Return false if there was an error executing any of the enqueued kernels. */
  virtual bool synchronize() = 0;

  /* Drain before changing device-owned state pointers, even with a latched logical error.
   * Ordinary synchronize() may short-circuit on that error while a prior launch is in flight.
   * Default unsupported; the diagnostic owner admits native CUDA/OptiX only. */
  virtual bool synchronize_for_state_rebind()
  {
    return false;
  }

  /* Copy memory to/from device as part of the command queue, to ensure
   * operations are done in order without having to synchronize. */
  virtual void zero_to_device(device_memory &mem) = 0;
  virtual void copy_to_device(device_memory &mem) = 0;
  virtual void copy_from_device(device_memory &mem) = 0;
  virtual void *copy_from_device_synchronized(device_memory &mem, vector<uint8_t> &storage) = 0;

  /* Optional same-device prefix copy. The caller must synchronize before reusing either buffer. */
  virtual bool supports_device_to_device_copy() const
  {
    return false;
  }

  /* Device-resident Deep numerical work; no input or output host materialization.
   * Input owners, prefix scratch and output/status buffers outlive queued kernels. */
  virtual bool supports_deep_frame_reduction() const
  {
    return false;
  }
  virtual bool enqueue_deep_frame_reduction(const DeepGPUFrameReduceParameters & /*parameters*/)
  {
    return false;
  }
  virtual bool enqueue_deep_frame_operation(DeepGPUFrameOperation /*operation*/,
                                            const DeepGPUFrameDispatch & /*parameters*/,
                                            uint32_t /*blocks*/,
                                            uint32_t /*threads*/)
  {
    return false;
  }
  /* Bounded Deep has two independent encoded leases on a single copy stream. */
  virtual void *deep_pin(size_t) { return nullptr; }
  virtual void deep_unpin(void *) {}
  virtual bool deep_zero(uint64_t, size_t) { return false; }
  virtual bool deep_mark_compact(unsigned) { return false; }
  virtual bool deep_copy_begin(unsigned) { return false; }
  virtual bool deep_copy_plane(void *, uint64_t, size_t) { return false; }
  virtual bool deep_copy_end(unsigned, bool) { return false; }
  /* 1 = complete, 0 = pending, -1 = failed. Only query recorded events. */
  virtual int deep_copy_ready(unsigned, bool, bool) { return -1; }
  virtual bool deep_copy_join() { return false; }
  /* 1 = peer DMA submitted, 0 = host staging required, -1 = failed. */
  virtual int deep_copy_peer(uint64_t, Device &, uint64_t, size_t) { return 0; }

  virtual bool copy_device_region(device_memory &destination,
                                   device_memory &source,
                                   size_t destination_offset,
                                   size_t source_offset,
                                   size_t bytes)
  {
    return !destination_offset && !source_offset &&
           copy_device_to_device(destination, source, bytes);
  }
  virtual bool read_device_region(device_memory & /*source*/,
                                   size_t /*source_offset*/,
                                   void * /*destination*/,
                                   size_t /*bytes*/)
  {
    return false;
  }
  virtual bool write_device_region(device_memory & /*destination*/,
                                    size_t /*destination_offset*/,
                                    const void * /*source*/,
                                    size_t /*bytes*/)
  {
    return false;
  }
  virtual bool copy_device_to_device(device_memory & /*destination*/,
                                     device_memory & /*source*/,
                                     const size_t /*bytes*/)
  {
    return false;
  }

  /* Optional experimental processing of a complete resident raw Deep batch. Output retains
   * per-pixel append order and full payloads; the caller still owns depth sorting/output. */
  virtual bool supports_deep_capture_processing() const
  {
    return false;
  }
  virtual bool process_deep_capture(device_memory & /*events*/,
                                    const vector<uint32_t> & /*counts*/,
                                    vector<uint32_t> & /*pixel_offsets*/,
                                    vector<uint8_t> & /*event_storage*/,
                                    const vector<uint32_t> * /*pixel_selection*/ = nullptr)
  {
    return false;
  }

  virtual bool supports_deep_volume_capture() const
  {
    return false;
  }
  virtual DeepVolumeCaptureResult process_deep_volume_capture(
      const vector<device_memory *> & /*batches*/,
      const vector<vector<uint32_t>> & /*counts*/,
      const vector<float> & /*population*/,
      const vector<float> & /*beauty*/,
      int /*max_profile_samples*/,
      vector<uint32_t> & /*profile_offsets*/,
      vector<uint8_t> & /*profile_storage*/,
      vector<uint32_t> & /*fallback_pixels*/)
  {
    return DeepVolumeCaptureResult::Unsupported;
  }

  /* Graphics resources interoperability.
   *
   * The interoperability comes here by the meaning that the device is capable of computing result
   * directly into an OpenGL (or other graphics library) buffer. */

  /* Create graphics interoperability context which will be taking care of mapping graphics
   * resource as a buffer writable by kernels of this device. */
  virtual unique_ptr<DeviceGraphicsInterop> graphics_interop_create()
  {
    LOG_FATAL << "Request of GPU interop of a device which does not support it.";
    return nullptr;
  }

  /* Device this queue has been created for. */
  Device *device = nullptr;

  virtual void *native_queue()
  {
    return nullptr;
  }

 protected:
  /* Hide construction so that allocation via `Device` API is enforced. */
  explicit DeviceQueue(Device *device);

  /* Implementations call these from the corresponding methods to generate debugging logs. */
  void debug_init_execution();
  void debug_enqueue_begin(DeviceKernel kernel, const int work_size);
  void debug_enqueue_end();
  void debug_synchronize();
  string debug_active_kernels();

  /* Combination of kernels enqueued together sync last synchronize. */
  DeviceKernelMask last_kernels_enqueued_ = {false};
  /* Time of synchronize call. */
  double last_sync_time_ = 0.0;
  /* Accumulated execution time for combinations of kernels launched together. */
  map<DeviceKernelMask, double> stats_kernel_time_;
  /* If it is true, then a performance statistics in the debugging logs will have focus on kernels
   * and an explicit queue synchronization will be added after each kernel execution. */
  bool is_per_kernel_performance_ = false;
};

CCL_NAMESPACE_END
