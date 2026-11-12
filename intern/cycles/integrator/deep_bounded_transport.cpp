/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "integrator/deep_bounded_work_internal.h"

CCL_NAMESPACE_BEGIN

static bool release_pinned(DeepBoundedWorkStorage &o, DeepBoundedCopyPosition &slot)
{
  if (slot.pinned) {
    o.queue.deep_unpin(slot.pinned);
    o.consumer.memory_budget().relinquish(slot.pinned_bytes);
    slot.pinned = nullptr;
    slot.pinned_bytes = 0;
  }
  return !o.device.have_error();
}

static bool pin(DeepBoundedWorkStorage &o, DeepBoundedCopyPosition &slot, const size_t bytes)
{
  slot.transfer_bytes = bytes;
  if (slot.pinned_bytes >= bytes) {
    return true;
  }
  if (!release_pinned(o, slot)) {
    return false;
  }
  o.consumer.memory_budget().acquire(bytes);
  slot.pinned = o.queue.deep_pin(bytes);
  if (!slot.pinned) {
    o.consumer.memory_budget().relinquish(bytes);
    return false;
  }
  slot.pinned_bytes = bytes;
  return true;
}

bool deep_bounded_progress_copy(DeepBoundedWorkStorage &o, const unsigned index, const bool wait)
{
  using Stage = DeepBoundedCopyPosition::Stage;
  auto &slot = o.transport.copies[index];
  if (slot.stage == Stage::Free) {
    return true;
  }
  const size_t pixels = size_t(o.transport.output->window.width) * o.transport.output->window.height;
  const auto &e = slot.encoding;
  const size_t meta_bytes = sizeof(DeepBoundedEncodedHeader) + pixels * sizeof(uint32_t);
  const auto &budget = o.consumer.memory_budget();
  if (slot.epoch != o.transport.output_epoch) {
    o.device.set_error("Stale bounded Deep copy position");
    return false;
  }
  using CopyStage = DeepBoundedCopyStage;
  if (slot.stage == Stage::Compacted) {
    if (!slot.recorded(CopyStage::COMPACT_EVENT_RECORDED) || !pin(o, slot, meta_bytes)) {
      return false;
    }
    slot.submitted_stages |= DeepBoundedCopyPosition::COPY_TOUCHED;
    if (!o.queue.deep_copy_begin(index) || !o.queue.deep_copy_plane(slot.pinned, e.header, meta_bytes))
    {
      return false;
    }
    slot.mark(CopyStage::HEADER_SUBMITTED);
    if (!o.queue.deep_copy_end(index, false)) {
      return false;
    }
    slot.mark(CopyStage::HEADER_EVENT_RECORDED);
    slot.stage = Stage::Header;
    if (!wait) {
      return true;
    }
  }
  if (slot.stage == Stage::Header) {
    const int ready = o.queue.deep_copy_ready(index, false, wait);
    if (ready <= 0) {
      return ready == 0;
    }
    const auto &header = *static_cast<const DeepBoundedEncodedHeader *>(slot.pinned);
    const bool dense = (header.layout & (1u << 16)) != 0;
    const bool simple = o.policy == DeepBoundedPolicy::SIMPLE && !e.alpha_only;
    if (header.status || header.M > e.capacity_records ||
        (header.layout & ~(1u << 16)) != e.layout || dense != (simple && header.M >= pixels))
    {
      o.device.set_error("Invalid bounded Deep encode header, status " +
                         std::to_string(header.status));
      return false;
    }
    auto payload = std::allocate_shared<DeepBoundedHostPayload>(
        DeepBudgetAllocator<DeepBoundedHostPayload>(budget), budget, o.overwrite_receive);
    payload->count = size_t(header.M);
    payload->half = e.half;
    payload->physical_rgb = o.policy == DeepBoundedPolicy::PHYSICAL && !e.alpha_only;
    payload->simple_rgb = simple;
    payload->dense_summary = dense;
    payload->offsets.resize(pixels + 1);
    const auto *counts = reinterpret_cast<const uint32_t *>(
        static_cast<const uint8_t *>(slot.pinned) + sizeof(DeepBoundedEncodedHeader));
    uint64_t offset = 0;
    for (size_t i = 0; i < pixels; ++i) {
      payload->offsets[i] = offset;
      if (counts[i] > DEEP_BOUNDED_K || counts[i] > header.M - offset) {
        o.device.set_error("Invalid bounded Deep pixel count");
        return false;
      }
      offset += counts[i];
    }
    payload->offsets.back() = offset;
    if (offset != header.M) {
      o.device.set_error("Bounded Deep count prefix does not match encoded length");
      return false;
    }
    const size_t depth_bytes = payload->count * 4;
    const size_t rgba_bytes = payload->count * (payload->half ? 2 : 4) *
                              (payload->physical_rgb ? 4 : 1);
    const size_t summary_bytes = deep_bounded_summary_bytes(simple, dense, pixels, payload->count);
    const size_t bytes = 2 * depth_bytes + rgba_bytes + summary_bytes;
    if (bytes) {
      if (!pin(o, slot, bytes)) {
        return false;
      }
      auto *dst = static_cast<uint8_t *>(slot.pinned);
      const uint64_t addresses[] = {e.z, e.z_back, e.rgba, e.summary};
      const size_t widths[] = {depth_bytes, depth_bytes, rgba_bytes, summary_bytes};
      for (unsigned plane = 0; plane < 4; ++plane) {
        if (!widths[plane]) {
          continue;
        }
        if (!o.queue.deep_copy_plane(dst, addresses[plane], widths[plane])) {
          return false;
        }
        slot.submitted_planes |= 1u << plane;
        dst += widths[plane];
      }
      slot.mark(CopyStage::PAYLOAD_SUBMITTED);
      if (!o.queue.deep_copy_end(index, true)) {
        return false;
      }
      slot.mark(CopyStage::DMA_EVENT_RECORDED);
    }
    else {
      slot.mark(CopyStage::HOST_VALIDATED);
      o.transport.output->consumers[slot.consumer] = std::move(payload);
      slot.mark(CopyStage::CPU_DONE);
      slot.mark(CopyStage::RETIRED);
      slot.stage = Stage::Free;
      return true;
    }
    slot.payload = std::move(payload);
    slot.stage = Stage::Payload;
    if (!wait) {
      return true;
    }
  }
  const int ready = o.queue.deep_copy_ready(index, true, wait);
  if (ready <= 0) {
    return ready == 0;
  }
  const uint32_t expected = (slot.payload->count ? 7u : 0u) |
                            (slot.payload->simple_rgb &&
                                     (slot.payload->dense_summary || slot.payload->count) ?
                                 8u :
                                 0u);
  if (slot.submitted_planes != expected) {
    o.device.set_error("Incomplete bounded Deep plane submission");
    return false;
  }
  slot.mark(CopyStage::HOST_VALIDATED);
  slot.payload->planes.resize(slot.transfer_bytes);
  std::memcpy(slot.payload->planes.data(), slot.pinned, slot.transfer_bytes);
  o.transport.output->consumers[slot.consumer] = std::move(slot.payload);
  slot.mark(CopyStage::CPU_DONE);
  slot.mark(CopyStage::RETIRED);
  slot.stage = Stage::Free;
  return true;
}

