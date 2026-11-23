/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once
#include <cfloat>

#include "kernel/film/deep_write.h"
#include "kernel/film/write.h"
#include "kernel/integrator/state_util.h"

#include "kernel/integrator/shadow_catcher.h"

#include "kernel/camera/camera.h"

#include "kernel/sample/pattern.h"
#include "util/atomic.h"

#include "kernel/integrator/deep_bounded_capture.h"

CCL_NAMESPACE_BEGIN

#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline bool deep_native_colour_capture_enabled()
{
  return kernel_integrator_state.deep_native_cameras &&
         !kernel_data.film.deep_simple_mode &&
         kernel_integrator_state.deep_native_capture_rgb;
}

ccl_device_inline double deep_native_promoted_camera_depth(
    KernelGlobals kg, ccl_global const DeepNativeCameraRecord *camera, const double t)
{
  if (camera->flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH)
    return double(camera->z_origin) + double(camera->z_direction) * double(t);
  const Transform transform = kernel_data.cam.cameratoworld;
  const double x = double(camera->origin[0]) - double(transform.x.w) +
                   double(camera->direction[0]) * t;
  const double y = double(camera->origin[1]) - double(transform.y.w) +
                   double(camera->direction[1]) * t;
  const double z = double(camera->origin[2]) - double(transform.z.w) +
                   double(camera->direction[2]) * t;
  return sqrt(x * x + y * y + z * z);
}

ccl_device_inline void deep_native_capture_error(const uint32_t error)
{
  atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_native_capture_error,
                             error);
}

ccl_device_inline bool deep_native_validation_once(const uint32_t fault)
{
  return kernel_integrator_state.deep_native_cameras &&
         (kernel_integrator_state.deep_native_validation_faults & fault) &&
         !(atomic_fetch_and_or_uint32(
               &kernel_integrator_state.queue_counter->deep_native_validation_seen, fault) &
           fault);
}

ccl_device_inline bool deep_native_validation_deny(const uint32_t fault)
{
  if (kernel_integrator_state.deep_native_cameras &&
      (kernel_integrator_state.deep_native_validation_faults & fault))
  {
    atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_native_validation_seen,
                               fault);
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return true;
  }
  return false;
}

ccl_device_inline bool deep_native_capture_combined_call(const uint32_t flags)
{
  if (deep_native_colour_capture_enabled() && deep_native_path_writes_combined(flags))
    return true;
  if (deep_native_colour_capture_enabled() && atomic_fetch_and_add_uint32(
          &kernel_integrator_state.queue_counter->deep_native_auxiliary_film_calls, 1) == UINT_MAX)
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
  return false;
}

ccl_device_inline ccl_global DeepNativeCameraRecord *deep_native_camera(ConstIntegratorState state)
{
  if (!kernel_integrator_state.deep_native_cameras) {
    return nullptr;
  }
  const uint index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  if (index >= kernel_integrator_state.deep_native_camera_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return nullptr;
  }
  ccl_global DeepNativeCameraRecord *camera = &kernel_integrator_state.deep_native_cameras[index];
  if (camera->pixel != INTEGRATOR_STATE(state, path, render_pixel_index) ||
      camera->sample != INTEGRATOR_STATE(state, path, sample))
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return nullptr;
  }
  return camera;
}

ccl_device_inline double deep_compact_logadd(const double a, const double b)
{
  if (a == -INFINITY) return b;
  if (b == -INFINITY) return a;
  const double hi = a > b ? a : b, lo = a > b ? b : a;
  return hi + log1p(exp(lo - hi));
}

ccl_device_inline bool deep_compact_emit(const DeepCompactPrimitive &primitive)
{
  auto *counter = &kernel_integrator_state.queue_counter->deep_compact_reserved;
  uint index;
#if defined(__KERNEL_CUDA__) || defined(__KERNEL_HIP__)
  index = atomicAdd(counter, 0u);
  for (;;) {
    if (index >= kernel_integrator_state.deep_compact_capacity) return false;
    const uint previous = atomicCAS(counter, index, index + 1);
    if (previous == index) break;
    index = previous;
  }
#else
  index = atomic_fetch_and_add_uint32(counter, 1);
  if (index >= kernel_integrator_state.deep_compact_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return false;
  }
#endif
  kernel_integrator_state.deep_compact_primitives[index] = primitive;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_compact_written, 1);
  return true;
}

#include "kernel/integrator/deep_compact_witness.h"
#include "kernel/integrator/deep_compact_trace.h"
#include "kernel/integrator/deep_compact_capture.h"
#include "kernel/integrator/deep_compact_cases.h"

ccl_device_inline float deep_native_surface_depth(KernelGlobals kg,
                                                  ConstIntegratorState state,
                                                  const ccl_private ShaderData *sd)
{
  if (deep_compact_camera_chain(state)) {
    const auto *camera = &kernel_integrator_state.deep_compact_cameras[deep_compact_camera_index(state)];
    /* A volume ending at this intersection and its point must round the same
     * camera-parametric depth. Projecting the independently rounded sd.P can
     * move the opaque point one ULP in front of the preceding volume. */
    return deep_compact_depth(kg, camera, double(sd->ray_length));
  }
  return camera_z_depth(kg, sd->P);
}

ccl_device_inline void deep_compact_capture_surface(KernelGlobals kg,
                                                   ConstIntegratorState state,
                                                   const ccl_private ShaderData *sd,
                                                   const Spectrum transparency,
                                                   const Spectrum holdout)
{
  if (!deep_compact_camera_chain(state))
    return;
  const uint index = deep_compact_camera_index(state);
  auto &ray = kernel_integrator_state.deep_compact_rays[index];
  if (!kernel_integrator_state.deep_optical_inputs && (ray.flags & DEEP_COMPACT_STOP_SHARING)) return;
  if ((ray.flags & DEEP_COMPACT_TERMINAL) ||
      ((ray.flags & DEEP_COMPACT_HAS_SURFACE) && ray.last_surface_t == sd->ray_length &&
       ray.last_surface_object == uint(sd->object) && ray.last_surface_prim == uint(sd->prim)))
    return;
  if ((sd->object_flag & SD_OBJECT_SHADOW_CATCHER) ||
      (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SHADOW_CATCHER_PASS)) {
    const uint prior = atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_native_capture_error, DEEP_NATIVE_ERROR_SURFACE);
    if (!(prior & DEEP_NATIVE_ERROR_SURFACE))
      printf("DEEP_UNSUPPORTED family=shadow_catcher camera=%u object=%d sample=%u\n",
             index, sd->object, INTEGRATOR_STATE(state, path, sample));
    return;
  }
  const double t = double(average(saturate(transparency)));
  const double h = fmin(1.0 - t, double(average(min(max(holdout, zero_spectrum()),
                                                  one_spectrum() - saturate(transparency)))));
  const double absorption = fmax(0.0, 1.0 - t - h);
  DeepCompactPrimitive p = {};
  p.front = p.back = deep_native_surface_depth(kg, state, sd);
  p.log_before = deep_compact_logadd(ray.log_m, ray.log_q);
  const double new_q = h > 0 ? deep_compact_logadd(ray.log_q, ray.log_m + log(h)) : ray.log_q;
  const double new_m = t > 0 ? ray.log_m + log(t) : -INFINITY;
  p.log_after = deep_compact_logadd(new_m, new_q);
  p.q = absorption > 0 ? exp(ray.log_m + log(absorption)) : 0;
  p.camera = index;
  p.ordinal = ray.next_ordinal;
  p.kind = h > 0 ? DEEP_COMPACT_HOLDOUT : DEEP_COMPACT_SURFACE;
  p.group = uint32_t(sd->shader_flag) |
            ((sd->runtime_flag & SR_BACKFACING) ? DEEP_COMPACT_SURFACE_BACKFACING : 0u) |
            ((sd->runtime_flag & SR_RAY_PORTAL) ? DEEP_COMPACT_SURFACE_RAY_PORTAL : 0u);
  p.object = uint(sd->object); p.prim = uint(sd->prim); p.shader = uint(sd->shader);
  p.normal = deep_pack_geometric_normal(sd->Ng);
  deep_witness_point(kg, index, p.front, sd->ray_length, t, h);
  if (absorption > 0 && ray.log_m != -INFINITY && !(p.q > 0)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_MASS_RANGE);
    return;
  }
  if (!(p.front >= 0) || !isfinite_safe(p.front)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return;
  }
  /* A zero-opacity holdout still owns a propagation boundary. */
  if (!deep_compact_submit_point(ray, p, new_m, new_q, sd->ray_length)) return;
  ray.last_surface_t = sd->ray_length;
  ray.last_surface_object = uint(sd->object); ray.last_surface_prim = uint(sd->prim);
  ray.flags |= DEEP_COMPACT_HAS_SURFACE;
  if (new_m == -INFINITY) ray.flags |= DEEP_COMPACT_TERMINAL;
}

ccl_device_inline void deep_compact_capture_background(KernelGlobals kg, ConstIntegratorState state,
                                                      const bool transparent)
{
  if (!deep_compact_camera_chain(state))
    return;
  const uint index = deep_compact_camera_index(state);
  auto *camera = &kernel_integrator_state.deep_compact_cameras[index];
  camera->flags |= DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
  auto &ray = kernel_integrator_state.deep_compact_rays[index];
  if (!kernel_integrator_state.deep_optical_inputs && (ray.flags & DEEP_COMPACT_STOP_SHARING)) return;
  deep_witness_background(kg, index, transparent);
  if (transparent) {
    ray.cursor = camera->tmax;
    if (deep_compact_flush_candidate(ray)) ray.flags |= DEEP_COMPACT_EXTINCTION_DONE;
    else ray.flags |= DEEP_COMPACT_STOP_SHARING | DEEP_COMPACT_FINISH_PENDING;
    return;
  }
  if (ray.flags & DEEP_COMPACT_TERMINAL) return;
  DeepCompactPrimitive p = {};
  p.front = p.back = FLT_MAX;
  p.camera = index;
  p.ordinal = ray.next_ordinal;
  p.kind = DEEP_COMPACT_BACKGROUND;
  p.log_before = deep_compact_logadd(ray.log_m, ray.log_q);
  p.log_after = ray.log_q;
  p.q = exp(ray.log_m);
  if (ray.log_m != -INFINITY && !(p.q > 0)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_MASS_RANGE);
    return;
  }
  deep_compact_submit_point(ray, p, -INFINITY, ray.log_q, camera->tmax);
}

