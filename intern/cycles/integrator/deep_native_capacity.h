/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "integrator/deep_frame_capture.h"
#include <limits>

CCL_NAMESPACE_BEGIN

struct DeepNativeCaptureCapacity {
  size_t cameras = 0, segments = 0, roots = 0, points = 0, indices = 0;
  size_t root_resident = 0, root_pins = 0;
  size_t scalar_records = 0, scalar_states = 0, scalar_stack = 0;
  size_t emission_states = 0;
  size_t expression_sources = 0, expression_nodes = 0, expression_calls = 0;
  size_t host_staging = 0, device = 0;
};

/* Shared reservation model. A split changes execution domains, never a logical sample.
 * Root descriptors have an initial live reserve plus their append/pin windows.
 * Completed point/homogeneous arrays are joined dispatch windows, not domain limits. */
inline bool deep_native_capture_capacity(const uint64_t admissions,
                                         const size_t path_states,
                                         const bool catcher,
                                         const bool may_emit,
                                         const bool ray_marching,
                                         DeepNativeCaptureCapacity &capacity,
                                         const bool volume_checkpoints = false,
                                         const bool capture_rgb = true,
                                         const bool expression_history = true,
                                         const bool compact_scalar = false,
                                         const size_t stack_size = 0)
{
  capacity = {};
  const uint64_t nodes = 512 + (ray_marching ? 0 : 16);
  if (!admissions || admissions > UINT32_MAX / 16 || (may_emit && admissions > UINT32_MAX / nodes))
    return false;
  capacity.cameras = size_t(admissions);
  if (compact_scalar) {
    capacity.scalar_records = size_t(admissions * 2);
    capacity.scalar_states = capacity.cameras;
    if (stack_size > SIZE_MAX / capacity.cameras) return false;
    capacity.scalar_stack = capacity.cameras * stack_size;
    if (capture_rgb && may_emit) capacity.emission_states = capacity.cameras;
  }
  capacity.roots = capture_rgb ? size_t(admissions * (compact_scalar ? 2 : 16)) : 0;
  capacity.segments = capture_rgb ? size_t(admissions * (compact_scalar ? int(may_emit) : 16)) : 0;
  capacity.points = compact_scalar ? 0 : capacity.segments;
  capacity.root_resident = capture_rgb && !compact_scalar ? size_t(admissions * 2) : 0;
  capacity.root_pins = compact_scalar ? 0 : capacity.segments + capacity.root_resident;
  capacity.indices = std::min(path_states, size_t(admissions * (catcher ? 2 : 1)));
  if (capture_rgb && may_emit && expression_history) {
    capacity.expression_sources = size_t(admissions * 256);
    capacity.expression_nodes = size_t(admissions * nodes);
    capacity.expression_calls = size_t(admissions * 16);
  }
  const auto add = [](size_t &sum, const size_t count, const size_t stride) {
    if (count > (SIZE_MAX - sum) / stride)
      return false;
    sum += count * stride;
    return true;
  };
  if (!add(capacity.host_staging, capacity.cameras, sizeof(DeepNativeCameraRecord)) ||
      !add(capacity.host_staging, capacity.segments, sizeof(DeepNativeHomogeneousRecord)) ||
      !add(capacity.host_staging, capacity.roots, sizeof(DeepNativeRootRecord)) ||
      !add(capacity.host_staging, capacity.root_resident, sizeof(DeepNativeRootRecord)) ||
      !add(capacity.host_staging, capacity.root_pins, sizeof(uint32_t)) ||
      !add(capacity.host_staging, capacity.points, sizeof(DeepNativePointRecord)) ||
      !add(capacity.host_staging,
           capacity.expression_sources,
           sizeof(DeepNativeExpressionSource)) ||
      !add(capacity.host_staging, capacity.expression_nodes, sizeof(DeepNativeExpressionNode)) ||
      !add(capacity.host_staging, capacity.expression_calls, sizeof(DeepNativeExpressionCall)))
    return false;
  capacity.device = capacity.host_staging;
  if (compact_scalar) capacity.host_staging = 0;
  if (!add(capacity.device, capacity.scalar_records, sizeof(DeepCompactPrimitive)) ||
      !add(capacity.device, capacity.scalar_states, sizeof(DeepCompactRayState)) ||
      !add(capacity.device, capacity.scalar_stack, sizeof(DeepCompactStackEntry)))
    return false;
  if (!add(capacity.device, capacity.emission_states, sizeof(DeepCompactEmissionState))) return false;
  if (volume_checkpoints &&
      !add(capacity.device, capacity.cameras, DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES))
    return false;
  return add(capacity.device, capacity.indices, 2 * sizeof(uint32_t)) &&
         add(capacity.device, path_states, 2 * sizeof(uint32_t));
}

/* Joined windows are empty before each producer dispatch. A background Sun
 * loop yields after one committed light, so these bounds also cover it. */
inline int deep_native_control_dispatch_limit(const size_t roots,
                                              const size_t points,
                                              const size_t homogeneous,
                                              const size_t calls,
                                              const bool expression_calls,
                                              const int requested)
{
  if (requested <= 0)
    return 0;
  size_t limit = std::min(size_t(requested), std::min(roots / 2, points / 2));
  limit = std::min(limit, homogeneous);
  if (expression_calls)
    limit = std::min(limit, calls);
  return int(limit);
}

inline int deep_native_batch_samples(const uint64_t pixels,
                                     const int requested,
                                     const size_t path_states,
                                     const bool catcher,
                                     const bool may_emit,
                                     const bool ray_marching,
                                     const size_t host_available,
                                     const size_t device_available,
                                     const bool volume_checkpoints = false,
                                     const bool capture_rgb = true,
                                     const bool expression_history = true,
                                     const bool compact_scalar = false,
                                     const size_t stack_size = 0)
{
  if (!pixels || requested <= 0)
    return 0;
  int low = 0, high = requested;
  while (low < high) {
    const int middle = low + int((int64_t(high) - low + 1) / 2);
    DeepNativeCaptureCapacity capacity;
    const bool fits = pixels <= UINT64_MAX / uint64_t(middle) &&
                      deep_native_capture_capacity(pixels * middle,
                                                   path_states,
                                                   catcher,
                                                   may_emit,
                                                   ray_marching,
                                                   capacity,
                                                   volume_checkpoints,
                                                   capture_rgb,
                                                   expression_history,
                                                   compact_scalar,
                                                   stack_size) &&
                      capacity.host_staging <= host_available &&
                      capacity.device <= device_available;
    if (fits)
      low = middle;
    else
      high = middle - 1;
  }
  return low;
}

CCL_NAMESPACE_END
