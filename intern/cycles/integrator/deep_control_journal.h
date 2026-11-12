/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "integrator/deep_frame_capture.h"
#include "session/deep_page_store.h"
#include <algorithm>
#include <cmath>
#include <cstring>

CCL_NAMESPACE_BEGIN

struct DeepControlPage {
  uint64_t domain = 0, device_generation = 0, layout_generation = 0;
  uint32_t root_base = 0, homogeneous_base = 0, point_base = 0;
  std::span<const DeepNativeRootRecord> roots;
  std::span<const DeepNativeHomogeneousRecord> homogeneous;
  std::span<const DeepNativePointRecord> points;
};

/* Completed immutable control records outlive resident GPU descriptors. The
 * native homogeneous ordinal remains its volume-boundary identity, not a page index. */
class DeepControlJournal {
 public:
  DeepControlJournal(const DeepMemoryBudget &budget,
                     const DeepNativeCaptureDomain &domain,
                     const uint32_t camera_capacity)
      : domain_(domain),
        camera_capacity_(camera_capacity),
        roots_(DeepBudgetAllocator<DeepNativeRootRecord>(budget)),
        homogeneous_(DeepBudgetAllocator<DeepNativeHomogeneousRecord>(budget)),
        points_(DeepBudgetAllocator<DeepNativePointRecord>(budget))
  {
  }

  size_t root_count() const
  {
    return roots_.size();
  }
  size_t homogeneous_count() const
  {
    return homogeneous_.size();
  }
  size_t point_count() const
  {
    return points_.size();
  }
  const DeepStatus &error() const
  {
    return error_;
  }

  DeepStatus append(const DeepControlPage &page)
  {
    if (!error_)
      return error_;
    if (released_)
      return fail(DeepErrorCode::InvalidState, "Control journal already released");
    if (!domain_.id || !domain_.device_generation || !domain_.layout_generation ||
        page.domain != domain_.id || page.device_generation != domain_.device_generation ||
        page.layout_generation != domain_.layout_generation)
      return fail(DeepErrorCode::InvalidFrame, "Control page belongs to another native binding");
    constexpr size_t limit = UINT32_MAX - 1;
    if (page.root_base != roots_.size() || page.homogeneous_base != homogeneous_.size() ||
        page.point_base != points_.size() || page.roots.size() > limit - roots_.size() ||
        page.homogeneous.size() > limit - homogeneous_.size() ||
        page.points.size() > limit - points_.size())
      return fail(DeepErrorCode::InvalidCount,
                  "Control journal has a hole, replay or overflowing range");
    const size_t total_roots = roots_.size() + page.roots.size();
    const auto root_at = [&](uint32_t handle) -> const DeepNativeRootRecord & {
      return handle <= roots_.size() ? roots_[handle - 1] :
                                       page.roots[handle - page.root_base - 1];
    };
    for (size_t i = 0; i < page.roots.size(); ++i) {
      const auto &root = page.roots[i];
      if (root.ordinal != page.root_base + i + 1 || root.camera_index >= camera_capacity_ ||
          root.kind < DEEP_NATIVE_ROOT_SURFACE || root.kind > DEEP_NATIVE_ROOT_DISTANT ||
          !(root.flags & DEEP_NATIVE_ROOT_CAMERA_SUPPORT) || !std::isfinite(root.z) ||
          root.z < 0 || !std::isfinite(root.physical_t))
        return fail(DeepErrorCode::InvalidPayload,
                    "Control root lost its camera or immutable handle");
    }
    for (const auto &segment : page.homogeneous) {
      if (segment.camera_index >= camera_capacity_ || segment.unsupported_features ||
          !std::isfinite(segment.z) || !std::isfinite(segment.zback) ||
          !std::isfinite(segment.tmin) || !std::isfinite(segment.tmax) || segment.z < 0 ||
          segment.zback <= segment.z || segment.tmax <= segment.tmin ||
          segment.emission_calls > 1 || !deep_native_path_writes_combined(segment.path_flags) ||
          !std::isfinite(segment.clamp_gain) || segment.clamp_gain < 0 || segment.clamp_gain > 1)
        return fail(DeepErrorCode::InvalidPayload, "Control homogeneous record is incomplete");
      for (int c = 0; c < 3; ++c)
        if (!std::isfinite(segment.tau[c]) || segment.tau[c] < 0 ||
            !std::isfinite(segment.pre_clamp_rgb[c]) || !std::isfinite(segment.post_clamp_rgb[c]))
          return fail(DeepErrorCode::InvalidPayload, "Control homogeneous record is nonfinite");
    }
    for (size_t i = 0; i < page.points.size(); ++i) {
      const auto &point = page.points[i];
      if (point.ordinal != page.point_base + i || point.camera_index >= camera_capacity_ ||
          !point.root_handle || point.root_handle > total_roots)
        return fail(DeepErrorCode::InvalidReference,
                    "Control Film call has a foreign or forward root");
      const auto &root = root_at(point.root_handle);
      if (root.camera_index != point.camera_index || point.root_kind != root.kind ||
          point.root_ordinal != root.ordinal || point.z != root.z ||
          point.routing != DEEP_NATIVE_POINT_COMBINED ||
          !deep_native_path_writes_combined(point.path_flags) ||
          !std::isfinite(point.clamp_gain) || point.clamp_gain < 0 || point.clamp_gain > 1)
        return fail(DeepErrorCode::InvalidReference,
                    "Control Film call lost its actual root identity");
      for (int c = 0; c < 3; ++c)
        if (!std::isfinite(point.pre_clamp_rgb[c]) || !std::isfinite(point.clamp_rgb[c]) ||
            !std::isfinite(point.final_rgb[c]) || !std::isfinite(point.routing_scale[c]))
          return fail(DeepErrorCode::InvalidPayload, "Control Film call is nonfinite");
    }
    try {
      /* Every allocation succeeds before any committed record count changes. */
      reserve_growth(roots_, total_roots);
      reserve_growth(homogeneous_, homogeneous_.size() + page.homogeneous.size());
      reserve_growth(points_, points_.size() + page.points.size());
      roots_.insert(roots_.end(), page.roots.begin(), page.roots.end());
      homogeneous_.insert(homogeneous_.end(), page.homogeneous.begin(), page.homogeneous.end());
      points_.insert(points_.end(), page.points.begin(), page.points.end());
    }
    catch (const std::bad_alloc &) {
      return fail(DeepErrorCode::BudgetExceeded, "Control journal exceeds native host quota");
    }
    return {};
  }