ccl_device_inline bool deep_native_capture_volume_film_call(ConstIntegratorState state,
                                                            const uint32_t segment_index,
                                                            const Spectrum raw_contribution,
                                                            const float clamp_gain,
                                                            const Spectrum contribution)
{
  if (!deep_native_colour_capture_enabled()) {
    return true;
  }
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  if (!camera || segment_index >= kernel_integrator_state.deep_native_homogeneous_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_EMISSION_CALL);
    return false;
  }
  ccl_global DeepNativeHomogeneousRecord *segment =
      &kernel_integrator_state.deep_native_homogeneous[segment_index];
  if (segment->camera_index != INTEGRATOR_STATE(state, path, deep_frame_camera_index) ||
      segment->pixel != camera->pixel || segment->sample != camera->sample ||
      segment->emission_calls != 0)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_EMISSION_CALL);
    return false;
  }
  const float3 rgb = spectrum_to_rgb(contribution);
  const float3 raw_rgb = spectrum_to_rgb(raw_contribution);
  if (!isfinite_safe(raw_rgb) || !isfinite_safe(rgb) || !isfinite_safe(clamp_gain)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return false;
  }
  segment->pre_clamp_rgb[0] = raw_rgb.x;
  segment->pre_clamp_rgb[1] = raw_rgb.y;
  segment->pre_clamp_rgb[2] = raw_rgb.z;
  segment->clamp_gain = clamp_gain;
  segment->post_clamp_rgb[0] = rgb.x;
  segment->post_clamp_rgb[1] = rgb.y;
  segment->post_clamp_rgb[2] = rgb.z;
  segment->emission_calls = 1;
  segment->flags |= DEEP_NATIVE_HOMOGENEOUS_EMISSION_FILM_CALL;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_emission_calls,
                              1);
  return true;
}

/* The old immutable prefix contains only descriptors pinned by actual queued
 * states. New roots use this dispatch's append window and global ordinals. */
ccl_device_inline uint32_t deep_native_root_slot(const uint32_t camera_index,
                                                 const uint32_t handle,
                                                 const bool joined_snapshot = false)
{
  if (!handle)
    return DEEP_NATIVE_INVALID_INDEX;
  ccl_global DeepNativeRootRecord *root = nullptr;
  uint32_t slot = DEEP_NATIVE_INVALID_INDEX;
  const uint resident = kernel_integrator_state.deep_native_root_resident_count;
  if (handle > kernel_integrator_state.deep_native_root_base) {
    const uint index = handle - kernel_integrator_state.deep_native_root_base - 1;
    if (kernel_integrator_state.deep_native_roots &&
        index < kernel_integrator_state.deep_native_root_capacity &&
        (!joined_snapshot ||
         index < kernel_integrator_state.queue_counter->deep_native_root_written))
    {
      root = &kernel_integrator_state.deep_native_roots[index];
      slot = resident + index;
    }
  }
  else if (kernel_integrator_state.deep_native_root_resident) {
    uint low = 0, high = resident;
    while (low < high) {
      const uint middle = low + (high - low) / 2;
      if (kernel_integrator_state.deep_native_root_resident[middle].ordinal < handle)
        low = middle + 1;
      else
        high = middle;
    }
    if (low < resident) {
      root = &kernel_integrator_state.deep_native_root_resident[low];
      slot = low;
    }
  }
  if (!root || root->camera_index != camera_index || root->ordinal != handle ||
      root->kind == DEEP_NATIVE_ROOT_NONE)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return DEEP_NATIVE_INVALID_INDEX;
  }
  return slot;
}

ccl_device_inline ccl_global DeepNativeRootRecord *deep_native_root_handle(
    const uint32_t camera_index, const uint32_t handle)
{
  const uint slot = deep_native_root_slot(camera_index, handle);
  if (slot == DEEP_NATIVE_INVALID_INDEX)
    return nullptr;
  return slot < kernel_integrator_state.deep_native_root_resident_count ?
             &kernel_integrator_state.deep_native_root_resident[slot] :
             &kernel_integrator_state
                  .deep_native_roots[slot -
                                     kernel_integrator_state.deep_native_root_resident_count];
}

ccl_device_inline ccl_global DeepNativeRootRecord *deep_native_root(const uint32_t camera_index)
{
  if (!deep_native_colour_capture_enabled() ||
      camera_index >= kernel_integrator_state.deep_native_camera_capacity)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return nullptr;
  }
  return deep_native_root_handle(
      camera_index, kernel_integrator_state.deep_native_cameras[camera_index].root_handle);
}

ccl_device_inline bool deep_native_is_camera_chain(ConstIntegratorState state)
{
  if (!deep_native_colour_capture_enabled() ||
      !deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
  {
    return false;
  }
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  return camera && camera->root_handle == 0;
}

ccl_device_inline uint32_t deep_native_append_root(ConstIntegratorState state,
                                                   const uint32_t kind,
                                                   const float z,
                                                   const int object,
                                                   const int prim,
                                                   const int shader,
                                                   const float physical_t = -1.0f,
                                                   const uint32_t packed_normal = 0,
                                                   const bool opaque_surface = true,
                                                   const bool mnee_admission = false,
                                                   const bool world_support = false)
{
  if (!deep_native_colour_capture_enabled() ||
      !deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
  {
    return 0;
  }
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  if (!camera) {
    return 0;
  }
  if (!isfinite_safe(z) || z < 0.0f ||
      (kind != DEEP_NATIVE_ROOT_SURFACE && kind != DEEP_NATIVE_ROOT_WORLD &&
       kind != DEEP_NATIVE_ROOT_LIGHT && kind != DEEP_NATIVE_ROOT_VOLUME_SCATTER &&
       kind != DEEP_NATIVE_ROOT_VOLUME_DIRECT && kind != DEEP_NATIVE_ROOT_DISTANT))
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return 0;
  }
  const float3 P = INTEGRATOR_STATE(state, ray, P), D = INTEGRATOR_STATE(state, ray, D);
  if (P.x != camera->origin[0] || P.y != camera->origin[1] || P.z != camera->origin[2] ||
      D.x != camera->direction[0] || D.y != camera->direction[1] || D.z != camera->direction[2] ||
      INTEGRATOR_STATE(state, ray, time) != camera->time)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return 0;
  }
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_root_reserved, 1);
  if (index >= kernel_integrator_state.deep_native_root_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return 0;
  }
  DeepNativeRootRecord record = {};
  record.z = z;
  record.kind = kind;
  if (uint64_t(kernel_integrator_state.deep_native_root_base) + index + 1 >= UINT32_MAX) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return 0;
  }
  record.ordinal = kernel_integrator_state.deep_native_root_base + index + 1;
  record.camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  record.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT |
                 (kind == DEEP_NATIVE_ROOT_SURFACE && opaque_surface ?
                      DEEP_NATIVE_ROOT_OPAQUE_SURFACE :
                  kind == DEEP_NATIVE_ROOT_WORLD ? DEEP_NATIVE_ROOT_WORLD_TERMINAL :
                                                   DEEP_NATIVE_ROOT_RADIANCE_ONLY);
  record.object = object;
  if (world_support) {
    if ((kind != DEEP_NATIVE_ROOT_VOLUME_DIRECT && kind != DEEP_NATIVE_ROOT_VOLUME_SCATTER) ||
        !kernel_integrator_state.deep_native_full_optical)
      deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    record.flags |= DEEP_NATIVE_ROOT_WORLD_SUPPORT;
  }
  if (mnee_admission) {
    if (kind != DEEP_NATIVE_ROOT_SURFACE || opaque_surface ||
        !kernel_integrator_state.deep_native_full_optical)
      deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    record.flags |= DEEP_NATIVE_ROOT_MNEE_ADMISSION;
  }
  record.prim = prim;
  record.shader = shader;
  record.physical_t = physical_t;
  record.packed_normal = packed_normal;
  kernel_integrator_state.deep_native_roots[index] = record;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_root_written, 1);
  return record.ordinal;
}

ccl_device_inline void deep_native_set_root(ConstIntegratorState state,
                                            const uint32_t kind,
                                            const float z,
                                            const int object,
                                            const int prim,
                                            const int shader,
                                            const float physical_t = -1.0f,
                                            const uint32_t packed_normal = 0,
                                            const bool world_support = false)
{
  if (!deep_native_colour_capture_enabled() ||
      !deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
  {
    return;
  }
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  if (!camera || camera->root_handle) {
    return;
  }
  camera->root_handle = deep_native_append_root(
      state, kind, z, object, prim, shader, physical_t, packed_normal, true, false, world_support);
  INTEGRATOR_STATE_WRITE(state, path, deep_frame_root_handle) = camera->root_handle;
}

ccl_device_inline void deep_native_copy_camera_to_shadow(IntegratorShadowState shadow_state,
                                                         ConstIntegratorState state)
{
  if (kernel_integrator_state.deep_native_cameras &&
      kernel_integrator_state.shadow_path.deep_frame_camera_index)
  {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, deep_frame_camera_index) = INTEGRATOR_STATE(
        state, path, deep_frame_camera_index);
  }
}

