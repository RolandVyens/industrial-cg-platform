/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/bvh/util.h"
#include "kernel/camera/camera.h"

#include "kernel/film/adaptive_sampling.h"
#include "kernel/film/deep_write.h"
#include "kernel/film/light_passes.h"

#include "kernel/integrator/deep_optical_capture.h"
#include "kernel/integrator/path_state.h"

#include "kernel/integrator/state_util.h"
#include "kernel/sample/pattern.h"

#include "kernel/integrator/deep_bounded_capture.h"

CCL_NAMESPACE_BEGIN

#ifdef __KERNEL_GPU__
#  ifdef __DEEP_OUTPUT__
ccl_device_inline uint32_t integrator_reserve_native_camera(IntegratorState state,
                                                            const int x,
                                                            const int y,
                                                            const int sample,
                                                            const int scheduled_sample,
                                                            const int sample_offset)
{
  if (!kernel_integrator_state.deep_native_cameras) {
    return DEEP_NATIVE_INVALID_INDEX;
  }
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_camera_reserved, 1);
  INTEGRATOR_STATE_WRITE(state, path, deep_frame_camera_index) = index;
  INTEGRATOR_STATE_WRITE(state, path, deep_frame_root_handle) = 0;
  if (index >= kernel_integrator_state.deep_native_camera_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return index;
  }
  DeepNativeCameraRecord camera = {};
  camera.pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  camera.x = x;
  camera.y = y;
  camera.sample = sample;
  camera.scheduled_sample = scheduled_sample;
  camera.subset_offset = sample_offset;
  camera.population_weight = 1.0f;
  kernel_integrator_state.deep_native_cameras[index] = camera;
  return index;
}

ccl_device_inline float3 deep_camera_projected_sensor(KernelGlobals kg,
                                                   const float2 raster,
                                                   const float time)
{
  const ProjectionTransform projection = kernel_data.cam.rastertocamera;
  float3 point = transform_perspective(&projection, make_float3(raster));
  if (kernel_data.cam.type == CAMERA_PERSPECTIVE && kernel_data.cam.have_perspective_motion) {
    const ProjectionTransform other = time < .5f ? kernel_data.cam.perspective_pre :
                                                   kernel_data.cam.perspective_post;
    const float3 endpoint = transform_perspective(&other, make_float3(raster));
    point = time < .5f ? interp(endpoint, point, 2.f * time) :
                         interp(point, endpoint, 2.f * time - 1.f);
  }
  return point;
}

ccl_device_inline void deep_camera_capture_projection(KernelGlobals kg,
                                                     ccl_global DeepNativeCameraRecord *camera,
                                                     const ccl_private Ray *ray,
                                                     const float2 raster)
{
  if (!kernel_integrator_state.deep_compact_has_volume || kernel_data.cam.interocular_offset != 0 ||
      (kernel_data.cam.type != CAMERA_PERSPECTIVE && kernel_data.cam.type != CAMERA_ORTHOGRAPHIC))
    return;
  Transform matrix = kernel_data.cam.cameratoworld;
  if (kernel_data.cam.num_motion_steps)
    transform_motion_array_interpolate(&matrix, kernel_data_array(camera_motion),
                                       kernel_data.cam.num_motion_steps, ray->time);
  const Transform depth = kernel_data.cam.worldtocamera;
  const float dz = transform_direction(&depth, ray->D).z;
  if (!(dz > 0)) return;
  float lengths = 0;
  const float3 center = deep_camera_projected_sensor(kg, raster, ray->time);
  for (int axis = 0; axis < 2; axis++) {
    const float2 shifted = raster + (axis == 0 ? make_float2(1, 0) : make_float2(0, 1));
    float3 adjacent = deep_camera_projected_sensor(kg, shifted, ray->time);
    float3 delta;
    if (kernel_data.cam.type == CAMERA_PERSPECTIVE) {
      if (kernel_data.cam.aperturesize > 0) {
        if (!(adjacent.z > 0)) return;
        adjacent *= kernel_data.cam.focaldistance / adjacent.z;
      }
      const float3 direction = transform_point(&matrix, adjacent) - ray->P;
      const float next_dz = transform_direction(&depth, direction).z;
      if (!(next_dz > 0)) return;
      delta = direction / next_dz - ray->D / dz;
    }
    else {
      const float3 shift = transform_direction(&matrix, adjacent - center);
      delta = shift - ray->D * (transform_direction(&depth, shift).z / dz);
    }
    lengths += len(delta);
  }
  if (!(lengths > 0) || !isfinite_safe(lengths)) return;
  const float projection = kernel_data.cam.type == CAMERA_PERSPECTIVE ? .5f * lengths : -.5f * lengths;
  camera->reserved[0] = __float_as_uint(projection);
  camera->reserved[1] = __float_as_uint(camera->z_origin);
}

