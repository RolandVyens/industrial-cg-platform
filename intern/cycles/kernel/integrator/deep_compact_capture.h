/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Included by the Film helpers inside their namespace and GPU feature guard. */
ccl_device_inline uint deep_compact_camera_index(ConstIntegratorState state)
{
  return kernel_integrator_state.deep_optical_inputs ?
             uint(state) + kernel_integrator_state.deep_compact_camera_base :
             INTEGRATOR_STATE(state, path, deep_frame_camera_index);
}

ccl_device_inline bool deep_compact_camera_chain(ConstIntegratorState state)
{
  if (!kernel_integrator_state.deep_compact_rays) return false;
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DEEP_VOLUME_OPTICAL) return true;
  if (!(INTEGRATOR_STATE(state, path, visibility) & PATH_RAY_VISIBILITY_CAMERA)) return false;
  const uint index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  if (index >= kernel_integrator_state.deep_native_camera_capacity) return false;
  const auto &camera = kernel_integrator_state.deep_compact_cameras[index];
  const float3 P = INTEGRATOR_STATE(state, ray, P), D = INTEGRATOR_STATE(state, ray, D);
  return P.x == camera.origin[0] && P.y == camera.origin[1] && P.z == camera.origin[2] &&
         D.x == camera.direction[0] && D.y == camera.direction[1] && D.z == camera.direction[2] &&
         INTEGRATOR_STATE(state, ray, time) == camera.time;
}

ccl_device_inline bool deep_compact_flush_candidate(ccl_global DeepCompactRayState &ray)
{
  if (!(ray.flags & DEEP_COMPACT_HAS_CANDIDATE)) return true;
  if (!deep_compact_emit(ray.candidate)) return false;
  ray.flags &= ~DEEP_COMPACT_HAS_CANDIDATE;
  return true;
}

ccl_device_inline double deep_compact_fraction(const double z,
                                             const DeepCompactPrimitive &candidate)
{
  return fmin(1.0, fmax((z - candidate.front) / (double(candidate.back) - candidate.front), 0.0));
}

ccl_device_inline bool deep_compact_submit_point(ccl_global DeepCompactRayState &ray,
                                               const DeepCompactPrimitive &point,
                                               const double log_m,
                                               const double log_q,
                                               const float physical_t)
{
  if (!deep_compact_flush_candidate(ray) || !deep_compact_emit(point)) {
    ray.pending_point = point;
    ray.pending_log_m = log_m; ray.pending_log_q = log_q;
    ray.pending_cursor = physical_t;
    ray.pending_phase = kernel_integrator_state.deep_optical_inputs ? 2u : 1u;
    ray.flags |= DEEP_COMPACT_STOP_SHARING | DEEP_COMPACT_PENDING_POINT;
    return false;
  }
  ray.log_m = log_m; ray.log_q = log_q;
  ray.cursor = physical_t;
  ray.next_ordinal++;
  ray.flags &= ~DEEP_COMPACT_PENDING_POINT;
  if (log_m == -INFINITY) ray.flags |= DEEP_COMPACT_TERMINAL;
  return true;
}

ccl_device_inline bool deep_compact_resume_point(ccl_global DeepCompactRayState &ray)
{
  const uint phase = ray.pending_phase;
  const bool committed = deep_compact_submit_point(ray, ray.pending_point, ray.pending_log_m,
                                                  ray.pending_log_q, float(ray.pending_cursor));
  ray.pending_phase = phase;
  return committed;
}

struct DeepCompactNativeEmitter {
  static constexpr bool production = true;
  ccl_device_inline_method bool operator()(const DeepCompactPrimitive &value) const
  {
    return deep_compact_emit(value);
  }
};