ccl_device_inline uint32_t deep_native_reserve_point_film_call(const uint32_t flags)
{
  if (!deep_native_colour_capture_enabled() || !deep_native_path_writes_combined(flags))
    return DEEP_NATIVE_INVALID_INDEX;
  if (kernel_integrator_state.deep_compact_root_colours) return 0;
  if (deep_native_validation_deny(DEEP_NATIVE_FAULT_POINT_DENY))
    return DEEP_NATIVE_INVALID_INDEX;
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_point_reserved, 1);
  if (index >= kernel_integrator_state.deep_native_point_capacity ||
      uint64_t(kernel_integrator_state.deep_native_point_base) + index >= UINT32_MAX)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return DEEP_NATIVE_INVALID_INDEX;
  }
  return index;
}

ccl_device_inline uint32_t deep_native_reserve_expression_film_call()
{
  if (deep_native_validation_deny(DEEP_NATIVE_FAULT_EXPRESSION_DENY))
    return DEEP_NATIVE_INVALID_INDEX;
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_call_reserved, 1);
  if (!kernel_integrator_state.deep_native_expression_calls ||
      index >= kernel_integrator_state.deep_native_expression_call_capacity ||
      uint64_t(kernel_integrator_state.deep_native_expression_call_base) + index + 1 >= UINT32_MAX)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return DEEP_NATIVE_INVALID_INDEX;
  }
  return index;
}

ccl_device_inline bool deep_native_capture_point_film_call(const uint32_t camera_index,
                                                           const uint32_t pixel,
                                                           const uint32_t sample,
                                                           const uint32_t path_flags,
                                                           const Spectrum raw,
                                                           const Spectrum clamped,
                                                           const float gain,
                                                           const Spectrum routing_scale,
                                                           const Spectrum final_rgb,
                                                           const uint32_t index,
                                                           const uint32_t root_handle = 0)
{
  if (!deep_native_colour_capture_enabled()) {
    return true;
  }
  if (!deep_native_capture_combined_call(path_flags))
    return true;
  ccl_global DeepNativeRootRecord *root = root_handle ?
                                              deep_native_root_handle(camera_index, root_handle) :
                                              deep_native_root(camera_index);
  if (!root || root->kind == DEEP_NATIVE_ROOT_NONE ||
      (!kernel_integrator_state.deep_native_points && !kernel_integrator_state.deep_compact_root_colours))
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_EMISSION_CALL);
    return false;
  }
  const DeepNativeCameraRecord camera = kernel_integrator_state.deep_native_cameras[camera_index];
  if (camera.pixel != pixel || camera.sample != sample) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return false;
  }
  if (!isfinite_safe(raw) || !isfinite_safe(clamped) || !isfinite_safe(gain) ||
      !isfinite_safe(routing_scale) || !isfinite_safe(final_rgb))
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return false;
  }
  if (kernel_integrator_state.deep_compact_root_colours) {
    if (!root->ordinal || root->ordinal > kernel_integrator_state.deep_compact_root_colour_capacity) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
      return false;
    }
    auto &bucket = kernel_integrator_state.deep_compact_root_colours[root->ordinal - 1];
    const float3 colour = spectrum_to_rgb(final_rgb);
    atomic_add_and_fetch_float(&bucket.rgb[0], colour.x);
    atomic_add_and_fetch_float(&bucket.rgb[1], colour.y);
    atomic_add_and_fetch_float(&bucket.rgb[2], colour.z);
    atomic_fetch_and_or_uint32(&bucket.flags, 1u);
    return true;
  }
  if (index >= kernel_integrator_state.deep_native_point_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return false;
  }
  DeepNativePointRecord record = {};
  record.camera_index = camera_index;
  record.pixel = pixel;
  record.sample = sample;
  record.ordinal = kernel_integrator_state.deep_native_point_base + index;
  record.root_handle = root->ordinal;
  record.root_kind = root->kind;
  record.root_ordinal = root->ordinal;
  record.path_flags = path_flags;
  record.routing = DEEP_NATIVE_POINT_COMBINED;
  record.z = root->z;
  const float3 pre = spectrum_to_rgb(raw), post = spectrum_to_rgb(clamped);
  const float3 scale = spectrum_to_rgb(routing_scale), rgb = spectrum_to_rgb(final_rgb);
  record.pre_clamp_rgb[0] = pre.x;
  record.pre_clamp_rgb[1] = pre.y;
  record.pre_clamp_rgb[2] = pre.z;
  record.clamp_rgb[0] = post.x;
  record.clamp_rgb[1] = post.y;
  record.clamp_rgb[2] = post.z;
  record.final_rgb[0] = rgb.x;
  record.final_rgb[1] = rgb.y;
  record.final_rgb[2] = rgb.z;
  record.routing_scale[0] = scale.x;
  record.routing_scale[1] = scale.y;
  record.routing_scale[2] = scale.z;
  record.clamp_gain = gain;
  kernel_integrator_state.deep_native_points[index] = record;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_point_written,
                              1);
  return true;
}

ccl_device_inline uint32_t deep_native_expression_node(ConstIntegratorState state,
                                                       const uint32_t operation,
                                                       const uint32_t a,
                                                       const uint32_t b = 0,
                                                       const uint32_t source = 0,
                                                       const Spectrum weight = one_spectrum(),
                                                       const bool select_b = false)
{
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_node_reserved, 1);
  if (!kernel_integrator_state.deep_native_expression_nodes ||
      index >= kernel_integrator_state.deep_native_expression_node_capacity)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return 0;
  }
  DeepNativeExpressionNode node = {};
  node.camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  node.ordinal = kernel_integrator_state.deep_native_expression_node_base + index + 1;
  node.operation = operation;
  node.a = a;
  node.b = b;
  node.source = source;
  node.select_b = select_b;
  node.weight[0] = weight.x;
  node.weight[1] = weight.y;
  node.weight[2] = weight.z;
  kernel_integrator_state.deep_native_expression_nodes[index] = node;
  atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_node_written, 1);
  return kernel_integrator_state.deep_native_expression_node_base + index + 1;
}

ccl_device_inline bool deep_native_world_support(KernelGlobals kg,
                                                 ConstIntegratorState state,
                                                 const float physical_t)
{
  if (!deep_native_colour_capture_enabled() ||
      !kernel_integrator_state.deep_native_full_optical ||
      kernel_data.background.volume_shader == SHADER_NONE || kernel_data.volume_stack_size < 2)
    return false;
  const ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  return camera && physical_t > camera->tmax &&
         INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object) ==
             kernel_data.background.object_index &&
         (INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, shader) & SHADER_MASK) ==
             (kernel_data.background.volume_shader & SHADER_MASK) &&
         INTEGRATOR_STATE_ARRAY(state, volume_stack, 1, shader) == SHADER_NONE;
}

ccl_device_inline uint32_t deep_native_expression_term(KernelGlobals kg,
                                                       ConstIntegratorState state,
                                                       const ccl_private Ray *ray,
                                                       const ccl_private ShaderData *sd,
                                                       const float tmin,
                                                       const float tmax,
                                                       const Spectrum raw,
                                                       const Spectrum tau,
                                                       const bool point = false,
                                                       const Spectrum sigma_t = zero_spectrum())
{
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_source_reserved, 1);
  if (!kernel_integrator_state.deep_native_expression_sources ||
      index >= kernel_integrator_state.deep_native_expression_source_capacity)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return 0;
  }
  const uint32_t camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  if (!camera) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return 0;
  }
  DeepNativeExpressionSource source = {};
  source.camera_index = camera_index;
  source.ordinal = kernel_integrator_state.deep_native_expression_source_base + index + 1;
  source.root_handle = camera->root_handle;
  source.object = sd->object;
  source.prim = sd->prim;
  source.shader = sd->shader;
  const float3 rgb = spectrum_to_rgb(raw);
  source.raw_rgb[0] = rgb.x;
  source.raw_rgb[1] = rgb.y;
  source.raw_rgb[2] = rgb.z;
  if (!source.root_handle) {
    if (ray->P.x != camera->origin[0] || ray->P.y != camera->origin[1] ||
        ray->P.z != camera->origin[2] || ray->D.x != camera->direction[0] ||
        ray->D.y != camera->direction[1] || ray->D.z != camera->direction[2] ||
        ray->time != camera->time)
      deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    source.flags = point ? DEEP_NATIVE_EXPR_CAMERA_POINT : DEEP_NATIVE_EXPR_CAMERA_SPAN;
    source.tmin = tmin;
    source.tmax = tmax;
    source.z = camera_z_depth(kg, ray->P + ray->D * tmin);
    source.zback = camera_z_depth(kg, ray->P + ray->D * tmax);
    const bool world_support = deep_native_world_support(kg, state, tmax);
    if (world_support) {
      source.flags |= DEEP_NATIVE_EXPR_WORLD_SUPPORT;
      source.object = INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object);
      source.shader = INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, shader);
      source.prim = PRIM_NONE;
      const double front = deep_native_promoted_camera_depth(kg, camera, tmin);
      const double back = deep_native_promoted_camera_depth(kg, camera, tmax);
      if (!point) {
        const bool radial = camera->flags & DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
        if (!(back >= front) || !deep_native_world_span_depth(front, radial, source.z) ||
            !deep_native_world_span_depth(back, radial, source.zback))
          deep_native_capture_error(DEEP_NATIVE_ERROR_INCOMPLETE);
      }
      else {
        if (!(front >= 0 && back >= front && back <= double(FLT_MAX)))
          deep_native_capture_error(DEEP_NATIVE_ERROR_INCOMPLETE);
        source.z = float(front);
        source.zback = float(back);
      }
    }
    if (!point) {
      if (world_support) {
        const double length = double(tmax) - double(tmin);
        source.tau[0] = double(sigma_t.x) * length;
        source.tau[1] = double(sigma_t.y) * length;
        source.tau[2] = double(sigma_t.z) * length;
      }
      else {
        source.tau[0] = tau.x;
        source.tau[1] = tau.y;
        source.tau[2] = tau.z;
      }
    }
  }
  else {
    const auto *root = deep_native_root_handle(camera_index, source.root_handle);
    if (!root)
      return 0;
    source.flags = DEEP_NATIVE_EXPR_INHERITED_POINT;
    source.z = source.zback = root->z;
    source.tmin = source.tmax = root->physical_t;
    source.packed_normal = root->packed_normal;
  }
  kernel_integrator_state.deep_native_expression_sources[index] = source;
  atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_source_written, 1);
  return deep_native_expression_node(state,
                                     DEEP_NATIVE_EXPR_TERM,
                                     0,
                                     0,
                                     kernel_integrator_state.deep_native_expression_source_base +
                                         index + 1);
}