ccl_device_inline void integrator_commit_native_camera(KernelGlobals kg,
                                                       const uint32_t index,
                                                       const uint rng_pixel,
                                                       const ccl_private Ray *ray,
                                                       const Spectrum throughput,
                                                       const bool cache_retry,
                                                       const float2 raster)
{
  if (!kernel_integrator_state.deep_native_cameras) {
    return;
  }
  if (index >= kernel_integrator_state.deep_native_camera_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return;
  }
  ccl_global DeepNativeCameraRecord *camera = &kernel_integrator_state.deep_native_cameras[index];
  if (camera->flags & DEEP_NATIVE_CAMERA_EVALUATED) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return;
  }
  camera->rng_pixel = rng_pixel;
  if (kernel_integrator_state.deep_compact_rays) {
    DeepCompactRayState scalar = {};
    scalar.log_m = 0.0;
    scalar.log_q = -INFINITY;
    scalar.last_surface_t = -1.0f;
    if (!is_zero(throughput)) scalar.cursor = ray->tmin;
    kernel_integrator_state.deep_compact_rays[index] = scalar;
  }
  camera->rng_offset = PRNG_BOUNCE_NUM;
  camera->throughput[0] = throughput.x;
  camera->throughput[1] = throughput.y;
  camera->throughput[2] = throughput.z;
  camera->flags = DEEP_NATIVE_CAMERA_EVALUATED |
                  (cache_retry ? DEEP_NATIVE_CAMERA_CACHE_RETRY : 0u);
  if (is_zero(throughput)) {
    /* camera_sample may return without initializing Ray. Do not inspect any Ray member here. */
    camera->disposition = DEEP_NATIVE_CAMERA_INVALID_BLACK;
  }
  else {
    camera->disposition = DEEP_NATIVE_CAMERA_RAY;
    camera->origin[0] = ray->P.x;
    camera->origin[1] = ray->P.y;
    camera->origin[2] = ray->P.z;
    camera->direction[0] = ray->D.x;
    camera->direction[1] = ray->D.y;
    camera->direction[2] = ray->D.z;
    camera->tmin = ray->tmin;
    camera->tmax = ray->tmax;
    camera->time = ray->time;
#    ifdef __RAY_DIFFERENTIALS__
    camera->dP = ray->dP;
    camera->dD = ray->dD;
#    endif
    if (kernel_data.cam.type == CAMERA_PERSPECTIVE || kernel_data.cam.type == CAMERA_ORTHOGRAPHIC)
    {
      camera->flags |= DEEP_NATIVE_CAMERA_AFFINE_DEPTH;
      camera->z_origin = camera_z_depth(kg, ray->P);
      const Transform worldtocamera = kernel_data.cam.worldtocamera;
      camera->z_direction = transform_direction(&worldtocamera, ray->D).z;
      deep_camera_capture_projection(kg, camera, ray, raster);
    }
    else if (kernel_data.cam.type == CAMERA_PANORAMA || kernel_data.cam.type == CAMERA_CUSTOM) {
      camera->flags |= DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
    }
    else {
      deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    }
  }
  deep_witness_init(kg, index, is_zero(throughput) ? 0.0f : ray->tmin, !is_zero(throughput));
  if (kernel_integrator_state.deep_compact_trace) deep_compact_capture_case();
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_camera_written,
                              1);
}
#  endif

