/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_optical_work.h"
#include "integrator/deep_gpu_frame_work.h"
#include "integrator/deep_state_binding.h"
#include "device/device.h"
#include "integrator/deep_camera_receipt_writer.h"
#include "integrator/deep_input_receipt_reader.h"
#include "scene/scene.h"
#include "session/buffers.h"
#include "util/string.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

CCL_NAMESPACE_BEGIN

namespace {
size_t optical_soa_stride(const uint kernel_features, const int stack_size)
{
  size_t bytes = 0;
#define KERNEL_STRUCT_BEGIN(name)
#define KERNEL_STRUCT_END(name)
#define KERNEL_STRUCT_MEMBER(parent, type, name, feature) \
  if (KernelFeatureRequest(feature).test(kernel_features)) bytes += sizeof(type);
#define KERNEL_STRUCT_ARRAY_MEMBER(parent, type, name, feature) \
  if (KernelFeatureRequest(feature).test(kernel_features)) bytes += sizeof(type) * size_t(stack_size);
#define KERNEL_STRUCT_END_ARRAY(name, cpu_size, gpu_size)
#define KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR KERNEL_STRUCT_MEMBER
#ifdef __INTEGRATOR_GPU_PACKED_STATE__
#  define KERNEL_STRUCT_BEGIN_PACKED(parent, feature) \
    KERNEL_STRUCT_MEMBER(parent, packed_##parent, packed, feature)
#  define KERNEL_STRUCT_MEMBER_PACKED(parent, type, name, feature)
#else
#  define KERNEL_STRUCT_BEGIN_PACKED(parent, feature)
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#endif
#include "kernel/integrator/state_template.h"
#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER_PACKED
  return bytes;
}

template<typename T>
void reserve_optical_append(DeepNativeCaptureVector<T> &values, const size_t additional)
{
  const size_t limit = values.max_size();
  if (additional > limit - values.size())
    throw std::bad_alloc();
  const size_t needed = values.size() + additional;
  if (needed <= values.capacity())
    return;
  const size_t grown = values.capacity() > limit / 2 ? limit : values.capacity() * 2;
  const size_t desired = std::max(needed, grown);
  try {
    values.reserve(desired);
  }
  catch (const std::bad_alloc &) {
    if (needed == desired)
      throw;
    values.reserve(needed);
  }
}
}  // namespace

DeepOpticalWork::DeepOpticalWork(Device *device)
    : device_(device),
      owned_queue_(device->gpu_queue_create()),
      queue_(owned_queue_.get()),
      counter_(device, "optical_counter", MEM_DEVICE_ONLY),
      inputs_(device, "optical_inputs", MEM_DEVICE_ONLY),
      events_(device, "optical_events", MEM_DEVICE_ONLY),
      ledger_(device, "optical_ledger", MEM_DEVICE_ONLY),
      indices_(device, "optical_active_indices", MEM_DEVICE_ONLY),
      index_count_(device, "optical_index_count", MEM_DEVICE_ONLY),
      owned_pieces_(device, "owned_optical_pieces", MEM_DEVICE_ONLY),
      owned_visibility_(device, "owned_optical_visibility", MEM_DEVICE_ONLY),
      owned_coverage_(device, "owned_optical_coverage", MEM_DEVICE_ONLY),
      owned_checkpoints_(device, "owned_optical_checkpoints", MEM_DEVICE_ONLY),
      validation_total_(device, "deep_gpu_optical_validation"),
      compact_primitives_(device, "deep_compact_supplement", MEM_DEVICE_ONLY)
{
}

DeepOpticalWork::DeepOpticalWork(Device *device,
                                 DeviceQueue *borrowed_queue,
                                 const DeepMemoryBudget &budget)
    : DeepOpticalWork(device, borrowed_queue, budget, budget)
{
}

DeepOpticalWork::DeepOpticalWork(Device *device,
                                 DeviceQueue *borrowed_queue,
                                 const DeepMemoryBudget &host_budget,
                                 const DeepMemoryBudget &device_budget)
    : device_(device),
      queue_(borrowed_queue),
      budget_(host_budget),
      device_budget_(device_budget),
      counter_(device, "owned_optical_counter", MEM_DEVICE_ONLY),
      inputs_(device, "owned_optical_inputs", MEM_DEVICE_ONLY),
      events_(device, "owned_optical_events", MEM_DEVICE_ONLY),
      ledger_(device, "owned_optical_ledger", MEM_DEVICE_ONLY),
      indices_(device, "owned_optical_indices", MEM_DEVICE_ONLY),
      index_count_(device, "owned_optical_index_count", MEM_DEVICE_ONLY),
      owned_pieces_(device, "owned_optical_pieces", MEM_DEVICE_ONLY),
      owned_visibility_(device, "owned_optical_visibility", MEM_DEVICE_ONLY),
      owned_coverage_(device, "owned_optical_coverage", MEM_DEVICE_ONLY),
      owned_checkpoints_(device, "owned_optical_checkpoints", MEM_DEVICE_ONLY),
      validation_total_(device, "deep_gpu_optical_validation"),
      compact_primitives_(device, "deep_compact_supplement", MEM_DEVICE_ONLY)
{
}

DeepOpticalWork::~DeepOpticalWork()
{
  release_allocations();
}

void DeepOpticalWork::release_allocations()
{
  for (auto &memory : soa_)
    memory.reset();
  soa_count_ = 0;
  counter_.free();
  inputs_.free();
  events_.free();
  ledger_.free();
  indices_.free();
  index_count_.free();
  owned_pieces_.free();
  owned_visibility_.free();
  owned_coverage_.free();
  owned_checkpoints_.free();
  validation_total_.free();
  compact_primitives_.free();
  event_capacity_ = 0;
  state_ = {};
  if (budget_ && allocation_charge_)
    budget_->relinquish(allocation_charge_);
  if (device_budget_ && device_allocation_charge_)
    device_budget_->relinquish(device_allocation_charge_);
  if (allocation_charge_ || device_allocation_charge_)
    std::fprintf(stderr,
                 "DEEP_FRAME_OPTICAL_RELEASE host_bytes=%llu device_bytes=%llu "
                 "host_used=%llu device_used=%llu\n",
                 static_cast<unsigned long long>(allocation_charge_),
                 static_cast<unsigned long long>(device_allocation_charge_),
                 static_cast<unsigned long long>(budget_->used()),
                 static_cast<unsigned long long>(device_budget_->used()));
  allocation_charge_ = 0;
  device_allocation_charge_ = 0;
}

bool DeepOpticalWork::fail(const string &message)
{
  device_->set_error("Independent Optical: " + message);
  return false;
}

bool DeepOpticalWork::allocate(DeviceScene *scene,
                               const int capacity,
                               const int event_capacity,
                               const bool full_coverage,
                               const bool compact)
{
  const uint kernel_features = scene->data.kernel_features;
  event_capacity_ = size_t(event_capacity);
  /* First ownership prototype keeps the feature-selected main layout. No shadow, sorting,
   * catcher split, Beauty arena or native admission allocation is shared. */
#define KERNEL_STRUCT_BEGIN(name) \
  for (int array_index = 0;; array_index++) {
#define KERNEL_STRUCT_MEMBER_IMPL(parent_struct, type, name, condition) \
  if ((condition) && (state_.parent_struct.name == nullptr)) { \
    auto array = allocate_soa<type>(capacity); \
    memcpy(&state_.parent_struct.name, &array->device_pointer, sizeof(array->device_pointer)); \
    if (soa_count_ == soa_.size()) { \
      return fail("independent SoA controller bound exceeded"); \
    } \
    soa_[soa_count_++] = std::move(array); \
  }
#define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
  KERNEL_STRUCT_MEMBER_IMPL( \
      parent_struct, type, name, KernelFeatureRequest(feature).test(kernel_features))
#define KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR(parent_struct, type, name, feature) \
  KERNEL_STRUCT_MEMBER_IMPL( \
      parent_struct, \
      type, \
      name, \
      shadow_state_unshadowed_throughput_is_needed(kernel_features, use_shadow_color))
#ifdef __INTEGRATOR_GPU_PACKED_STATE__
#  define KERNEL_STRUCT_MEMBER_PACKED(parent_struct, type, name, feature) \
    if ((KernelFeatureRequest(feature).test(kernel_features))) { \
      string name_str = string_printf("%sintegrator_state_" #parent_struct "_" #name, \
                                      shadow ? "shadow_" : ""); \
      LOG_TRACE << "Skipping " << name_str \
                << " -- data is packed inside integrator_state_" #parent_struct "_packed"; \
    }
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) \
    KERNEL_STRUCT_BEGIN(parent_struct) \
    KERNEL_STRUCT_MEMBER(parent_struct, packed_##parent_struct, packed, feature)
#else
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) KERNEL_STRUCT_BEGIN(parent_struct)
#endif

#define KERNEL_STRUCT_ARRAY_MEMBER(parent_struct, type, name, feature) \
  if ((KernelFeatureRequest(feature).test(kernel_features)) && \
      (state_.parent_struct[array_index].name == nullptr)) \
  { \
    auto array = allocate_soa<type>(capacity); \
    memcpy(&state_.parent_struct[array_index].name, \
           &array->device_pointer, \
           sizeof(array->device_pointer)); \
    if (soa_count_ == soa_.size()) { \
      return fail("independent SoA controller bound exceeded"); \
    } \
    soa_[soa_count_++] = std::move(array); \
  }
#define KERNEL_STRUCT_END(name) \
  (void)array_index; \
  break; \
  }
#define KERNEL_STRUCT_END_ARRAY(name, cpu_array_size, gpu_array_size) \
  if (array_index >= gpu_array_size - 1) { \
    break; \
  } \
  }