ccl_device_inline bool deep_native_expression_film_call(ConstIntegratorState state,
                                                        const uint32_t head,
                                                        const Spectrum raw,
                                                        const Spectrum post,
                                                        const float gain,
                                                        const uint32_t index)
{
  if (!kernel_integrator_state.deep_native_expression_calls ||
      index >= kernel_integrator_state.deep_native_expression_call_capacity)
  {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return false;
  }
  if (!isfinite_safe(raw) || !isfinite_safe(post) || !isfinite_safe(gain)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return false;
  }
  DeepNativeExpressionCall call = {};
  call.camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  call.ordinal = kernel_integrator_state.deep_native_expression_call_base + index + 1;
  call.head = head;
  call.path_flags = INTEGRATOR_STATE(state, path, flag);
  call.pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  call.sample = INTEGRATOR_STATE(state, path, sample);
  call.routing = DEEP_NATIVE_POINT_COMBINED;
  call.clamp_gain = gain;
  const float3 before = spectrum_to_rgb(raw), after = spectrum_to_rgb(post);
  call.pre_clamp_rgb[0] = before.x;
  call.pre_clamp_rgb[1] = before.y;
  call.pre_clamp_rgb[2] = before.z;
  call.post_clamp_rgb[0] = after.x;
  call.post_clamp_rgb[1] = after.y;
  call.post_clamp_rgb[2] = after.z;
  kernel_integrator_state.deep_native_expression_calls[index] = call;
  atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_expression_call_written, 1);
  return true;
}
#endif

/* --------------------------------------------------------------------
 * BSDF Evaluation
 *
 * BSDF evaluation result, split between diffuse and glossy. This is used to
 * accumulate render passes separately. Note that reflection, transmission
 * and volume scattering are written to different render passes, but we assume
 * that only one of those can happen at a bounce, and so do not need to accumulate
 * them separately. */

ccl_device_inline void bsdf_eval_init(ccl_private BsdfEval *eval,
                                      const ccl_private ShaderClosure *sc,
                                      const float3 wo,
                                      Spectrum value)
{
  eval->diffuse = zero_spectrum();
  eval->glossy = zero_spectrum();

  if (CLOSURE_IS_BSDF_DIFFUSE(sc->type)) {
    eval->diffuse = value;
  }
  else if (CLOSURE_IS_BSDF_GLOSSY(sc->type)) {
    eval->glossy = value;
  }
  else if (CLOSURE_IS_GLASS(sc->type)) {
    /* Glass can count as glossy or transmission, depending on which side we end up on. */
    if (dot(sc->N, wo) > 0.0f) {
      eval->glossy = value;
    }
  }

  eval->sum = value;
}

ccl_device_inline void bsdf_eval_init(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->diffuse = zero_spectrum();
  eval->glossy = zero_spectrum();
  eval->sum = value;
}

ccl_device_inline void bsdf_eval_accum(ccl_private BsdfEval *eval,
                                       const ccl_private ShaderClosure *sc,
                                       const float3 wo,
                                       Spectrum value)
{
  if (CLOSURE_IS_BSDF_DIFFUSE(sc->type)) {
    eval->diffuse += value;
  }
  else if (CLOSURE_IS_BSDF_GLOSSY(sc->type)) {
    eval->glossy += value;
  }
  else if (CLOSURE_IS_GLASS(sc->type)) {
    if (dot(sc->N, wo) > 0.0f) {
      eval->glossy += value;
    }
  }

  eval->sum += value;
}

ccl_device_inline void bsdf_eval_accum(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->sum += value;
}

ccl_device_inline bool bsdf_eval_is_zero(ccl_private BsdfEval *eval)
{
  return is_zero(eval->sum);
}

ccl_device_inline void bsdf_eval_mul(ccl_private BsdfEval *eval, const float value)
{
  eval->diffuse *= value;
  eval->glossy *= value;
  eval->sum *= value;
}

ccl_device_inline void bsdf_eval_mul(ccl_private BsdfEval *eval, Spectrum value)
{
  eval->diffuse *= value;
  eval->glossy *= value;
  eval->sum *= value;
}

ccl_device_inline Spectrum bsdf_eval_sum(const ccl_private BsdfEval *eval)
{
  return eval->sum;
}

ccl_device_inline Spectrum bsdf_eval_pass_diffuse_weight(const ccl_private BsdfEval *eval)
{
  /* Ratio of diffuse weight to recover proportions for writing to render pass.
   * We assume reflection, transmission and volume scatter to be exclusive. */
  return safe_divide(eval->diffuse, eval->sum);
}

ccl_device_inline Spectrum bsdf_eval_pass_glossy_weight(const ccl_private BsdfEval *eval)
{
  /* Ratio of glossy weight to recover proportions for writing to render pass.
   * We assume reflection, transmission and volume scatter to be exclusive. */
  return safe_divide(eval->glossy, eval->sum);
}

/* --------------------------------------------------------------------
 * Clamping
 *
 * Clamping is done on a per-contribution basis so that we can write directly
 * to render buffers instead of using per-thread memory, and to avoid the
 * impact of clamping on other contributions. */

ccl_device_forceinline void film_clamp_light(KernelGlobals kg,
                                             ccl_private Spectrum *L,
                                             const int bounce,
                                             ccl_private float *captured_gain = nullptr)
{
  if (captured_gain) {
    *captured_gain = 1.0f;
  }
#ifdef __KERNEL_DEBUG_NAN__
  if (!isfinite_safe(*L)) {
    kernel_assert(!"Cycles sample with non-finite value detected");
  }
#endif
  /* Make sure all components are finite, allowing the contribution to be usable by adaptive
   * sampling convergence check, but also to make it so render result never causes issues with
   * post-processing. */
  *L = ensure_finite(*L);

#ifdef __CLAMP_SAMPLE__
  const float limit = (bounce > 0) ? kernel_data.integrator.sample_clamp_indirect :
                                     kernel_data.integrator.sample_clamp_direct;
  const float sum = reduce_add(fabs(*L));
  if (sum > limit) {
    const float gain = limit / sum;
    *L *= gain;
    if (captured_gain) {
      *captured_gain = gain;
    }
  }
#endif
}

/* --------------------------------------------------------------------
 * Pass accumulation utilities.
 */

/* --------------------------------------------------------------------
 * Adaptive sampling.
 */

ccl_device_inline void film_write_lightgroup_pass(ccl_global float *ccl_restrict buffer,
                                                  const int pass_offset,
                                                  const int lightgroup,
                                                  const Spectrum contribution)
{
  if (lightgroup == LIGHTGROUP_NONE || pass_offset == PASS_UNUSED) {
    return;
  }

  film_write_pass_spectrum(buffer + pass_offset + 3 * lightgroup, contribution);
}

ccl_device_inline int film_get_split_lightgroup_index(KernelGlobals kg, const int lightgroup)
{
  if (lightgroup == LIGHTGROUP_NONE || lightgroup < 0 ||
      lightgroup >= kernel_data.film.lightgroup_split_index_count)
  {
    return LIGHTGROUP_NONE;
  }

  const int split_lightgroup = kernel_data_fetch(lightgroup_split_index, lightgroup);
  return (split_lightgroup >= 0) ? split_lightgroup : LIGHTGROUP_NONE;
}

ccl_device_inline void film_write_lightgroup_split_pass(KernelGlobals kg,
                                                        ccl_global float *ccl_restrict buffer,
                                                        const int pass_offset,
                                                        const int lightgroup,
                                                        const Spectrum contribution)
{
  if (pass_offset == PASS_UNUSED) {
    return;
  }

  const int split_lightgroup = film_get_split_lightgroup_index(kg, lightgroup);
  if (split_lightgroup == LIGHTGROUP_NONE) {
    return;
  }

  film_write_pass_spectrum(buffer + pass_offset + 3 * split_lightgroup, contribution);
}

ccl_device_inline int film_write_sample(KernelGlobals kg,
                                        ConstIntegratorState state,
                                        ccl_global float *ccl_restrict render_buffer,
                                        const int sample,
                                        const int sample_offset)
{
  if (kernel_data.film.pass_sample_count == PASS_UNUSED) {
    return sample;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  return atomic_fetch_and_add_uint32(
             (ccl_global uint *)(buffer) + kernel_data.film.pass_sample_count, 1) +
         sample_offset;
}

ccl_device void film_write_adaptive_buffer(KernelGlobals kg,
                                           const int sample,
                                           const Spectrum contribution,
                                           ccl_global float *ccl_restrict buffer)
{
  /* Adaptive Sampling. Fill the additional buffer with only one half of the samples and
   * calculate our stopping criteria. This is the heuristic from "A hierarchical automatic
   * stopping condition for Monte Carlo global illumination" except that here it is applied
   * per pixel and not in hierarchical tiles. */

  if (kernel_data.film.pass_adaptive_aux_buffer == PASS_UNUSED) {
    return;
  }

  if (sample_is_class_A(kernel_data.integrator.sampling_pattern, sample)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_adaptive_aux_buffer,
                           make_float4(contribution_rgb.x * 2.0f,
                                       contribution_rgb.y * 2.0f,
                                       contribution_rgb.z * 2.0f,
                                       0.0f));
  }
}

