/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "session/deep_camera_depth.h"

CCL_NAMESPACE_BEGIN
struct DeepDepthSupportStorage;
struct DeepDepthSupportBuildState;

class DeepDepthSupportView {
 public:
  explicit operator bool() const
  {
    return bool(storage_);
  }
  const DeepFrameKey *frame_key() const;
  uint64_t ray() const;
  size_t support_count() const;
  bool is_charged_to(const DeepMemoryBudget &budget) const;
  DeepStatus read_map(DeepFrozenDepthMap &output) const;
  DeepStatus make_model(DeepCameraDepthMap &output) const;
  DeepStatus read_support(DeepPhysicalSourceKind kind,
                          uint64_t source,
                          DeepPhysicalSourceSupport &output) const;

 private:
  std::shared_ptr<const DeepDepthSupportStorage> storage_;
  size_t map_ = 0, first_ = 0, count_ = 0;
  friend class DeepDepthSupportTable;
};

class DeepDepthSupportTable {
 public:
  explicit operator bool() const
  {
    return bool(storage_);
  }
  DeepStatus view(uint64_t ray, DeepDepthSupportView &output) const;

 private:
  std::shared_ptr<const DeepDepthSupportStorage> storage_;
  friend class DeepDepthSupportBuilder;
};

/* One mutable producer; immutable views own sealed leases after publication.
 * All data/control allocations share the caller's existing frame budget. */
class DeepDepthSupportBuilder {
 public:
  DeepDepthSupportBuilder(const DeepFrameKey &frame,
                          const DeepMemoryBudget &budget,
                          size_t page_bytes = 65536);
  DeepDepthSupportBuilder(const DeepDepthSupportBuilder &) = delete;
  DeepDepthSupportBuilder &operator=(const DeepDepthSupportBuilder &) = delete;
  DeepStatus add_map(uint64_t ray, const DeepFrozenDepthMap &input, uint64_t &handle);
  DeepStatus add_support(uint64_t ray, const DeepPhysicalSourceSupport &input);
  DeepStatus finish(DeepDepthSupportTable &output);

 private:
  std::shared_ptr<DeepDepthSupportBuildState> state_;
};
CCL_NAMESPACE_END