#define KERNEL_STRUCT_VOLUME_STACK_SIZE (scene->data.volume_stack_size)

  bool shadow = false;
#include "kernel/integrator/state_template.h"

#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_IMPL
#undef KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_VOLUME_STACK_SIZE

  if (budget_) {
    const size_t bytes =
        (sizeof(IntegratorQueueCounter) +
         size_t(capacity) * (sizeof(DeepCameraReceipt) + sizeof(DeepOpticalLedger) + sizeof(int)) +
         size_t(event_capacity) *
             (compact ? 0 : full_coverage ? sizeof(DeepOwnedOpticalPiece) + sizeof(DeepOwnedOpticalVisibility) :
                              sizeof(DeepOpticalEvent)) +
         (full_coverage ? size_t(capacity) *
                              (sizeof(DeepOwnedOpticalCoverage) + sizeof(DeepOpticalCheckpoint)) :
                          0) +
         sizeof(int));
    const size_t host_bytes = compact ? sizeof(IntegratorQueueCounter) + sizeof(int) : bytes;
    budget_->acquire(host_bytes);
    allocation_charge_ += host_bytes;
    device_budget_->acquire(bytes);
    device_allocation_charge_ += bytes;
  }
  counter_.alloc(1);
  if (compact) { inputs_.alloc_device_only(capacity); inputs_.alloc_to_device(); }
  else { inputs_.alloc(capacity); inputs_.copy_to_device(); }
  if (full_coverage) {
    if (!compact) {
      owned_pieces_.alloc(event_capacity);
      owned_pieces_.alloc_to_device();
      owned_visibility_.alloc(event_capacity);
      owned_visibility_.alloc_to_device();
    }
    if (compact) owned_coverage_.alloc_device_only(capacity);
    else owned_coverage_.alloc(capacity);
    owned_coverage_.alloc_to_device();
    if (compact) owned_checkpoints_.alloc_device_only(capacity);
    else owned_checkpoints_.alloc(capacity);
    owned_checkpoints_.alloc_to_device();
  }
  else {
    events_.alloc(event_capacity);
    events_.copy_to_device();
  }
  if (compact) {
    ledger_.alloc_device_only(capacity); ledger_.alloc_to_device();
    indices_.alloc_device_only(capacity); indices_.alloc_to_device();
  }
  else {
    ledger_.alloc(capacity); ledger_.copy_to_device();
    indices_.alloc(capacity); indices_.copy_to_device();
  }
  index_count_.alloc(1);
  index_count_.zero_to_device();
  counter_.zero_to_device();
  state_.queue_counter = reinterpret_cast<IntegratorQueueCounter *>(counter_.device_pointer);
  state_.deep_optical_inputs = reinterpret_cast<DeepCameraReceipt *>(inputs_.device_pointer);
  state_.deep_optical_events = reinterpret_cast<DeepOpticalEvent *>(events_.device_pointer);
  state_.deep_optical_ledger = reinterpret_cast<DeepOpticalLedger *>(ledger_.device_pointer);
  state_.deep_optical_event_capacity = event_capacity;
  if (full_coverage) {
    state_.deep_owned_optical_pieces = (DeepOwnedOpticalPiece *)owned_pieces_.device_pointer;
    state_.deep_owned_optical_visibility = (DeepOwnedOpticalVisibility *)
                                               owned_visibility_.device_pointer;
    state_.deep_owned_optical_coverage = (DeepOwnedOpticalCoverage *)
                                             owned_coverage_.device_pointer;
    state_.deep_owned_optical_visibility_capacity = event_capacity;
    state_.deep_owned_optical_checkpoints = reinterpret_cast<DeepOpticalCheckpoint *>(
        owned_checkpoints_.device_pointer);
    state_.deep_owned_optical_operation_steps = 16;
    if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_OPERATION_STEPS")) {
      char *end = nullptr;
      errno = 0;
      const long parsed = strtol(value, &end, 10);
      if (errno || end == value || *end || parsed < 1 || parsed > 16)
        return fail("Optical operation step limit must be an integer in [1,16]");
      state_.deep_owned_optical_operation_steps = uint(parsed);
    }
  }
  size_t bytes = counter_.memory_size() + inputs_.memory_size() + events_.memory_size() +
                 ledger_.memory_size() + indices_.memory_size() + index_count_.memory_size() +
                 owned_pieces_.memory_size() + owned_visibility_.memory_size() +
                 owned_coverage_.memory_size() + owned_checkpoints_.memory_size();
  for (const auto &memory : soa_) {
    if (!memory)
      continue;
    if (!memory->device_pointer || memory->shared_pointer) {
      return fail("SoA allocation is missing or mapped to host");
    }
    bytes += memory->memory_size();
  }
  for (device_memory *memory : std::initializer_list<device_memory *>{&counter_,
                                                                      &inputs_,
                                                                      &events_,
                                                                      &ledger_,
                                                                      &indices_,
                                                                      &index_count_,
                                                                      &owned_pieces_,
                                                                      &owned_visibility_,
                                                                      &owned_coverage_,
                                                                      &owned_checkpoints_})
  {
    if (!memory->memory_size())
      continue;
    if (!memory->device_pointer || memory->shared_pointer) {
      return fail("independent allocation is missing or mapped to host");
    }
  }
  fprintf(stderr,
          "DEEP_OPTICAL_ALLOCATION bytes=%zu states=%d events=%d shadow=0 shared=0\n",
          bytes,
          capacity,
          event_capacity);
  return !device_->have_error();
}

