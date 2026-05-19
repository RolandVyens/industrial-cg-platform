/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/film/write.h"
#include "kernel/film/deep_write.h"
#include "kernel/integrator/state_util.h"

#include "kernel/integrator/shadow_catcher.h"

#include "kernel/camera/camera.h"

#include "kernel/sample/pattern.h"
#include "util/atomic.h"

CCL_NAMESPACE_BEGIN

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
                                             const int bounce)
{
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
    *L *= limit / sum;
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
  const int split_lightgroup = film_get_split_lightgroup_index(kg, lightgroup);
  if (split_lightgroup == LIGHTGROUP_NONE || pass_offset == PASS_UNUSED) {
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
ccl_device_inline void film_write_volume_scattering_guiding_pass(KernelGlobals kg,
                                                                 ccl_global float *ccl_restrict
                                                                     buffer,
                                                                 const uint32_t path_flag,
                                                                 const Spectrum contribution)
{
  int pass_offset = PASS_UNUSED;
  if (path_flag & PATH_RAY_VOLUME_PRIMARY_TRANSMIT) {
    pass_offset = kernel_data.film.pass_volume_transmit;
  }
  else if (path_flag & PATH_RAY_VOLUME_SCATTER) {
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
                                                const uint32_t path_flag,
                                                const int sample,
                                                const Spectrum contribution,
                                                ccl_global float *ccl_restrict buffer,
                                                const float depth,
                                                const uint32_t pixel_index)
{
#ifdef __SHADOW_CATCHER__
  if (film_write_shadow_catcher(kg, path_flag, contribution, buffer)) {
    return;
  }
#endif

  if (kernel_data.film.light_pass_flag & PASSMASK(COMBINED)) {
    film_write_pass_spectrum(buffer + kernel_data.film.pass_combined, contribution);
  }

  film_write_adaptive_buffer(kg, sample, contribution, buffer);
  film_write_volume_scattering_guiding_pass(kg, buffer, path_flag, contribution);

#ifdef __DEEP_OUTPUT__
  /* Deep samples for surfaces are written once in shade_surface.h at first hit.
   * This function (film_write_combined_pass) is called multiple times per pixel
   * for incremental light contributions, so we don't write here.
   * Volumes write their own samples in shade_volume.h. */
  (void)depth;
  (void)pixel_index;
#endif
}

/* Write combined pass with transparency. */
ccl_device_inline void film_write_combined_transparent_pass(KernelGlobals kg,
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
  film_write_volume_scattering_guiding_pass(kg, buffer, path_flag, contribution);
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
    film_write_lightgroup_pass(
        buffer, kernel_data.film.pass_lightgroup, lightgroup, contribution);
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
                                                       kernel_data.film
                                                           .pass_transmission_indirect;

      if (transmission_pass_offset != PASS_UNUSED) {
        film_write_pass_spectrum(buffer + transmission_pass_offset, transmission_contribution);
      }

      if (write_lightgroup_lobes) {
        film_write_lightgroup_split_pass(
            kg,
            buffer, kernel_data.film.pass_lightgroup_diffuse, lightgroup, diffuse_contribution);
        film_write_lightgroup_split_pass(
            kg,
            buffer, kernel_data.film.pass_lightgroup_glossy, lightgroup, glossy_contribution);
        film_write_lightgroup_split_pass(kg,
                                         buffer,
                                         kernel_data.film.pass_lightgroup_transmission,
                                         lightgroup,
                                         transmission_contribution);
        film_write_lightgroup_split_pass(
            kg,
            buffer,
            is_direct ? kernel_data.film.pass_lightgroup_diffuse_direct :
                        kernel_data.film.pass_lightgroup_diffuse_indirect,
            lightgroup,
            diffuse_contribution);
        film_write_lightgroup_split_pass(
            kg,
            buffer,
            is_direct ? kernel_data.film.pass_lightgroup_glossy_direct :
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
            kg,
            buffer, kernel_data.film.pass_lightgroup_volume, lightgroup, contribution);
        film_write_lightgroup_split_pass(
            kg,
            buffer,
            is_direct ? kernel_data.film.pass_lightgroup_volume_direct :
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
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, shadow_path, bounce));

  ccl_global float *buffer = film_pass_pixel_render_buffer_shadow(kg, state, render_buffer);

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

  /* Ambient occlusion. */
  if (path_flag & PATH_RAY_SHADOW_FOR_AO) {
    if ((kernel_data.kernel_features & KERNEL_FEATURE_AO_PASS) && (path_flag & PATH_RAY_CAMERA)) {
      film_write_pass_spectrum(buffer + kernel_data.film.pass_ao, contribution);
    }
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
      const Spectrum ao_weight = INTEGRATOR_STATE(state, shadow_path, unshadowed_throughput);
      film_write_combined_pass(
          kg, path_flag, sample, contribution * ao_weight, buffer, depth, pixel_index);
    }
    return;
  }

  /* Direct light shadow. */
  film_write_combined_pass(kg, path_flag, sample, contribution, buffer, depth, pixel_index);

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
        const int transmission_pass_offset =
            is_direct ? kernel_data.film.pass_transmission_direct :
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
          film_write_lightgroup_split_pass(
              kg,
              buffer, kernel_data.film.pass_lightgroup_diffuse, lightgroup, diffuse_contribution);
          film_write_lightgroup_split_pass(
              kg,
              buffer, kernel_data.film.pass_lightgroup_glossy, lightgroup, glossy_contribution);
          film_write_lightgroup_split_pass(kg,
                                           buffer,
                                           kernel_data.film.pass_lightgroup_transmission,
                                           lightgroup,
                                           transmission_contribution);
          film_write_lightgroup_split_pass(
              kg,
              buffer,
              is_direct ? kernel_data.film.pass_lightgroup_diffuse_direct :
                          kernel_data.film.pass_lightgroup_diffuse_indirect,
              lightgroup,
              diffuse_contribution);
          film_write_lightgroup_split_pass(
              kg,
              buffer,
              is_direct ? kernel_data.film.pass_lightgroup_glossy_direct :
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
              kg,
              buffer, kernel_data.film.pass_lightgroup_volume, lightgroup, contribution);
          film_write_lightgroup_split_pass(
              kg,
              buffer,
              is_direct ? kernel_data.film.pass_lightgroup_volume_direct :
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
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (is_transparent_background_ray) {
    film_write_transparent(kg, path_flag, transparent, buffer);
  }
  else {
    const int sample = INTEGRATOR_STATE(state, path, sample);
    film_write_combined_transparent_pass(kg, path_flag, sample, contribution, transparent, buffer);
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
ccl_device_inline void film_write_volume_emission(KernelGlobals kg,
                                                  ConstIntegratorState state,
                                                  const Spectrum L,
                                                  ccl_global float *ccl_restrict render_buffer,
                                                  const float depth,
                                                  const int lightgroup = LIGHTGROUP_NONE)
{
  Spectrum contribution = L;
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const int sample = INTEGRATOR_STATE(state, path, sample);
  const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

  /* Write deep sample if depth is valid (>= 0). */
  film_write_combined_pass(kg, path_flag, sample, contribution, buffer, depth, pixel_index);
  film_write_emission_or_background_pass(
      kg, state, contribution, buffer, kernel_data.film.pass_emission, false, lightgroup, false);
}

ccl_device_inline void film_write_surface_emission(KernelGlobals kg,
                                                   ConstIntegratorState state,
                                                   const Spectrum L,
                                                   const float mis_weight,
                                                   ccl_global float *ccl_restrict render_buffer,
                                                   const int lightgroup = LIGHTGROUP_NONE,
                                                   const bool split_lightgroup_lobes = false)
{
  Spectrum contribution = INTEGRATOR_STATE(state, path, throughput) * L * mis_weight;
  film_clamp_light(kg, &contribution, INTEGRATOR_STATE(state, path, bounce) - 1);

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
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

  film_write_combined_pass(kg, path_flag, sample, contribution, buffer, depth, pixel_index);
  film_write_emission_or_background_pass(
      kg,
      state,
      contribution,
      buffer,
      kernel_data.film.pass_emission,
      false,
      lightgroup,
      split_lightgroup_lobes);
}

CCL_NAMESPACE_END
