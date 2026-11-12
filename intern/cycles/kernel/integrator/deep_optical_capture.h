/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/camera/camera.h"
#include "kernel/integrator/state_util.h"
#include "util/atomic.h"

CCL_NAMESPACE_BEGIN

#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline void deep_owned_optical_error(const uint32_t code)
{
  atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_optical_error, code);
}

/* One bounded first-fault witness per failed chunk. This does not relax the
 * structural error or publish the rejected piece. No successful-path logging. */
ccl_device_noinline void deep_owned_optical_bounds(KernelGlobals kg,
                                                   IntegratorState state,
                                                   const uint32_t reason,
                                                   const float a,
                                                   const float b,
                                                   const float z,
                                                   const float zback,
                                                   const Spectrum tau)
{
  const uint32_t prior = atomic_fetch_and_or_uint32(
      &kernel_integrator_state.queue_counter->deep_optical_error, DEEP_OPTICAL_ERROR_BOUNDS);
#  if defined(__KERNEL_CUDA__) || defined(__KERNEL_OPTIX__)
  if (prior == 0) {
    const float3 P = INTEGRATOR_STATE(state, ray, P), D = INTEGRATOR_STATE(state, ray, D);
    const float cursor = kernel_integrator_state.deep_owned_optical_coverage ?
                             kernel_integrator_state.deep_owned_optical_coverage[state].cursor_t :
                             -1.0f;
    printf(
        "DEEP_OPTICAL_FIRST_BOUNDS reason=%u state=%u segment=%u a=%.9g b=%.9g "
        "cursor=%.9g z=%.9g back=%.9g tau=(%.9g,%.9g,%.9g) "
        "P=(%.9g,%.9g,%.9g) D=(%.9g,%.9g,%.9g)\n",
        reason,
        uint32_t(state),
        kernel_integrator_state.deep_optical_ledger[state].segments,
        double(a),
        double(b),
        double(cursor),
        double(z),
        double(zback),
        double(tau.x),
        double(tau.y),
        double(tau.z),
        double(P.x),
        double(P.y),
        double(P.z),
        double(D.x),
        double(D.y),
        double(D.z));
  }
#  endif
  (void)kg;
  (void)prior;
}

ccl_device_inline bool deep_owned_optical_piece(KernelGlobals kg,
                                                IntegratorState state,
                                                const float a,
                                                const float b,
                                                const Spectrum tau,
                                                const uint32_t flags,
                                                const double scalar_sigma = -1.0)
{
  if (kernel_integrator_state.deep_compact_rays) {
    auto &coverage = kernel_integrator_state.deep_owned_optical_coverage[state];
    if (!(b >= a) || a != coverage.cursor_t || !isfinite_safe(tau) ||
        min(tau.x, min(tau.y, tau.z)) < 0) {
      deep_owned_optical_error(DEEP_OPTICAL_ERROR_BOUNDS);
      return false;
    }
    const double sigma = scalar_sigma >= 0 ? scalar_sigma :
        (b > a ? (double(tau.x) + double(tau.y) + double(tau.z)) / (3.0 * (double(b) - a)) : 0.0);
    if (!deep_compact_cell(kg, state, a, b, sigma,
          kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)].reserved))
      return false;
    coverage.cursor_t = b;
    coverage.z_back = deep_compact_depth(kg,
        &kernel_integrator_state.deep_compact_cameras[deep_compact_camera_index(state)], b);
    coverage.pieces++;
    (void)flags;
    return true;
  }
  if (!kernel_integrator_state.deep_owned_optical_pieces)
    return false;
  ccl_global DeepOwnedOpticalCoverage *coverage =
      &kernel_integrator_state.deep_owned_optical_coverage[state];
  if (!isfinite_safe(a) || !isfinite_safe(b) || b < a || a != coverage->cursor_t ||
      !isfinite_safe(tau) || min(tau.x, min(tau.y, tau.z)) < 0)
  {
    deep_owned_optical_bounds(kg, state, 1, a, b, 0, 0, tau);
    return false;
  }
  if (b == a) {
    if (!is_zero(tau))
      deep_owned_optical_bounds(kg, state, 2, a, b, 0, 0, tau);
    return is_zero(tau);
  }
  const float3 P = INTEGRATOR_STATE(state, ray, P), D = INTEGRATOR_STATE(state, ray, D);
  const float z = camera_z_depth(kg, P + D * a), zback = camera_z_depth(kg, P + D * b);
  /* Equal projected endpoints retain their physical extent and extinction.
   * Only the host's certified canonical coalescer may redistribute that tau. */
  if (!isfinite_safe(z) || !isfinite_safe(zback) || z < 0 || zback < z)
  {
    deep_owned_optical_bounds(kg, state, 3, a, b, z, zback, tau);
    return false;
  }
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_optical_event_count, 1);
  if (index >= kernel_integrator_state.deep_optical_event_capacity) {
    deep_owned_optical_error(DEEP_OPTICAL_ERROR_CAPACITY);
    return false;
  }
  uint64_t hash = 1469598103934665603ull;
#  ifdef __VOLUME__
  if (kernel_data.kernel_features & KERNEL_FEATURE_VOLUME) {
    for (int i = 0; i < kernel_data.volume_stack_size; i++) {
      const int shader = INTEGRATOR_STATE_ARRAY(state, volume_stack, i, shader);
      if (shader == SHADER_NONE)
        break;
      hash = (hash ^ uint32_t(shader)) * 1099511628211ull;
      hash = (hash ^ uint32_t(INTEGRATOR_STATE_ARRAY(state, volume_stack, i, object))) *
             1099511628211ull;
    }
  }
#  endif
  DeepOwnedOpticalPiece piece = {};
  piece.receipt = state;
  piece.segment = kernel_integrator_state.deep_optical_ledger[state].segments;
  piece.tmin = a;
  piece.tmax = b;
  piece.z = z;
  piece.zback = zback;
  piece.tau[0] = tau.x;
  piece.tau[1] = tau.y;
  piece.tau[2] = tau.z;
  piece.flags = flags;
  piece.stack_hash[0] = uint32_t(hash);
  piece.stack_hash[1] = uint32_t(hash >> 32);
  kernel_integrator_state.deep_owned_optical_pieces[index] = piece;
  coverage->cursor_t = b;
  coverage->z_back = zback;
  coverage->pieces++;
  kernel_integrator_state.deep_optical_ledger[state].events++;
  return true;
}
#endif

CCL_NAMESPACE_END