  /* A joined GPU snapshot selects descriptors, never historical contributions.
   * The append window must be the newest suffix of this journal. */
  DeepStatus select_live_roots(const std::span<const DeepNativeRootRecord> resident,
                               const std::span<const DeepNativeRootRecord> window,
                               const std::span<const uint32_t> pins,
                               DeepNativeCaptureVector<DeepNativeRootRecord> &selected)
  {
    if (!error_)
      return error_;
    if (released_ || !selected.empty() || !(selected.get_allocator() == roots_.get_allocator()))
      return fail(DeepErrorCode::InvalidState, "Resident root snapshot has a foreign owner");
    if (window.size() > roots_.size() || pins.size() != resident.size() + window.size())
      return fail(DeepErrorCode::InvalidCount, "Resident root snapshot has an invalid pin range");
    const size_t base = roots_.size() - window.size();
    uint32_t previous = 0;
    size_t count = 0;
    for (size_t i = 0; i < pins.size(); ++i) {
      const bool old = i < resident.size();
      const auto &root = old ? resident[i] : window[i - resident.size()];
      if ((pins[i] & ~uint32_t(15)) || !root.ordinal || root.ordinal > roots_.size() ||
          (old && (root.ordinal <= previous || root.ordinal > base)) ||
          (!old && root.ordinal != base + i - resident.size() + 1) ||
          std::memcmp(&root, &roots_[root.ordinal - 1], sizeof(root)) != 0)
        return fail(DeepErrorCode::InvalidReference,
                    "Resident root snapshot lost an immutable global descriptor");
      previous = root.ordinal;
      count += size_t(pins[i] != 0);
    }
    try {
      reserve_growth(selected, count);
      for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i])
          selected.push_back(i < resident.size() ? resident[i] : window[i - resident.size()]);
    }
    catch (const std::bad_alloc &) {
      return fail(DeepErrorCode::BudgetExceeded, "Resident root snapshot exceeds host quota");
    }
    return {};
  }

  DeepStatus release_to(DeepNativeCaptureBatch &batch)
  {
    if (!error_)
      return error_;
    if (released_ || batch.domain.id != domain_.id ||
        batch.domain.device_generation != domain_.device_generation ||
        batch.domain.layout_generation != domain_.layout_generation || !domain_.id ||
        !domain_.device_generation || !domain_.layout_generation ||
        !batch.domain.main_shadow_mnee_joined || !batch.roots.empty() ||
        !batch.homogeneous.empty() || !batch.points.empty() ||
        !(batch.roots.get_allocator() == roots_.get_allocator()) ||
        !(batch.homogeneous.get_allocator() == homogeneous_.get_allocator()) ||
        !(batch.points.get_allocator() == points_.get_allocator()))
      return fail(DeepErrorCode::InvalidState,
                  "Control journal cannot publish into this native packet");
    if (batch.domain.root_reserved != roots_.size() ||
        batch.domain.root_written != roots_.size() ||
        batch.domain.homogeneous_reserved != homogeneous_.size() ||
        batch.domain.homogeneous_written != homogeneous_.size() ||
        batch.domain.point_reserved != points_.size() ||
        batch.domain.point_written != points_.size())
      return fail(DeepErrorCode::InvalidCount,
                  "Control journal disagrees with authoritative producer counts");
    batch.roots = std::move(roots_);
    batch.homogeneous = std::move(homogeneous_);
    batch.points = std::move(points_);
    released_ = true;
    return {};
  }

 private:
  template<typename T>
  static void reserve_growth(DeepNativeCaptureVector<T> &values, const size_t needed)
  {
    if (needed <= values.capacity())
      return;
    const size_t desired = std::max(needed,
                                    std::min(size_t(UINT32_MAX - 1), values.capacity() * 2));
    try {
      values.reserve(desired);
    }
    catch (const std::bad_alloc &) {
      if (needed == desired)
        throw;
      values.reserve(needed);
    }
  }
  DeepStatus fail(const DeepErrorCode code, const char *message)
  {
    if (error_)
      error_ = DeepStatus::fail(code, message);
    return error_;
  }
  DeepNativeCaptureDomain domain_;
  uint32_t camera_capacity_;
  bool released_ = false;
  DeepStatus error_;
  DeepNativeCaptureVector<DeepNativeRootRecord> roots_;
  DeepNativeCaptureVector<DeepNativeHomogeneousRecord> homogeneous_;
  DeepNativeCaptureVector<DeepNativePointRecord> points_;
};

CCL_NAMESPACE_END
