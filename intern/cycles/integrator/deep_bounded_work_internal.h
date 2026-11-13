/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "integrator/deep_bounded_work.h"
#include "integrator/deep_bounded_publication.h"
#include "integrator/deep_bounded_plan.h"
#include "device/device.h"
#include "device/memory.h"
#include "device/queue.h"
#include "kernel/integrator/state.h"
#include "session/buffers.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <cstdio>

CCL_NAMESPACE_BEGIN

class BoundedDeviceBytes {
 public:
  BoundedDeviceBytes(Device &device, const char *name, const DeepMemoryBudget &budget)
      : memory(&device, name), budget_(budget)
  {
  }
  ~BoundedDeviceBytes()
  {
    if (!quarantined_) {
      memory.free();
      budget_.relinquish(bytes_);
    }
  }
  bool reserve(const size_t bytes)
  {
    if (bytes <= bytes_) {
      return true;
    }
    budget_.acquire(bytes - bytes_);
    bytes_ = bytes;
    memory.alloc_to_device(bytes);
    return memory.device_pointer != 0;
  }
  uint64_t address() const { return memory.device_pointer; }
  size_t bytes() const { return bytes_; }
  void quarantine()
  {
    quarantined_ = true;
    memory.device_pointer = 0;
  }

  device_only_memory<uint8_t> memory;

 private:
  DeepMemoryBudget budget_;
  size_t bytes_ = 0;
  bool quarantined_ = false;
};

struct DeepBoundedDeviceStorage {
  DeepBoundedDeviceStorage(Device &device, const DeepMemoryBudget &budget)
      : budget(budget), sources(&device, "Deep bounded source identity"),
        collector_mem(device, "Deep bounded collector", budget),
        phase(device, "Deep bounded phase arena", budget),
        encoded_a(device, "Deep bounded encoded A", budget),
        encoded_b(device, "Deep bounded encoded B", budget),
        control(device, "Deep bounded frame control", budget),
        native_active(device, "Deep bounded adaptive camera list", budget) {}
  ~DeepBoundedDeviceStorage()
  {
    if (!quarantined) {
      sources.free();
      budget.relinquish(source_charge);
    }
    else {
      sources.device_pointer = 0;
      collector_mem.quarantine(); phase.quarantine(); encoded_a.quarantine();
      encoded_b.quarantine(); control.quarantine(); native_active.quarantine();
    }
  }
  const DeepMemoryBudget &budget;
  device_only_memory<uint8_t> sources;
  size_t source_charge = 0;
  uint32_t native_states = 0;
  BoundedDeviceBytes collector_mem, phase, encoded_a, encoded_b, control, native_active;
  bool quarantined = false;
};

struct DeepBoundedCopyPosition {
  /* Teardown joins every touched execution domain, even when its following
   * event submission fails. These mask bits are not ordered CopyStage states. */
  static constexpr uint32_t COMPUTE_TOUCHED = 1u << 30;
  static constexpr uint32_t COPY_TOUCHED = 1u << 31;
  enum class Stage { Free, Compacted, Header, Payload };
  Stage stage = Stage::Free;
  DeepBoundedEncoding encoding;
  uint64_t epoch = 0;
  uint32_t submitted_stages = 0, submitted_planes = 0;
  size_t consumer = 0;

  void mark(const DeepBoundedCopyStage stage)
  {
    submitted_stages |= 1u << uint32_t(stage);
  }

  bool recorded(const DeepBoundedCopyStage stage) const
  {
    return (submitted_stages & (1u << uint32_t(stage))) != 0;
  }
  void *pinned = nullptr;
  size_t pinned_bytes = 0, transfer_bytes = 0;
  std::shared_ptr<DeepBoundedHostPayload> payload;
};

/* Copy and ready ownership are Work-local until the PathTrace owner publishes.
 * This is composed by value inside the existing PIMPL, not another heap layer. */
struct DeepBoundedTransportStorage {
  explicit DeepBoundedTransportStorage(const DeepMemoryBudget &budget)
      : output_aliases(DeepBudgetAllocator<size_t>(budget)) {}
  DeepBoundedCopyPosition copies[2];
  std::unique_ptr<DeepBoundedHostTile> output;
  DeepBoundedPublicationSlot ready;
  std::vector<size_t, DeepBudgetAllocator<size_t>> output_aliases;
  uint64_t output_epoch = 0;
  bool copy_failed = false;
};

struct DeepBoundedWorkStorage {
  DeepBoundedWorkStorage(Device &device, DeviceQueue &queue, DeepFrameCaptureConsumer &consumer)
      : device(device),
        queue(queue),
        consumer(consumer),
        budget(consumer.device_memory_budget()),
        policy(DeepBoundedPolicy(device.deep_bounded_policy())),
        encoding_plan(deep_bounded_encoding_plan(consumer.bounded_consumers(), policy)),
        resources(device, budget),
        transport(consumer.memory_budget())
  {
    const char *identity = getenv("CYCLES_TEST_DEEP_COMPACT_IDENTITY");
    compact_identity_requested = !identity || std::strcmp(identity, "0") != 0;
    const char *pack = getenv("CYCLES_TEST_DEEP_PACK_ACTIVE_PIXELS");
    pack_active_pixels = !pack || std::strcmp(pack, "0") != 0;
    const char *exact = getenv("CYCLES_TEST_DEEP_EXACT_PACKET_ADMISSION");
    exact_admission = !exact || std::strcmp(exact, "0") != 0;
    const char *overwrite = getenv("CYCLES_TEST_DEEP_OVERWRITE_RECEIVE");
    overwrite_receive = !overwrite || std::strcmp(overwrite, "0") != 0;

  }

  ~DeepBoundedWorkStorage()
  {
    if (quarantined) resources.quarantined = true;
  }

  Device &device;
  DeviceQueue &queue;
  DeepFrameCaptureConsumer &consumer;
  DeepMemoryBudget budget;
  const DeepBoundedPolicy policy;
  const DeepBoundedEncodingPlan encoding_plan;
  DeepBoundedDeviceStorage resources;
  DeepBoundedTransportStorage transport;
  bool compact_identity_requested = false, compact_identity = false;
  bool pack_active_pixels = false;
  bool exact_admission = false;
  bool overwrite_receive = true;
  uint32_t root_pitch = 0;
  uint32_t native_count = 0, native_columns = 0;
  uint32_t native_width = 0;
  int32_t native_x = 0, native_y = 0, native_offset = 0;
  uint32_t native_stride = 0;
  DeepBoundedAdmission admission;
  DeepBoundedBinding capture = {};
  DeepBoundedCollector collector;
  DeepBoundedModel models[2];
  DeepBoundedMergePolicy strict = {0.01f, 0.01f, 0.006f, 0.002f, 0.0f};
  std::array<int64_t, 4> tile = {};
  uint32_t families = 0;
  uint32_t admitted_samples = UINT32_MAX;
  size_t admitted_bytes = SIZE_MAX, admitted_auxiliary_growth = SIZE_MAX;
  uint64_t scan_scratch = 0;
  uint64_t epoch = 0;
  bool first_tile_packet = true, first_frame_packet = true;
  bool prepared = false;
  bool quarantined = false;
};


bool deep_bounded_progress_copy(DeepBoundedWorkStorage &owner, unsigned index, bool wait);
bool deep_bounded_pump_copies(DeepBoundedWorkStorage &owner, bool wait);

CCL_NAMESPACE_END