bool DeepOpticalWork::run_chunk(const int count,
                                const bool *cancel,
                                DeepNativeCaptureBatch *capture,
                                const size_t camera_offset,
                                const std::function<bool()> &cancel_poll,
                                DeepGPUFrameWork *device_capture)
{
  queue_->zero_to_device(counter_);
  if (!device_capture)
    inputs_.copy_to_device();
  device_ptr null_pointer = 0;
  const int zero = 0;
  const DeviceKernelArguments init_args(
      &null_pointer, &zero, &null_pointer, &zero, &null_pointer, &count);
  if (!queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA, count, init_args)) {
    return false;
  }
  /* At most one kernel transition per dispatch. Bound an undetected queue-cycle failure without
   * turning exhausted traversal into a successful empty result. */
  uint64_t cache_services = 0, drained_events = 0;
  for (int iteration = 0; iteration < 8192; ++iteration) {
    queue_->copy_from_device(counter_);
    if (!queue_->synchronize() || device_->have_error()) {
      return false;
    }
    if ((cancel && *cancel) || (cancel_poll && cancel_poll())) {
      const auto &counter = counter_.data()[0];
      int queued = 0;
      for (int kernel = 0; kernel < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++kernel)
        queued += counter.num_queued[kernel];
      std::fprintf(
          stderr,
          "DEEP_OPTICAL_CANCEL states=%d completed=%u queued=%d accepted_events=%llu "
          "producer_joined=1\n",
          count,
          counter.deep_optical_completed,
          queued,
          static_cast<unsigned long long>(drained_events + counter.deep_optical_event_count));
      return false;
    }
    IntegratorQueueCounter &counter = counter_.data()[0];
    if (counter.deep_optical_error || counter.deep_native_capture_error ||
        counter.deep_optical_event_count > event_capacity_) {
      return fail(string_printf("%s chunk incomplete: error_bits=%u native_bits=%u event_count=%u capacity=%zu",
                                counter.deep_native_capture_error & DEEP_NATIVE_ERROR_MASS_RANGE ? "NumericMassRange" : "OptiX",
                                counter.deep_optical_error,
                                counter.deep_native_capture_error,
                                counter.deep_optical_event_count,
                                event_capacity_));
    }
    if (device_capture && state_.deep_compact_primitives && counter.deep_compact_written) {
      if (counter.deep_compact_reserved != counter.deep_compact_written ||
          counter.deep_compact_written > compact_primitives_.size() ||
          !device_capture->append_native(DeepGPUResidentKind::Primitive, compact_primitives_,
                                        counter.deep_compact_written))
        return fail("Incomplete compact supplement publication");
      counter.deep_compact_reserved = counter.deep_compact_written = 0;
      queue_->copy_to_device(counter_);
    }
    if ((capture || device_capture) &&
        (counter.deep_optical_event_count || counter.deep_owned_optical_visibility_count))
    {
      const uint32_t events = counter.deep_optical_event_count;
      const uint32_t visibility = counter.deep_owned_optical_visibility_count;
      if (!state_.deep_owned_optical_checkpoints || visibility > owned_visibility_.size())
        return fail("owned Optical page has no checkpoint or exceeds its visibility window");
      if (device_capture) {
        if (!device_capture->append_optical(owned_pieces_, events, owned_visibility_, visibility,
                                              uint32_t(camera_offset), uint32_t(count)))
          return false;
      }
      else {
        if (events) owned_pieces_.copy_from_device(0, events, 1);
        if (visibility) owned_visibility_.copy_from_device(0, visibility, 1);
        if (device_->have_error()) return false;
        for (uint32_t i = 0; i < events; ++i)
          if (owned_pieces_.data()[i].receipt >= uint32_t(count))
            return fail("owned Optical page piece references another chunk");
        for (uint32_t i = 0; i < visibility; ++i)
          if (owned_visibility_.data()[i].receipt >= uint32_t(count))
            return fail("owned Optical page visibility references another chunk");
        reserve_optical_append(capture->optical_pieces, events);
        reserve_optical_append(capture->optical_visibility, visibility);
        for (uint32_t i = 0; i < events; ++i) {
          auto piece = owned_pieces_.data()[i]; piece.receipt += uint32_t(camera_offset);
          capture->optical_pieces.push_back(piece);
        }
        for (uint32_t i = 0; i < visibility; ++i) {
          auto point = owned_visibility_.data()[i]; point.receipt += uint32_t(camera_offset);
          capture->optical_visibility.push_back(point);
        }
      }
      drained_events += events;
      counter.deep_optical_event_count = 0;
      counter.deep_owned_optical_visibility_count = 0;
      queue_->copy_to_device(counter_);
      if (!device_capture) std::fprintf(stderr,
                   "DEEP_OPTICAL_PAGE domain=%llu camera_offset=%zu events_base=%zu "
                   "visibility_base=%zu events=%u visibility=%u resident_capacity=%zu\n",
                   static_cast<unsigned long long>(capture->domain.id),
                   camera_offset,
                   capture->optical_pieces.size() - events,
                   capture->optical_visibility.size() - visibility,
                   events,
                   visibility,
                   event_capacity_);
    }
    if (counter.cache_miss) {
      if (!state_.deep_owned_optical_checkpoints)
        return fail("diagnostic cache retry needs an owned optical checkpoint");
      device_->image_load_requested_gpu(*queue_);
      if (!queue_->synchronize() || device_->have_error())
        return false;
      counter.cache_miss = false;
      queue_->copy_to_device(counter_);
      ++cache_services;
    }
    DeviceKernel next = DEVICE_KERNEL_NUM;
    int max_queued = 0;
    for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++i) {
      const int queued = counter.num_queued[i];
      if (queued < 0 || queued > count) {
        return fail("invalid independent queue counter");
      }
      if (!queued) {
        continue;
      }
      if (i != DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST &&
          i != DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK &&
          i != DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE &&
          i != DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL)
      {
        return fail(string_printf("forbidden Beauty/shadow/split queue %d", i));
      }
      if (queued > max_queued) {
        next = DeviceKernel(i);
        max_queued = queued;
      }
    }
    if (next == DEVICE_KERNEL_NUM) {
      if (counter.deep_optical_completed != uint(count)) {
        return fail("not every admitted receipt reached its completion ledger");
      }
      if (device_capture) {
        queue_->zero_to_device(validation_total_);
        DeepGPUFrameDispatch p;
        p.a = ledger_.device_pointer; p.b = owned_checkpoints_.device_pointer;
        p.c = inputs_.device_pointer; p.d = owned_coverage_.device_pointer;
        p.e = validation_total_.device_pointer; p.f = device_capture->status_pointer();
        p.g = drained_events + counter.deep_optical_event_count; p.n = uint32_t(count);
        return queue_->enqueue_deep_frame_operation(DeepGPUFrameOperation::ValidateOptical, p,
                                                      uint32_t((count + 255) / 256), 256) &&
               queue_->enqueue_deep_frame_operation(DeepGPUFrameOperation::ValidateOpticalFinish,
                                                      p, 1, 1);
      }
      ledger_.copy_from_device(0, count, 1);
      uint64_t total_events = 0;
      for (int i = 0; i < count; ++i) {
        const auto &entry = ledger_.data()[i];
        const bool zero_input = inputs_.data()[i].flags & DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT;
        if (entry.status != (zero_input ? DEEP_OPTICAL_ZERO_THROUGHPUT : DEEP_OPTICAL_COMPLETE) ||
            entry.reserved != 0 || (zero_input && (entry.events || entry.segments)))
        {
          return fail("invalid completion disposition");
        }
        total_events += entry.events;
      }
      if (total_events != drained_events + counter.deep_optical_event_count) {
        return fail("event population disagrees with independent ledger");
      }
      if (state_.deep_owned_optical_checkpoints) {
        owned_checkpoints_.copy_from_device(0, count, 1);
        if (device_->have_error())
          return false;
        uint64_t yields = 0, retries = 0;
        for (int i = 0; i < count; ++i) {
          const auto &checkpoint = owned_checkpoints_.data()[i];
          if (checkpoint.active || checkpoint.reserved)
            return fail("unfinished optical operation at chunk join");
          yields += checkpoint.yields;
          retries += checkpoint.retries;
        }
        std::fprintf(stderr,
                     "DEEP_OPTICAL_CONTINUATION states=%d yields=%llu retries=%llu "
                     "cache_services=%llu checkpoint_bytes=%zu\n",
                     count,
                     static_cast<unsigned long long>(yields),
                     static_cast<unsigned long long>(retries),
                     static_cast<unsigned long long>(cache_services),
                     sizeof(DeepOpticalCheckpoint));
      }
      return !device_->have_error();
    }
    queue_->zero_to_device(index_count_);
    const int kernel_index = next;
    const DeviceKernelArguments indices_args(
        &count, &indices_.device_pointer, &index_count_.device_pointer, &kernel_index);
    if (!queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY, count, indices_args)) {
      return false;
    }
    queue_->copy_from_device(index_count_);
    if (!queue_->synchronize() || index_count_.data()[0] != max_queued) {
      return fail("active-index population disagrees with independent counter");
    }
    const DeviceKernelArguments args(&indices_.device_pointer, &null_pointer, &max_queued);
    const DeviceKernelArguments volume_stack_args(&indices_.device_pointer, &max_queued);
    if (!queue_->enqueue(
            next,
            max_queued,
            next == DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK ? volume_stack_args : args))
    {
      return false;
    }
    if (iteration == 0) {
      const char *inject = getenv("CYCLES_EXPERIMENTAL_DEEP_OPTICAL_FAIL_AFTER_DISPATCH");
      if (inject && strcmp(inject, "1") == 0) {
        return fail("injected logical failure after launch; must drain before restoration");
      }
    }
  }
  return fail("independent traversal dispatch bound exhausted");
}