/* Write the volume and surface contribution for volume scattering probability guiding. */
ccl_device_inline void film_write_volume_scattering_guiding_pass(
    KernelGlobals kg,
    ccl_global float *ccl_restrict buffer,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag,
    const Spectrum contribution)
{
  int pass_offset = PASS_UNUSED;
  if (path_flag & PATH_RAY_VOLUME_PRIMARY_TRANSMIT) {
    pass_offset = kernel_data.film.pass_volume_transmit;
  }
  else if (path_visibility & PATH_RAY_VISIBILITY_VOLUME_SCATTER) {
    pass_offset = kernel_data.film.pass_volume_scatter;
  }

  if (pass_offset != PASS_UNUSED) {
    film_write_pass_spectrum(buffer + pass_offset, contribution);
  }
}

/* --------------------------------------------------------------------
 * Shadow catcher.
 */

#ifdef __SHADOW_CATCHER__

/* Accumulate contribution to the Shadow Catcher pass.
 *
 * Returns truth if the contribution is fully handled here and is not to be added to the other
 * passes (like combined, adaptive sampling). */

ccl_device bool film_write_shadow_catcher(KernelGlobals kg,
                                          const uint32_t path_flag,
                                          const Spectrum contribution,
                                          ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return false;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher_matte, contribution);
    /* NOTE: Accumulate the combined pass and to the samples count pass, so that the adaptive
     * sampling is based on how noisy the combined pass is as if there were no catchers in the
     * scene. */
  }

  /* Shadow catcher pass. */
  if (kernel_shadow_catcher_is_object_pass(path_flag)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher, contribution);
    return true;
  }

  return false;
}

ccl_device bool film_write_shadow_catcher_transparent(KernelGlobals kg,
                                                      const uint32_t path_flag,
                                                      const Spectrum contribution,
                                                      const float transparent,
                                                      ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return false;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  if (path_flag & PATH_RAY_SHADOW_CATCHER_BACKGROUND) {
    return true;
  }

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_shadow_catcher_matte,
                           make_float4(contribution_rgb, transparent));
    /* NOTE: Accumulate the combined pass and to the samples count pass, so that the adaptive
     * sampling is based on how noisy the combined pass is as if there were no catchers in the
     * scene. */
  }

  /* Shadow catcher pass. */
  if (kernel_shadow_catcher_is_object_pass(path_flag)) {
    /* NOTE: The transparency of the shadow catcher pass is ignored. It is not needed for the
     * calculation and the alpha channel of the pass contains numbers of samples contributed to a
     * pixel of the pass. */
    film_write_pass_spectrum(buffer + kernel_data.film.pass_shadow_catcher, contribution);
    return true;
  }

  return false;
}

ccl_device void film_write_shadow_catcher_transparent_only(KernelGlobals kg,
                                                           const uint32_t path_flag,
                                                           const float transparent,
                                                           ccl_global float *ccl_restrict buffer)
{
  if (!kernel_data.integrator.has_shadow_catcher) {
    return;
  }

  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  /* Matte pass. */
  if (kernel_shadow_catcher_is_matte_path(path_flag)) {
    film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_matte + 3, transparent);
  }
}

/* Write shadow catcher passes on a bounce from the shadow catcher object. */
ccl_device_forceinline void film_write_shadow_catcher_bounce_data(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  kernel_assert(kernel_data.film.pass_shadow_catcher_sample_count != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_shadow_catcher_matte != PASS_UNUSED);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  /* Count sample for the shadow catcher object. */
  film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_sample_count, 1.0f);

  /* Since the split is done, the sample does not contribute to the matte, so accumulate it as
   * transparency to the matte. */
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  film_write_pass_float(buffer + kernel_data.film.pass_shadow_catcher_matte + 3,
                        average(throughput));
}

#endif /* __SHADOW_CATCHER__ */

/* --------------------------------------------------------------------
 * Render passes.
 */

/* Write combined pass. */
ccl_device_inline void film_write_combined_pass(KernelGlobals kg,
                                                const PathRayVisibility path_visibility,
                                                const uint32_t path_flag,
                                                const int sample,
                                                const Spectrum contribution,
                                                ccl_global float *ccl_restrict buffer,
                                                const float depth,
                                                const uint32_t pixel_index
#if __DEEP_BOUNDED_POLICY__ == 3
                                                , const DeepBoundedFilmSource bounded_source = {}
#endif
                                                )
{
#ifdef __SHADOW_CATCHER__
  if (film_write_shadow_catcher(kg, path_flag, contribution, buffer)) {
    return;
  }
#endif

  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
#if __DEEP_BOUNDED_POLICY__ == 3
    if (bounded_source.terminal_fraction > 0.0f) {
      const float fraction = bounded_source.terminal_fraction;
      deep_bounded_write_rgb(kg, bounded_source, contribution * (1.0f - fraction));
      const DeepBoundedFilmSource terminal = {
          deep_bounded_source(deep_bounded_slot(bounded_source.source), DeepBoundedSourceKind::POINT),
          DEEP_BOUNDED_WORLD_TERMINAL};
      deep_bounded_write_rgb(kg, terminal, contribution * fraction);
    }
    else {
      deep_bounded_write_rgb(kg, bounded_source, contribution);
    }
#endif
    film_write_pass_spectrum(buffer + kernel_data.film.pass_combined, contribution);
  }

  film_write_adaptive_buffer(kg, sample, contribution, buffer);
  film_write_volume_scattering_guiding_pass(kg, buffer, path_visibility, path_flag, contribution);

#ifdef __DEEP_OUTPUT__
  /* Deep samples for surfaces are written once in shade_surface.h at first hit.
   * This function (film_write_combined_pass) is called multiple times per pixel
   * for incremental light contributions, so we don't write here.
   * Volumes write their own samples in shade_volume.h. */
  (void)depth;
  (void)pixel_index;
#endif
}

#ifdef __DEEP_OUTPUT__
ccl_device_inline void film_accumulate_deep_surface_rgb_path(KernelGlobals kg,
                                                             ConstIntegratorState state,
                                                             const Spectrum contribution)
{
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_surface_receipts) {
    const uint32_t surface = INTEGRATOR_STATE(state, path, deep_surface_sample_idx);
    if (surface != 0xffffffffu) {
      film_accumulate_surface_receipt_rgb(
          INTEGRATOR_STATE(state, path, render_pixel_index), surface, contribution);
    }
    else if (INTEGRATOR_STATE(state, path, deep_volume_radiance_idx) != 0xffffffffu) {
      film_accumulate_volume_radiance_receipt(
          INTEGRATOR_STATE(state, path, render_pixel_index),
          INTEGRATOR_STATE(state, path, sample),
          INTEGRATOR_STATE(state, path, deep_volume_radiance_idx),
          contribution,
          DEEP_VOLUME_PATH);
    }
    return;
  }
#  endif
  if (!kernel_data.film.use_deep_output) {
    return;
  }

  const uint32_t sample_idx = INTEGRATOR_STATE(state, path, deep_surface_sample_idx);
  if (sample_idx == DEEP_INVALID_SAMPLE_INDEX) {
    return;
  }

  ccl_global KernelDeepSample *deep_samples = (ccl_global KernelDeepSample *)
                                                  kernel_data.film.deep_samples_ptr;
  if (deep_samples == nullptr) {
    return;
  }

  film_accumulate_deep_surface_rgb(kg,
                                   INTEGRATOR_STATE(state, path, render_pixel_index),
                                   sample_idx,
                                   deep_samples,
                                   contribution);
}

ccl_device_inline void film_accumulate_deep_surface_rgb_shadow(KernelGlobals kg,
                                                               ConstIntegratorShadowState state,
                                                               const Spectrum contribution)
{
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_surface_receipts) {
    const uint32_t surface = INTEGRATOR_STATE(state, shadow_path, deep_surface_sample_idx);
    if (surface != 0xffffffffu) {
      film_accumulate_surface_receipt_rgb(
          INTEGRATOR_STATE(state, shadow_path, render_pixel_index), surface, contribution);
    }
    else if (INTEGRATOR_STATE(state, shadow_path, deep_volume_radiance_idx) != 0xffffffffu) {
      film_accumulate_volume_radiance_receipt(
          INTEGRATOR_STATE(state, shadow_path, render_pixel_index),
          INTEGRATOR_STATE(state, shadow_path, sample),
          INTEGRATOR_STATE(state, shadow_path, deep_volume_radiance_idx),
          contribution,
          DEEP_VOLUME_SHADOW);
    }
    return;
  }
#  endif
  if (!kernel_data.film.use_deep_output) {
    return;
  }

  const uint32_t sample_idx = INTEGRATOR_STATE(state, shadow_path, deep_surface_sample_idx);
  if (sample_idx == DEEP_INVALID_SAMPLE_INDEX) {
    return;
  }

  ccl_global KernelDeepSample *deep_samples = (ccl_global KernelDeepSample *)
                                                  kernel_data.film.deep_samples_ptr;
  if (deep_samples == nullptr) {
    return;
  }

  film_accumulate_deep_surface_rgb(kg,
                                   INTEGRATOR_STATE(state, shadow_path, render_pixel_index),
                                   sample_idx,
                                   deep_samples,
                                   contribution);
}
#endif

