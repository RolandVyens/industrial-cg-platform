/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "integrator/deep_frame_capture.h"
#include "session/deep_page_store.h"
#include <cmath>

CCL_NAMESPACE_BEGIN

struct DeepExpressionPage {
  uint64_t domain = 0, device_generation = 0, layout_generation = 0;
  uint32_t source_base = 0, node_base = 0, call_base = 0;
  std::span<const DeepNativeExpressionSource> sources;
  std::span<const DeepNativeExpressionNode> nodes;
  std::span<const DeepNativeExpressionCall> calls;
};

/* A domain-global immutable append history, independent of resident GPU windows.
 * Pages are validated and copied before publication. Failed growth or a bad range
 * is sticky; no partial journal can be released as a completed native packet. */
class DeepExpressionJournal {
 public:
  DeepExpressionJournal(const DeepMemoryBudget &budget,
                        const DeepNativeCaptureDomain &domain,
                        const uint32_t camera_capacity)
      : domain_(domain),
        camera_capacity_(camera_capacity),
        sources_(DeepBudgetAllocator<DeepNativeExpressionSource>(budget)),
        nodes_(DeepBudgetAllocator<DeepNativeExpressionNode>(budget)),
        calls_(DeepBudgetAllocator<DeepNativeExpressionCall>(budget))
  {
  }

  const DeepStatus &error() const
  {
    return error_;
  }
  size_t source_count() const
  {
    return sources_.size();
  }
  size_t node_count() const
  {
    return nodes_.size();
  }
  size_t call_count() const
  {
    return calls_.size();
  }

  DeepStatus append(const DeepExpressionPage &page)
  {
    if (!error_)
      return error_;
    if (released_)
      return fail(DeepErrorCode::InvalidState, "Expression journal already released");
    if (!domain_.id || page.domain != domain_.id ||
        page.device_generation != domain_.device_generation ||
        page.layout_generation != domain_.layout_generation)
      return fail(DeepErrorCode::InvalidFrame,
                  "Expression page belongs to another native binding");
    if (page.source_base != sources_.size() || page.node_base != nodes_.size() ||
        page.call_base != calls_.size() ||
        page.sources.size() > (UINT32_MAX - 1) - sources_.size() ||
        page.nodes.size() > (UINT32_MAX - 1) - nodes_.size() ||
        page.calls.size() > (UINT32_MAX - 1) - calls_.size())
      return fail(DeepErrorCode::InvalidCount,
                  "Expression journal has a hole, replay or overflowing handle");
    const size_t total_sources = sources_.size() + page.sources.size();
    const size_t total_nodes = nodes_.size() + page.nodes.size();
    const auto source_camera = [&](uint32_t handle) {
      return handle <= sources_.size() ? sources_[handle - 1].camera_index :
                                         page.sources[handle - page.source_base - 1].camera_index;
    };
    const auto node_camera = [&](uint32_t handle) {
      return handle <= nodes_.size() ? nodes_[handle - 1].camera_index :
                                       page.nodes[handle - page.node_base - 1].camera_index;
    };
    for (size_t i = 0; i < page.sources.size(); i++) {
      const auto &source = page.sources[i];
      if (source.ordinal != page.source_base + i + 1 || source.camera_index >= camera_capacity_)
        return fail(DeepErrorCode::InvalidReference, "Expression source lost its global identity");
    }
    for (size_t i = 0; i < page.nodes.size(); i++) {
      const auto &node = page.nodes[i];
      if (node.ordinal != page.node_base + i + 1 || node.camera_index >= camera_capacity_ ||
          node.operation > DEEP_NATIVE_EXPR_SELECT || node.a >= node.ordinal ||
          node.b >= node.ordinal || node.source > total_sources ||
          (node.a && node_camera(node.a) != node.camera_index) ||
          (node.b && node_camera(node.b) != node.camera_index) ||
          (node.source && source_camera(node.source) != node.camera_index) ||
          (node.operation == DEEP_NATIVE_EXPR_TERM && !node.source) ||
          (node.operation != DEEP_NATIVE_EXPR_TERM && node.source))
        return fail(DeepErrorCode::InvalidReference,
                    "Expression page contains a foreign or forward DAG handle");
      for (float weight : node.weight)
        if (!std::isfinite(weight))
          return fail(DeepErrorCode::InvalidPayload, "Expression page has nonfinite weight");
    }
    for (size_t i = 0; i < page.calls.size(); i++) {
      const auto &call = page.calls[i];
      if (call.ordinal != page.call_base + i + 1 || call.camera_index >= camera_capacity_ ||
          call.head > total_nodes || (call.head && node_camera(call.head) != call.camera_index))
        return fail(DeepErrorCode::InvalidReference, "Expression Film call lost its global head");
    }
    try {
      /* Reserve every array before changing any committed count. */
      reserve_growth(sources_, total_sources);
      reserve_growth(nodes_, total_nodes);
      reserve_growth(calls_, calls_.size() + page.calls.size());
      sources_.insert(sources_.end(), page.sources.begin(), page.sources.end());
      nodes_.insert(nodes_.end(), page.nodes.begin(), page.nodes.end());
      calls_.insert(calls_.end(), page.calls.begin(), page.calls.end());
    }
    catch (const std::bad_alloc &) {
      return fail(DeepErrorCode::BudgetExceeded,
                  "Expression journal exceeds the native host quota");
    }
    return {};
  }