bool deep_bounded_pump_copies(DeepBoundedWorkStorage &o, const bool wait)
{
  if (!o.transport.output) {
    return true;
  }
  if (o.transport.copy_failed || o.device.have_error()) {
    return false;
  }
  try {
    for (unsigned i = 0; i < 2; ++i) {
      if (!deep_bounded_progress_copy(o, i, wait)) {
        o.transport.copy_failed = true;
        return false;
      }
    }
    return true;
  }
  catch (const std::bad_alloc &) {
    o.device.set_error("Bounded Deep readback exceeds host quota");
    o.transport.copy_failed = true;
    return false;
  }
}

bool DeepBoundedWork::set_tile_beauty(const RenderBuffers &buffers)
{
  auto &o = *storage_;
  if (!o.transport.output || o.policy != DeepBoundedPolicy::SIMPLE) {
    return true;
  }
  try {
    if (!deep_extract_bounded_beauty(buffers, o.transport.output->window, o.transport.output->beauty)) {
      o.device.set_error("Deep Simple final Beauty has incompatible Film layout");
      return false;
    }
    return true;
  }
  catch (const std::bad_alloc &) {
    o.device.set_error("Deep Simple final Beauty exceeds its host quota");
    return false;
  }
}

void DeepBoundedWork::after_native_init()
{
  deep_bounded_pump_copies(*storage_, false);
}

bool DeepBoundedWork::drain()
{
  auto &o = *storage_;
  if (!o.transport.output) {
    return good();
  }
  if (!deep_bounded_pump_copies(o, true)) {
    return false;
  }
  for (size_t i = 0; i < o.transport.output_aliases.size(); ++i) {
    if (o.transport.output_aliases[i] != i) {
      o.transport.output->consumers[i] = o.transport.output->consumers[o.transport.output_aliases[i]];
    }
  }
  if (!o.transport.ready.stage(o.transport.output)) {
    o.device.set_error("Bounded Deep ready slot requires an owner publication checkpoint");
    return false;
  }
  return true;
}

bool DeepBoundedWork::publish_ready()
{
  auto &o = *storage_;
  return good() && o.transport.ready.publish(o.consumer);
}


CCL_NAMESPACE_END