ccl_device_inline void integrator_init_from_optical_receipt(KernelGlobals kg,
                                                            IntegratorState state)
{
#if __DEEP_BOUNDED_POLICY__ == 0
  const DeepCameraReceipt input = kernel_integrator_state.deep_optical_inputs[state];
  path_state_init_queues(state);
  kernel_integrator_state.deep_optical_ledger[state] = {};
  if (kernel_integrator_state.deep_owned_optical_checkpoints)
    kernel_integrator_state.deep_owned_optical_checkpoints[state] = {};
  if (kernel_integrator_state.deep_owned_optical_coverage) {
    kernel_integrator_state.deep_owned_optical_coverage[state] = {};
    if (input.flags & DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT)
      kernel_integrator_state.deep_owned_optical_coverage[state].flags =
          DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_NO_RAY;
  }
  if (input.flags & DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT) {
    kernel_integrator_state.deep_optical_ledger[state].status = DEEP_OPTICAL_ZERO_THROUGHPUT;
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_optical_completed, 1);
    return;
  }
  /* Actual admitted Ray and RNG, with no camera evaluation or film/count writes. Host admission
   * verifies finite fields and the exact disposition before this entry can be dispatched. */
  Ray ray = {};
  ray.P = make_float3(input.origin[0], input.origin[1], input.origin[2]);
  ray.D = make_float3(input.direction[0], input.direction[1], input.direction[2]);
  ray.tmin = input.tmin;
  ray.tmax = input.tmax;
  ray.time = input.time;
#  ifdef __RAY_DIFFERENTIALS__
  ray.dP = input.dP;
  ray.dD = input.dD;
#  endif
  integrator_state_write_ray(state, &ray);
  if (kernel_integrator_state.deep_owned_optical_coverage) {
    auto &coverage = kernel_integrator_state.deep_owned_optical_coverage[state];
    coverage.front_t = coverage.cursor_t = input.tmin;
    coverage.z_front = coverage.z_back = camera_z_depth(kg, ray.P + ray.D * input.tmin);
  }
  path_state_init_integrator(
      kg,
      state,
      input.sample,
      input.rng_pixel,
      make_float3(input.throughput[0], input.throughput[1], input.throughput[2]));
  INTEGRATOR_STATE_WRITE(state, path, render_pixel_index) = input.pixel;
  INTEGRATOR_STATE_WRITE(state, path, rng_offset) = input.rng_offset;
  INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_DEEP_VOLUME_OPTICAL;
  bool compact_stack = false;
  if (kernel_integrator_state.deep_compact_rays) {
    const uint index = deep_compact_camera_index(state);
    auto &scalar = kernel_integrator_state.deep_compact_rays[index];
    compact_stack = scalar.stack_valid;
    if (scalar.last_surface_t == scalar.cursor)
      INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(scalar.cursor);
    if (compact_stack) {
      for (int i = 0; i < kernel_data.volume_stack_size; i++) {
        const auto &entry = kernel_integrator_state.deep_compact_stacks[
            size_t(index) * kernel_integrator_state.deep_compact_stack_stride + i];
        INTEGRATOR_STATE_ARRAY_WRITE(state, volume_stack, i, object) = entry.object;
        INTEGRATOR_STATE_ARRAY_WRITE(state, volume_stack, i, shader) = entry.shader;
      }
    }
    if (scalar.flags & (DEEP_COMPACT_PENDING_POINT | DEEP_COMPACT_FINISH_PENDING)) {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE);
      return;
    }
    if (scalar.flags & DEEP_COMPACT_PENDING_CELL) {
      scalar.flags |= DEEP_COMPACT_TRANSFER_PENDING;
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL);
      return;
    }
  }
  integrator_path_init(state,
                       kernel_data.cam.is_inside_volume && !compact_stack ?
                           DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK :
                           DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
#endif
}

