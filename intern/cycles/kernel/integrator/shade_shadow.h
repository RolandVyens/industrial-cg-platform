/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shade_volume.h"
#include "kernel/integrator/surface_shader.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/geom/shader_data.h"
#include "kernel/light/light.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline bool shadow_intersections_has_remaining(const uint num_hits)
{
  return num_hits >= INTEGRATOR_SHADOW_ISECT_SIZE;
}

#ifdef __TRANSPARENT_SHADOWS__
ccl_device_inline Spectrum integrate_transparent_surface_shadow(KernelGlobals kg,
                                                                IntegratorShadowState state,
                                                                const int hit)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SURFACE);

  /* TODO: does aliasing like this break automatic SoA in CUDA?
   * Should we instead store closures separate from ShaderData?
   *
   * TODO: is it better to declare this outside the loop or keep it local
   * so the compiler can see there is no dependency between iterations? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data at surface. */
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_shadow_isect(state, &isect, hit);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);

  shader_setup_from_ray(kg, shadow_sd, &ray, &isect);

  /* Evaluate shader. */
  if (!(shadow_sd->flag & SD_HAS_ONLY_VOLUME)) {
    surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
        kg, state, shadow_sd, nullptr, PATH_RAY_SHADOW);
  }
  else {
    INTEGRATOR_STATE_WRITE(state, shadow_path, volume_bounds_bounce) += 1;
  }

#  ifdef __VOLUME__
  /* Exit/enter volume. */
  volume_stack_enter_exit<true>(kg, state, shadow_sd);
#  endif

  /* Disable transparent shadows for ray portals */
  if (shadow_sd->flag & SD_RAY_PORTAL) {
    return zero_spectrum();
  }

  /* Compute transparency from closures. */
  return surface_shader_transparency(shadow_sd);
}

#  ifdef __VOLUME__
ccl_device_inline void integrate_transparent_volume_shadow(KernelGlobals kg,
                                                           IntegratorShadowState state,
                                                           const int hit,
                                                           const int num_recorded_hits,
                                                           ccl_private Spectrum *ccl_restrict
                                                               throughput)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_VOLUME);

  /* TODO: deduplicate with surface, or does it not matter for memory usage? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data. */
  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);
  ray.self.object = OBJECT_NONE;
  ray.self.prim = PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  /* Modify ray position and length to match current segment. */
  ray.tmin = (hit == 0) ? ray.tmin : INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit - 1, t);
  ray.tmax = (hit < num_recorded_hits) ? INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit, t) :
                                         ray.tmax;

  /* `object` is only needed for light tree with light linking, it is irrelevant for shadow. */
  shader_setup_from_volume(shadow_sd, &ray, OBJECT_NONE);

  if (kernel_data.integrator.volume_ray_marching) {
    const float step_size = volume_stack_step_size<true>(kg, state);
    volume_shadow_ray_marching(kg, state, &ray, shadow_sd, throughput, step_size);
  }
  else {
    volume_shadow_null_scattering(kg, state, &ray, shadow_sd, throughput);
  }
}
#  endif