template<typename Emit>
ccl_device_inline bool deep_compact_fit_impl(ccl_global DeepCompactRayState &ray,
                                            const DeepCompactPrimitive &right, const Emit &emit)
{
  if (!(ray.flags & DEEP_COMPACT_HAS_CANDIDATE)) {
    ray.candidate = ray.reference = right;
    ray.flags |= DEEP_COMPACT_HAS_CANDIDATE;
    return true;
  }
  const DeepCompactPrimitive left = ray.candidate;
  const double gap = fmax(0.0, double(right.front) - left.back);
  bool accept = left.kind == DEEP_COMPACT_VOLUME && right.kind == DEEP_COMPACT_VOLUME &&
                left.group == right.group && double(right.back) - right.front >= gap;
  DeepCompactPrimitive trial = left;
  if (accept) {
    const double total = left.q + right.q;
    const double center = (left.q * (double(left.front) + left.back) +
                           right.q * (double(right.front) + right.back)) / (2.0 * total);
    const double radius = fmin(center - left.front, double(right.back) - center);
    trial.front = float(center - radius);
    trial.back = float(center + radius);
    trial.q = total;
    trial.log_after = right.log_after;
    accept = total > 0 && radius > 0 && trial.back > trial.front &&
             fabs(deep_compact_fraction(ray.reference.back, trial) * total - ray.reference.q) <= .004 &&
             fabs(deep_compact_fraction(left.back, trial) * total - left.q) <= .004;
  }
  if (!accept) {
    if constexpr (Emit::production)
      if (kernel_integrator_state.deep_compact_trace)
        deep_trace_native(left, ray.reference, right, trial, false);
    if (!emit(left)) return false;
    ray.candidate = ray.reference = right;
    return true;
  }
  ray.candidate = trial;
  if constexpr (Emit::production)
    if (kernel_integrator_state.deep_compact_trace)
      deep_trace_native(left, ray.reference, right, trial, true);
  const double h_length = double(ray.reference.back) - ray.reference.front;
  const double length = double(right.back) - ray.reference.front;
  if (h_length > 0 && length > 0 && ray.reference.q / h_length <= left.q / length) {
    ray.reference.q *= length / h_length;
    ray.reference.back = right.back;
  }
  else {
    ray.reference = left;
    ray.reference.back = right.back;
  }
  return true;
}

ccl_device_inline bool deep_compact_fit(ccl_global DeepCompactRayState &ray,
                                       const DeepCompactPrimitive &right)
{
  return deep_compact_fit_impl(ray, right, DeepCompactNativeEmitter{});
}

ccl_device_inline float deep_compact_depth(KernelGlobals kg,
                                         ccl_global const DeepNativeCameraRecord *camera,
                                         const double t)
{
  const double z = deep_native_promoted_camera_depth(kg, camera, t);
  if (!(z >= 0 && z <= double(FLT_MAX))) return -1.0f;
  if (camera->flags & DEEP_NATIVE_CAMERA_RADIAL_DEPTH) {
    const Transform transform = kernel_data.cam.cameratoworld;
    const double along = (double(camera->origin[0]) - transform.x.w) * camera->direction[0] +
                         (double(camera->origin[1]) - transform.y.w) * camera->direction[1] +
                         (double(camera->origin[2]) - transform.z.w) * camera->direction[2] + t;
    if (along < 0) return -1.0f;
  }
  else if (!(camera->z_direction > 0)) return -1.0f;
  return float(z);
}