ccl_device_inline void integrator_capture_camera_receipt(IntegratorState state,
                                                         const int x,
                                                         const int y,
                                                         const int sample,
                                                         const uint rng_pixel,
                                                         const ccl_private Ray *ray,
                                                         const Spectrum throughput,
                                                         const bool cache_retry)
{
  if (!kernel_integrator_state.deep_camera_receipts) {
    return;
  }
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_camera_receipt_count, 1);
  if (index >= kernel_integrator_state.deep_camera_receipt_capacity) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_camera_receipt_overflow, 1);
    return;
  }
  DeepCameraReceipt receipt = {};
  receipt.pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  receipt.sample = sample;
  receipt.rng_pixel = rng_pixel;
  receipt.rng_offset = PRNG_BOUNCE_NUM;
  receipt.x = x;
  receipt.y = y;
  receipt.population_weight = 1.0f;
  receipt.flags = cache_retry ? DEEP_CAMERA_RECEIPT_CACHE_RETRY : 0;
  receipt.throughput[0] = throughput.x;
  receipt.throughput[1] = throughput.y;
  receipt.throughput[2] = throughput.z;
  if (is_zero(throughput)) {
    receipt.flags |= DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT;
  }
  else {
    receipt.flags |= DEEP_CAMERA_RECEIPT_RAY_VALID;
    receipt.origin[0] = ray->P.x;
    receipt.origin[1] = ray->P.y;
    receipt.origin[2] = ray->P.z;
    receipt.direction[0] = ray->D.x;
    receipt.direction[1] = ray->D.y;
    receipt.direction[2] = ray->D.z;
    receipt.tmin = ray->tmin;
    receipt.tmax = ray->tmax;
    receipt.time = ray->time;
#  ifdef __RAY_DIFFERENTIALS__
    receipt.dP = ray->dP;
    receipt.dD = ray->dD;
#  endif
  }
  kernel_integrator_state.deep_camera_receipts[index] = receipt;
}
#endif

#ifdef __DEEP_OUTPUT__
template<DeviceKernel current_kernel>
ccl_device_forceinline void integrator_finish_deep_volume_optical_traversal(KernelGlobals kg,
                                                                            IntegratorState state)
{
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_optical_inputs) {
    if (kernel_integrator_state.deep_compact_rays) {
      auto &scalar = kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)];
      if (!deep_compact_flush_candidate(scalar)) {
        scalar.flags |= DEEP_COMPACT_FINISH_PENDING;
        integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE);
        return;
      }
      if (!(scalar.flags & DEEP_COMPACT_TERMINAL)) {
        const uint previous = scalar.next_ordinal;
        deep_compact_capture_background(kg, state, kernel_data.background.transparent);
        if (!kernel_data.background.transparent && scalar.next_ordinal == previous) {
          integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE);
          return;
        }
      }
      scalar.flags &= ~DEEP_COMPACT_FINISH_PENDING;
    }
    DeepOpticalLedger &ledger = kernel_integrator_state.deep_optical_ledger[state];
    if (kernel_integrator_state.deep_owned_optical_checkpoints &&
        kernel_integrator_state.deep_owned_optical_checkpoints[state].active)
      deep_owned_optical_error(DEEP_OPTICAL_ERROR_COMPLETION);
    if (kernel_integrator_state.deep_owned_optical_coverage) {
      auto &coverage = kernel_integrator_state.deep_owned_optical_coverage[state];
      if (!(coverage.flags & DEEP_OWNED_OPTICAL_VISIBILITY_END)) {
        if (coverage.cursor_t != kernel_integrator_state.deep_optical_inputs[state].tmax)
          deep_owned_optical_error(DEEP_OPTICAL_ERROR_COMPLETION);
        coverage.flags |= DEEP_OWNED_OPTICAL_FAR;
      }
      coverage.flags |= DEEP_OWNED_OPTICAL_COMPLETE;
    }
    if (ledger.status != 0) {
      atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_optical_error,
                                 DEEP_OPTICAL_ERROR_COMPLETION);
    }
    else {
      ledger.status = DEEP_OPTICAL_COMPLETE;
      atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_optical_completed,
                                  1);
    }
    integrator_path_terminate(kg, state, nullptr, current_kernel);
    return;
  }
