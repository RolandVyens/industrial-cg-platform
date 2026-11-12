/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Decimal MiB override; reject signs, whitespace, suffixes and overflow before
 * creating either ledger. Default uses a conservative current physical limit. */
inline bool deep_host_memory_quota(const size_t available, const char *override_mb, size_t &quota)
{
  quota = 0;
  if (!override_mb) {
    const uint64_t limit = uint64_t(8) << 30;
    quota = size_t(std::min(uint64_t(available / 4), limit));
    return quota >= 1024;
  }
  size_t mb = 0;
  if (!*override_mb)
    return false;
  for (const char *c = override_mb; *c; ++c) {
    if (*c < '0' || *c > '9' || mb > (std::numeric_limits<size_t>::max() - size_t(*c - '0')) / 10)
      return false;
    mb = mb * 10 + size_t(*c - '0');
  }
  if (!mb || mb > std::numeric_limits<size_t>::max() / (1024 * 1024))
    return false;
  quota = mb * 1024 * 1024;
  return true;
}

/* Charged allocation bytes and explicitly tagged cold auxiliary capacity credits.
 * Allocation bytes include STL nodes, buckets, shared control blocks and this
 * ledger. The allocator's private heap bookkeeping is outside this application quota.
 * Intrusive lifetime avoids an uncharged shared control block for the ledger itself. */
class DeepMemoryBudget {
  struct State {
    explicit State(const size_t quota) : quota(quota), used(sizeof(State)), peak(sizeof(State)) {}
    const size_t quota;
    std::atomic<size_t> used;
    std::atomic<size_t> peak;
    std::atomic<size_t> references{1};
    std::atomic<size_t> reserved_credit{0};
  };

 public:
  explicit DeepMemoryBudget(const size_t quota)
  {
    if (quota < sizeof(State))
      throw std::bad_alloc();
    state_ = new State(quota);
  }
  DeepMemoryBudget(const DeepMemoryBudget &other) : state_(other.state_)
  {
    state_->references.fetch_add(1, std::memory_order_relaxed);
  }
  DeepMemoryBudget &operator=(const DeepMemoryBudget &other)
  {
    if (state_ != other.state_) {
      other.state_->references.fetch_add(1, std::memory_order_relaxed);
      release();
      state_ = other.state_;
    }
    return *this;
  }
  ~DeepMemoryBudget()
  {
    release();
  }
  size_t used() const
  {
    return state_->used.load(std::memory_order_relaxed);
  }
  size_t quota() const
  {
    return state_->quota;
  }
  size_t peak() const
  {
    return state_->peak.load(std::memory_order_relaxed);
  }
  bool operator==(const DeepMemoryBudget &other) const
  {
    return state_ == other.state_;
  }

  void acquire(const size_t bytes) const
  {
    size_t used = state_->used.load(std::memory_order_relaxed);
    do {
      if (used > state_->quota || bytes > state_->quota - used)
        throw std::bad_alloc();
    } while (!state_->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    const size_t current = used + bytes;
    size_t peak = state_->peak.load(std::memory_order_relaxed);
    while (peak < current &&
           !state_->peak.compare_exchange_weak(peak, current, std::memory_order_relaxed))
    {
    }
  }
  /* Cold auxiliary capacity credits are charged but are not resident payload.
   * peak() remains the charged peak; do not subtract independent peaks. */
  size_t reserved_credit() const
  {
    return state_->reserved_credit.load(std::memory_order_relaxed);
  }
  void reserve_credit(const size_t bytes) const
  {
    acquire(bytes);
    state_->reserved_credit.fetch_add(bytes, std::memory_order_relaxed);
  }
  void release_credit(const size_t bytes) const
  {
    state_->reserved_credit.fetch_sub(bytes, std::memory_order_relaxed);
    relinquish(bytes);
  }

  void relinquish(const size_t bytes) const
  {
    state_->used.fetch_sub(bytes, std::memory_order_relaxed);
  }

 private:
  void release()
  {
    if (state_->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete state_;
  }
  State *state_;
};

template<typename T> class DeepBudgetAllocator {
 public:
  using value_type = T;
  explicit DeepBudgetAllocator(const DeepMemoryBudget &budget) : budget_(budget) {}
  template<typename U>
  DeepBudgetAllocator(const DeepBudgetAllocator<U> &other) : budget_(other.budget_)
  {
  }

  T *allocate(const size_t count)
  {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T))
      throw std::bad_alloc();
    const size_t bytes = count * sizeof(T);
    budget_.acquire(bytes);
    try {
      return std::allocator<T>{}.allocate(count);
    }
    catch (...) {
      budget_.relinquish(bytes);
      throw;
    }
  }
  void deallocate(T *pointer, const size_t count)
  {
    std::allocator<T>{}.deallocate(pointer, count);
    budget_.relinquish(count * sizeof(T));
  }
  template<typename U> bool operator==(const DeepBudgetAllocator<U> &other) const
  {
    return budget_ == other.budget_;
  }

 private:
  DeepMemoryBudget budget_;
  template<typename U> friend class DeepBudgetAllocator;
};

/* Fully overwritten receive arrays use one allocation-level initialization
 * decision, never an element constructor with a per-byte branch. */
template<typename T> class DeepReceiveBuffer {
  static_assert(std::is_trivial_v<T>);
 public:
  DeepReceiveBuffer(const DeepMemoryBudget &budget, const bool overwrite = false)
      : allocator_(budget), overwrite_(overwrite)
  {
  }
  DeepReceiveBuffer(const DeepReceiveBuffer &) = delete;
  DeepReceiveBuffer &operator=(const DeepReceiveBuffer &) = delete;
  ~DeepReceiveBuffer()
  {
    if (pointer_) allocator_.deallocate(pointer_, capacity_);
  }
  T *data() { return pointer_; }
  const T *data() const { return pointer_; }
  size_t size() const { return size_; }
  T &operator[](const size_t index) { return pointer_[index]; }
  const T &operator[](const size_t index) const { return pointer_[index]; }
  T &back() { return pointer_[size_ - 1]; }
  const T &back() const { return pointer_[size_ - 1]; }
  void resize(const size_t count)
  {
    reserve(count);
    if (!overwrite_ && count > size_) {
      std::memset(pointer_ + size_, 0, (count - size_) * sizeof(T));
    }
    size_ = count;
  }
  void assign(const T *first, const T *last)
  {
    const size_t count = size_t(last - first);
    reserve(count);
    if (count) std::memmove(pointer_, first, count * sizeof(T));
    size_ = count;
  }
 private:
  void reserve(const size_t count)
  {
    if (count <= capacity_) return;
    /* std::allocator::allocate starts the implicit-lifetime scalar array;
     * no value construction is required before the complete overwrite. */
    T *replacement = allocator_.allocate(count);
    if (size_) std::memcpy(replacement, pointer_, size_ * sizeof(T));
    if (pointer_) allocator_.deallocate(pointer_, capacity_);
    pointer_ = replacement;
    capacity_ = count;
  }
  DeepBudgetAllocator<T> allocator_;
  bool overwrite_;
  T *pointer_ = nullptr;
  size_t size_ = 0, capacity_ = 0;
};

CCL_NAMESPACE_END