bool DeepOpticalWork::replay_device(DeviceScene *scene,
                                    const IntegratorStateGPU &beauty,
                                    DeepGPUFrameWork &capture,
                                    const bool *cancel,
                                    RenderBuffers *beauty_buffers,
                                    const std::function<bool()> &cancel_poll)
{
  if (!budget_ || !device_budget_ || !queue_ || !beauty_buffers ||
      device_->info.type != DEVICE_OPTIX || !queue_->supports_deep_frame_reduction())
    return fail("resident Optical requires the owning OptiX queue and explicit quotas");
  const uint32_t count = capture.native_count(DeepGPUResidentKind::Camera);
  if (!count) return fail("resident Optical has no native camera inputs");
  const size_t stride = optical_soa_stride(scene->data.kernel_features, scene->data.volume_stack_size) +
      sizeof(DeepCameraReceipt) + sizeof(DeepOpticalLedger) + sizeof(int) +
      sizeof(DeepOwnedOpticalCoverage) + sizeof(DeepOpticalCheckpoint) +
      16 * sizeof(DeepCompactPrimitive);
  size_t total = 0, free = 0;
  device_->get_device_memory_info(total, free);
  const size_t remaining = device_budget_->quota() - device_budget_->used();
  const size_t available = (total ? std::min(remaining, free) : remaining) / 2;
  const size_t fixed = sizeof(IntegratorQueueCounter) + sizeof(int) + sizeof(uint64_t);
  if (available <= fixed || available - fixed < stride)
    return fail("complete extinction cannot reserve its minimum OptiX continuation transaction");
  const int capacity = int(std::min({size_t(count), size_t(65536), (available - fixed) / stride}));
  if (!allocate(scene, capacity, capacity * 16, true, true)) return false;
  const size_t scalar_bytes = size_t(capacity) * 16 * sizeof(DeepCompactPrimitive);
  device_budget_->acquire(scalar_bytes);
  device_allocation_charge_ += scalar_bytes;
  compact_primitives_.alloc_device_only(size_t(capacity) * 16);
  compact_primitives_.alloc_to_device();
  state_.deep_compact_primitives = reinterpret_cast<DeepCompactPrimitive *>(
      compact_primitives_.device_pointer);
  state_.deep_compact_capacity = uint(capacity * 16);
  state_.deep_compact_rays = reinterpret_cast<DeepCompactRayState *>(
      capture.native_pointer(DeepGPUResidentKind::ScalarState));
  state_.deep_compact_cameras = reinterpret_cast<DeepNativeCameraRecord *>(
      capture.native_pointer(DeepGPUResidentKind::Camera));
  state_.deep_compact_stacks = reinterpret_cast<DeepCompactStackEntry *>(
      capture.native_pointer(DeepGPUResidentKind::Stack));
  state_.deep_compact_stack_stride = capture.domain().stack_stride;
  state_.deep_compact_has_volume = 1;
  if (!capture.bind_witness(state_)) return false;
  device_budget_->acquire(sizeof(uint64_t));
  device_allocation_charge_ += sizeof(uint64_t);
  validation_total_.alloc_to_device(1);
  if (!queue_->synchronize()) return false;
  IntegratorStateGPU saved = beauty;
  DeepStateBindingGuard binding(
      [&]() noexcept { return queue_->synchronize_for_state_rebind(); },
      [&]() noexcept { return device_->restore_integrator_state(&saved, sizeof(saved)); });
  device_->const_copy_to("integrator_state", &state_, sizeof(state_));
  bool success = true;
  for (uint32_t offset = 0; success && offset < count; offset += uint32_t(capacity)) {
    if ((cancel && *cancel) || (cancel_poll && cancel_poll())) { success = false; break; }
    const uint32_t current = std::min(uint32_t(capacity), count - offset);
    const auto &domain = capture.domain();
    state_.deep_compact_camera_base = offset;
    if (!queue_->synchronize_for_state_rebind()) { success = false; break; }
    device_->const_copy_to("integrator_state", &state_, sizeof(state_));
    DeepGPUFrameDispatch p;
    p.a = capture.native_pointer(DeepGPUResidentKind::Camera); p.b = inputs_.device_pointer;
    p.c = capture.native_pointer(DeepGPUResidentKind::ScalarState);
    p.n = current; p.base = offset;
    p.m = uint32_t(domain.core_width ? domain.core_x : domain.x);
    p.k = uint32_t(domain.core_height ? domain.core_y : domain.y);
    p.width = uint32_t(domain.core_width ? domain.core_width : domain.width);
    p.pixel = uint32_t(domain.core_height ? domain.core_height : domain.height);
    success = queue_->enqueue_deep_frame_operation(DeepGPUFrameOperation::NativeCameraInput,
                                                    p, (current + 255) / 256, 256) &&
              run_chunk(int(current), cancel, nullptr, offset, cancel_poll, &capture);
  }
  const bool restored = binding.restore();
  return success && restored && capture.good();
}

