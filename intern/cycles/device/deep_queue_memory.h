/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <optional>

#include "util/types.h"
#include "session/deep_memory_budget.h"

CCL_NAMESPACE_BEGIN

class DeepQueueFrameReservation;

/* Queue-lifetime auxiliary storage is not owned by a temporary Deep frame.
 * Cold registration/allocation only; no lock is taken on launch or DMA reuse. */
class DeepQueueAuxiliaryBudget {
  struct Registry {
    std::mutex mutex;
    size_t reserved = 0, resident = 0, peak = 0;
    uint64_t generation = 0;
    DeepQueueFrameReservation *frames = nullptr;
  };
  static Registry &registry()
  {
    static Registry state;
    return state;
  }
  friend class DeepQueueFrameReservation;

 public:
  explicit DeepQueueAuxiliaryBudget(const size_t capacity) : capacity_(capacity)
  {
    auto &r = registry();
    const std::lock_guard lock(r.mutex);
    if (capacity > SIZE_MAX - r.reserved || r.generation == UINT64_MAX)
      throw std::bad_alloc();
    generation_ = ++r.generation;
    r.reserved += capacity;
  }
  ~DeepQueueAuxiliaryBudget()
  {
    auto &r = registry();
    const std::lock_guard lock(r.mutex);
    r.reserved -= capacity_;
    r.resident -= resident_;
  }
  DeepQueueAuxiliaryBudget(const DeepQueueAuxiliaryBudget &) = delete;
  DeepQueueAuxiliaryBudget &operator=(const DeepQueueAuxiliaryBudget &) = delete;
  bool acquire(size_t bytes);
  /* Cold allocation seam used by the real CUDA allocator and failure tests.
   * Fixed queue metadata registers the successful pointer without throwing. */
  template<typename Allocate> void *allocate_with(const size_t bytes, Allocate &&allocate)
  {
    if (!acquire(bytes)) return nullptr;
    void *pointer = nullptr;
    try { pointer = allocate(); }
    catch (...) { relinquish(bytes); throw; }
    if (!pointer) relinquish(bytes);
    return pointer;
  }
  void relinquish(const size_t bytes)
  {
    auto &r = registry();
    const std::lock_guard lock(r.mutex);
    resident_ -= bytes;
    r.resident -= bytes;
  }
  struct Statistics { size_t reserved, pinned_resident, process_pinned_peak; };
  static Statistics statistics()
  {
    auto &r = registry();
    const std::lock_guard lock(r.mutex);
    return {r.reserved, r.resident, r.peak};
  }
  static size_t resident_bytes()
  {
    auto &r = registry();
    const std::lock_guard lock(r.mutex);
    return r.resident;
  }

 private:
  const size_t capacity_;
  size_t resident_ = 0;
  uint64_t generation_ = 0;
};

/* Subtract conservative credits for all retained queues before constructing
 * the frame ledger. No eager pinned allocation, and peaks are not summed.
 * A later queue extends each live frame's credit before allocating. A credit
 * is not queue ownership: actual blocks still live in the independent ledger. */
class DeepQueueFrameReservation {
 public:
  explicit DeepQueueFrameReservation(const size_t total_quota) : total_quota_(total_quota)
  {
    auto &r = DeepQueueAuxiliaryBudget::registry();
    const std::lock_guard lock(r.mutex);
    if (r.reserved >= total_quota)
      throw std::bad_alloc();
    reserved_ = r.reserved;
    generation_ = r.generation;
    next_ = r.frames;
    r.frames = this;
  }
  ~DeepQueueFrameReservation()
  {
    auto &r = DeepQueueAuxiliaryBudget::registry();
    const std::lock_guard lock(r.mutex);
    if (payload_budget_) payload_budget_->release_credit(extra_credit_);
    auto **entry = &r.frames;
    while (*entry != this) entry = &(*entry)->next_;
    *entry = next_;
  }
  DeepQueueFrameReservation(const DeepQueueFrameReservation &) = delete;
  DeepQueueFrameReservation &operator=(const DeepQueueFrameReservation &) = delete;
  size_t payload_quota() const { return total_quota_ - reserved_bytes(); }
  size_t total_quota() const { return total_quota_; }
  size_t reserved_bytes() const
  {
    auto &r = DeepQueueAuxiliaryBudget::registry();
    const std::lock_guard lock(r.mutex);
    return reserved_;
  }
  size_t extra_credit_bytes() const
  {
    auto &r = DeepQueueAuxiliaryBudget::registry();
    const std::lock_guard lock(r.mutex);
    return extra_credit_;
  }
  void bind_payload_budget(const DeepMemoryBudget &budget)
  {
    auto &r = DeepQueueAuxiliaryBudget::registry();
    const std::lock_guard lock(r.mutex);
    if (payload_budget_) throw std::bad_alloc();
    const size_t growth = r.reserved > reserved_ ? r.reserved - reserved_ : 0;
    budget.reserve_credit(growth);
    payload_budget_.emplace(budget);
    extra_credit_ = growth;
    reserved_ += growth;
    generation_ = r.generation;
  }

 private:
  friend class DeepQueueAuxiliaryBudget;
  size_t total_quota_, reserved_;
  uint64_t generation_;
  DeepQueueFrameReservation *next_ = nullptr;
  std::optional<DeepMemoryBudget> payload_budget_;
  size_t extra_credit_ = 0, pending_credit_ = 0;
};

inline bool DeepQueueAuxiliaryBudget::acquire(const size_t bytes)
{
  auto &r = registry();
  const std::lock_guard lock(r.mutex);
  if (bytes > capacity_ - resident_)
    return false;
  if (bytes > SIZE_MAX - r.resident)
    return false;
  /* Admission is a cold transaction across live frames. Roll back earlier
   * credits if any frame lacks room. No pinned allocation has happened yet. */
  bool admitted = true;
  for (auto *frame = r.frames; frame; frame = frame->next_) {
    if (generation_ <= frame->generation_) continue;
    if (!frame->payload_budget_) { admitted = false; break; }
    const size_t growth = r.reserved > frame->reserved_ ? r.reserved - frame->reserved_ : 0;
    try {
      frame->payload_budget_->reserve_credit(growth);
      frame->pending_credit_ = growth;
    }
    catch (const std::bad_alloc &) { admitted = false; break; }
  }
  for (auto *frame = r.frames; frame; frame = frame->next_) {
    if (!admitted) {
      if (frame->pending_credit_) frame->payload_budget_->release_credit(frame->pending_credit_);
    }
    else if (generation_ > frame->generation_) {
      frame->reserved_ += frame->pending_credit_;
      frame->extra_credit_ += frame->pending_credit_;
      frame->generation_ = r.generation;
    }
    frame->pending_credit_ = 0;
  }
  if (!admitted) return false;
  resident_ += bytes;
  r.resident += bytes;
  r.peak = std::max(r.peak, r.resident);
  return true;
}

CCL_NAMESPACE_END