  DeepStatus release_to(DeepNativeCaptureBatch &batch)
  {
    if (!error_)
      return error_;
    if (released_ || !domain_.id || !domain_.device_generation || !domain_.layout_generation ||
        batch.domain.id != domain_.id ||
        batch.domain.device_generation != domain_.device_generation ||
        batch.domain.layout_generation != domain_.layout_generation ||
        !batch.domain.main_shadow_mnee_joined || !batch.expression_sources.empty() ||
        !batch.expression_nodes.empty() || !batch.expression_calls.empty() ||
        !(batch.expression_sources.get_allocator() == sources_.get_allocator()) ||
        !(batch.expression_nodes.get_allocator() == nodes_.get_allocator()) ||
        !(batch.expression_calls.get_allocator() == calls_.get_allocator()))
      return fail(DeepErrorCode::InvalidState,
                  "Expression journal cannot publish into this native packet");
    if (batch.domain.expression_source_reserved != sources_.size() ||
        batch.domain.expression_source_written != sources_.size() ||
        batch.domain.expression_node_reserved != nodes_.size() ||
        batch.domain.expression_node_written != nodes_.size() ||
        batch.domain.expression_call_reserved != calls_.size() ||
        batch.domain.expression_call_written != calls_.size())
      return fail(DeepErrorCode::InvalidCount,
                  "Expression journal disagrees with authoritative producer counts");
    batch.expression_sources = std::move(sources_);
    batch.expression_nodes = std::move(nodes_);
    batch.expression_calls = std::move(calls_);
    released_ = true;
    return {};
  }

 private:
  template<typename T>
  static void reserve_growth(DeepNativeCaptureVector<T> &values, size_t needed)
  {
    if (needed <= values.capacity())
      return;
    const size_t limit = UINT32_MAX - 1;
    const size_t desired = std::max(needed, std::min(limit, values.capacity() * 2));
    try {
      values.reserve(desired);
    }
    catch (const std::bad_alloc &) {
      if (desired == needed)
        throw;
      values.reserve(needed);
    }
  }
  DeepStatus fail(DeepErrorCode code, const char *message)
  {
    if (error_)
      error_ = DeepStatus::fail(code, message);
    return error_;
  }
  DeepNativeCaptureDomain domain_;
  uint32_t camera_capacity_;
  bool released_ = false;
  DeepStatus error_;
  DeepNativeCaptureVector<DeepNativeExpressionSource> sources_;
  DeepNativeCaptureVector<DeepNativeExpressionNode> nodes_;
  DeepNativeCaptureVector<DeepNativeExpressionCall> calls_;
};

CCL_NAMESPACE_END