bool DeepOpticalWork::replay_owned(DeviceScene *scene,
                                   const IntegratorStateGPU &beauty,
                                   DeepNativeCaptureBatch &capture,
                                   const bool *cancel,
                                   RenderBuffers *beauty_buffers,
                                   const bool full_coverage,
                                   const std::function<bool()> &cancel_poll)
{
  if (!budget_ || !queue_ || !beauty_buffers || !capture.domain.main_shadow_mnee_joined ||
      capture.optical_replay_domain || !capture.optical_events.empty() ||
      !capture.optical_ledger.empty())
    return fail("owned replay needs one joined, unreplayed input domain and borrowed queue");
  try {
    const size_t count = capture.cameras.size();
    if (count > UINT32_MAX)
      return fail("owned camera dispatch range exceeds UINT32");
    const int capacity = int(std::min(count, size_t(256)));
    if (!capacity) {
      capture.optical_replay_domain = capture.domain.id;
      capture.optical_replay_joined = true;
      capture.optical_full_coverage = full_coverage;
      return true;
    }
    /* One chosen dispatch emits at most sixteen pieces per volume state or
     * one vacuum piece per non-volume state. Joined drains preserve all history. */
    const int page_capacity = full_coverage ?
                                  capacity *
                                      ((scene->data.kernel_features & KERNEL_FEATURE_VOLUME) ? 16 :
                                                                                               1) :
                                  65536;
    if (!allocate(scene, capacity, page_capacity, full_coverage))
      return false;
    DeepNativeCaptureVector<float> before{DeepBudgetAllocator<float>(*budget_)};
    const char *verify = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_FILM");
    const bool verify_film = verify && strcmp(verify, "1") == 0;
    if (verify_film) {
      if (!beauty_buffers->copy_from_device())
        return fail("cannot snapshot the joined native film");
      before.assign(beauty_buffers->buffer.data(),
                    beauty_buffers->buffer.data() + beauty_buffers->buffer.size());
    }
    capture.optical_ledger.resize(count);
    if (full_coverage)
      capture.optical_coverage.resize(count);
    queue_->init_execution();
    if (!queue_->synchronize())
      return false;
    IntegratorStateGPU saved_beauty = beauty;
    DeepStateBindingGuard binding(
        [&]() noexcept { return queue_->synchronize_for_state_rebind(); },
        [&]() noexcept {
          return device_->restore_integrator_state(&saved_beauty, sizeof(saved_beauty));
        });
    device_->const_copy_to("integrator_state", &state_, sizeof(state_));
    const bool completed = [&]() {
      for (size_t offset = 0; offset < count; offset += size_t(capacity)) {
        if ((cancel && *cancel) || (cancel_poll && cancel_poll()))
          return false;
        const int current = int(std::min(size_t(capacity), count - offset));
        for (int i = 0; i < current; i++) {
          const auto &camera = capture.cameras[offset + i];
          DeepCameraReceipt input = {};
          input.pixel = camera.pixel;
          input.sample = camera.sample;
          input.rng_pixel = camera.rng_pixel;
          input.rng_offset = camera.rng_offset;
          input.flags = camera.disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK ?
                            DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT :
                            DEEP_CAMERA_RECEIPT_RAY_VALID;
          input.x = camera.x;
          input.y = camera.y;
          input.population_weight = camera.population_weight;
          if (input.flags == DEEP_CAMERA_RECEIPT_RAY_VALID) {
            for (int c = 0; c < 3; c++) {
              input.origin[c] = camera.origin[c];
              input.direction[c] = camera.direction[c];
              input.throughput[c] = camera.throughput[c];
            }
            input.tmin = camera.tmin;
            input.tmax = camera.tmax;
            input.time = camera.time;
            input.dP = camera.dP;
            input.dD = camera.dD;
          }
          inputs_.data()[i] = input;
        }
        if (!run_chunk(current, cancel, full_coverage ? &capture : nullptr, offset, cancel_poll))
          return false;
        const uint32_t events = counter_.data()[0].deep_optical_event_count;
        if (full_coverage) {
          owned_coverage_.copy_from_device(0, current, 1);
          if (device_->have_error())
            return false;
          std::copy_n(owned_coverage_.data(), current, capture.optical_coverage.data() + offset);
        }
        else {
          if (events)
            events_.copy_from_device(0, events, 1);
          if (device_->have_error())
            return false;
          capture.optical_events.reserve(capture.optical_events.size() + events);
          for (uint32_t i = 0; i < events; i++) {
            auto event = events_.data()[i];
            if (event.receipt >= uint32_t(current))
              return fail("owned Optical event references another chunk");
            event.receipt += uint32_t(offset);
            capture.optical_events.push_back(event);
          }
        }
        std::copy_n(ledger_.data(), current, capture.optical_ledger.data() + offset);
      }
      return true;
    }();
    const bool restored = binding.restore();
    const bool unchanged = !verify_film || (restored && beauty_buffers->copy_from_device() &&
                                            before.size() == beauty_buffers->buffer.size() &&
                                            std::memcmp(before.data(),
                                                        beauty_buffers->buffer.data(),
                                                        before.size() * sizeof(float)) == 0);
    std::fprintf(stderr,
                 "DEEP_FRAME_OWNED_OPTICAL domain=%llu cameras=%zu events=%zu joined=%d "
                 "binding_restored=%d film_unchanged=%d film_verified=%d\n",
                 static_cast<unsigned long long>(capture.domain.id),
                 count,
                 full_coverage ? capture.optical_pieces.size() : capture.optical_events.size(),
                 completed,
                 restored,
                 unchanged,
                 int(verify_film));
    if (!completed && restored && unchanged &&
        ((cancel && *cancel) || (cancel_poll && cancel_poll())))
      return false; /* A joined ordinary cancellation is not a sticky device error. */
    if (!completed || !restored || !unchanged)
      return fail("owned replay is incomplete, unbound, or changed native film");
    capture.optical_replay_domain = capture.domain.id;
    capture.optical_replay_joined = true;
    capture.optical_full_coverage = full_coverage;
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail("owned replay exceeds the shared frame quota");
  }
  catch (const std::length_error &) {
    return fail("owned replay exceeds checked host ranges");
  }
}