ccl_device_inline bool deep_compact_cell(KernelGlobals kg,
                                       ConstIntegratorState state,
                                       const float front,
                                       const float back,
                                       const double sigma,
                                       const uint group = 0)
{
  if (!deep_compact_camera_chain(state)) return true;
  const uint camera_index = deep_compact_camera_index(state);
  auto &ray = kernel_integrator_state.deep_compact_rays[camera_index];
  const bool supplement = kernel_integrator_state.deep_optical_inputs != nullptr;
  if (!supplement && (ray.flags & (DEEP_COMPACT_STOP_SHARING | DEEP_COMPACT_TERMINAL))) return true;
  if (!(sigma >= 0) || !(back >= front)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return false;
  }
  auto *camera = &kernel_integrator_state.deep_compact_cameras[camera_index];
  deep_witness_cell(kg, camera_index, front, back, sigma);
  double cursor = (ray.flags & DEEP_COMPACT_PENDING_CELL) ? ray.pending_cursor : double(front);
  while (cursor < back && ray.log_m != -INFINITY) {
    const double remaining_tau = sigma * (double(back) - cursor);
    if (!(remaining_tau >= 0) || remaining_tau > DBL_MAX) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_MASS_RANGE);
      return false;
    }
    if (sigma == 0) break;
    const double remaining_q = exp(ray.log_m) * (-expm1(-remaining_tau));
    const bool radial = camera->flags & DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
    double h = remaining_tau;
    if (remaining_q > .004) {
      if (radial) h = -log1p(-fmin(.004 / exp(ray.log_m), .9999999999999999));
      else h = fmin(remaining_tau, sqrt(.032 / exp(ray.log_m)));
    }
    const double next = h >= remaining_tau ? double(back) : cursor + h / sigma;
    if (!(next > cursor)) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
      return false;
    }
    DeepCompactPrimitive p = {};
    p.front = deep_compact_depth(kg, camera, cursor);
    p.back = deep_compact_depth(kg, camera, next);
    p.camera = camera_index;
    p.ordinal = ray.next_ordinal;
    p.kind = p.front == p.back ? DEEP_COMPACT_COLLAPSED_VOLUME : DEEP_COMPACT_VOLUME;
    p.group = group;
    p.log_before = deep_compact_logadd(ray.log_m, ray.log_q);
    const double next_m = ray.log_m - sigma * (next - cursor);
    if (next_m == -INFINITY) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_MASS_RANGE);
      return false;
    }
    p.log_after = deep_compact_logadd(next_m, ray.log_q);
    p.q = exp(ray.log_m) * (-expm1(-sigma * (next - cursor)));
    if (!(p.q > 0)) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_MASS_RANGE);
      return false;
    }
    if (!(p.front >= 0) || p.back < p.front) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
      return false;
    }
    if (!deep_compact_fit(ray, p)) {
      ray.pending_front = front; ray.pending_back = back;
      ray.pending_sigma = sigma; ray.pending_cursor = cursor; ray.pending_phase = group;
      ray.flags |= DEEP_COMPACT_STOP_SHARING | DEEP_COMPACT_PENDING_CELL;
      return false;
    }
    ray.log_m = next_m;
    ray.next_ordinal++;
    cursor = next;
  }
  ray.flags &= ~DEEP_COMPACT_PENDING_CELL;
  ray.cursor = back;
  return true;
}

ccl_device_inline void deep_compact_save_stack(KernelGlobals kg, ConstIntegratorState state,
                                              const bool boundary = false)
{
#ifdef __VOLUME__
  if (!deep_compact_camera_chain(state) || !kernel_integrator_state.deep_compact_stacks ||
      kernel_integrator_state.deep_optical_inputs)
    return;
  const uint index = deep_compact_camera_index(state);
  auto &ray = kernel_integrator_state.deep_compact_rays[index];
  if (ray.flags & DEEP_COMPACT_TERMINAL) return;
  if ((ray.flags & DEEP_COMPACT_STOP_SHARING) &&
      (!(ray.flags & DEEP_COMPACT_PENDING_POINT) || ray.pending_phase != 1u)) return;
  const float start = INTEGRATOR_STATE(state, ray, tmin);
  if (!boundary && ray.stack_valid && ray.stack_front == start) return;
  uint hash = 2166136261u;
  bool ended = false;
  for (int i = 0; i < kernel_data.volume_stack_size; i++) {
    auto &entry = kernel_integrator_state.deep_compact_stacks[
        size_t(index) * kernel_integrator_state.deep_compact_stack_stride + i];
    entry.shader = ended ? SHADER_NONE : INTEGRATOR_STATE_ARRAY(state, volume_stack, i, shader);
    entry.object = entry.shader == SHADER_NONE ? OBJECT_NONE :
        INTEGRATOR_STATE_ARRAY(state, volume_stack, i, object);
    ended |= entry.shader == SHADER_NONE;
    if (!ended) {
      hash = (hash ^ uint(entry.object)) * 16777619u;
      hash = (hash ^ uint(entry.shader)) * 16777619u;
    }
    if (entry.shader != SHADER_NONE && entry.object == OBJECT_NONE) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_BACKGROUND);
      return;
    }
  }
  ray.stack_valid = 1; ray.stack_front = start;
  ray.reserved = hash;
  if (ray.flags & DEEP_COMPACT_PENDING_POINT) ray.pending_phase = 3u;
#else
  (void)kg; (void)state; (void)boundary;
#endif
}