ccl_device_inline bool integrate_transparent_shadow(KernelGlobals kg,
                                                    IntegratorShadowState state,
                                                    const uint num_hits)
{
  /* Accumulate shadow for transparent surfaces. */
  const uint num_recorded_hits = min(num_hits, (uint)INTEGRATOR_SHADOW_ISECT_SIZE);

  /* Plus one to account for world volume, which has no boundary to hit but casts shadows. */
  for (uint hit = 0; hit < num_recorded_hits + 1; hit++) {
    /* Volume shaders. */
    if (hit < num_recorded_hits || !shadow_intersections_has_remaining(num_hits)) {
#  ifdef __VOLUME__
      if (!integrator_state_shadow_volume_stack_is_empty(kg, state)) {
        Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput);
        integrate_transparent_volume_shadow(kg, state, hit, num_recorded_hits, &throughput);
        if (is_zero(throughput)) {
          return true;
        }

        INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      }
#  endif
    }

    /* Surface shaders. */
    if (hit < num_recorded_hits) {
      const Spectrum shadow = integrate_transparent_surface_shadow(kg, state, hit);
      const Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput) * shadow;
      if (is_zero(throughput)) {
        return true;
      }

      INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      INTEGRATOR_STATE_WRITE(state, shadow_path, transparent_bounce) += 1;
      INTEGRATOR_STATE_WRITE(state, shadow_path, rng_offset) += PRNG_BOUNCE_NUM;
    }

    if (INTEGRATOR_STATE(state, shadow_path, volume_bounds_bounce) > VOLUME_BOUNDS_MAX) {
      return true;
    }

    /* Note we do not need to check max_transparent_bounce here, the number
     * of intersections is already limited and made opaque in the
     * INTERSECT_SHADOW kernel. */
  }

  if (shadow_intersections_has_remaining(num_hits)) {
    /* There are more hits that we could not recorded due to memory usage,
     * adjust ray to intersect again from the last hit. */
    const float last_hit_t = INTEGRATOR_STATE_ARRAY(state, shadow_isect, num_recorded_hits - 1, t);
    INTEGRATOR_STATE_WRITE(state, shadow_ray, tmin) = intersection_t_offset(last_hit_t);
  }

  return false;
}
#endif /* __TRANSPARENT_SHADOWS__ */

ccl_device void integrator_shade_shadow(KernelGlobals kg,
                                        IntegratorShadowState state,
                                        ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SETUP);
  uint num_hits = INTEGRATOR_STATE(state, shadow_path, num_hits);
  const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);
  Spectrum shadow_color = zero_spectrum();
  bool apply_shadow_color = false;

  if (!(path_flag & PATH_RAY_SHADOW_FOR_AO)) {
    Ray ray ccl_optional_struct_init;
    integrator_state_read_shadow_ray(state, &ray);
    integrator_state_read_shadow_ray_self(state, &ray);

    if (ray.self.light_object != OBJECT_NONE && ray.self.light_prim != PRIM_NONE) {
      const KernelObject &kobject = kernel_data_fetch(objects, ray.self.light_object);
      if (kobject.primitive_type == PRIMITIVE_LAMP) {
        const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ray.self.light_prim);
        shadow_color = make_float3(klight->shadow_color[0],
                                   klight->shadow_color[1],
                                   klight->shadow_color[2]);
        shadow_color = clamp(shadow_color, zero_spectrum(), one_spectrum());
        apply_shadow_color = !is_zero(shadow_color);
      }
    }
  }

#ifdef __TRANSPARENT_SHADOWS__
  /* Evaluate transparent shadows. */
  const bool opaque = integrate_transparent_shadow(kg, state, num_hits);
  if (opaque && !apply_shadow_color) {
    integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }
  if (opaque) {
    num_hits = 0;
  }
#endif

  if (shadow_intersections_has_remaining(num_hits)) {
    /* More intersections to find, continue shadow ray. */
    integrator_shadow_path_next(
        state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
    return;
  }

  if (apply_shadow_color) {
    /* Shadow color tinting.
     * Compute transmittance as the ratio of shadowed to unshadowed throughput, then linearly
     * interpolate: full occlusion (T=0) yields shadow_color, no occlusion (T=1) yields white.
     * Formula: tinted = T + (1 - T) * shadow_color. */
    const Spectrum unshadowed_throughput = INTEGRATOR_STATE(
        state, shadow_path, unshadowed_throughput);
    if (!is_zero(unshadowed_throughput)) {
      const Spectrum shadowed_throughput = INTEGRATOR_STATE(state, shadow_path, throughput);
      Spectrum transmittance = safe_divide_color(shadowed_throughput, unshadowed_throughput);
      transmittance = clamp(transmittance, zero_spectrum(), one_spectrum());
      const Spectrum tinted_transmittance =
          transmittance + (one_spectrum() - transmittance) * shadow_color;
      INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) =
          unshadowed_throughput * tinted_transmittance;
    }
  }

  guiding_record_direct_light(kg, state);
  film_write_direct_light(kg, state, render_buffer);
  integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
}

CCL_NAMESPACE_END