/* Write combined pass with transparency. */
ccl_device_inline void film_write_combined_transparent_pass(
    KernelGlobals kg,
    const PathRayVisibility path_visibility,
    const uint32_t path_flag,
    const int sample,
    const Spectrum contribution,
    const float transparent,
    ccl_global float *ccl_restrict buffer)
{
#ifdef __SHADOW_CATCHER__
  if (film_write_shadow_catcher_transparent(kg, path_flag, contribution, transparent, buffer)) {
    return;
  }
#endif

  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    const float3 contribution_rgb = spectrum_to_rgb(contribution);

    film_write_pass_float4(buffer + kernel_data.film.pass_combined,
                           make_float4(contribution_rgb, transparent));
  }

  /* Deep samples are written at primary surface hits; background/holdout passes have no
   * reliable depth here, so skip deep output in this pass. */

  film_write_adaptive_buffer(kg, sample, contribution, buffer);
  film_write_volume_scattering_guiding_pass(kg, buffer, path_visibility, path_flag, contribution);
}

/* Write background or emission to appropriate pass. */
ccl_device_inline void film_write_emission_or_background_pass(
    KernelGlobals kg,
    ConstIntegratorState state,
    Spectrum contribution,
    ccl_global float *ccl_restrict buffer,
    const int pass,
    const bool is_background,
    const int lightgroup = LIGHTGROUP_NONE,
    const bool split_lightgroup_lobes = false)
{
  if (!(kernel_data.film.light_pass_flag & PASS_ANY)) {
    return;
  }

#ifdef __PASSES__
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  int pass_offset = PASS_UNUSED;

  /* Denoising albedo. */
#  ifdef __DENOISING_FEATURES__
  if (path_flag & PATH_RAY_DENOISING_FEATURES) {
    if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
      const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
          state, path, denoising_feature_throughput);
      const Spectrum denoising_albedo = denoising_feature_throughput * contribution;
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
    }
  }
#  endif /* __DENOISING_FEATURES__ */

  const bool is_shadowcatcher = (path_flag & PATH_RAY_SHADOW_CATCHER_HIT) != 0;
  if (!is_shadowcatcher) {
    film_write_lightgroup_pass(buffer, kernel_data.film.pass_lightgroup, lightgroup, contribution);
  }

  if (!(path_flag & PATH_RAY_ANY_PASS)) {
    if (!is_shadowcatcher && is_background) {
      /* Camera-visible world background has no native surface/volume lobe label.
       * Keep Combined_<world> reconstructable by assigning it to the least-wrong existing bucket:
       * diffuse direct. */
      film_write_lightgroup_split_pass(
          kg, buffer, kernel_data.film.pass_lightgroup_diffuse, lightgroup, contribution);
      film_write_lightgroup_split_pass(
          kg, buffer, kernel_data.film.pass_lightgroup_diffuse_direct, lightgroup, contribution);
    }
    /* Directly visible, write to emission or background pass. */
    pass_offset = pass;
  }
  else if (is_shadowcatcher) {
    /* Don't write any light passes for shadow catcher, for easier
     * compositing back together of the combined pass. */
    return;
  }
  else if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    /* Keep emissive-surface events in Combined_<lg> only by default.
     *
     * World background always splits into the existing lobe family. Ordinary light objects may
     * optionally do the same when their caller provides pass weights for reflective/transmissive
     * visibility, while mesh emission remains combined-only.
     *
     * Use explicit contribution kind instead of comparing pass offsets: when both the background
     * pass and emission pass are disabled they both become PASS_UNUSED, and pass equality no
     * longer identifies whether this came from the world background or from emissive geometry.
     */
    const bool write_lightgroup_lobes = is_background || split_lightgroup_lobes;

    if (path_flag & PATH_RAY_SURFACE_PASS) {
      /* Indirectly visible through reflection. */
      const Spectrum diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      const Spectrum glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
      const Spectrum transmission_weight = one_spectrum() - diffuse_weight - glossy_weight;
      const Spectrum diffuse_contribution = diffuse_weight * contribution;
      const Spectrum glossy_contribution = glossy_weight * contribution;
      const Spectrum transmission_contribution = transmission_weight * contribution;
      const bool is_direct = (INTEGRATOR_STATE(state, path, bounce) == 1);

      /* Glossy */
      const int glossy_pass_offset = is_direct ? kernel_data.film.pass_glossy_direct :
                                                 kernel_data.film.pass_glossy_indirect;
      if (glossy_pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + glossy_pass_offset, glossy_contribution);
      }

      /* Transmission */
      const int transmission_pass_offset = is_direct ? kernel_data.film.pass_transmission_direct :
                                                       kernel_data.film.pass_transmission_indirect;

      if (transmission_pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + transmission_pass_offset, transmission_contribution);
      }

      if (write_lightgroup_lobes) {
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         kernel_data.film.pass_lightgroup_diffuse,
                                         lightgroup,
                                         diffuse_contribution);
        film_write_lightgroup_split_pass(
            kg, buffer, kernel_data.film.pass_lightgroup_glossy, lightgroup, glossy_contribution);
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         kernel_data.film.pass_lightgroup_transmission,
                                         lightgroup,
                                         transmission_contribution);
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         is_direct ?
                                             kernel_data.film.pass_lightgroup_diffuse_direct :
                                             kernel_data.film.pass_lightgroup_diffuse_indirect,
                                         lightgroup,
                                         diffuse_contribution);
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         is_direct ?
                                             kernel_data.film.pass_lightgroup_glossy_direct :
                                             kernel_data.film.pass_lightgroup_glossy_indirect,
                                         lightgroup,
                                         glossy_contribution);
        film_write_lightgroup_split_pass(
            kg,
            buffer,
            is_direct ? kernel_data.film.pass_lightgroup_transmission_direct :
                        kernel_data.film.pass_lightgroup_transmission_indirect,
            lightgroup,
            transmission_contribution);
      }

      /* Reconstruct diffuse subset of throughput. */
      pass_offset = is_direct ? kernel_data.film.pass_diffuse_direct :
                                kernel_data.film.pass_diffuse_indirect;
      if (pass_offset != PASS_UNUSED) {
        contribution = diffuse_contribution;
      }
    }
    else if (path_flag & PATH_RAY_VOLUME_PASS) {
      /* Indirectly visible through volume. */
      const bool is_direct = (INTEGRATOR_STATE(state, path, bounce) == 1);
      pass_offset = is_direct ? kernel_data.film.pass_volume_direct :
                                kernel_data.film.pass_volume_indirect;

      if (write_lightgroup_lobes) {
        film_write_lightgroup_split_pass(
            kg, buffer, kernel_data.film.pass_lightgroup_volume, lightgroup, contribution);
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         is_direct ?
                                             kernel_data.film.pass_lightgroup_volume_direct :
                                             kernel_data.film.pass_lightgroup_volume_indirect,
                                         lightgroup,
                                         contribution);
      }
    }
  }

  /* Single write call for GPU coherence. */
  if (pass_offset != PASS_UNUSED) {
    film_write_pass_spectrum(buffer + pass_offset, contribution);
  }
#endif /* __PASSES__ */
}

/* Write light contribution to render buffer. */
ccl_device_inline void film_write_direct_light(KernelGlobals kg,
                                               ConstIntegratorShadowState state,
                                               ccl_global float *ccl_restrict render_buffer)
{
  /* The throughput for shadow paths already contains the light shader evaluation. */
  Spectrum contribution = INTEGRATOR_STATE(state, shadow_path, throughput);
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  const uint32_t native_flags = INTEGRATOR_STATE(state, shadow_path, flag);
  const bool native_point_call = !(native_flags & PATH_RAY_SHADOW_FOR_AO) ||
                                 (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE);
  const uint32_t native_point_slot = native_point_call ?
                                         deep_native_reserve_point_film_call(native_flags) :
                                         DEEP_NATIVE_INVALID_INDEX;
  if (deep_native_colour_capture_enabled() && native_point_call &&
      deep_native_path_writes_combined(native_flags) &&
      native_point_slot == DEEP_NATIVE_INVALID_INDEX)
    return;
  const Spectrum native_raw = contribution;
  float native_gain = 1;
  if (deep_native_colour_capture_enabled() && !isfinite_safe(native_raw)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return;
  }
  film_clamp_light(kg,
                   &contribution,
                   INTEGRATOR_STATE(state, shadow_path, bounce),
                   deep_native_colour_capture_enabled() ? &native_gain : nullptr);
#else
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, shadow_path, bounce));
#endif

  ccl_global float *buffer = film_pass_pixel_render_buffer_shadow(kg, state, render_buffer);

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, shadow_path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);
  const int sample = INTEGRATOR_STATE(state, shadow_path, sample);

  uint32_t pixel_index = 0;
  float depth = -1.0f;

#ifdef __DEEP_OUTPUT__
  pixel_index = INTEGRATOR_STATE(state, shadow_path, render_pixel_index);
  if (kernel_data.film.use_deep_output && (INTEGRATOR_STATE(state, shadow_path, bounce) == 0)) {
    Ray ray ccl_optional_struct_init;
    integrator_state_read_shadow_ray(state, &ray);
    depth = camera_z_depth(kg, ray.P);
  }