bool DeepOpticalWork::replay(DeviceScene *scene,
                             const IntegratorStateGPU &beauty,
                             const string &camera_path,
                             const string &surface_path,
                             const uint64_t receipt_count,
                             const uint64_t surface_count,
                             const bool *cancel,
                             RenderBuffers *beauty_buffers)
{
  uint32_t camera_header[32] = {}, surface_header[32] = {};
  if (!DeepCameraReceiptReader::sealed_header(
          camera_path, 0x44435248, sizeof(DeepCameraReceipt), receipt_count, camera_header) ||
      !DeepCameraReceiptReader::sealed_header(
          surface_path, 0x44535248, sizeof(DeepSurfaceReceipt), surface_count, surface_header) ||
      memcmp(camera_header + 4, surface_header + 4, 28 * sizeof(uint32_t)) != 0)
  {
    return fail("Camera/Surface joint close-success identity is invalid");
  }
  DeepCameraReceiptReader reader;
  if (!reader.begin(camera_path, receipt_count, camera_header)) {
    return fail("cannot open immutable receipt input");
  }
  const int capacity = 32768;
  int event_capacity = 1048576;
  if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_OPTICAL_EVENT_CAPACITY")) {
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed < 1 || parsed > 16777216) {
      return fail("invalid event capacity");
    }
    event_capacity = int(parsed);
  }
  size_t total = 0, available = 0;
  device_->get_device_memory_info(total, available);
  /* Conservative preflight for the feature-selected main layout plus raw arena. Actual bytes
   * are logged and no mapped allocation is admitted. This does not prove texture residency. */
  const size_t budget = size_t(capacity) * 4096 +
                        size_t(event_capacity) * sizeof(DeepOpticalEvent);
  if (!total || available < budget || available - budget < 256 * 1024 * 1024) {
    return fail("independent chunk exceeds available device budget");
  }
  if (!allocate(scene, capacity, event_capacity)) {
    return false;
  }
  vector<float> film_before;
  const char *verify = getenv("CYCLES_EXPERIMENTAL_DEEP_OPTICAL_VERIFY_FILM");
  const bool verify_film = verify && strcmp(verify, "1") == 0;
  if (verify_film) {
    if (!beauty_buffers || !beauty_buffers->copy_from_device()) {
      return fail("cannot snapshot Beauty film");
    }
    film_before.assign(beauty_buffers->buffer.data(),
                       beauty_buffers->buffer.data() + beauty_buffers->buffer.size());
  }
  queue_->init_execution();
  if (!queue_->synchronize()) {
    return false;
  }
  IntegratorStateGPU saved_beauty = beauty;
  DeepStateBindingGuard binding(
      [&]() noexcept { return queue_->synchronize_for_state_rebind(); },
      [&]() noexcept {
        return device_->restore_integrator_state(&saved_beauty, sizeof(saved_beauty));
      });
  device_->const_copy_to("integrator_state", &state_, sizeof(state_));
  uint64_t chunks = 0, total_events = 0;
  const bool replay_ok = [&]() {
    for (uint64_t offset = 0; offset < receipt_count; offset += capacity) {
      if (cancel && *cancel) {
        return false;
      }
      const int count = int(std::min(uint64_t(capacity), receipt_count - offset));
      if (!reader.read(inputs_.data(), count)) {
        return fail("receipt stream is truncated");
      }
      for (int i = 0; i < count; ++i) {
        const auto &input = inputs_.data()[i];
        if (input.population_weight != 1.0f ||
            (input.flags != DEEP_CAMERA_RECEIPT_RAY_VALID &&
             input.flags != DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT))
        {
          return fail("unsupported receipt weight or disposition");
        }
        for (float channel : input.throughput) {
          if (!std::isfinite(channel)) {
            return fail("non-finite actual camera throughput");
          }
        }
        if (input.flags == DEEP_CAMERA_RECEIPT_RAY_VALID) {
          for (float field : {input.origin[0],
                              input.origin[1],
                              input.origin[2],
                              input.direction[0],
                              input.direction[1],
                              input.direction[2],
                              input.tmin,
                              input.tmax,
                              input.time,
                              input.dP,
                              input.dD})
          {
            if (!std::isfinite(field)) {
              return fail("non-finite actual camera Ray; not normalized or hidden");
            }
          }
          if (input.tmin < 0 || input.tmax < input.tmin || input.tmax == 0) {
            return fail("invalid actual camera clipping interval");
          }
        }
      }
      if (!run_chunk(count, cancel)) {
        return false;
      }
      const uint event_count = counter_.data()[0].deep_optical_event_count;
      if (event_count) {
        events_.copy_from_device(0, event_count, 1);
      }
      if (device_->have_error()) {
        return false;
      }
      const string prefix = camera_path + string_printf(".optical-chunk-%llu",
                                                        static_cast<unsigned long long>(chunks));
      uint32_t header[32];
      memcpy(header, camera_header, sizeof(header));
      header[0] = 0x444f4548;
      header[2] = sizeof(DeepOpticalEvent);
      header[3] = event_capacity;
      header[30] = uint32_t(offset);
      header[31] = uint32_t(offset >> 32);
      DeepInputReceiptWriter<DeepOpticalEvent, 0x444f4548, 0x444f4546> event_writer;
      DeepInputReceiptWriter<DeepOpticalLedger, 0x444f4c48, 0x444f4c46> ledger_writer;
      bool written = event_writer.begin(prefix + ".events", header);
      header[0] = 0x444f4c48;
      header[2] = sizeof(DeepOpticalLedger);
      header[3] = capacity;
      written &= ledger_writer.begin(prefix + ".ledger", header);
      written &= !event_count || event_writer.append(events_.data(), event_count);
      written &= ledger_writer.append(ledger_.data(), count);
      written &= event_writer.finish(written);
      written &= ledger_writer.finish(written);
      if (!written) {
        return fail("Optical diagnostic backing/close failed");
      }
      total_events += event_count;
      ++chunks;
      fprintf(stderr,
              "DEEP_OPTICAL_CHUNK offset=%llu receipts=%d events=%u complete=%d\n",
              static_cast<unsigned long long>(offset),
              count,
              event_count,
              count);
    }
    return reader.finish(receipt_count);
  }();
  /* Every normal/cancel/failure exit after binding reaches restoration before allocation release.
   * A failed GPU synchronization cannot certify restoration, and cannot publish a success seal. */
  const bool restored = binding.restore();
  bool film_unchanged = !verify_film;
  if (verify_film && restored && beauty_buffers->copy_from_device()) {
    film_unchanged = beauty_buffers->buffer.size() == film_before.size() &&
                     memcmp(beauty_buffers->buffer.data(),
                            film_before.data(),
                            film_before.size() * sizeof(float)) == 0;
  }
  fprintf(stderr,
          "DEEP_OPTICAL_END complete=%d receipts=%llu events=%llu chunks=%llu "
          "binding_restored=%d film_verified=%d film_unchanged=%d\n",
          int(replay_ok && restored && film_unchanged && !device_->have_error() &&
              !(cancel && *cancel)),
          static_cast<unsigned long long>(receipt_count),
          static_cast<unsigned long long>(total_events),
          static_cast<unsigned long long>(chunks),
          int(restored),
          int(verify_film),
          int(film_unchanged));
  if (!replay_ok || !restored || !film_unchanged || device_->have_error() || (cancel && *cancel)) {
    if (!device_->have_error() && !(cancel && *cancel)) {
      return fail("input completion, binding restoration or Beauty immutability failed");
    }
    return false;
  }
  /* Domain success is additional to every chunk seal and outer frame/process success. */
  FILE *seal = path_fopen(camera_path + ".optical.sealed", "wbx");
  if (!seal) {
    return fail("cannot exclusively create Optical domain success marker");
  }
  const uint64_t marker[8] = {0x444f444f,
                              1,
                              uint64_t(camera_header[4]) | (uint64_t(camera_header[5]) << 32),
                              receipt_count,
                              surface_count,
                              chunks,
                              total_events,
                              1};
  const bool published = fwrite(marker, 1, sizeof(marker), seal) == sizeof(marker);
  const bool closed = fclose(seal) == 0;
  return published && closed ? true : fail("Optical domain marker close failed");
}

CCL_NAMESPACE_END