#  endif
  const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  const uint32_t last_sample_idx = INTEGRATOR_STATE(state, path, deep_surface_sample_idx);
  film_mark_deep_volume_primary_ray_flag(
      kg,
      pixel_index,
      last_sample_idx,
      (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr,
      DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_PROFILE | DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_ray(state, &ray);
  ray.tmin = INTEGRATOR_STATE(state, path, min_ray_pdf);

  const int sample = INTEGRATOR_STATE(state, path, sample);
  const uint rng_pixel = INTEGRATOR_STATE(state, path, rng_pixel);
  const Spectrum camera_throughput = INTEGRATOR_STATE(state, path, throughput);
  integrator_state_write_ray(state, &ray);
  path_state_init_integrator(kg, state, sample, rng_pixel, camera_throughput);

#  ifdef __KERNEL_GPU__
  if (kernel_data.film.deep_optical_only_kernel == 3) {
    /* The queue holds these completed states until the optical wavefront has drained.
     *
     * Release the existing restored ray directly; the render buffer index is tile-relative
     *
     * and cannot be used to regenerate the camera coordinates. */
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY);
    return;
  }
#  endif

  integrator_path_next(state,
                       current_kernel,
                       kernel_data.cam.is_inside_volume ?
                           DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK :
                           DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
}
#endif

ccl_device_inline Spectrum integrate_camera_sample(KernelGlobals kg,
                                                   const int sample,
                                                   const int x,
                                                   const int y,
                                                   const uint rng_pixel,
                                                   ccl_private Ray *ray,
                                                   ccl_private int &r_cache_miss,
                                                   ccl_private float2 *deep_raster = nullptr
#if __DEEP_BOUNDED_POLICY__ > 0
                                                   , ccl_private uint32_t *bounded_bucket = nullptr
#endif
                                                   )
{
  /* Filter sampling. */
  const float2 rand_filter = (sample == 0) ? make_float2(0.5f, 0.5f) :
                                             path_rng_2D(kg, rng_pixel, sample, PRNG_FILTER);
#if __DEEP_BOUNDED_POLICY__ > 0
  if (bounded_bucket) {
    const uint32_t bx = min(uint32_t(rand_filter.x * 4.0f), 3u);
    const uint32_t by = min(uint32_t(rand_filter.y * 4.0f), 3u);
    *bounded_bucket = by * 4 + bx;
  }
#endif

  /* Motion blur (time) and depth of field (lens) sampling. (time, lens_x, lens_y) */
  const bool use_motionblur = kernel_data.cam.shuttertime != -1.0f;
  const bool use_dof = kernel_data.cam.aperturesize > 0.0f;
  const bool use_custom_cam = kernel_data.cam.type == CAMERA_CUSTOM;
  const float3 rand_time_lens = (use_motionblur || use_dof || use_custom_cam) ?
                                    path_rng_3D(kg, rng_pixel, sample, PRNG_LENS_TIME) :
                                    zero_float3();

  /* We use x for time and y,z for lens because in practice with Sobol
   * sampling this seems to give better convergence when an object is
   * both motion blurred and out of focus, without significantly harming
   * convergence for focal blur alone.  This is a little surprising,
   * because one would expect using x,y for lens (the 2d part) would be
   * best, since x,y are the best stratified.  Since it's not entirely
   * clear why this is, this is probably worth revisiting at some point
   * to investigate further. */
  const float rand_time = rand_time_lens.x;
  const float2 rand_lens = make_float2(rand_time_lens.y, rand_time_lens.z);

  /* Generate camera ray. */
  return camera_sample(kg, x, y, rand_filter, rand_time, rand_lens, ray, r_cache_miss, deep_raster);
}

/* Return false to indicate that this pixel is finished.
 * Used by CPU implementation to not attempt to sample pixel for multiple samples once its known
 * that the pixel did converge. */
ccl_device bool integrator_init_from_camera(KernelGlobals kg,
                                            IntegratorState state,
                                            const ccl_global KernelWorkTile *ccl_restrict tile,
                                            ccl_global float *render_buffer,
                                            const int x_,
                                            const int y_,
                                            const int scheduled_sample)
{
  PROFILING_INIT(kg, PROFILING_RAY_SETUP);

  int x, y, sample;
  uint32_t deep_native_camera_index = DEEP_NATIVE_INVALID_INDEX;

  if (tile == nullptr) {
    /* Restart from miss. Reconstruct x, y, sample from state. */
    const uint pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
    x = pixel_index % (int)kernel_data.cam.width;
    y = pixel_index / (int)kernel_data.cam.width;
    sample = INTEGRATOR_STATE(state, path, sample);
#if __DEEP_BOUNDED_POLICY__ > 0
    deep_bounded_retry_raster(kg, state, x, y);
#endif
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (kernel_integrator_state.deep_native_cameras) {
      deep_native_camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
      if (deep_native_camera_index < kernel_integrator_state.deep_native_camera_capacity) {
        /* Retry from the actual absolute raster coordinates, including crop/overscan. */
        x = kernel_integrator_state.deep_native_cameras[deep_native_camera_index].x;
        y = kernel_integrator_state.deep_native_cameras[deep_native_camera_index].y;
      }
    }
#endif
  }
  else {
    x = x_;
    y = y_;
#if __DEEP_BOUNDED_POLICY__ > 0
    if (!deep_bounded_camera_pixel(kg, x, y)) {
      return false;
    }
#endif

    /* Initialize path state to give basic buffer access and allow early outputs. */
    path_state_init(state, tile, x, y);
    /* Check whether the pixel has converged and should not be sampled anymore. */
    if (!film_need_sample_pixel(kg, state, render_buffer)) {
      return false;
    }
#if __DEEP_BOUNDED_POLICY__ > 0
    deep_bounded_bind_camera(kg, state, x, y, scheduled_sample);
#endif

    /* Count the sample and get an effective sample for this pixel. */
    sample = film_write_sample(kg, state, render_buffer, scheduled_sample, tile->sample_offset);
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    deep_native_camera_index = integrator_reserve_native_camera(
        state, x, y, sample, scheduled_sample, tile->sample_offset);
#endif
  }

  /* Initialize random number seed for path. */
  const uint rng_pixel = path_rng_pixel_init(kg, sample, x, y);

  /* Generate camera ray. */
  Ray ray;
  int cache_miss = 0;
  float2 deep_raster = zero_float2();
  ccl_private float2 *deep_raster_output = nullptr;
#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_native_cameras) deep_raster_output = &deep_raster;
#endif
#if __DEEP_BOUNDED_POLICY__ > 0
  uint32_t bounded_bucket = 0;
  Spectrum T = integrate_camera_sample(
      kg, sample, x, y, rng_pixel, &ray, cache_miss, nullptr, &bounded_bucket);
#else
  Spectrum T = integrate_camera_sample(
      kg, sample, x, y, rng_pixel, &ray, cache_miss, deep_raster_output);
#endif
  if (cache_miss) {
    if (tile != nullptr) {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
      INTEGRATOR_STATE_WRITE(state, path, sample) = sample;
    }
    integrator_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
    return true;
  }

#if __DEEP_BOUNDED_POLICY__ > 0
  deep_bounded_camera_success(kg, state, bounded_bucket);
#endif
#ifdef __KERNEL_GPU__
  /* Cache attempts do not contribute twice. Failed camera evaluation still contributes one
   *
   * zero-throughput sample to the denominator without touching an undefined Ray. */
#  if __DEEP_BOUNDED_POLICY__ == 0
  integrator_capture_camera_receipt(state, x, y, sample, rng_pixel, &ray, T, tile == nullptr);
#  endif
#  ifdef __DEEP_OUTPUT__
  integrator_commit_native_camera(
      kg, deep_native_camera_index, rng_pixel, &ray, T, tile == nullptr, deep_raster);
#  endif
#endif

  if (is_zero(T)) {
    if (tile == nullptr) {
      integrator_path_terminate(
          kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
    }
#if __DEEP_BOUNDED_POLICY__ == 1 || __DEEP_BOUNDED_POLICY__ == 2
    else {
      integrator_deep_camera_close(state);
    }
#endif
    return true;
  }

  /* Write camera ray to state. */
  integrator_state_write_ray(state, &ray);

  if (tile == nullptr) {
    /* Re-initialize path state for path integration. */
    path_state_init_integrator(kg, state, sample, rng_pixel, T);
    integrator_path_next(state,
                         DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA,
                         kernel_data.cam.is_inside_volume ?
                             DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK :
                             DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
  }
  else {
    /* Initialize path state for path integration. */
    path_state_init_integrator(kg, state, sample, rng_pixel, T);

    /* Continue with intersect_closest kernel, optionally initializing volume
     * stack before that if the camera may be inside a volume. */
    if (kernel_data.cam.is_inside_volume) {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK);
    }
    else {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
    }
  }

#if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_native_cameras) {
    /* path_state_init_integrator clears ordinary state fields; reattach the independent owner. */
    INTEGRATOR_STATE_WRITE(state, path, deep_frame_camera_index) = deep_native_camera_index;
    INTEGRATOR_STATE_WRITE(state, path, deep_frame_root_handle) = 0;
  }
#endif

#ifdef __DEEP_OUTPUT__
  if (kernel_data.film.use_deep_output >= 2) {
    /* Preserve the original camera near distance while the optical traversal advances ray.tmin

     * * through volume boundaries. Beauty initialization restores this value. */
    INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = ray.tmin;
    INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_DEEP_VOLUME_OPTICAL;
  }
#endif

  return true;
}

CCL_NAMESPACE_END
