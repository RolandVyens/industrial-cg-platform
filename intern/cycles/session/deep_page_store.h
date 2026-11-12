/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "session/deep_capture_types.h"
#include "session/deep_memory_budget.h"

CCL_NAMESPACE_BEGIN

enum class DeepErrorCode {
  None,
  InvalidFrame,
  InvalidState,
  InvalidReference,
  InvalidLayout,
  InvalidCount,
  InvalidPayload,
  Incomplete,
  DuplicateSample,
  DuplicatePixel,
  PopulationMismatch,
  NeedPage,
  BudgetExceeded,
  Cancelled,
  OutputFailure,
};

struct DeepStatus {
  DeepErrorCode code = DeepErrorCode::None;
  std::string message;
  explicit operator bool() const
  {
    return code == DeepErrorCode::None;
  }
  static DeepStatus fail(DeepErrorCode code, const std::string &message)
  {
    return {code, message};
  }
};

struct DeepStoredPage;

/* Immutable borrowed storage. Retiring a page removes the producer's reference only;
 * consumers/copies keep its actual bytes charged until their final lease is destroyed. */
class DeepPageLease {
 public:
  const DeepPageDescriptor *descriptor() const;
  const DeepFrameKey *frame_key() const;
  uint64_t valid_handle_at(uint32_t slot) const;
  bool is_charged_to(const DeepMemoryBudget &budget) const;
  DeepStatus read(uint64_t handle, DeepRecordTag &tag, std::span<const uint8_t> &payload) const;
  explicit operator bool() const
  {
    return bool(page_);
  }

 private:
  std::shared_ptr<const DeepStoredPage> page_;
  friend class DeepPageStore;
};

/* Immutable page-list ownership shares the frame's allocation quota. */
class DeepPageSequence {
 public:
  explicit DeepPageSequence(const DeepMemoryBudget &budget)
      : pages_(DeepBudgetAllocator<DeepPageLease>(budget))
  {
  }
  DeepStatus append(const DeepPageLease &page)
  {
    try {
      pages_.push_back(page);
      return {};
    }
    catch (const std::bad_alloc &) {
      return DeepStatus::fail(DeepErrorCode::BudgetExceeded, "Deep page-list quota exhausted");
    }
  }
  std::span<const DeepPageLease> pages() const
  {
    return pages_;
  }

 private:
  std::vector<DeepPageLease, DeepBudgetAllocator<DeepPageLease>> pages_;
};

/* Host/reference PageStore. Calls are serialized by the frame owner; GPU publication will
 * provide a separate producer adapter, not access mutable host vectors from kernels. */
class DeepPageStore {
 public:
  DeepPageStore(const DeepFrameKey &frame, size_t budget_bytes, size_t page_bytes = 65536);
  DeepPageStore(const DeepFrameKey &frame,
                const DeepMemoryBudget &budget,
                size_t page_bytes = 65536);
  ~DeepPageStore();
  DeepPageStore(const DeepPageStore &) = delete;
  DeepPageStore &operator=(const DeepPageStore &) = delete;

  DeepStatus allocate(DeepRecordKind kind, uint32_t &page);
  DeepStatus reserve(uint32_t page, uint64_t &handle);
  DeepStatus write(uint64_t handle, const DeepRecordTag &tag, std::span<const uint8_t> payload);
  DeepStatus commit(uint64_t handle);
  DeepStatus abort(uint64_t handle);
  DeepStatus seal(uint32_t page, DeepPageLease &lease);
  DeepStatus retire(uint32_t page);
  DeepStatus publication_status() const;
  size_t bytes_used() const;
  size_t budget_bytes() const;
  const DeepMemoryBudget &memory_budget() const
  {
    return budget_;
  }
  const DeepFrameKey &frame_key() const
  {
    return frame_;
  }

 private:
  DeepStatus locate(uint64_t handle, std::shared_ptr<DeepStoredPage> &page, uint16_t &slot) const;
  DeepFrameKey frame_;
  size_t page_bytes_;
  DeepMemoryBudget budget_;
  mutable DeepErrorCode first_error_ = DeepErrorCode::None;
  using PageEntry = std::pair<const uint32_t, std::shared_ptr<DeepStoredPage>>;
  std::unordered_map<uint32_t,
                     std::shared_ptr<DeepStoredPage>,
                     std::hash<uint32_t>,
                     std::equal_to<uint32_t>,
                     DeepBudgetAllocator<PageEntry>>
      pages_;
};

CCL_NAMESPACE_END