#endif

  /* Validate the optional Combined record before any AO or Film pass write. */
  if (path_flag & PATH_RAY_SHADOW_FOR_AO) {
    const bool additive = kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE;
    Spectrum ao_weight = one_spectrum();
    if (additive)
      ao_weight = INTEGRATOR_STATE(state, shadow_path, unshadowed_throughput);
    const Spectrum combined_contribution = contribution * ao_weight;
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (additive && deep_native_colour_capture_enabled() &&
        !deep_native_capture_point_film_call(
            INTEGRATOR_STATE(state, shadow_path, deep_frame_camera_index),
            INTEGRATOR_STATE(state, shadow_path, render_pixel_index),
            sample,
            path_flag,
            native_raw,
            contribution,
            native_gain,
            ao_weight,
            combined_contribution,
            native_point_slot,
            INTEGRATOR_STATE(state, shadow_path, deep_frame_root_handle)))
      return;
#endif
    if ((kernel_data.kernel_features & KERNEL_FEATURE_AO_PASS) &&
        (path_visibility & PATH_RAY_VISIBILITY_CAMERA))
      film_write_pass_spectrum(buffer + kernel_data.film.pass_ao, contribution);
    if (additive) {
      film_write_combined_pass(kg,
                               path_visibility,
                               path_flag,
                               sample,
                               combined_contribution,
                               buffer,
                               depth,
                               pixel_index
#if __DEEP_BOUNDED_POLICY__ == 3
                               ,
                               deep_bounded_film_source(kg, state)
#endif
      );
#ifdef __DEEP_OUTPUT__
      film_accumulate_deep_surface_rgb_shadow(kg, state, combined_contribution);
#endif
    }
    return;
  }

  /* Direct light shadow. */
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (deep_native_colour_capture_enabled()) {
    if (!deep_native_capture_point_film_call(
            INTEGRATOR_STATE(state, shadow_path, deep_frame_camera_index),
            INTEGRATOR_STATE(state, shadow_path, render_pixel_index),
            sample,
            path_flag,
            native_raw,
            contribution,
            native_gain,
            one_spectrum(),
            contribution,
            native_point_slot,
            INTEGRATOR_STATE(state, shadow_path, deep_frame_root_handle)))
      return;
  }
#endif
  film_write_combined_pass(kg,
                           path_visibility,
                           path_flag,
                           sample,
                           contribution,
                           buffer,
                           depth,
                           pixel_index
#if __DEEP_BOUNDED_POLICY__ == 3
                           ,
                           deep_bounded_film_source(kg, state)
#endif
  );
#ifdef __DEEP_OUTPUT__
  film_accumulate_deep_surface_rgb_shadow(kg, state, contribution);
#endif

#ifdef __PASSES__
  if (kernel_data.film.light_pass_flag & PASS_ANY) {
    const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);

    /* Don't write any light passes for shadow catcher, for easier
     * compositing back together of the combined pass. */
    if (path_flag & PATH_RAY_SHADOW_CATCHER_HIT) {
      return;
    }

    /* Write lightgroup pass. LIGHTGROUP_NONE is ~0 so decode from unsigned to signed */
    const int lightgroup = (int)(INTEGRATOR_STATE(state, shadow_path, lightgroup)) - 1;
    film_write_lightgroup_pass(buffer, kernel_data.film.pass_lightgroup, lightgroup, contribution);

    if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
      int pass_offset = PASS_UNUSED;

      if (path_flag & PATH_RAY_SURFACE_PASS) {
        /* Indirectly visible through reflection. */
        const Spectrum diffuse_weight = INTEGRATOR_STATE(state, shadow_path, pass_diffuse_weight);
        const Spectrum glossy_weight = INTEGRATOR_STATE(state, shadow_path, pass_glossy_weight);
        const Spectrum transmission_weight = one_spectrum() - diffuse_weight - glossy_weight;
        const Spectrum diffuse_contribution = diffuse_weight * contribution;
        const Spectrum glossy_contribution = glossy_weight * contribution;
        const Spectrum transmission_contribution = transmission_weight * contribution;
        const bool is_direct = (INTEGRATOR_STATE(state, shadow_path, bounce) == 0);

        /* Glossy */
        const int glossy_pass_offset = is_direct ? kernel_data.film.pass_glossy_direct :
                                                   kernel_data.film.pass_glossy_indirect;
        if (glossy_pass_offset != PASS_UNUSED) {
          film_write_pass_spectrum(buffer + glossy_pass_offset, glossy_contribution);
        }

        /* Transmission */
        const int transmission_pass_offset = is_direct ?
                                                 kernel_data.film.pass_transmission_direct :
                                                 kernel_data.film.pass_transmission_indirect;

        if (transmission_pass_offset != PASS_UNUSED) {
          film_write_pass_spectrum(buffer + transmission_pass_offset, transmission_contribution);
        }

        const bool has_lightgroup_surface_passes =
            kernel_data.film.pass_lightgroup_diffuse != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_glossy != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_transmission != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_diffuse_direct != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_diffuse_indirect != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_glossy_direct != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_glossy_indirect != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_transmission_direct != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_transmission_indirect != PASS_UNUSED;
        if (has_lightgroup_surface_passes) {
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           kernel_data.film.pass_lightgroup_diffuse,
                                           lightgroup,
                                           diffuse_contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           kernel_data.film.pass_lightgroup_glossy,
                                           lightgroup,
                                           glossy_contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           kernel_data.film.pass_lightgroup_transmission,
                                           lightgroup,
                                           transmission_contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           is_direct ?
                                               kernel_data.film.pass_lightgroup_diffuse_direct :
                                               kernel_data.film.pass_lightgroup_diffuse_indirect,
                                           lightgroup,
                                           diffuse_contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           is_direct ?
                                               kernel_data.film.pass_lightgroup_glossy_direct :
                                               kernel_data.film.pass_lightgroup_glossy_indirect,
                                           lightgroup,
                                           glossy_contribution);
          film_write_lightgroup_split_pass(
              kg,
              buffer,
              is_direct ? kernel_data.film.pass_lightgroup_transmission_direct :
                          kernel_data.film.pass_lightgroup_transmission_indirect,
              lightgroup,
              transmission_contribution);
        }

        /* Reconstruct diffuse subset of throughput. */
        pass_offset = is_direct ? kernel_data.film.pass_diffuse_direct :
                                  kernel_data.film.pass_diffuse_indirect;
        if (pass_offset != PASS_UNUSED) {
          contribution = diffuse_contribution;
        }
      }
      else if (path_flag & PATH_RAY_VOLUME_PASS) {
        /* Indirectly visible through volume. */
        const bool is_direct = (INTEGRATOR_STATE(state, shadow_path, bounce) == 0);
        pass_offset = is_direct ? kernel_data.film.pass_volume_direct :
                                  kernel_data.film.pass_volume_indirect;

        if (kernel_data.film.pass_lightgroup_volume != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_volume_direct != PASS_UNUSED ||
            kernel_data.film.pass_lightgroup_volume_indirect != PASS_UNUSED)
        {
          film_write_lightgroup_split_pass(
              kg, buffer, kernel_data.film.pass_lightgroup_volume, lightgroup, contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           is_direct ?
                                               kernel_data.film.pass_lightgroup_volume_direct :
                                               kernel_data.film.pass_lightgroup_volume_indirect,
                                           lightgroup,
                                           contribution);
        }
      }

      /* Single write call for GPU coherence. */
      if (pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + pass_offset, contribution);
      }
    }
  }
#endif
}

/* Write transparency to render buffer.
 *
 * Note that we accumulate transparency = 1 - alpha in the render buffer.
 * Otherwise we'd have to write alpha on path termination, which happens
 * in many places. */
ccl_device_inline void film_write_transparent(KernelGlobals kg,
                                              const uint32_t path_flag,
                                              const float transparent,
                                              ccl_global float *ccl_restrict buffer)
{
  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    film_write_pass_float(buffer + kernel_data.film.pass_combined + 3, transparent);
  }

#ifdef __SHADOW_CATCHER__
  film_write_shadow_catcher_transparent_only(kg, path_flag, transparent, buffer);
#endif

  if (path_flag & PATH_RAY_VOLUME_PRIMARY_TRANSMIT) {
    kernel_assert(kernel_data.film.pass_volume_transmit != PASS_UNUSED);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_volume_transmit,
                             make_spectrum(transparent));
  }
}

/* Write holdout to render buffer. */
ccl_device_inline void film_write_holdout(KernelGlobals kg,
                                          ConstIntegratorState state,
                                          const uint32_t path_flag,
                                          const float transparent,
                                          ccl_global float *ccl_restrict render_buffer)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_transparent(kg, path_flag, transparent, buffer);
}

/* Write background contribution to render buffer.
 *
 * Includes transparency, matching film_write_transparent. */
ccl_device_inline void film_write_background(KernelGlobals kg,
                                             ConstIntegratorState state,
                                             const Spectrum L,
                                             const float transparent,
                                             const bool is_transparent_background_ray,
                                             ccl_global float *ccl_restrict render_buffer)
{
  Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L;
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  deep_compact_capture_background(kg, state, is_transparent_background_ray);
  const uint32_t native_point_slot = !is_transparent_background_ray &&
                                             deep_native_colour_capture_enabled() ?
                                         deep_native_reserve_point_film_call(
                                             INTEGRATOR_STATE(state, path, flag)) :
                                         DEEP_NATIVE_INVALID_INDEX;
  if (!is_transparent_background_ray && deep_native_colour_capture_enabled() &&
      deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)) &&
      native_point_slot == DEEP_NATIVE_INVALID_INDEX)
    return;
  const Spectrum native_raw = contribution;
  float native_gain = 1;
  if (deep_native_colour_capture_enabled() && !isfinite_safe(native_raw)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return;
  }
  film_clamp_light(kg,
                   &contribution,
                   INTEGRATOR_STATE(state, path, bounce) - 1,
                   deep_native_colour_capture_enabled() ? &native_gain : nullptr);
#else
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);
#endif

#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (deep_native_colour_capture_enabled() &&
      deep_native_capture_combined_call(INTEGRATOR_STATE(state, path, flag)))
  {
    ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
    if (!is_transparent_background_ray) {
      if (deep_native_is_camera_chain(state)) {
        deep_native_set_root(
            state, DEEP_NATIVE_ROOT_WORLD, FLT_MAX, OBJECT_NONE, PRIM_NONE, SHADER_NONE);
      }
      if (!deep_native_capture_point_film_call(
              INTEGRATOR_STATE(state, path, deep_frame_camera_index),
              INTEGRATOR_STATE(state, path, render_pixel_index),
              INTEGRATOR_STATE(state, path, sample),
              INTEGRATOR_STATE(state, path, flag),
              native_raw,
              contribution,
              native_gain,
              one_spectrum(),
              contribution,
              native_point_slot))
        return;
    }
    if (camera) {
      camera->flags |= DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
    }
  }
