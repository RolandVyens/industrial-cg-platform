/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/memory.h"
#include "device/queue.h"
#include "kernel/integrator/state.h"
#include "util/vector.h"

#include "integrator/deep_frame_capture.h"
#include <array>
#include <functional>
#include <optional>

CCL_NAMESPACE_BEGIN

class DeviceScene;
class RenderBuffers;
class DeepGPUFrameWork;

/* M2 diagnostic owner, invoked only with a closed, successful Camera + Surface input domain.
 * Native Beauty descriptor and allocation ownership remain unchanged. No production output. */
class DeepOpticalWork {
 public:
  explicit DeepOpticalWork(Device *device);
  DeepOpticalWork(Device *device, DeviceQueue *borrowed_queue, const DeepMemoryBudget &budget);
  DeepOpticalWork(Device *device,
                  DeviceQueue *borrowed_queue,
                  const DeepMemoryBudget &host_budget,
                  const DeepMemoryBudget &device_budget);
  ~DeepOpticalWork();
  /* Actual in-memory camera input. This first owned seam qualifies native
   * replay/ownership; complete coverage remains separate from queue completion. */
  bool replay_owned(DeviceScene *scene,
                    const IntegratorStateGPU &beauty,
                    DeepNativeCaptureBatch &capture,
                    const bool *cancel,
                    RenderBuffers *beauty_buffers,
                    bool full_coverage = false,
                    const std::function<bool()> &cancel_poll = {});
  bool replay_device(DeviceScene *scene,
                      const IntegratorStateGPU &beauty,
                      DeepGPUFrameWork &capture,
                      const bool *cancel,
                      RenderBuffers *beauty_buffers,
                      const std::function<bool()> &cancel_poll = {});
  bool replay(DeviceScene *scene,
              const IntegratorStateGPU &beauty,
              const string &camera_path,
              const string &surface_path,
              uint64_t receipt_count,
              uint64_t surface_count,
              const bool *cancel,
              RenderBuffers *beauty_buffers);

 private:
  bool allocate(DeviceScene *scene, int capacity, int event_capacity,
                bool full_coverage = false, bool compact = false);
  bool run_chunk(int count,
                 const bool *cancel,
                 DeepNativeCaptureBatch *capture = nullptr,
                 size_t camera_offset = 0,
                 const std::function<bool()> &cancel_poll = {},
                 DeepGPUFrameWork *device_capture = nullptr);
  bool fail(const string &message);
  void release_allocations();

  template<typename T> unique_ptr<device_memory> allocate_soa(const int capacity)
  {
    const size_t host_bytes = sizeof(device_only_memory<T>);
    const size_t device_bytes = size_t(capacity) * sizeof(T);
    if (budget_)
      budget_->acquire(host_bytes);
    bool device_reserved = false;
    try {
      if (device_budget_) {
        device_budget_->acquire(device_bytes);
        device_reserved = true;
      }
      auto memory = make_unique<device_only_memory<T>>(device_, "deep_optical_soa");
      memory->alloc_to_device(capacity);
      allocation_charge_ += budget_ ? host_bytes : 0;
      device_allocation_charge_ += device_reserved ? device_bytes : 0;
      return memory;
    }
    catch (...) {
      if (budget_)
        budget_->relinquish(host_bytes);
      if (device_reserved)
        device_budget_->relinquish(device_bytes);
      throw;
    }
  }

  Device *device_;
  unique_ptr<DeviceQueue> owned_queue_;
  DeviceQueue *queue_;
  std::optional<DeepMemoryBudget> budget_;
  std::optional<DeepMemoryBudget> device_budget_;
  size_t allocation_charge_ = 0;
  size_t device_allocation_charge_ = 0;
  size_t event_capacity_ = 0;
  IntegratorStateGPU state_ = {};
  /* No reallocating controller vector or temporary name pointers. */
  std::array<unique_ptr<device_memory>, 512> soa_;
  size_t soa_count_ = 0;
  device_vector<IntegratorQueueCounter> counter_;
  device_vector<DeepCameraReceipt> inputs_;
  device_vector<DeepOpticalEvent> events_;
  device_vector<DeepOpticalLedger> ledger_;
  device_vector<int> indices_;
  device_vector<int> index_count_;
  device_vector<DeepOwnedOpticalPiece> owned_pieces_;
  device_vector<DeepOwnedOpticalVisibility> owned_visibility_;
  device_vector<DeepOwnedOpticalCoverage> owned_coverage_;
  device_vector<DeepOpticalCheckpoint> owned_checkpoints_;
  device_only_memory<uint64_t> validation_total_;
  device_vector<DeepCompactPrimitive> compact_primitives_;
};

CCL_NAMESPACE_END
