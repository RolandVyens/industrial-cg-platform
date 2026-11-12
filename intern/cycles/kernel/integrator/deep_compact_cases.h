/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* An isolated qualification destination exercises the exact capture fitter.
 * It never changes a real camera, sampling state, Film or production page. */
struct DeepCompactCaseEmitter {
  static constexpr bool production = false;
  ccl_global DeepCompactCaptureCase *output;
  ccl_device_inline_method bool operator()(const DeepCompactPrimitive &value) const
  {
    if (output->count >= 4) { output->status = 1; return false; }
    output->primitives[output->count++] = value;
    return true;
  }
};

ccl_device_noinline void deep_compact_capture_case()
{
  const auto *trace = kernel_integrator_state.deep_compact_trace;
  if (!trace || !trace->capture_case) return;
  auto *output = reinterpret_cast<ccl_global DeepCompactCaptureCase *>(trace->capture_case);
  if (atomicCAS(&output->claimed, 0u, 1u) != 0) return;
  DeepCompactCaseEmitter emit{output};
  DeepCompactPrimitive prefix = {};
  prefix.kind = DEEP_COMPACT_SURFACE; prefix.q = 1.0 - .002;
  prefix.log_before = 0; prefix.log_after = log(.002);
  emit(prefix);
  /* Use an independently owned global state slot after the four output PODs.
   * This avoids adding a large local stack to the ordinary camera kernel. */
  auto &ray = *reinterpret_cast<ccl_global DeepCompactRayState *>(
      reinterpret_cast<ccl_global char *>(output) + sizeof(DeepCompactCaptureCase));
  ray = {};
  DeepCompactPrimitive a = {};
  a.front = 1; a.back = 2; a.q = .001; a.log_before = log(.002); a.log_after = log(.001);
  a.kind = DEEP_COMPACT_VOLUME; a.ordinal = 1;
  deep_compact_fit_impl(ray, a, emit);
  DeepCompactPrimitive b = a;
  b.front = 2; b.back = 3; b.q = .001 - exp(-100.0); b.log_before = a.log_after; b.log_after = -100;
  b.ordinal = 2;
  deep_compact_fit_impl(ray, b, emit);
  if (ray.flags & DEEP_COMPACT_HAS_CANDIDATE) emit(ray.candidate);
  atomic_fetch_and_or_uint32(&output->claimed, 2u);
}