#endif

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (is_transparent_background_ray) {
    film_write_transparent(kg, path_flag, transparent, buffer);
  }
  else {
#if __DEEP_BOUNDED_POLICY__ > 0
    deep_bounded_background(kg, state);
#  if __DEEP_BOUNDED_POLICY__ == 3
    if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
      deep_bounded_write_rgb(kg, deep_bounded_film_source(kg, state), contribution);
    }
#  endif
#endif
    const int sample = INTEGRATOR_STATE(state, path, sample);
    film_write_combined_transparent_pass(
        kg, path_visibility, path_flag, sample, contribution, transparent, buffer);
#ifdef __DEEP_OUTPUT__
    film_accumulate_deep_surface_rgb_path(kg, state, contribution);
#endif
  }
  film_write_emission_or_background_pass(kg,
                                         state,
                                         contribution,
                                         buffer,
                                         kernel_data.film.pass_background,
                                         true,
                                         kernel_data.background.lightgroup,
                                         true);
}

/* Write emission to render buffer. */
ccl_device_inline void film_write_volume_emission(
    KernelGlobals kg,
    ConstIntegratorState state,
    const Spectrum L,
    ccl_global float *ccl_restrict render_buffer,
    const float depth,
    const int lightgroup = LIGHTGROUP_NONE,
    const uint32_t deep_native_segment = DEEP_NATIVE_INVALID_INDEX,
    const uint32_t deep_native_expression_head = DEEP_NATIVE_INVALID_INDEX,
    const bool deep_discard_emission = false
#if __DEEP_BOUNDED_POLICY__ == 3
    , const bool bounded_permit = false, const float bounded_terminal_fraction = 0.0f
#endif
    )
{
  Spectrum contribution = L;
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  uint32_t native_call_slot = DEEP_NATIVE_INVALID_INDEX;
  if (deep_native_colour_capture_enabled() && !deep_discard_emission &&
      deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
  {
    if (deep_native_expression_head != DEEP_NATIVE_INVALID_INDEX)
      native_call_slot = deep_native_reserve_expression_film_call();
    else if (deep_native_root(INTEGRATOR_STATE(state, path, deep_frame_camera_index)))
      native_call_slot = deep_native_reserve_point_film_call(INTEGRATOR_STATE(state, path, flag));
    else if (deep_native_segment >= kernel_integrator_state.deep_native_homogeneous_capacity) {
      const uint prior = atomic_fetch_and_or_uint32(
          &kernel_integrator_state.queue_counter->deep_native_capture_error, DEEP_NATIVE_ERROR_EMISSION_CALL);
      if (!is_zero(L) && !(prior & DEEP_NATIVE_ERROR_EMISSION_CALL))
        printf("DEEP_UNSUPPORTED family=unbound_volume_emission camera=%u sample=%u\n",
               INTEGRATOR_STATE(state, path, deep_frame_camera_index), INTEGRATOR_STATE(state, path, sample));
      return;
    }
    if ((deep_native_expression_head != DEEP_NATIVE_INVALID_INDEX ||
         deep_native_root(INTEGRATOR_STATE(state, path, deep_frame_camera_index))) &&
        native_call_slot == DEEP_NATIVE_INVALID_INDEX)
      return;
  }
  float deep_native_clamp_gain = 1.0f;
  if (deep_native_colour_capture_enabled() && !isfinite_safe(L)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return;
  }
  film_clamp_light(kg,
                   &contribution,
                   INTEGRATOR_STATE(state, path, bounce) - 1,
                   deep_native_colour_capture_enabled() ? &deep_native_clamp_gain :
                                                                 nullptr);
  if (deep_native_colour_capture_enabled() && !deep_discard_emission &&
      deep_native_capture_combined_call(INTEGRATOR_STATE(state, path, flag)))
  {
    const uint32_t camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
    ccl_global DeepNativeRootRecord *root = deep_native_root(camera_index);
    if (!kernel_integrator_state.deep_native_expression_nodes && !is_zero(L) &&
        !root && deep_native_segment == DEEP_NATIVE_INVALID_INDEX) {
      /* A proven non-emitting compiled volume needs no expression arena. An
       * unexpected actual emission invalidates that proof before film commit. */
      deep_native_capture_error(DEEP_NATIVE_ERROR_HETEROGENEOUS);
      return;
    }
    if (deep_native_expression_head != DEEP_NATIVE_INVALID_INDEX) {
      if (!deep_native_expression_film_call(state,
                                            deep_native_expression_head,
                                            L,
                                            contribution,
                                            deep_native_clamp_gain,
                                            native_call_slot))
        return;
    }
    else if (root && root->kind != DEEP_NATIVE_ROOT_NONE) {
      if (!deep_native_capture_point_film_call(camera_index,
                                               INTEGRATOR_STATE(state, path, render_pixel_index),
                                               INTEGRATOR_STATE(state, path, sample),
                                               INTEGRATOR_STATE(state, path, flag),
                                               L,
                                               contribution,
                                               deep_native_clamp_gain,
                                               one_spectrum(),
                                               contribution,
                                               native_call_slot))
        return;
    }
    else {
      if (!deep_native_capture_volume_film_call(
              state, deep_native_segment, L, deep_native_clamp_gain, contribution))
        return;
    }
  }
#else
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);
#endif

  /* L is already throughput-weighted. Capture the exact post-clamp film contribution once. */
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  film_accumulate_volume_radiance_receipt(INTEGRATOR_STATE(state, path, render_pixel_index),
                                          INTEGRATOR_STATE(state, path, sample),
                                          INTEGRATOR_STATE(state, path, deep_volume_radiance_idx),
                                          contribution,
                                          DEEP_VOLUME_EMISSION);
#endif

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const int sample = INTEGRATOR_STATE(state, path, sample);
  const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

#if __DEEP_BOUNDED_POLICY__ == 3
  auto bounded_source = deep_bounded_film_source(kg, state, bounded_permit);
  bounded_source.terminal_fraction = bounded_permit ? bounded_terminal_fraction : 0.0f;
#endif
  /* Write deep sample if depth is valid (>= 0). */
  film_write_combined_pass(kg,
                           path_visibility,
                           path_flag,
                           sample,
                           contribution,
                           buffer,
                           depth,
                           pixel_index
#if __DEEP_BOUNDED_POLICY__ == 3
                           ,
                           bounded_source
#endif
  );
  film_write_emission_or_background_pass(
      kg, state, contribution, buffer, kernel_data.film.pass_emission, false, lightgroup, false);
}

ccl_device_inline void film_write_surface_emission(KernelGlobals kg,
                                                   ConstIntegratorState state,
                                                   const Spectrum L,
                                                   const float mis_weight,
                                                   ccl_global float *ccl_restrict render_buffer,
                                                   const int lightgroup = LIGHTGROUP_NONE,
                                                   const bool split_lightgroup_lobes = false,
                                                   const uint32_t deep_root_handle = 0)
{
  Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L * mis_weight;
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  const uint32_t native_point_slot = deep_native_colour_capture_enabled() ?
                                         deep_native_reserve_point_film_call(
                                             INTEGRATOR_STATE(state, path, flag)) :
                                         DEEP_NATIVE_INVALID_INDEX;
  if (deep_native_colour_capture_enabled() &&
      deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)) &&
      native_point_slot == DEEP_NATIVE_INVALID_INDEX)
    return;
  const Spectrum native_raw = contribution;
  float native_gain = 1;
  if (deep_native_colour_capture_enabled() && !isfinite_safe(native_raw)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_NONFINITE);
    return;
  }
  film_clamp_light(kg,
                   &contribution,
                   INTEGRATOR_STATE(state, path, bounce) - 1,
                   deep_native_colour_capture_enabled() ? &native_gain : nullptr);
#else
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);
#endif

#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (deep_native_colour_capture_enabled()) {
    if (!deep_native_capture_point_film_call(
            INTEGRATOR_STATE(state, path, deep_frame_camera_index),
            INTEGRATOR_STATE(state, path, render_pixel_index),
            INTEGRATOR_STATE(state, path, sample),
            INTEGRATOR_STATE(state, path, flag),
            native_raw,
            contribution,
            native_gain,
            one_spectrum(),
            contribution,
            native_point_slot,
            deep_root_handle ? deep_root_handle :
                               INTEGRATOR_STATE(state, path, deep_frame_root_handle)))
      return;
  }
#endif

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const int sample = INTEGRATOR_STATE(state, path, sample);
  const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  float depth = -1.0f;
#ifdef __DEEP_OUTPUT__
  if (kernel_data.film.use_deep_output && (INTEGRATOR_STATE(state, path, bounce) == 0)) {
    Intersection isect ccl_optional_struct_init;
    integrator_state_read_isect(state, &isect);
    /* Reconstruct camera depth from the primary ray and intersection. */
    Ray ray ccl_optional_struct_init;
    integrator_state_read_ray(state, &ray);
    float3 P = ray.P + ray.D * isect.t;
    depth = camera_z_depth(kg, P);
  }
#endif

  film_write_combined_pass(kg,
                           path_visibility,
                           path_flag,
                           sample,
                           contribution,
                           buffer,
                           depth,
                           pixel_index
#if __DEEP_BOUNDED_POLICY__ == 3
                           ,
                           deep_bounded_film_source(kg, state)
#endif
  );
#ifdef __DEEP_OUTPUT__
  film_accumulate_deep_surface_rgb_path(kg, state, contribution);
#endif
  film_write_emission_or_background_pass(kg,
                                         state,
                                         contribution,
                                         buffer,
                                         kernel_data.film.pass_emission,
                                         false,
                                         lightgroup,
                                         split_lightgroup_lobes);
}

CCL_NAMESPACE_END
