/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/closure/volume.h"

#include "kernel/film/denoising_passes.h"
#include "kernel/film/light_passes.h"
#include "kernel/integrator/deep_optical_capture.h"

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/intersect_closest.h"
#include "kernel/integrator/path_state.h"
#include "kernel/integrator/shadow_linking.h"
#include "kernel/integrator/state.h"
#include "kernel/integrator/state_flow.h"
#include "kernel/integrator/volume_shader.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/light/light.h"
#include "kernel/light/sample.h"

#include "kernel/geom/shader_data.h"

#include "kernel/sample/lcg.h"

CCL_NAMESPACE_BEGIN

/* Events for probabilistic scattering. */

enum VolumeIntegrateEvent {
  VOLUME_PATH_SCATTERED = 0,
  VOLUME_PATH_ATTENUATED = 1,
  VOLUME_PATH_MISSED = 2,
  VOLUME_PATH_CACHE_MISS = 3,
  VOLUME_PATH_PAUSED = 4
};

#ifdef __VOLUME__

namespace {
struct VolumeIntegrateResult {
  /* Throughput and offset for direct light scattering. */
  bool direct_scatter;
  Spectrum direct_throughput;
  float direct_t;
  ShaderVolumePhases direct_phases;
#  if defined(__PATH_GUIDING__)
  VolumeSampleMethod direct_sample_method;
#  endif

  /* Throughput and offset for indirect light scattering. */
  bool indirect_scatter;
  Spectrum indirect_throughput;
  float indirect_t;
  ShaderVolumePhases indirect_phases;
#  if __DEEP_BOUNDED_POLICY__ == 3
  bool bounded_colour_permit;
#  endif
};
}  // namespace

/* We use both volume octree and volume stack, sometimes they disagree on whether a point is inside
 * a volume or not. We accept small numerical precision issues, above this threshold the volume
 * stack shall prevail. */
/* TODO(weizhen): tweak this value. */
#  define OVERLAP_EXP 5e-4f
/* Restrict the number of steps in case of numerical problems */
#  define VOLUME_MAX_STEPS 1024

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline void volume_capture_primary_emission_span(KernelGlobals kg,
                                                            ConstIntegratorState state,
                                                            const ccl_private Ray *ray,
                                                            const float tmin,
                                                            const float tmax,
                                                            const Spectrum emission,
                                                            const Spectrum optical_depth,
                                                            const float alpha,
                                                            const uint32_t kind)
{
  if (!kernel_integrator_state.deep_volume_emission_spans ||
      INTEGRATOR_STATE(state, path, bounce) != 0 || is_zero(emission))
  {
    return;
  }
  DeepVolumeEmissionSpan span = {};
  span.root = INTEGRATOR_STATE(state, path, deep_volume_radiance_idx);
  span.pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  span.sample = INTEGRATOR_STATE(state, path, sample);
  span.kind = kind;
  span.path_flags = INTEGRATOR_STATE(state, path, flag);
  span.z = camera_z_depth(kg, ray->P + ray->D * tmin);
  span.zback = camera_z_depth(kg, ray->P + ray->D * tmax);
  span.alpha = alpha;
  span.time = ray->time;
  const float3 rgb = spectrum_to_rgb(emission);
  const float3 tau = spectrum_to_rgb(optical_depth);
  span.rgb[0] = rgb.x;
  span.rgb[1] = rgb.y;
  span.rgb[2] = rgb.z;
  span.tau[0] = tau.x;
  span.tau[1] = tau.y;
  span.tau[2] = tau.z;
  film_append_volume_emission_span(span);
}
#  endif
/* Number of mantissa bits of floating-point numbers. */
#  define MANTISSA_BITS 23

/* Volume shader properties
 *
 * extinction coefficient = absorption coefficient + scattering coefficient
 * sigma_t = sigma_a + sigma_s */

struct VolumeShaderCoefficients {
  Spectrum sigma_t;
  Spectrum sigma_s;
  Spectrum emission;
};

struct EquiangularCoefficients {
  float3 P;
  Interval<float> t_range;
};

/* Evaluate extinction coefficient at `sd->P`. */
template<const bool shadow, typename IntegratorGenericState>
ccl_device_inline Spectrum volume_shader_eval_extinction(KernelGlobals kg,
                                                         const IntegratorGenericState state,
                                                         ccl_private ShaderData *ccl_restrict sd,
                                                         const PathRayVisibility path_visibility,
                                                         uint32_t path_flag)
{
  /* Use emission flag to avoid storing phase function. */
  /* TODO(weizhen): we could add another flag to skip evaluating the emission, but we've run out of
   * bits for the path flag.*/
  path_flag |= PATH_RAY_EMISSION;

  volume_shader_eval<shadow>(kg, state, sd, path_visibility, path_flag);

  return (sd->flag & SD_EXTINCTION) ? sd->closure_transparent_extinction : zero_spectrum();
}

/* Evaluate shader to get absorption, scattering and emission at P. */
ccl_device_inline bool volume_shader_sample(KernelGlobals kg,
                                            IntegratorState state,
                                            ccl_private ShaderData *ccl_restrict sd,
                                            ccl_private VolumeShaderCoefficients *coeff)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  volume_shader_eval<false>(kg, state, sd, path_visibility, path_flag);

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_native_cameras && (sd->flag & SD_CACHE_MISS))
    return false;
#  endif

  if (!(sd->flag & (SD_EXTINCTION | SD_SCATTER | SD_EMISSION))) {
    return false;
  }

  coeff->sigma_s = zero_spectrum();
  coeff->sigma_t = (sd->flag & SD_EXTINCTION) ? sd->closure_transparent_extinction :
                                                zero_spectrum();
  coeff->emission = (sd->flag & SD_EMISSION) ? sd->closure_emission_background : zero_spectrum();

  if (sd->flag & SD_SCATTER) {
    for (int i = 0; i < sd->num_closure; i++) {
      const ccl_private ShaderClosure *sc = &sd->closure[i];

      if (CLOSURE_IS_VOLUME(sc->type)) {
        coeff->sigma_s += sc->weight;
      }
    }
  }

  return true;
}

/* -------------------------------------------------------------------- */
/** \name Hierarchical DDA for ray tracing the volume octree
 *
 * Following "Efficient Sparse Voxel Octrees" by Samuli Laine and Tero Karras,
 * and the implementation in https://dubiousconst282.github.io/2024/10/03/voxel-ray-tracing/
 *
 * The ray segment is transformed into octree space [1, 2), with `ray->D` pointing all negative
 * directions. At each ray tracing step, we intersect the backface of the current active leaf node
 * to find `t.max`, then store a point `current_P` which lies in the adjacent leaf node. The next
 * leaf node is found by checking the higher bits of `current_P`.
 *
 * The paper suggests to keep a stack of parent nodes, in practice such a stack (even when the size
 * is just 8) slows down performance on GPU. Instead we store the parent index in the leaf node
 * directly, since there is sufficient space due to alignment.
 *
 * \{ */

struct OctreeTracing {
  /* Current active leaf node. */
  ccl_global const KernelOctreeNode *node = nullptr;

  /* Current active ray segment, typically spans from the front face to the back face of the
   * current leaf node. */
  Interval<float> t;

  /* Ray origin in octree coordinate space. */
  packed_float3 ray_P;

  /* Ray direction in octree coordinate space. */
  packed_float3 ray_D;

  /* Current active position in octree coordinate space. */
  uint3 current_P;

  /* Object and shader which the octree represents. */
  VolumeStack entry = {OBJECT_NONE, SHADER_NONE};

  /* Scale of the current active leaf node, relative to the smallest possible size representable by
   * float. Initialize to the number of float mantissa bits. */
  uint8_t scale = MANTISSA_BITS;
  uint8_t next_scale;
  /* Mark the dimension (x,y,z) to negate the ray so that we find the correct octant. */
  uint8_t octant_mask;

  /* Whether multiple volumes overlap in the ray segment. */
  bool no_overlap = false;

  /* Maximum and minimum of the densities in the current segment. */
  Extrema<float> sigma = 0.0f;

  ccl_device_inline_method OctreeTracing(const float tmin)
  {
    /* Initialize t.max to FLT_MAX so that any intersection with the node face is smaller. */
    t = {tmin, FLT_MAX};
  }

  enum Dimension { DIM_X = 1U << 0U, DIM_Y = 1U << 1U, DIM_Z = 1U << 2U };

  /* Given ray origin `P` and direction `D` in object space, convert them into octree space
   * [1.0, 2.0).
   * Returns false if ray is leaving the octree or octree has degenerate shape. */
  ccl_device_inline_method bool to_octree_space(ccl_private const float3 &P,
                                                ccl_private const float3 &D,
                                                const float3 scale,
                                                const float3 translation)
  {
    if (!isfinite_safe(scale)) {
      /* Octree with a degenerate shape. */
      return false;
    }

    /* Starting point of octree tracing. */
    float3 local_P = (P + D * t.min) * scale + translation;
    ray_D = D * scale;

    /* Select octant mask to mirror the coordinate system so that ray direction is negative along
     * each axis, and adjust `local_P` accordingly. */
    const auto positive = ray_D > 0.0f;
    octant_mask = (!!positive.x * DIM_X) | (!!positive.y * DIM_Y) | (!!positive.z * DIM_Z);
    local_P = select(positive, 3.0f - local_P, local_P);

    /* Clamp to the largest floating-point number smaller than 2.0f, for numerical stability. */
    local_P = min(local_P, make_float3(1.9999999f));
    current_P = float3_as_uint3(local_P);

    ray_D = -fabs(ray_D);

    /* Ray origin. */
    ray_P = local_P - ray_D * t.min;

    /* Returns false if point lies outside of the octree and the ray is leaving the octree. */
    return all(local_P > 1.0f);
  }

  /* Find the bounding box min of the node that `current_P` lies in within the current scale. */
  ccl_device_inline_method float3 floor_pos() const
  {
    /* Erase bits lower than scale. */
    const uint mask = ~0u << scale;
    return make_float3(__uint_as_float(current_P.x & mask),
                       __uint_as_float(current_P.y & mask),
                       __uint_as_float(current_P.z & mask));
  }

  /* Find arbitrary position inside the next node.
   * We use the end of the current segment offsetted by half of the minimal node size in the normal
   * direction of the last face intersection. */
  ccl_device_inline_method void find_next_pos(const float3 bbox_min,
                                              const float3 t,
                                              const float tmax)
  {
    constexpr float half_size = 1.0f / (2 << VOLUME_OCTREE_MAX_DEPTH);
    const uint3 next_P = float3_as_uint3(
        select(t == tmax, bbox_min - half_size, ray_D * tmax + ray_P));

    /* Find the nearest common ancestor of two positions by checking the shared higher bits. */
    const uint diff = (current_P.x ^ next_P.x) | (current_P.y ^ next_P.y) |
                      (current_P.z ^ next_P.z);

    current_P = next_P;
    next_scale = 32u - count_leading_zeros(diff);
  }

  /* See `ray_aabb_intersect()`. We only need to intersect the 3 back sides because the ray
   * direction is all negative. */
  ccl_device_inline_method float ray_voxel_intersect(const float ray_tmax)
  {
    const float3 bbox_min = floor_pos();

    /* Distances to the three surfaces. */
    float3 intersect_t = (bbox_min - ray_P) / ray_D;

    /* Select the smallest element that is larger than `t.min`, to avoid self intersection. */
    intersect_t = select(intersect_t > t.min, intersect_t, make_float3(FLT_MAX));

    /* The first intersection is given by the smallest t. */
    const float tmax = reduce_min(intersect_t);

    find_next_pos(bbox_min, intersect_t, tmax);

    return fminf(tmax, ray_tmax);
  }

  /* Returns the octant of `current_P` in the node at given scale. */
  ccl_device_inline_method int get_octant() const
  {
    const uint8_t x = (current_P.x >> scale) & 1u;
    const uint8_t y = ((current_P.y >> scale) & 1u) << 1u;
    const uint8_t z = ((current_P.z >> scale) & 1u) << 2u;
    return (x | y | z) ^ octant_mask;
  }
};

/* Check if an octree node is leaf node. */
ccl_device_inline bool volume_node_is_leaf(const ccl_global KernelOctreeNode *knode)
{
  return knode->first_child == -1;
}

/* Find the leaf node of the current position, and replace `octree.node` with that node. */
ccl_device void volume_voxel_get(KernelGlobals kg, ccl_private OctreeTracing &octree)
{
  while (!volume_node_is_leaf(octree.node)) {
    octree.scale -= 1;
    const int child_index = octree.node->first_child + octree.get_octant();
    octree.node = &kernel_data_fetch(volume_tree_nodes, child_index);
  }
}

/* If there exists a Light Path Node, it could affect the density evaluation at runtime.
 * Randomly sample a few points on the ray to estimate the extrema. */
template<const bool shadow, typename IntegratorGenericState>
ccl_device_noinline Extrema<float> volume_estimate_extrema(KernelGlobals kg,
                                                           const ccl_private Ray *ccl_restrict ray,
                                                           ccl_private ShaderData *ccl_restrict sd,
                                                           const IntegratorGenericState state,
                                                           const ccl_private RNGState *rng_state,
                                                           const PathRayVisibility path_visibility,
                                                           const uint32_t path_flag,
/* Work around apparent HIP compiler bug. */
#  ifdef __KERNEL_HIP__
                                                           const ccl_private OctreeTracing &octree
#  else
                                                           const Interval<float> t,
                                                           const VolumeStack entry
#  endif
)
{
#  ifdef __KERNEL_HIP__
  const ccl_private Interval<float> &t = octree.t;
  const ccl_private VolumeStack &entry = octree.entry;
#  endif
  const bool homogeneous = volume_is_homogeneous(kg, entry);
  const int samples = homogeneous ? 1 : 4;
  const float shade_offset = homogeneous ?
                                 0.5f :
                                 path_state_rng_2D(kg, rng_state, PRNG_VOLUME_SHADE_OFFSET).y;
  const float step_size = t.length() / float(samples);

  /* Do not allocate closures. */
  sd->num_closure_left = 0;

  Extrema<float> extrema = {FLT_MAX, -FLT_MAX};
  for (int i = 0; i < samples; i++) {
    const float shade_t = t.min + (shade_offset + i) * step_size;
    sd->P = ray->P + ray->D * shade_t;

    sd->closure_transparent_extinction = zero_float3();
    sd->closure_emission_background = zero_float3();

    volume_shader_eval_entry<shadow, KERNEL_FEATURE_NODE_MASK_VOLUME>(
        kg, state, sd, entry, path_visibility, path_flag);

    const float sigma = reduce_max(sd->closure_transparent_extinction);
    const float emission = reduce_max(sd->closure_emission_background);

    extrema = merge(extrema, fmaxf(sigma, emission));
  }

  if (!homogeneous) {
    /* Slightly increase the majorant in case the estimation is not accurate. */
    extrema.max = fmaxf(0.5f, extrema.max * 1.5f);
  }

  return extrema;
}

/* Given an octree node, compute it's extrema.
 * In most common cases, the extrema are already stored in the node, but if the shader contains
 * a light path node, we need to evaluate the densities on the fly. */
template<const bool shadow, typename IntegratorGenericState>
ccl_device_inline Extrema<float> volume_object_get_extrema(KernelGlobals kg,
                                                           const ccl_private Ray *ccl_restrict ray,
                                                           ccl_private ShaderData *ccl_restrict sd,
                                                           const IntegratorGenericState state,
                                                           const ccl_private OctreeTracing &octree,
                                                           const ccl_private RNGState *rng_state,
                                                           const PathRayVisibility path_visibility,
                                                           const uint32_t path_flag)
{
  const int shader_flag = kernel_data_fetch(shaders, (octree.entry.shader & SHADER_MASK)).flags;
  if ((path_visibility & PATH_RAY_VISIBILITY_CAMERA) || !(shader_flag & SD_HAS_LIGHT_PATH_NODE)) {
    /* Use the baked volume density extrema. */
    return octree.node->sigma * object_volume_density(kg, octree.entry.object);
  }

#  ifdef __KERNEL_HIP__
  return volume_estimate_extrema<shadow>(
      kg, ray, sd, state, rng_state, path_visibility, path_flag, octree);
#  else
  return volume_estimate_extrema<shadow>(
      kg, ray, sd, state, rng_state, path_visibility, path_flag, octree.t, octree.entry);
#  endif
}

/* Find the octree root node in the kernel array that corresponds to the volume stack entry. */
ccl_device_inline const ccl_global KernelOctreeRoot *volume_find_octree_root(
    KernelGlobals kg, const VolumeStack entry)
{
  int root = kernel_data_fetch(volume_tree_root_ids, entry.object);
  const ccl_global KernelOctreeRoot *kroot = &kernel_data_fetch(volume_tree_roots, root);
  while ((entry.shader & SHADER_MASK) != kroot->shader) {
    /* If one object has multiple shaders, we store the index of the last shader, and search
     * backwards for the octree with the corresponding shader. */
    kroot = &kernel_data_fetch(volume_tree_roots, --root);
  }
  return kroot;
}

/* Find the current active ray segment.
 * We might have multiple overlapping octrees, so find the smallest `tmax` of all and store the
 * information of that octree in `OctreeTracing`.
 * Meanwhile, accumulate the density of all the leaf nodes that overlap with the active segment. */
template<const bool shadow, typename IntegratorGenericState>
ccl_device bool volume_octree_setup(KernelGlobals kg,
                                    const ccl_private Ray *ccl_restrict ray,
                                    ccl_private ShaderData *ccl_restrict sd,
                                    const IntegratorGenericState state,
                                    const ccl_private RNGState *rng_state,
                                    const PathRayVisibility path_visibility,
                                    const uint32_t path_flag,
                                    ccl_private OctreeTracing &global)
{
  if (global.no_overlap) {
    /* If the current active octree is already set up. */
    return !global.t.is_empty();
  }

  const VolumeStack skip = global.entry;

  int i = 0;
  for (;; i++) {
    /* Loop through all the object in the volume stack and find their octrees. */
    const VolumeStack entry = volume_stack_read<shadow>(state, i);

    if (entry.shader == SHADER_NONE) {
      break;
    }

    if (entry.object == skip.object && entry.shader == skip.shader) {
      continue;
    }

    const ccl_global KernelOctreeRoot *kroot = volume_find_octree_root(kg, entry);

    OctreeTracing local(global.t.min);
    local.node = &kernel_data_fetch(volume_tree_nodes, kroot->id);
    local.entry = entry;

    /* Convert to object space. */
    float3 local_P = ray->P, local_D = ray->D;
    if (!(kernel_data_fetch(object_flag, entry.object) & SD_OBJECT_TRANSFORM_APPLIED)) {
      const Transform itfm = object_fetch_transform(kg, entry.object, OBJECT_INVERSE_TRANSFORM);
      local_P = transform_point(&itfm, ray->P);
      local_D = transform_direction(&itfm, ray->D);
    }

    /* Convert to octree space. */
    if (local.to_octree_space(local_P, local_D, kroot->scale, kroot->translation)) {
      volume_voxel_get(kg, local);
      local.t.max = local.ray_voxel_intersect(ray->tmax);
    }
    else {
      /* Current ray segment lies outside of the octree, usually happens with implicit volume, i.e.
       * everything behind a surface is considered as volume. */
      local.t.max = ray->tmax;
    }

    global.sigma += volume_object_get_extrema<shadow>(
        kg, ray, sd, state, local, rng_state, path_visibility, path_flag);
    if (local.t.max <= global.t.max) {
      /* Replace the current active octree with the one that has the smallest `tmax`. */
      local.sigma = global.sigma;
      global = local;
    }
  }

  if (i == 1) {
    global.no_overlap = true;
  }

  return global.node && !global.t.is_empty();
}

/* Advance to the next adjacent leaf node and update the active interval. */
template<const bool shadow, typename IntegratorGenericState>
ccl_device_inline bool volume_octree_advance(KernelGlobals kg,
                                             const ccl_private Ray *ccl_restrict ray,
                                             ccl_private ShaderData *ccl_restrict sd,
                                             const IntegratorGenericState state,
                                             const ccl_private RNGState *rng_state,
                                             const PathRayVisibility path_visibility,
                                             const uint32_t path_flag,
                                             ccl_private OctreeTracing &octree)
{
  if (octree.t.max >= ray->tmax) {
    /* Reached the last segment. */
    return false;
  }

  if (octree.next_scale > MANTISSA_BITS) {
    if (fabsf(octree.t.max - ray->tmax) <= OVERLAP_EXP) {
      /* This could happen due to numerical issues, when the bounding box overlaps with a
       * primitive, but different intersections are registered for octree and ray intersection. */
      return false;
    }

    /* Outside of the root node, continue tracing using the extrema of the root node. */
    octree.t = {octree.t.max, ray->tmax};
    octree.node = &kernel_data_fetch(volume_tree_nodes,
                                     volume_find_octree_root(kg, octree.entry)->id);
  }
  else {
    kernel_assert(octree.next_scale > octree.scale);

    /* Fetch the common ancestor of the current and the next leaf nodes. */
    for (; octree.scale < octree.next_scale; octree.scale++) {
      kernel_assert(octree.node->parent != -1);
      octree.node = &kernel_data_fetch(volume_tree_nodes, octree.node->parent);
    }

    /* Find the current active leaf node. */
    volume_voxel_get(kg, octree);

    /* Advance to the next segment. */
    octree.t.min = octree.t.max;
    octree.t.max = octree.ray_voxel_intersect(ray->tmax);
  }

  octree.sigma = volume_object_get_extrema<shadow>(
      kg, ray, sd, state, octree, rng_state, path_visibility, path_flag);
  return volume_octree_setup<shadow>(
      kg, ray, sd, state, rng_state, path_visibility, path_flag, octree);
}

/** \} */

/* Volume Shadows
 *
 * These functions are used to attenuate shadow rays to lights. Both absorption
 * and scattering will block light, represented by the extinction coefficient. */

/* Advance until the majorant optical depth is at least one, or we have reached the end of the
 * volume. Because telescoping has to take at least one sample per segment, having a larger
 * segment helps to take less samples. */
ccl_device_inline bool volume_octree_advance_shadow(KernelGlobals kg,
                                                    const ccl_private Ray *ccl_restrict ray,
                                                    ccl_private ShaderData *ccl_restrict sd,
                                                    const IntegratorShadowState state,
                                                    ccl_private RNGState *rng_state,
                                                    const PathRayVisibility path_visibility,
                                                    const uint32_t path_flag,
                                                    ccl_private OctreeTracing &octree)
{
  /* Advance random number offset. */
  rng_state->rng_offset += PRNG_BOUNCE_NUM;

  Extrema<float> sigma = octree.t.is_empty() ? Extrema<float>(FLT_MAX, -FLT_MAX) : octree.sigma;
  const float tmin = octree.t.min;

  while (octree.t.is_empty() || sigma.range() * octree.t.length() < 1.0f) {
    if (!volume_octree_advance<true>(
            kg, ray, sd, state, rng_state, path_visibility, path_flag, octree))
    {
      return !octree.t.is_empty();
    }

    octree.sigma = sigma = merge(sigma, octree.sigma);
    octree.t.min = tmin;
  }

  return true;
}

/**
 * Compute transmittance along the ray using
 * "Unbiased and consistent rendering using biased estimators" by Misso et. al,
 * https://cs.dartmouth.edu/~wjarosz/publications/misso22unbiased.html
 *
 * The telescoping sum is
 *         T = T_k + \sum_{j=k}^\infty(T_{j+1} - T_{j})
 * where T_k is a biased estimation of the transmittance T by taking k samples,
 * and (T_{j+1} - T_{j}) is the debiasing term.
 * We decide the order k based on the optical thickness, and randomly pick a debiasing term of
 * order j to evaluate.
 * In the practice we take the powers of 2 to reuse samples for all orders.
 *
 * \param sigma_c: the difference between the density majorant and minorant
 * \param t: the ray segment between which we compute the transmittance
 */
template<const bool shadow, typename IntegratorGenericState>
ccl_device Spectrum volume_transmittance(KernelGlobals kg,
                                         IntegratorGenericState state,
                                         const ccl_private Ray *ray,
                                         ccl_private ShaderData *ccl_restrict sd,
                                         const float sigma_c,
                                         const Interval<float> t,
                                         const ccl_private RNGState *rng_state,
                                         const PathRayVisibility path_visibility,
                                         const uint32_t path_flag)
{
  constexpr float r = 0.9f;
  const float ray_length = t.length();

  /* Expected number of steps with residual ratio tracking. */
  const float expected_steps = sigma_c * ray_length;
  /* Number of samples for the biased estimator. */
  const int k = clamp(int(roundf(expected_steps)), 1, VOLUME_MAX_STEPS);

  /* Sample the evaluation order of the debiasing term. */
  /* Use the same random number for all pixels to sync the workload on GPU. */
  /* TODO(weizhen): need to check if such correlation introduces artefacts. */
  const float rand = path_rng_1D(
      kg, 0, rng_state->sample, rng_state->rng_offset + PRNG_VOLUME_EXPANSION_ORDER);
  /* A hard cut-off to prevent taking too many samples on the GPU. The probability of going beyond
   * this order is 1e-5f. */
  constexpr int cut_off = 4;
  float pmf;
  /* Number of independent estimators of T_k. */
  const int N = (sigma_c == 0.0f) ?
                    1 :
                    power_of_2(sample_geometric_distribution(rand, r, pmf, cut_off));

  /* Total number of density evaluations. */
  const int samples = N * k;

  const float shade_offset = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_SHADE_OFFSET);
  const float step_size = ray_length / float(samples);

  if (N == 1) {
    /* Only compute the biased estimator. */
    Spectrum tau_k = zero_spectrum();
    for (int i = 0; i < k; i++) {
      const float shade_t = min(t.max, t.min + (shade_offset + i) * step_size);
      sd->P = ray->P + ray->D * shade_t;
      tau_k += volume_shader_eval_extinction<shadow>(kg, state, sd, path_visibility, path_flag);
    }
    /* OneAPI has some problem with exp(-0 * FLT_MAX). */
    return is_zero(tau_k) ? one_spectrum() : exp(-tau_k * step_size);
  }

  /* Estimations of optical thickness. */
  Spectrum tau_j[2];
  tau_j[0] = zero_spectrum();
  tau_j[1] = zero_spectrum();
  Spectrum tau_j_1 = zero_spectrum();

  Spectrum T_k = zero_spectrum();
  for (int n = 0; n < N; n++) {
    Spectrum tau_k = zero_spectrum();
    for (int i = 0; i < k; i++) {
      const int step = i * N + n;
      const float shade_t = min(t.max, t.min + (shade_offset + step) * step_size);
      sd->P = ray->P + ray->D * shade_t;

      const Spectrum tau = step_size * volume_shader_eval_extinction<shadow>(
                                           kg, state, sd, path_visibility, path_flag);

      tau_k += tau * N;
      tau_j[step % 2] += tau * 2.0f;
      tau_j_1 += tau;
    }
    T_k += exp(-tau_k);
  }

  const Spectrum T_j_1 = exp(-tau_j_1);

  /* Eq (16). This is the secondary estimator which averages a few independent estimations. */
  T_k /= float(N);
  const Spectrum T_j = 0.5f * (exp(-tau_j[0]) + exp(-tau_j[1]));

  /* Eq (14), single-term primary estimator. */
  return T_k + (T_j_1 - T_j) / pmf;
}

/* Compute the volumetric transmittance of the segment [ray->tmin, ray->tmax],
 * used for the shadow ray throughput. */
ccl_device void volume_shadow_null_scattering(KernelGlobals kg,
                                              IntegratorShadowState state,
                                              ccl_private Ray *ccl_restrict ray,
                                              ccl_private ShaderData *ccl_restrict sd,
                                              ccl_private Spectrum *ccl_restrict throughput)
{
  /* Load random number state. */
  RNGState rng_state;
  shadow_path_state_rng_load(state, &rng_state);

  /* For stochastic texture sampling. */
  sd->lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0xd9111870);

  path_state_rng_scramble(&rng_state, 0x8647ace4);

  OctreeTracing octree(ray->tmin);
  const PathRayVisibility path_visibility = PATH_RAY_VISIBILITY_SHADOW;
  const uint32_t path_flag = PATH_RAY_FLAG_NONE;
  if (!volume_octree_setup<true>(
          kg, ray, sd, state, &rng_state, path_visibility, path_flag, octree))
  {
    return;
  }

  while (volume_octree_advance_shadow(
      kg, ray, sd, state, &rng_state, path_visibility, path_flag, octree))
  {
    const float sigma = octree.sigma.range();
    *throughput *= volume_transmittance<true>(
        kg, state, ray, sd, sigma, octree.t, &rng_state, path_visibility, path_flag);

    if (reduce_max(fabs(*throughput)) < VOLUME_THROUGHPUT_EPSILON) {
      return;
    }
    octree.t.min = octree.t.max;
  }
}

/* Equi-angular sampling as in:
 * "Importance Sampling Techniques for Path Tracing in Participating Media" */

/* Below this pdf we ignore samples, as they tend to lead to very long distances.
 * This can cause performance issues with BVH traversal in OptiX, leading it to
 * traverse many nodes. Since these contribute very little to the image, just ignore
 * those samples. */
#  define VOLUME_SAMPLE_PDF_CUTOFF 1e-8f

ccl_device float volume_equiangular_sample(const ccl_private Ray *ccl_restrict ray,
                                           const ccl_private EquiangularCoefficients &coeffs,
                                           const float xi,
                                           ccl_private float *pdf)
{
  const float delta = dot((coeffs.P - ray->P), ray->D);
  const float D = len(coeffs.P - ray->P - ray->D * delta);
  if (UNLIKELY(D == 0.0f)) {
    *pdf = 0.0f;
    return 0.0f;
  }
  const float tmin = coeffs.t_range.min;
  const float tmax = coeffs.t_range.max;

  const float theta_a = atan2f(tmin - delta, D);
  const float theta_b = atan2f(tmax - delta, D);
  const float theta_d = theta_b - theta_a;
  if (UNLIKELY(theta_d < 1e-6f)) {
    /* Use uniform sampling when `theta_d` is too small. */
    *pdf = safe_divide(1.0f, tmax - tmin);
    return mix(tmin, tmax, xi);
  }

  const float t_ = D * tanf((xi * theta_b) + (1 - xi) * theta_a);
  *pdf = D / (theta_d * (D * D + t_ * t_));

  return clamp(delta + t_, tmin, tmax); /* clamp is only for float precision errors */
}

ccl_device float volume_equiangular_pdf(const ccl_private Ray *ccl_restrict ray,
                                        const ccl_private EquiangularCoefficients &coeffs,
                                        const float sample_t)
{
  const float delta = dot((coeffs.P - ray->P), ray->D);
  const float D = len(coeffs.P - ray->P - ray->D * delta);
  if (UNLIKELY(D == 0.0f)) {
    return 0.0f;
  }

  const float tmin = coeffs.t_range.min;
  const float tmax = coeffs.t_range.max;

  const float theta_a = atan2f(tmin - delta, D);
  const float theta_b = atan2f(tmax - delta, D);
  const float theta_d = theta_b - theta_a;
  if (UNLIKELY(theta_d < 1e-6f)) {
    return safe_divide(1.0f, tmax - tmin);
  }

  const float t_ = sample_t - delta;
  const float pdf = D / (theta_d * (D * D + t_ * t_));

  return pdf;
}

/* Compute ray segment directly visible to the sampled light. */
ccl_device_inline bool volume_valid_direct_ray_segment(KernelGlobals kg,
                                                       const float3 ray_P,
                                                       const float3 ray_D,
                                                       ccl_private Interval<float> *t_range,
                                                       const ccl_private LightSample *ls)
{
  if (ls->type == LIGHT_SPOT) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
    return spot_light_valid_ray_segment(kg, klight, ray_P, ray_D, t_range);
  }
  if (ls->type == LIGHT_AREA) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(lights, ls->prim);
    return area_light_valid_ray_segment(&klight->area, ray_P - klight->co, ray_D, t_range);
  }
  if (ls->type == LIGHT_TRIANGLE) {
    return triangle_light_valid_ray_segment(kg, ray_P - ls->P, ray_D, t_range, ls);
  }

  /* Point light or distant light, the whole range of the ray is visible. */
  kernel_assert(ls->type == LIGHT_POINT || ls->t == FLT_MAX);
  return !t_range->is_empty();
}

/* Emission */

ccl_device Spectrum volume_emission_integrate(ccl_private VolumeShaderCoefficients *coeff,
                                              const int closure_flag,
                                              const float t)
{
  /* integral E * exp(-sigma_t * t) from 0 to t = E * (1 - exp(-sigma_t * t))/sigma_t
   * this goes to E * t as sigma_t goes to zero. */
  Spectrum emission = coeff->emission;

  if (closure_flag & SD_EXTINCTION) {
    const Spectrum optical_depth = coeff->sigma_t * t;
    emission *= select(optical_depth > 1e-5f,
                       (1.0f - exp(-optical_depth)) / coeff->sigma_t,
                       /* Second order Taylor expansion to avoid precision issue. */
                       t * (1.0f - 0.5f * optical_depth));
  }
  else {
    emission *= t;
  }

  return emission;
}

/* Volume Integration */

namespace {
struct VolumeIntegrateState {
#  if __DEEP_BOUNDED_POLICY__ > 0
  DeepBoundedBridge bounded;
#  endif
  /* Random number. */
  float rscatter;

  /* Method used for sampling direct scatter position. */
  VolumeSampleMethod direct_sample_method;
  /* Probability of sampling the scatter position using null scattering. */
  float distance_pdf;
  /* Probability of sampling the scatter position using equiangular sampling. */
  float equiangular_pdf;
  /* Majorant density at the equiangular scatter position. Used to compute the pdf. */
  float sigma_max;

  /* Ratio tracking estimator of the volume transmittance, with MIS applied. */
  float transmittance;
  /* Current sample position. */
  float t;
  /* Majorant optical depth until now. */
  float optical_depth;
  /* Steps taken while tracking. Should not exceed `VOLUME_MAX_STEPS`. */
  uint16_t step;
  /* Multiple importance sampling. */
  bool use_mis;

  /* Volume scattering probability guiding. */
  bool vspg;
  /* The guided probability that the ray is scattered in the volume. `P_vol` in the paper. */
  float scatter_prob;
  /* Minimal scale of majorant for achieving the desired scatter probability. */
  float majorant_scale;
  /* Scale to apply after direct throughput due to Russian Roulette. */
  float direct_rr_scale;

  /* Extra fields for path guiding and denoising. */
  PackedSpectrum emission;
  /* Private token for this exact homogeneous source and its later native film call. */
  uint32_t deep_native_segment;
  uint32_t deep_expression_head;
#  ifdef __DENOISING_FEATURES__
  PackedSpectrum albedo;
#  endif

  /* The distance between the current and the last sample position. */
  float dt;
  /* `dt` at equiangular scatter position. Used to compute the pdf. */
  float sample_dt;
};
}  // namespace

/* Accumulate transmittance for equiangular distance sampling without MIS. Using telescoping to
 * reduce noise. */
ccl_device_inline void volume_equiangular_transmittance(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    const ccl_private Extrema<float> &sigma,
    const ccl_private Interval<float> &interval,
    ccl_private ShaderData *ccl_restrict sd,
    const ccl_private RNGState *rng_state,
    const ccl_private VolumeIntegrateState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result)
{
  if (vstate.direct_sample_method != VOLUME_SAMPLE_EQUIANGULAR || vstate.use_mis ||
      result.direct_scatter)
  {
    return;
  }

  Interval<float> t;
  if (interval.contains(result.direct_t)) {
    /* Compute transmittance until the direct scatter position. */
    t = {interval.min, result.direct_t};
    result.direct_scatter = true;
  }
  else {
    /* Compute transmittance of the whole segment. */
    t = interval;
  }

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  result.direct_throughput *= volume_transmittance<false>(
      kg, state, ray, sd, sigma.range(), t, rng_state, path_visibility, path_flag);
}

/* Sample the next candidate indirect scatter position following exponential distribution,
 * and compute the direct throughput for equiangular sampling if using MIS.
 * Returns true if should continue advancing. */
ccl_device_inline bool volume_indirect_scatter_advance(const ccl_private OctreeTracing &octree,
                                                       const bool equiangular,
                                                       ccl_private float &residual_optical_depth,
                                                       ccl_private VolumeIntegrateState &vstate,
                                                       ccl_private VolumeIntegrateResult &result)
{
  const float sigma_max = octree.sigma.max * vstate.majorant_scale;
  residual_optical_depth = (octree.t.max - vstate.t) * sigma_max;
  if (sigma_max == 0.0f) {
    return true;
  }

  vstate.dt = sample_exponential_distribution(vstate.rscatter, sigma_max);
  vstate.t += vstate.dt;

  const bool segment_has_equiangular = equiangular && octree.t.contains(result.direct_t);
  if (segment_has_equiangular && vstate.t > result.direct_t && !result.direct_scatter) {
    /* Stepped beyond the equiangular scatter position, compute direct throughput. */
    result.direct_scatter = true;
    result.direct_throughput = result.indirect_throughput * vstate.transmittance *
                               vstate.direct_rr_scale;
    vstate.sample_dt = result.direct_t - vstate.t + vstate.dt;
    vstate.distance_pdf = vstate.transmittance * sigma_max;
    vstate.sigma_max = sigma_max;
  }

  /* Sampled a position outside the current voxel. */
  return vstate.t > octree.t.max;
}

/** Advance to the next candidate indirect scatter position, and compute the direct throughput. */
ccl_device_inline bool volume_integrate_advance(KernelGlobals kg,
                                                const ccl_private Ray *ccl_restrict ray,
                                                ccl_private ShaderData *ccl_restrict sd,
                                                const IntegratorState state,
                                                ccl_private RNGState *rng_state,
                                                const PathRayVisibility path_visibility,
                                                const uint32_t path_flag,
                                                ccl_private OctreeTracing &octree,
                                                ccl_private VolumeIntegrateState &vstate,
                                                ccl_private VolumeIntegrateResult &result)
{
  if (vstate.step++ > VOLUME_MAX_STEPS) {
    /* Exceeds maximal steps. */
    return false;
  }

  float residual_optical_depth;
  vstate.rscatter = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_SCATTER_DISTANCE);
  const bool equiangular = (vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) &&
                           vstate.use_mis;

  while (
      volume_indirect_scatter_advance(octree, equiangular, residual_optical_depth, vstate, result))
  {
    /* Advance to the next voxel if the sampled distance is beyond the current voxel. */
    if (!volume_octree_advance<false>(
            kg, ray, sd, state, rng_state, path_visibility, path_flag, octree))
    {
      return false;
    }

    vstate.optical_depth += octree.sigma.max * octree.t.length();
    vstate.t = octree.t.min;
    volume_equiangular_transmittance(
        kg, state, ray, octree.sigma, octree.t, sd, rng_state, vstate, result);

    /* Scale the random number by the residual depth for reusing. */
    vstate.rscatter = saturatef(1.0f - (1.0f - vstate.rscatter) * expf(residual_optical_depth));
  }

  /* Advance random number offset. */
  rng_state->rng_offset += PRNG_BOUNCE_NUM;

  return true;
}

/* -------------------------------------------------------------------- */
/** \name Volume Scattering Probability Guiding
 *
 * Following https://kehanxuuu.github.io/vspg-website/ by Kehan Xu et. al.
 *
 * Instead of stopping at the first real scatter event, we step through the entire ray to gather
 * candidate scatter positions, and guide the probability of scattering inside a volume or
 * transmitting through the volume by the contribution of both types of events.
 *
 * We only guide primary rays, secondary rays could be supported in the OpenPGL in the future.
 * \{ */

/* Candidate scatter position for VSPG. */
struct VolumeSampleCandidate {
  PackedSpectrum emission;
  float t;
  PackedSpectrum throughput;
  float distance_pdf;
#  ifdef __DENOISING_FEATURES__
  PackedSpectrum albedo;
#  endif
  /* Remember the random number so that we sample the sample point for stochastic evaluation. */
  uint lcg_state;
  uint32_t deep_expression_head = DEEP_NATIVE_INVALID_INDEX;
};

/* Sample reservoir for VSPG. */
struct VolumeSampleReservoir {
  float total_weight = 0.0f;
  float rand;
  VolumeSampleCandidate candidate;

  ccl_device_inline_method VolumeSampleReservoir(const float rand_) : rand(rand_) {}

  /* Stream the candidate samples through the reservoir. */
  ccl_device_inline_method void add_sample(const float weight,
                                           const VolumeSampleCandidate new_candidate,
                                           const uint32_t emission_head)
  {
    if (!(weight > 0.0f)) {
      return;
    }

    total_weight += weight;
    const float thresh = weight / total_weight;

    if ((rand <= thresh) || (total_weight == weight)) {
      /* Explicitly select the first candidate in case of numerical issues. */
      candidate = new_candidate;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
      /* Heads name immutable append-only prefixes. The actual native selection
       * already decides which prefix survives; no Snapshot/Select records are needed. */
      candidate.deep_expression_head = emission_head;
#  endif
      rand /= thresh;
    }
    else {
      rand = (rand - thresh) / (1.0f - thresh);
    }

    /* Ensure the `rand` is always within 0..1 range, which could be violated above when
     * `-ffast-math` is used. */
    rand = saturatef(rand);
  }

  ccl_device_inline_method bool is_empty() const
  {
    return total_weight == 0.0f;
  }
};

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
struct DeepNativeVolumeShaderState {
  float3 P;
  int flag, object, prim, shader, type;
  uint object_flag, lcg_state;
};

ccl_device_inline DeepNativeVolumeShaderState
deep_native_volume_shader_state(const ccl_private ShaderData *sd)
{
  return {
      sd->P, sd->flag, sd->object, sd->prim, sd->shader, sd->type, sd->object_flag, sd->lcg_state};
}
ccl_device_inline void deep_native_restore_volume_shader(ccl_private ShaderData *sd,
                                                         const DeepNativeVolumeShaderState saved)
{
  sd->P = saved.P;
  sd->flag = saved.flag;
  sd->object = saved.object;
  sd->prim = saved.prim;
  sd->shader = saved.shader;
  sd->type = saved.type;
  sd->object_flag = saved.object_flag;
  sd->lcg_state = saved.lcg_state;
}

struct DeepNativeNullCheckpoint {
  uint active = 0, cache_miss, kind, sample, bounce;
  uint64_t domain;
  Ray ray;
  DeepNativeVolumeShaderState shader;
  EquiangularCoefficients equiangular;
  LightSample light;
  VolumeIntegrateState state;
  VolumeIntegrateResult result;
  RNGState rng;
  OctreeTracing octree;
  VolumeSampleReservoir reservoir;
  uint phase;
};
static_assert(sizeof(DeepNativeNullCheckpoint) <= DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES,
              "Native null checkpoint exceeds its charged slot");

ccl_device_inline ccl_global DeepNativeNullCheckpoint *deep_native_null_checkpoint(
    ConstIntegratorState state)
{
  if (!kernel_integrator_state.deep_native_volume_checkpoints ||
      !deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
    return nullptr;
  const uint camera = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  if (camera >= kernel_integrator_state.deep_native_volume_checkpoint_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return nullptr;
  }
  return reinterpret_cast<ccl_global DeepNativeNullCheckpoint *>(
      static_cast<ccl_global char *>(kernel_integrator_state.deep_native_volume_checkpoints) +
      size_t(camera) * DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES);
}

ccl_device_inline bool deep_native_null_checkpoint_matches(
    ConstIntegratorState state,
    const ccl_private Ray *ray,
    const ccl_global DeepNativeNullCheckpoint *checkpoint)
{
  const Ray previous = checkpoint->ray;
  return checkpoint->kind == 2 &&
         checkpoint->domain == kernel_integrator_state.deep_native_domain_id &&
         checkpoint->sample == INTEGRATOR_STATE(state, path, sample) &&
         checkpoint->bounce == INTEGRATOR_STATE(state, path, bounce) && previous.P.x == ray->P.x &&
         previous.P.y == ray->P.y && previous.P.z == ray->P.z && previous.D.x == ray->D.x &&
         previous.D.y == ray->D.y && previous.D.z == ray->D.z && previous.tmin == ray->tmin &&
         previous.tmax == ray->tmax && previous.time == ray->time;
}

ccl_device_inline void deep_native_store_null_checkpoint(
    ConstIntegratorState state,
    const ccl_private Ray *ray,
    ccl_global DeepNativeNullCheckpoint *checkpoint,
    const DeepNativeVolumeShaderState shader,
    const ccl_private VolumeIntegrateState &vstate,
    const ccl_private VolumeIntegrateResult &result,
    const ccl_private RNGState &rng,
    const ccl_private OctreeTracing &octree,
    const ccl_private VolumeSampleReservoir &reservoir,
    const uint phase,
    const bool cache_miss)
{
  checkpoint->kind = 2;
  checkpoint->domain = kernel_integrator_state.deep_native_domain_id;
  checkpoint->sample = INTEGRATOR_STATE(state, path, sample);
  checkpoint->bounce = INTEGRATOR_STATE(state, path, bounce);
  checkpoint->ray.P = ray->P;
  checkpoint->ray.D = ray->D;
  checkpoint->ray.tmin = ray->tmin;
  checkpoint->ray.tmax = ray->tmax;
  checkpoint->ray.time = ray->time;
#    ifdef __RAY_DIFFERENTIALS__
  checkpoint->ray.dP = ray->dP;
  checkpoint->ray.dD = ray->dD;
#    endif
  checkpoint->ray.self = {PRIM_NONE, OBJECT_NONE, PRIM_NONE, OBJECT_NONE};
  checkpoint->shader = shader;
  checkpoint->state = vstate;
  checkpoint->result = result;
  checkpoint->rng = rng;
  checkpoint->octree = octree;
  checkpoint->reservoir = reservoir;
  if (reservoir.is_empty())
    checkpoint->reservoir.candidate = {};
  checkpoint->phase = phase;
  checkpoint->cache_miss = cache_miss;
  if (!checkpoint->active)
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_pending,
                                1);
  checkpoint->active = 1;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_yields,
                              1);
  if (cache_miss)
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_retries,
                                1);
}
#  endif

/* Estimate volume majorant optical depth `\sum\sigma_{max}t` along the ray, by accumulating the
 * result from previous samples in a render buffer. */
ccl_device_inline float volume_majorant_optical_depth(KernelGlobals kg,
                                                      const ccl_global float *buffer)
{
  kernel_assert(kernel_data.film.pass_volume_majorant != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_volume_majorant_sample_count != PASS_UNUSED);

  const ccl_global float *accumulated_optical_depth = buffer +
                                                      kernel_data.film.pass_volume_majorant;
  const ccl_global float *count = buffer + kernel_data.film.pass_volume_majorant_sample_count;

  /* Assume `FLT_MAX` when we have no information of the optical depth. */
  return (*count == 0.0f) ? FLT_MAX : *accumulated_optical_depth / *count;
}

/* Compute guided volume scatter probability and the majorant scale needed for achieving the
 * scatter probability, for heterogeneous volume. */
ccl_device_inline void volume_scatter_probability_heterogeneous(
    KernelGlobals kg,
    const IntegratorState state,
    ccl_global float *ccl_restrict render_buffer,
    ccl_private VolumeIntegrateState &vstate)
{
  if (!vstate.vspg) {
    return;
  }

  const ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  kernel_assert(kernel_data.film.pass_volume_scatter_denoised != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_volume_transmit_denoised != PASS_UNUSED);

  /* Contribution based criterion, see Eq. (15). */
  const float L_scattered = reduce_add(
      kernel_read_pass_rgbe(buffer + kernel_data.film.pass_volume_scatter_denoised));
  const float L_transmitted = reduce_add(
      kernel_read_pass_rgbe(buffer + kernel_data.film.pass_volume_transmit_denoised));
  const float L_volume = L_transmitted + L_scattered;

  /* Compute guided scattering probability. */
  if (L_volume == 0.0f) {
    /* Equal probability if no information gathered yet. */
    vstate.scatter_prob = 0.5f;
  }
  else {
    /* Exponential distribution has non-zero probability beyond the boundary, so the scatter
     * probability can never reach 1. Clamp to avoid scaling the majorant to infinity. */
    vstate.scatter_prob = fminf(L_scattered / L_volume, 0.9999f);
  }

  const float optical_depth = volume_majorant_optical_depth(kg, buffer);

  /* There is a non-zero probability of sampling no scatter events in the volume segment. In order
   * to reach the desired scattering probability, we might need to upscale the majorant and/or the
   * guiding scattering probability. See Eq (25,26). */
  vstate.majorant_scale = (optical_depth == 0.0f) ?
                              1.0f :
                              -fast_logf(1.0f - vstate.scatter_prob) / optical_depth;
  if (vstate.majorant_scale < 1.0f) {
    vstate.majorant_scale = 1.0f;
    vstate.scatter_prob = safe_divide(vstate.scatter_prob, 1.0f - fast_expf(-optical_depth));
  }
  else {
    vstate.scatter_prob = 1.0f;
  }
}

/* Final guiding decision on sampling scatter or transmit event. */
ccl_device_inline void volume_distance_sampling_finalize(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    ccl_private ShaderData *ccl_restrict sd,
    ccl_private VolumeIntegrateState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result,
    ccl_private VolumeSampleReservoir &reservoir)
{
  if (reservoir.is_empty()) {
    return;
  }

  const bool sample_distance = !(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_TERMINATE) &&
                               (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE);

  if (!vstate.vspg) {
    result.indirect_throughput = reservoir.candidate.throughput;
    vstate.emission = reservoir.candidate.emission;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (vstate.deep_expression_head != DEEP_NATIVE_INVALID_INDEX)
      vstate.deep_expression_head = reservoir.candidate.deep_expression_head;
#  endif
#  ifdef __DENOISING_FEATURES__
    vstate.albedo = reservoir.candidate.albedo;
#  endif
    result.indirect_t = reservoir.candidate.t;

    if (sample_distance) {
      /* If using distance sampling for direct light, just copy parameters of indirect light
       * since we scatter at the same point. */
      result.direct_scatter = true;
      result.direct_t = result.indirect_t;
      result.direct_throughput = result.indirect_throughput;
      if (vstate.use_mis) {
        vstate.distance_pdf = reservoir.candidate.distance_pdf;
      }
    }
    return;
  }

  const uint lcg_state = reservoir.candidate.lcg_state;

  if (sample_distance) {
    /* Always sample direct scatter, regardless of indirect scatter guiding decision. */
    result.direct_throughput = reservoir.candidate.throughput * reservoir.total_weight;
    vstate.distance_pdf = reservoir.candidate.distance_pdf;
  }

  /* We only guide scatter decisions, no need to apply on emission and albedo. */
  vstate.emission = mix(vstate.emission, reservoir.candidate.emission, reservoir.total_weight);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (vstate.deep_expression_head != DEEP_NATIVE_INVALID_INDEX)
    vstate.deep_expression_head = deep_native_expression_node(
        state,
        DEEP_NATIVE_EXPR_MIX,
        vstate.deep_expression_head,
        reservoir.candidate.deep_expression_head,
        0,
        make_spectrum(reservoir.total_weight));
#  endif
#  ifdef __DENOISING_FEATURES__
  vstate.albedo = mix(vstate.albedo, reservoir.candidate.albedo, reservoir.total_weight);
#  endif

  const float unguided_scatter_prob = reservoir.total_weight;
  float guided_scatter_prob;
  if (is_zero(result.indirect_throughput)) {
    /* Always sample scatter event if the contribution of transmitted event is zero. */
    guided_scatter_prob = 1.0f;
  }
  else {
    /* Defensive resampling. */
    const float alpha = 0.75f;
    reservoir.total_weight = mix(reservoir.total_weight, vstate.scatter_prob, alpha);
    guided_scatter_prob = reservoir.total_weight;

    /* Add transmitted candidate. */
    reservoir.add_sample(
        1.0f - guided_scatter_prob,
#  ifdef __DENOISING_FEATURES__
        { vstate.emission, reservoir.candidate.t, result.indirect_throughput, 0.0f, vstate.albedo }
#  else
        {vstate.emission, reservoir.candidate.t, result.indirect_throughput, 0.0f}
#  endif
        ,
        vstate.deep_expression_head);
  }

  const bool scatter = (reservoir.candidate.distance_pdf > 0.0f);
  const float scale = scatter ? unguided_scatter_prob / guided_scatter_prob :
                                (1.0f - unguided_scatter_prob) / (1.0f - guided_scatter_prob);
  result.indirect_throughput = reservoir.candidate.throughput * scale;

  if (!scatter && !sample_distance) {
    /* No scatter event sampled. */
    return;
  }

  /* Recover the volume coefficients at the scatter position. */
  sd->P = ray->P + ray->D * reservoir.candidate.t;
  sd->lcg_state = lcg_state;
  VolumeShaderCoefficients coeff ccl_optional_struct_init;
  if (!volume_shader_sample(kg, state, sd, &coeff)) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (kernel_integrator_state.deep_native_cameras && (sd->flag & SD_CACHE_MISS))
      return;
#  endif
    kernel_assert(false);
    return;
  }

  kernel_assert(sd->flag & SD_SCATTER);
  if (sample_distance) {
    /* Direct scatter. */
    result.direct_scatter = true;
    result.direct_t = reservoir.candidate.t;
    volume_shader_copy_phases(&result.direct_phases, sd);
  }

  if (scatter) {
    /* Indirect scatter. */
    result.indirect_scatter = true;
    result.indirect_t = reservoir.candidate.t;
    volume_shader_copy_phases(&result.indirect_phases, sd);
  }
}

/** \} */

ccl_device bool volume_integrate_should_stop(const ccl_private VolumeIntegrateResult &result)
{
  if (is_zero(result.indirect_throughput) && is_zero(result.direct_throughput)) {
    /* Stopped during Russian Roulette. */
    return true;
  }

  /* If we have scattering data for both direct and indirect, we're done. */
  return (result.direct_scatter && result.indirect_scatter);
}

/* Perform Russian Roulette termination to avoid drawing too many samples for indirect scatter, but
 * only if both direct and indirect scatter positions are available, or if no scattering is needed.
 */
ccl_device_inline bool volume_russian_roulette_termination(
    const IntegratorState state,
    ccl_private VolumeSampleReservoir &reservoir,
    ccl_private VolumeIntegrateResult &ccl_restrict result,
    ccl_private VolumeIntegrateState &ccl_restrict vstate)
{
  if (result.direct_scatter && result.indirect_scatter) {
    return true;
  }

  const float thresh = reduce_max(fabs(result.indirect_throughput));
  if (thresh > 0.05f) {
    /* Only stop if contribution is low enough. */
    return false;
  }

  /* Whether equiangular estimator of the direct throughput depends on the indirect throughput. */
  const bool equiangular = (vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) &&
                           vstate.use_mis && !result.direct_scatter;
  /* Whether both indirect and direct scatter are possible. */
  const bool has_scatter_samples = !reservoir.is_empty() && !equiangular;
  /* The path is to be terminated, no scatter position is needed along the ray. */
  const bool absorption_only = INTEGRATOR_STATE(state, path, flag) & PATH_RAY_TERMINATE;

  /* Randomly stop indirect scatter. */
  if (absorption_only || has_scatter_samples) {
    if (reservoir.rand > thresh) {
#  if __DEEP_BOUNDED_POLICY__ > 0
      if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING) {
        vstate.bounded = {vstate.t, BOUNDED_VOLUME_ZERO_EVENT};
      }
#  endif
      result.indirect_throughput = zero_spectrum();
      if (equiangular || (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE)) {
        /* Direct throughput depends on the indirect throughput, set to 0 for early termination. */
        result.direct_throughput = zero_spectrum();
      }
      return true;
    }

    reservoir.rand = saturatef(reservoir.rand / thresh);
    result.indirect_throughput /= thresh;
  }

  /* Randomly stop direct scatter. */
  if (equiangular) {
    if (reservoir.rand > thresh) {
      result.direct_scatter = true;
      result.direct_throughput = zero_spectrum();
      reservoir.rand = (reservoir.rand - thresh) / (1.0f - thresh);
    }
    else {
      reservoir.rand /= thresh;
      vstate.direct_rr_scale /= thresh;
    }
    reservoir.rand = saturatef(reservoir.rand);
  }

  return false;
}

/* -------------------------------------------------------------------- */
/** \name Null Scattering
 * \{ */

/* In a null-scattering framework, we fill the volume with fictitious particles, so that the
 * density is `majorant` everywhere. The null-scattering coefficients `sigma_n` is then defined by
 * the density of such particles. */
ccl_device_inline Spectrum volume_null_event_coefficients(const Spectrum sigma_t,
                                                          const float sigma_max,
                                                          ccl_private float &majorant)
{
  majorant = fmaxf(reduce_max(sigma_t), sigma_max);
  return make_spectrum(majorant) - sigma_t;
}

/* The probability of sampling real scattering event at each candidate point of delta tracking. */
ccl_device_inline float volume_scatter_probability(
    const ccl_private VolumeShaderCoefficients &coeff,
    const Spectrum sigma_n,
    const Spectrum throughput)
{
  /* We use `sigma_s` instead of `sigma_t` to skip sampling the absorption event, because it
   * always returns zero and has high variance. */
  const Spectrum sigma_c = coeff.sigma_s + sigma_n;

  /* Set `albedo` to 1 for the channel where extinction coefficient `sigma_t` is zero, to make
   * sure that we sample a distance outside the current segment when that channel is picked,
   * meaning light passes through without attenuation. */
  const Spectrum albedo = safe_divide_color(coeff.sigma_s, coeff.sigma_t, 1.0f);

  /* Assign weights per channel to pick scattering event based on throughput and single
   * scattering albedo.  */
  /* TODO(weizhen): currently the sample distance is the same for each color channel, revisit
   * the MIS weight when we use Spectral Majorant. */
  const Spectrum channel_pdf = volume_sample_channel_pdf(albedo, throughput);

  return dot(coeff.sigma_s / sigma_c, channel_pdf);
}

/* Decide between real and null scatter events at the current position. */
ccl_device_inline void volume_sample_indirect_scatter(
    ConstIntegratorState state,
    const float sigma_max,
    const float prob_s,
    const Spectrum sigma_s,
    ccl_private ShaderData *ccl_restrict sd,
    ccl_private VolumeIntegrateState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result,
    const uint lcg_state,
    ccl_private VolumeSampleReservoir &reservoir)
{
  const float weight = vstate.transmittance * prob_s;
  const Spectrum throughput = result.indirect_throughput * sigma_s / prob_s;

  if (vstate.vspg) {
    /* If we guide the scatter probability, simply put the candidate in the reservoir. */
    reservoir.add_sample(
#  ifdef __DENOISING_FEATURES__
        weight,
        {vstate.emission, vstate.t, throughput, weight * sigma_max, vstate.albedo, lcg_state}
#  else
        weight,
        {vstate.emission, vstate.t, throughput, weight * sigma_max, lcg_state}
#  endif
        ,
        vstate.deep_expression_head);
  }
  else if (!result.indirect_scatter) {
    /* If no guiding and indirect scatter position has not been found, decide between real and null
     * scatter events. */
    if (reservoir.rand <= prob_s) {
      /* Rescale random number for reusing. */
      reservoir.rand /= prob_s;

      /* Sampled scatter event. */
      result.indirect_scatter = true;
      volume_shader_copy_phases(&result.indirect_phases, sd);
      reservoir.add_sample(
#  ifdef __DENOISING_FEATURES__
          weight,
          {vstate.emission, vstate.t, throughput, weight * sigma_max, vstate.albedo, lcg_state}
#  else
          weight,
          {vstate.emission, vstate.t, throughput, weight * sigma_max, lcg_state}
#  endif
          ,
          vstate.deep_expression_head);

      if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
        result.direct_scatter = true;
        volume_shader_copy_phases(&result.direct_phases, sd);
      }
    }
    else {
      /* Rescale random number for reusing. */
      reservoir.rand = (reservoir.rand - prob_s) / (1.0f - prob_s);
    }
    reservoir.rand = saturatef(reservoir.rand);
  }
}

/**
 * Integrate volume based on weighted delta tracking, from
 * [Spectral and Decomposition Tracking for Rendering Heterogeneous Volumes]
 * (https://disneyanimation.com/publications/spectral-and-decomposition-tracking-for-rendering-heterogeneous-volumes)
 * by Peter Kutz et. al.
 *
 * The recursive Monte Carlo estimation of the Radiative Transfer Equation is
 * <L> = T(x -> y) / p(x -> y) * (L_e + sigma_s * L_s + sigma_n * L),
 * where T(x -> y) = exp(-sigma_max * dt) is the majorant transmittance between points x and y,
 * and p(x -> y) = sigma_max * exp(-sigma_max * dt) is the probability of sampling point y from
 * point x following exponential distribution.
 * At each recursive step, we randomly pick one of the two events proportional to their weights:
 * - If ξ < sigma_s / (sigma_s + |sigma_n|), we sample scatter event and evaluate L_s.
 * - Otherwise, no real collision happens and we continue the recursive process.
 * The emission L_e is evaluated at each step.
 */
ccl_device void volume_integrate_step_scattering(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    const float sigma_max,
    ccl_private ShaderData *ccl_restrict sd,
    ccl_private VolumeIntegrateState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result,
    ccl_private VolumeSampleReservoir &reservoir)
{
  if (volume_russian_roulette_termination(state, reservoir, result, vstate)) {
    return;
  }

  sd->P = ray->P + ray->D * vstate.t;
  VolumeShaderCoefficients coeff ccl_optional_struct_init;
  const uint lcg_state = sd->lcg_state;
  if (!volume_shader_sample(kg, state, sd, &coeff)) {
    return;
  }

#  if __DEEP_BOUNDED_POLICY__ >= 2
  if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING) {
    vstate.bounded.has_extinction |= !is_zero(coeff.sigma_t);
  }
#  endif
  kernel_assert(sigma_max != 0.0f);

  /* Null scattering coefficients. */
  float majorant;
  const Spectrum sigma_n = volume_null_event_coefficients(coeff.sigma_t, sigma_max, majorant);
  if (majorant != sigma_max) {
    /* Standard null scattering uses the majorant as the rate parameter for distance sampling, thus
     * the MC estimator is
     *   <L> = T(t) / p(t) * (L_e + sigma_s * L_s + sigma_n * L)
     *       = 1 / majorant * (L_e + sigma_s * L_s + sigma_n * L).
     * If we use another rate parameter sigma for distance sampling, the equation becomes
     *   <L> = T(t) / p(t) * (L_e + sigma_s * L_s + sigma_n * L)
     *       = exp(-majorant * t) / sigma * exp(-sigma * t ) * (L_e + sigma_s * L_s + sigma_n *L),
     * there is a scaling of majorant / sigma * exp(-(majorant - sigma) * t).
     * NOTE: this is not really unbiased, because the scaling is only applied when we sample an
     * event inside the segment, but in practice, if the majorant is reasonable, this doesn't
     * happen too often and shouldn't affect the result much. */
    result.indirect_throughput *= expf((sigma_max - majorant) * vstate.dt) / sigma_max;
  }
  else {
    result.indirect_throughput /= majorant;
  }

  /* Emission. */
  if (sd->flag & SD_EMISSION) {
    /* Emission = inv_sigma * (L_e + sigma_n * (inv_sigma * (L_e + sigma_n * ···))). */
    vstate.emission += result.indirect_throughput * coeff.emission;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (vstate.deep_expression_head != DEEP_NATIVE_INVALID_INDEX) {
      const Spectrum raw = result.indirect_throughput * coeff.emission;
      const uint32_t term = deep_native_expression_term(
          kg, state, ray, sd, vstate.t, vstate.t, raw, zero_spectrum(), true);
      vstate.deep_expression_head = deep_native_expression_node(
          state, DEEP_NATIVE_EXPR_ADD, vstate.deep_expression_head, term);
    }
#  endif
    if (!result.indirect_scatter) {
      /* Record emission until scatter position. */
      guiding_record_volume_emission(kg, state, coeff.emission);
    }
  }

  if (reduce_add(coeff.sigma_s) == 0.0f) {
    /* Absorption only. Deterministically choose null scattering and estimate the transmittance
     * of the current ray segment. */
    result.indirect_throughput *= sigma_n;
#  if __DEEP_BOUNDED_POLICY__ > 0
    if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING && is_zero(result.indirect_throughput)) {
      vstate.bounded = {vstate.t, BOUNDED_VOLUME_ZERO_EVENT};
    }
#  endif
    return;
  }

#  ifdef __DENOISING_FEATURES__
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DENOISING_FEATURES) {
    /* Albedo = inv_sigma * (sigma_s + sigma_n * (inv_sigma * (sigma_s + sigma_n * ···))). */
    vstate.albedo += result.indirect_throughput * coeff.sigma_s;
  }
#  endif

  /* Indirect scatter. */
  const float prob_s = volume_scatter_probability(coeff, sigma_n, result.indirect_throughput);
  volume_sample_indirect_scatter(
      state, sigma_max, prob_s, coeff.sigma_s, sd, vstate, result, lcg_state, reservoir);

  /* Null scattering. Accumulate weight and continue. */
  const float prob_n = 1.0f - prob_s;
  result.indirect_throughput *= safe_divide(sigma_n, prob_n);
  vstate.transmittance *= prob_n;
#  if __DEEP_BOUNDED_POLICY__ > 0
    if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING && is_zero(result.indirect_throughput)) {
      vstate.bounded = {vstate.t, BOUNDED_VOLUME_ZERO_EVENT};
    }
#  endif
}

/* Evaluate coefficients at the equiangular scatter position, and update the direct throughput. */
ccl_device_inline void volume_equiangular_direct_scatter(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    ccl_private ShaderData *ccl_restrict sd,
    ccl_private VolumeIntegrateState &vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result)
{
  if (vstate.direct_sample_method != VOLUME_SAMPLE_EQUIANGULAR || !result.direct_scatter) {
    return;
  }

  sd->P = ray->P + ray->D * result.direct_t;
  VolumeShaderCoefficients coeff ccl_optional_struct_init;
  if (volume_shader_sample(kg, state, sd, &coeff) && (sd->flag & SD_SCATTER)) {
#  if __DEEP_BOUNDED_POLICY__ == 2
    /* This direct-light sample can find real medium even when distance sampling
     * found no extinction. Its native coefficients also qualify Simple support.
     * The distance walk has already changed TRACKING to INTERVAL at this point. */
    if (vstate.bounded.reason != BOUNDED_VOLUME_INACTIVE) {
      vstate.bounded.has_extinction |= !is_zero(coeff.sigma_t);
    }
#  endif
    volume_shader_copy_phases(&result.direct_phases, sd);

    if (vstate.use_mis) {
      /* Compute distance pdf for multiple importance sampling. */
      float majorant;
      const Spectrum sigma_n = volume_null_event_coefficients(
          coeff.sigma_t, vstate.sigma_max, majorant);
      if ((vstate.sample_dt != FLT_MAX) && (majorant != vstate.sigma_max)) {
        result.direct_throughput *= expf((vstate.sigma_max - majorant) * vstate.sample_dt);
      }
      vstate.distance_pdf *= volume_scatter_probability(coeff, sigma_n, result.direct_throughput);
    }

    result.direct_throughput *= safe_divide(coeff.sigma_s, vstate.equiangular_pdf);
  }
  else {
    /* Scattering coefficient is zero at the sampled position. */
    result.direct_scatter = false;
  }
}

/* Multiple Importance Sampling between equiangular sampling and distance sampling.
 *
 * According to [A null-scattering path integral formulation of light transport]
 * (https://cs.dartmouth.edu/~wjarosz/publications/miller19null.html), the pdf of sampling a
 * scattering event at point P using distance sampling is the probability of sampling a series of
 * null events, and then a scatter event at P, i.e.
 *
 *                distance_pdf = (∏p_dist * p_null) * p_dist * p_scatter,
 *
 * where `p_dist = sigma_max * exp(-sigma_max * dt)` is the probability of sampling an incremental
 * distance `dt` following exponential distribution, and `p_null = sigma_n / sigma_c` is the
 * probability of sampling a null event at a certain point, `p_scatter = sigma_s / sigma_c` the
 * probability of sampling a scatter event.
 *
 * The pdf of sampling a scattering event at point P using equiangular sampling is the probability
 * of sampling a series of null events deterministically, and then a scatter event at the point of
 * equiangular sampling, i.e.
 *
 *                     equiangular_pdf = (∏p_dist * 1) * T * p_equi,
 *
 * where `T = exp(-sigma_max * dt)` is the probability of sampling a distance beyond `dt` following
 * exponential distribution, `p_equi` is the equiangular pdf. Since the null events are sampled
 * deterministically, the pdf is 1 instead of `p_null`.
 *
 * When performing MIS between distance and equiangular sampling, since we use single-channel
 * majorant, `p_dist` is shared in both pdfs, therefore we can write
 *
 *       distance_pdf / equiangular_pdf = (∏p_null) * sigma_max * p_scatter / p_equi.
 *
 * If we want to use multi-channel majorants in the future, the components do not cancel, but we
 * can divide by the `p_dist` of the hero channel to alleviate numerical issues.
 */
ccl_device_inline void volume_direct_scatter_mis(
    const ccl_private Ray *ccl_restrict ray,
    const ccl_private VolumeIntegrateState &ccl_restrict vstate,
    const ccl_private EquiangularCoefficients &equiangular_coeffs,
    ccl_private VolumeIntegrateResult &ccl_restrict result)
{
  if (!vstate.use_mis || !result.direct_scatter) {
    return;
  }

  float mis_weight;
  if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
    const float equiangular_pdf = volume_equiangular_pdf(ray, equiangular_coeffs, result.direct_t);
    mis_weight = power_heuristic(vstate.distance_pdf, equiangular_pdf);
  }
  else {
    kernel_assert(vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR);
    mis_weight = power_heuristic(vstate.equiangular_pdf, vstate.distance_pdf);
  }

  result.direct_throughput *= 2.0f * mis_weight;
}

/** \} */

ccl_device_inline void volume_integrate_state_init(KernelGlobals kg,
                                                   const IntegratorState state,
                                                   const VolumeSampleMethod direct_sample_method,
                                                   const ccl_private RNGState *rng_state,
                                                   const float tmin,
                                                   ccl_private VolumeIntegrateState &vstate)
{
  vstate.rscatter = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_SCATTER_DISTANCE);

  /* Multiple importance sampling: pick between equiangular and distance sampling strategy. */
  vstate.direct_sample_method = direct_sample_method;
  vstate.use_mis = (direct_sample_method == VOLUME_SAMPLE_MIS);
  if (vstate.use_mis) {
    if (vstate.rscatter < 0.5f) {
      vstate.direct_sample_method = VOLUME_SAMPLE_DISTANCE;
      vstate.rscatter *= 2.0f;
    }
    else {
      /* Rescale for equiangular distance sampling. */
      vstate.rscatter = (vstate.rscatter - 0.5f) * 2.0f;
      vstate.direct_sample_method = VOLUME_SAMPLE_EQUIANGULAR;
    }
  }

  vstate.distance_pdf = 0.0f;
  vstate.equiangular_pdf = 0.0f;
  vstate.sigma_max = 0.0f;
  vstate.transmittance = 1.0f;
  vstate.t = tmin;
  vstate.optical_depth = 0.0f;
  vstate.step = 0;
  /* Only guide primary rays. */
  vstate.vspg = (INTEGRATOR_STATE(state, path, bounce) == 0);
  vstate.scatter_prob = 1.0f;
  vstate.majorant_scale = 1.0f;
  vstate.direct_rr_scale = 1.0f;
  vstate.emission = zero_spectrum();
  vstate.deep_native_segment = DEEP_NATIVE_INVALID_INDEX;
  vstate.deep_expression_head = DEEP_NATIVE_INVALID_INDEX;
#  ifdef __DENOISING_FEATURES__
  vstate.albedo = zero_spectrum();
#  endif
}

ccl_device_inline void volume_integrate_result_init(
    ConstIntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    ccl_private VolumeIntegrateState &vstate,
    const ccl_private EquiangularCoefficients &equiangular_coeffs,
    ccl_private VolumeIntegrateResult &result)
{
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  result.direct_throughput = (vstate.direct_sample_method == VOLUME_SAMPLE_NONE) ?
                                 zero_spectrum() :
                                 throughput;
  result.indirect_throughput = throughput;

  /* Equiangular sampling: compute distance and PDF in advance. */
  if (vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) {
    result.direct_t = volume_equiangular_sample(
        ray, equiangular_coeffs, vstate.rscatter, &vstate.equiangular_pdf);
  }

#  if defined(__PATH_GUIDING__)
  result.direct_sample_method = vstate.direct_sample_method;
#  endif
}

/* Compute guided volume scatter probability and the majorant scale needed for achieving the
 * scatter probability, for homogeneous volume. */
ccl_device_inline Spectrum
volume_scatter_probability_homogeneous(KernelGlobals kg,
                                       const IntegratorState state,
                                       ccl_global float *ccl_restrict render_buffer,
                                       const float ray_length,
                                       const ccl_private VolumeShaderCoefficients &coeff,
                                       ccl_private VolumeIntegrateState &vstate)
{
  const bool attenuation_only = (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_TERMINATE) ||
                                is_zero(coeff.sigma_s);
  if (attenuation_only) {
    return zero_spectrum();
  }

  const Spectrum attenuation = 1.0f - volume_color_transmittance(coeff.sigma_t, ray_length);
  if (!vstate.vspg) {
    return attenuation;
  }

  const ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  kernel_assert(kernel_data.film.pass_volume_scatter_denoised != PASS_UNUSED);
  kernel_assert(kernel_data.film.pass_volume_transmit_denoised != PASS_UNUSED);

  /* Contribution based criterion, see Eq. (15). */
  const Spectrum L_scattered = kernel_read_pass_rgbe(
      buffer + kernel_data.film.pass_volume_scatter_denoised);
  const Spectrum L_transmitted = kernel_read_pass_rgbe(
      buffer + kernel_data.film.pass_volume_transmit_denoised);
  const Spectrum L_volume = L_transmitted + L_scattered;

  Spectrum guided_scatter_prob;
  if (is_zero(L_volume)) {
    /* Equal probability if no information gathered yet. */
    guided_scatter_prob = select(coeff.sigma_t > 0.0f, make_spectrum(0.5f), zero_spectrum());
  }
  else {
    /* VSPG guide the scattering probability along the primary ray, but not necessarily in the
     * current segment. Scale the probability based on the relative majorant transmittance. */
    /* TODO(weizhen): spectrum optical depth. */
    const float optical_depth = volume_majorant_optical_depth(kg, buffer);
    const float scale = reduce_max(attenuation) / (1.0f - expf(-optical_depth));

    guided_scatter_prob = clamp(
        safe_divide(L_scattered, L_volume) * scale, zero_spectrum(), one_spectrum());
  }

  /* Defensive sampling. */
  return mix(attenuation, guided_scatter_prob, 0.75f);
}

/* Homogeneous volume distance sampling, using analytic solution to avoid drawing multiple samples
 * with the reservoir.
 * Decide the indirect scatter probability, and sample an indirect scatter position inside the
 * volume or transmit through the volume.
 * Direct scatter is always sampled, if possible. */
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline bool volume_native_world_support(KernelGlobals kg,
                                                   ConstIntegratorState state,
                                                   const float physical_t)
{
  return deep_native_world_support(kg, state, physical_t);
}

ccl_device_inline uint32_t
volume_capture_native_homogeneous(KernelGlobals kg,
                                  ConstIntegratorState state,
                                  const ccl_private Ray *ray,
                                  const ccl_private ShaderData *sd,
                                  const ccl_private VolumeShaderCoefficients &coeff)
{
  if (!deep_native_colour_capture_enabled() || !kernel_integrator_state.deep_native_homogeneous_capacity) {
    return DEEP_NATIVE_INVALID_INDEX;
  }
  if (sd->flag & SD_CACHE_MISS) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_SHADER_RETRY);
    return DEEP_NATIVE_INVALID_INDEX;
  }
  ccl_global DeepNativeCameraRecord *camera = deep_native_camera(state);
  if (!camera) {
    return DEEP_NATIVE_INVALID_INDEX;
  }
  ccl_global DeepNativeRootRecord *root = deep_native_root(
      INTEGRATOR_STATE(state, path, deep_frame_camera_index));
  if (root && root->kind != DEEP_NATIVE_ROOT_NONE) {
    /* An existing first source owns all later radiance. A bent descendant
     * volume segment is not a primary camera optical/emission span. */
    return DEEP_NATIVE_INVALID_INDEX;
  }
  uint32_t unsupported = 0;
  const bool original_camera_support = ray->P.x == camera->origin[0] &&
                                       ray->P.y == camera->origin[1] &&
                                       ray->P.z == camera->origin[2] &&
                                       ray->D.x == camera->direction[0] &&
                                       ray->D.y == camera->direction[1] &&
                                       ray->D.z == camera->direction[2] &&
                                       ray->time == camera->time;
  if (!original_camera_support) {
    unsupported |= DEEP_NATIVE_ERROR_IDENTITY;
  }
  const bool world_support = volume_native_world_support(kg, state, ray->tmax);
  if ((INTEGRATOR_STATE(state, path, bounce) != 0 || !is_zero(coeff.sigma_s)) &&
      !kernel_integrator_state.deep_native_full_optical)
  {
    unsupported |= DEEP_NATIVE_ERROR_SCATTERING;
  }
  if (kernel_data.volume_stack_size > 1 &&
      INTEGRATOR_STATE_ARRAY(state, volume_stack, 1, shader) != SHADER_NONE &&
      !kernel_integrator_state.deep_native_full_optical)
  {
    unsupported |= DEEP_NATIVE_ERROR_HETEROGENEOUS;
  }
  if (!(camera->flags & (DEEP_NATIVE_CAMERA_AFFINE_DEPTH | DEEP_NATIVE_CAMERA_RADIAL_DEPTH)) ||
      ray->tmin < camera->tmin || (ray->tmax > camera->tmax && !world_support) ||
      ray->tmax <= ray->tmin)
  {
    unsupported |= DEEP_NATIVE_ERROR_INCOMPLETE;
  }
  if (unsupported) {
    camera->unsupported_features |= unsupported;
    deep_native_capture_error(unsupported);
  }
  if (deep_native_validation_deny(DEEP_NATIVE_FAULT_HOMOGENEOUS_DENY))
    return DEEP_NATIVE_INVALID_INDEX;
  const uint index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_homogeneous_reserved, 1);
  if (index >= kernel_integrator_state.deep_native_homogeneous_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_OVERFLOW);
    return DEEP_NATIVE_INVALID_INDEX;
  }
  DeepNativeHomogeneousRecord segment = {};
  segment.camera_index = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  segment.pixel = camera->pixel;
  segment.sample = camera->sample;
  segment.ordinal = INTEGRATOR_STATE(state, path, volume_bounds_bounce);
  segment.path_flags = INTEGRATOR_STATE(state, path, flag);
  segment.flags = DEEP_NATIVE_HOMOGENEOUS_OPTICAL;
  if (world_support)
    segment.flags |= DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT;
  if (original_camera_support &&
      (camera->flags & (DEEP_NATIVE_CAMERA_AFFINE_DEPTH | DEEP_NATIVE_CAMERA_RADIAL_DEPTH)))
  {
    segment.flags |= DEEP_NATIVE_HOMOGENEOUS_CAMERA_SUPPORT_VERIFIED;
  }
  segment.unsupported_features = unsupported;
  segment.z = camera_z_depth(kg, ray->P + ray->D * ray->tmin);
  if (!world_support)
    segment.zback = camera_z_depth(kg, ray->P + ray->D * ray->tmax);
  if (world_support) {
    /* The frozen camera definition avoids an overflowing intermediate world point.
     * The host proof charges endpoint quantization; physical support is unchanged. */
    const double end = deep_native_promoted_camera_depth(kg, camera, ray->tmax);
    if (!(end > segment.z) ||
        !deep_native_world_span_depth(
            end, bool(camera->flags & DEEP_NATIVE_CAMERA_RADIAL_DEPTH), segment.zback))
      deep_native_capture_error(DEEP_NATIVE_ERROR_INCOMPLETE);
  }
  segment.tmin = ray->tmin;
  segment.tmax = ray->tmax;
  const double native_length = double(ray->tmax) - double(ray->tmin);
  segment.tau[0] = double(coeff.sigma_t.x) * native_length;
  segment.tau[1] = double(coeff.sigma_t.y) * native_length;
  segment.tau[2] = double(coeff.sigma_t.z) * native_length;
  segment.source_object = sd->object;
  segment.source_shader = INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, shader);
  kernel_integrator_state.deep_native_homogeneous[index] = segment;
  atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_native_homogeneous_written, 1);
  return index;
}
#  endif

ccl_device_forceinline void volume_integrate_homogeneous(KernelGlobals kg,
                                                         const IntegratorState state,
                                                         const ccl_private Ray *ccl_restrict ray,
                                                         ccl_private ShaderData *ccl_restrict sd,
                                                         const ccl_private RNGState *rng_state,
                                                         ccl_global float *ccl_restrict
                                                             render_buffer,
                                                         ccl_private VolumeIntegrateState &vstate,
                                                         const Interval<float> interval,
                                                         ccl_private VolumeIntegrateResult &result)
{
#  ifdef __DEEP_OUTPUT__
  ccl_global KernelDeepSample *deep_samples = nullptr;
  uint32_t deep_pixel_index = 0;
  uint32_t deep_sample_idx = DEEP_INVALID_SAMPLE_INDEX;
#  endif
  sd->P = ray->P + ray->D * ray->tmin;
  VolumeShaderCoefficients coeff ccl_optional_struct_init;
  if (!volume_shader_sample(kg, state, sd, &coeff)) {
    if (sd->flag & SD_CACHE_MISS)
      return; /* No segment, guiding receipt or Film write has been committed. */
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    deep_compact_save_stack(kg, state);
    if (deep_compact_camera_chain(state)) {
      const uint i = deep_compact_camera_index(state);
      deep_compact_cell(kg, state, ray->tmin, ray->tmax, 0.0,
                        kernel_integrator_state.deep_compact_rays[i].reserved);
    }
    if (kernel_integrator_state.deep_native_cameras) {
      const VolumeShaderCoefficients clear_coeff = {};
      vstate.deep_native_segment = volume_capture_native_homogeneous(
          kg, state, ray, sd, clear_coeff);
    }
#  endif
    return;
  }

  const float ray_length = ray->tmax - ray->tmin;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  deep_compact_save_stack(kg, state);
  if (deep_compact_camera_chain(state)) {
    const uint i = deep_compact_camera_index(state);
    deep_compact_cell(kg, state, ray->tmin, ray->tmax,
                      (double(coeff.sigma_t.x) + coeff.sigma_t.y + coeff.sigma_t.z) / 3.0,
                      kernel_integrator_state.deep_compact_rays[i].reserved);
  }
#  endif
  vstate.optical_depth = reduce_max(coeff.sigma_t) * ray_length;

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  vstate.deep_native_segment = volume_capture_native_homogeneous(kg, state, ray, sd, coeff);
#  endif

  /* Emission. */
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  if (sd->flag & SD_EMISSION) {
    const Spectrum emission = volume_emission_integrate(&coeff, sd->flag, ray_length);
    vstate.emission = throughput * emission;
    guiding_record_volume_emission(kg, state, emission);
  }

  /* Deep output: homogeneous volume sample. */
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_volume_emission_spans) {
    const float span_alpha = saturatef(
        1.0f - average(volume_color_transmittance(coeff.sigma_t, ray_length)));
    volume_capture_primary_emission_span(
        kg,
        state,
        ray,
        ray->tmin,
        ray->tmax,
        vstate.emission,
        coeff.sigma_t * ray_length,
        span_alpha,
        DEEP_EMISSION_HOMOGENEOUS |
            (!is_zero(coeff.sigma_t) ? DEEP_EMISSION_LEGACY_OPACITY_ELIGIBLE : 0u));
  }
#  endif
#  ifdef __DEEP_OUTPUT__
  /* Write deep sample for homogeneous volume (alpha-only, RGB=0).
   * Volume color is applied via deep recolor in compositing. */
  if (kernel_data.film.use_deep_output == 1 && (INTEGRATOR_STATE(state, path, bounce) == 0)) {
    /* Only write if volume has extinction (opacity). */
    if (!is_zero(coeff.sigma_t)) {
      deep_samples = (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr;
      ccl_global uint32_t *deep_sample_counts = (ccl_global uint32_t *)
                                                    kernel_data.film.deep_sample_counts_ptr;
      if (deep_samples && deep_sample_counts) {
        deep_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);

        /* Calculate front and back Z depths from ray entry/exit positions. */
        float3 P_front = ray->P + ray->D * ray->tmin;
        float3 P_back = ray->P + ray->D * ray->tmax;
        float z_front = camera_z_depth(kg, P_front);
        float z_back = camera_z_depth(kg, P_back);

        /* Volume opacity: 1 - transmittance. */
        const Spectrum local_transmittance = volume_color_transmittance(coeff.sigma_t, ray_length);
        float alpha = 1.0f - average(local_transmittance);
        alpha = saturatef(alpha);

        /* Write alpha-only sample (RGB=0), color applied via deep recolor. */
        deep_sample_idx = film_write_deep_sample_volume(kg,
                                                        deep_pixel_index,
                                                        deep_samples,
                                                        deep_sample_counts,
                                                        alpha,
                                                        z_front,
                                                        z_back,
                                                        INTEGRATOR_STATE(state, path, sample),
                                                        vstate.emission,
                                                        coeff.sigma_t * ray_length);
      }
    }
  }
#  endif

  /* Transmittance of the complete ray segment. */
  const Spectrum transmittance = volume_color_transmittance(coeff.sigma_t, ray_length);
#  if __DEEP_BOUNDED_POLICY__ > 0
  if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING) {
    vstate.bounded = {ray->tmax, BOUNDED_VOLUME_INTERVAL,
                      __DEEP_BOUNDED_POLICY__ >= 2 && ray_length > 0.0f &&
                          !is_zero(coeff.sigma_t)};
  }
#  endif
  if ((INTEGRATOR_STATE(state, path, flag) & PATH_RAY_TERMINATE) || is_zero(coeff.sigma_s)) {
    /* Attenuation only. */
    result.indirect_throughput *= transmittance;
    return;
  }

  float rchannel = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_RESERVOIR);
  /* Single scattering albedo. */
  const Spectrum albedo = safe_divide_color(coeff.sigma_s, coeff.sigma_t);
  /* Multiple scattering albedo. */
  vstate.albedo = albedo * (1.0f - transmittance) * throughput;

  /* Indirect scatter. */
  {
    /* Consider the contribution of both scattering and transmission when sampling indirect
     * scatter. */
    Spectrum channel_pdf;
    const int channel = volume_sample_channel(
        vstate.albedo + transmittance, throughput, &rchannel, &channel_pdf);

    const Spectrum scatter_prob = volume_scatter_probability_homogeneous(
        kg, state, render_buffer, ray_length, coeff, vstate);
    const float scatter_pdf_channel = volume_channel_get(scatter_prob, channel);

    if (vstate.rscatter < scatter_pdf_channel) {
      /* Sampled scatter event. */
      vstate.rscatter /= scatter_pdf_channel;
      const Interval<float> t_range = {0.0f, ray_length};
      result.indirect_scatter = !t_range.is_empty();
      const float sigma = volume_channel_get(coeff.sigma_t, channel);
      const float dt = sample_exponential_distribution(vstate.rscatter, sigma, t_range);
      result.indirect_t = ray->tmin + dt;
      const Spectrum distance_pdf = pdf_exponential_distribution(dt, coeff.sigma_t, t_range);
      const float indirect_distance_pdf = dot(distance_pdf * scatter_prob, channel_pdf);
      const Spectrum transmittance = volume_color_transmittance(coeff.sigma_t, dt);
      result.indirect_throughput *= safe_divide(coeff.sigma_s * transmittance,
                                                indirect_distance_pdf);
      volume_shader_copy_phases(&result.indirect_phases, sd);
    }
    else {
      /* Sampled transmit event. */
      const float indirect_distance_pdf = dot((1.0f - scatter_prob), channel_pdf);
      result.indirect_throughput *= safe_divide(transmittance, indirect_distance_pdf);
      vstate.rscatter = (vstate.rscatter - scatter_pdf_channel) / (1.0f - scatter_pdf_channel);
    }
  }

#  ifdef __DEEP_OUTPUT__
  if (deep_sample_idx != DEEP_INVALID_SAMPLE_INDEX) {
    uint32_t deep_ray_flags = DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE;
    if (result.indirect_scatter) {
      deep_ray_flags |= DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED;
    }
    film_mark_deep_volume_primary_ray_flag(
        kg, deep_pixel_index, deep_sample_idx, deep_samples, deep_ray_flags);
  }
#  endif

  /* Direct scatter. */
  if (vstate.direct_sample_method == VOLUME_SAMPLE_NONE) {
    return;
  }

  /* Sample inside the valid ray segment. */
  const Interval<float> t_range = {interval.min - ray->tmin, interval.max - ray->tmin};
  result.direct_scatter = !t_range.is_empty();
  volume_shader_copy_phases(&result.direct_phases, sd);

  Spectrum channel_pdf;
  const int channel = volume_sample_channel(vstate.albedo, throughput, &rchannel, &channel_pdf);

  if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
    const float sigma = volume_channel_get(coeff.sigma_t, channel);
    const float dt = sample_exponential_distribution(vstate.rscatter, sigma, t_range);
    result.direct_t = ray->tmin + dt;
    const Spectrum distance_pdf = pdf_exponential_distribution(dt, coeff.sigma_t, t_range);
    vstate.distance_pdf = dot(distance_pdf, channel_pdf);
    const Spectrum transmittance = volume_color_transmittance(coeff.sigma_t, dt);
    result.direct_throughput *= safe_divide(coeff.sigma_s * transmittance, vstate.distance_pdf);
  }
  else {
    kernel_assert(vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR);
    const float dt = result.direct_t - ray->tmin;
    const Spectrum transmittance = volume_color_transmittance(coeff.sigma_t, dt);
    result.direct_throughput *= safe_divide(coeff.sigma_s * transmittance, vstate.equiangular_pdf);
    if (vstate.use_mis) {
      vstate.distance_pdf = dot(pdf_exponential_distribution(dt, coeff.sigma_t, t_range),
                                channel_pdf);
    }
  }
}

/* heterogeneous volume distance sampling: integrate stepping through the
 * volume until we reach the end, get absorbed entirely, or run out of
 * iterations. this does probabilistically scatter or get transmitted through
 * for path tracing where we don't want to branch. */
ccl_device_forceinline bool volume_integrate_heterogeneous(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    ccl_private ShaderData *ccl_restrict sd,
    RNGState rng_state,
    ccl_global float *ccl_restrict render_buffer,
    ccl_private VolumeIntegrateState &vstate,
    ccl_private VolumeIntegrateResult &result)
{
  OctreeTracing octree(ray->tmin);
  VolumeSampleReservoir reservoir(0.0f);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  uint phase = 0;
  bool resumed = false;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  auto *checkpoint = deep_native_null_checkpoint(state);
  resumed = checkpoint && checkpoint->active;
  if (resumed) {
    rng_state = checkpoint->rng;
    octree = checkpoint->octree;
    reservoir = checkpoint->reservoir;
    phase = checkpoint->phase;
  }
#  endif
  if (!resumed) {
    if (!volume_octree_setup<false>(
            kg, ray, sd, state, &rng_state, path_visibility, path_flag, octree))
      return !(sd->flag & SD_CACHE_MISS);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (kernel_integrator_state.deep_native_expression_nodes &&
        kernel_integrator_state.deep_native_full_optical && deep_native_is_camera_chain(state))
      vstate.deep_expression_head = 0;
#  endif
    vstate.optical_depth = octree.sigma.max * octree.t.length();
    volume_scatter_probability_heterogeneous(kg, state, render_buffer, vstate);
    reservoir = VolumeSampleReservoir(path_state_rng_1D(kg, &rng_state, PRNG_VOLUME_RESERVOIR));
    path_state_rng_scramble(&rng_state, 0xe35fad82);
  }
  if (!resumed || phase == 2) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const auto before_shader = deep_native_volume_shader_state(sd);
    const VolumeIntegrateResult before_result = result;
#  endif
    volume_equiangular_transmittance(
        kg, state, ray, octree.sigma, octree.t, sd, &rng_state, vstate, result);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (checkpoint && (sd->flag & SD_CACHE_MISS)) {
      deep_native_store_null_checkpoint(state,
                                        ray,
                                        checkpoint,
                                        before_shader,
                                        vstate,
                                        before_result,
                                        rng_state,
                                        octree,
                                        reservoir,
                                        2,
                                        true);
      return false;
    }
#  endif
    phase = 0;
  }

  /* Deep output: unbiased volume segments. */
#  ifdef __DEEP_OUTPUT__
  /* Track accumulated transmittance for deep output. */
  float prev_transmittance = vstate.transmittance;
  /* Legacy Deep capture remains active outside the experimental optical-profile mode. */
  const bool write_deep = kernel_data.film.use_deep_output == 1 &&
                          (INTEGRATOR_STATE(state, path, bounce) == 0);
  ccl_global KernelDeepSample *deep_samples = nullptr;
  ccl_global uint32_t *deep_sample_counts = nullptr;
  uint32_t pixel_index = 0;
  uint32_t last_deep_sample_idx = DEEP_INVALID_SAMPLE_INDEX;

  if (write_deep) {
    deep_samples = (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr;
    deep_sample_counts = (ccl_global uint32_t *)kernel_data.film.deep_sample_counts_ptr;
    pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  }
#  endif

  uint operation_steps = 0;
  while (phase == 0) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const VolumeIntegrateState before_state = vstate;
    const VolumeIntegrateResult before_result = result;
    const RNGState before_rng = rng_state;
    const OctreeTracing before_octree = octree;
    const VolumeSampleReservoir before_reservoir = reservoir;
    const auto before_shader = deep_native_volume_shader_state(sd);
#  endif
    const bool advanced = volume_integrate_advance(
        kg, ray, sd, state, &rng_state, path_visibility, path_flag, octree, vstate, result);
    if (advanced) {
      const float sigma_max = octree.sigma.max * vstate.majorant_scale;
      volume_integrate_step_scattering(kg, state, ray, sigma_max, sd, vstate, result, reservoir);
    }
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (checkpoint && (sd->flag & SD_CACHE_MISS)) {
      deep_native_store_null_checkpoint(state,
                                        ray,
                                        checkpoint,
                                        before_shader,
                                        before_state,
                                        before_result,
                                        before_rng,
                                        before_octree,
                                        before_reservoir,
                                        0,
                                        true);
      return false;
    }
#  endif
    if (!advanced) {
#  if __DEEP_BOUNDED_POLICY__ > 0
      if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING &&
          vstate.step <= VOLUME_MAX_STEPS + 1 && !(sd->flag & SD_CACHE_MISS))
      {
        vstate.bounded.event_t = min(octree.t.max, ray->tmax);
      }
#  endif
      break;
    }

    /* Deep output: per-segment samples. */
#  ifdef __DEEP_OUTPUT__
    /* Write deep sample for this octree node segment in unbiased mode.
     * We compute segment alpha from the change in throughput over this segment. */
    if (write_deep && deep_samples && deep_sample_counts) {
      /* Estimate segment opacity from ratio-tracking transmittance change.
       * transmittance = vstate.transmittance / prev_transmittance
       * alpha = 1 - transmittance */
      const float transmittance_ratio = (prev_transmittance > 0.0f) ?
                                            (vstate.transmittance / prev_transmittance) :
                                            0.0f;
      float segment_alpha = 1.0f - saturatef(transmittance_ratio);

      const float deep_segment_alpha_epsilon = 1e-6f;
      if (segment_alpha > deep_segment_alpha_epsilon) {
        /* Calculate front and back Z depths from octree segment. */
        float3 P_front = ray->P + ray->D * octree.t.min;
        float3 P_back = ray->P + ray->D * octree.t.max;
        float z_front = camera_z_depth(kg, P_front);
        float z_back = camera_z_depth(kg, P_back);

        last_deep_sample_idx = film_write_deep_sample_volume(
            kg,
            pixel_index,
            deep_samples,
            deep_sample_counts,
            segment_alpha,
            z_front,
            z_back,
            INTEGRATOR_STATE(state, path, sample),
            zero_spectrum(),
            make_spectrum(-logf(max(1.0f - segment_alpha, 1.0e-20f))));
      }

      /* Update previous transmittance for next segment. */
      prev_transmittance = vstate.transmittance;
    }
#  endif

    if (volume_integrate_should_stop(result)) {
      break;
    }
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (checkpoint && ++operation_steps >= kernel_integrator_state.deep_native_volume_step_limit) {
      deep_native_store_null_checkpoint(state,
                                        ray,
                                        checkpoint,
                                        deep_native_volume_shader_state(sd),
                                        vstate,
                                        result,
                                        rng_state,
                                        octree,
                                        reservoir,
                                        0,
                                        false);
      return false;
    }
#  endif
  }

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  /* Both finalizers only update private results and append immutable DAG candidates.
   * A failed candidate never has a Film receipt and may remain unreferenced. */
  const VolumeIntegrateState before_state = vstate;
  const VolumeIntegrateResult before_result = result;
  const VolumeSampleReservoir before_reservoir = reservoir;
  const auto before_shader = deep_native_volume_shader_state(sd);
#  endif
#  if __DEEP_BOUNDED_POLICY__ > 0
  if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING && !(sd->flag & SD_CACHE_MISS)) {
    vstate.bounded.event_t = max(vstate.t, vstate.bounded.event_t);
    vstate.bounded.reason = BOUNDED_VOLUME_INTERVAL;
  }
#  endif
  volume_distance_sampling_finalize(kg, state, ray, sd, vstate, result, reservoir);
  if (!(sd->flag & SD_CACHE_MISS))
    volume_equiangular_direct_scatter(kg, state, ray, sd, vstate, result);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (checkpoint && (sd->flag & SD_CACHE_MISS)) {
    deep_native_store_null_checkpoint(state,
                                      ray,
                                      checkpoint,
                                      before_shader,
                                      before_state,
                                      before_result,
                                      rng_state,
                                      octree,
                                      before_reservoir,
                                      1,
                                      true);
    return false;
  }
#  endif
#  ifdef __DEEP_OUTPUT__
  if (last_deep_sample_idx != DEEP_INVALID_SAMPLE_INDEX) {
    uint32_t deep_ray_flags = DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE;
    if (result.indirect_scatter) {
      deep_ray_flags |= DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED;
    }
    film_mark_deep_volume_primary_ray_flag(
        kg, pixel_index, last_deep_sample_idx, deep_samples, deep_ray_flags);
  }
#  endif
  return true;
}

/* Path tracing: sample point on light using equiangular sampling. */
ccl_device_forceinline bool integrate_volume_sample_direct_light(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    const ccl_private ShaderData *ccl_restrict sd,
    const ccl_private RNGState *ccl_restrict rng_state,
    ccl_private EquiangularCoefficients *ccl_restrict equiangular_coeffs,
    ccl_private LightSample *ccl_restrict ls)
{
  /* Test if there is a light or BSDF that needs direct light. */
  if (!kernel_data.integrator.use_direct_light) {
    return false;
  }

  /* Sample position on a light. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const uint bounce = INTEGRATOR_STATE(state, path, bounce);
  const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);

  if (!light_sample_from_volume_segment(kg,
                                        rand_light,
                                        sd->time,
                                        sd->P,
                                        ray->D,
                                        ray->tmax - ray->tmin,
                                        light_link_receiver_nee(kg, sd),
                                        bounce,
                                        path_flag,
                                        ls))
  {
    ls->emitter_id = EMITTER_NONE;
    return false;
  }

  if (ls->shader & SHADER_EXCLUDE_SCATTER) {
    ls->emitter_id = EMITTER_NONE;
    return false;
  }

  equiangular_coeffs->P = ls->P;

  return volume_valid_direct_ray_segment(kg, ray->P, ray->D, &equiangular_coeffs->t_range, ls);
}

/* Determine the method to sample direct light, based on the volume property and settings. */
ccl_device_forceinline VolumeSampleMethod
volume_direct_sample_method(KernelGlobals kg,
                            const IntegratorState state,
                            const ccl_private Ray *ccl_restrict ray,
                            const ccl_private ShaderData *ccl_restrict sd,
                            const ccl_private RNGState *ccl_restrict rng_state,
                            ccl_private EquiangularCoefficients *coeffs,
                            ccl_private LightSample *ccl_restrict ls)
{
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_TERMINATE) {
    return VOLUME_SAMPLE_NONE;
  }

  if (!integrate_volume_sample_direct_light(kg, state, ray, sd, rng_state, coeffs, ls)) {
    return VOLUME_SAMPLE_NONE;
  }

  /* Sample the scatter position with distance sampling for distant light. */
  const bool has_equiangular_sample = (ls->t != FLT_MAX);
  return has_equiangular_sample ? volume_stack_sample_method(kg, state) : VOLUME_SAMPLE_DISTANCE;
}

/* Shared function of integrating homogeneous and heterogeneous volume. */
ccl_device bool volume_integrate_null_scattering(KernelGlobals kg,
                                                 const IntegratorState state,
                                                 const ccl_private Ray *ccl_restrict ray,
                                                 ccl_private ShaderData *ccl_restrict sd,
                                                 const ccl_private RNGState *rng_state,
                                                 ccl_global float *ccl_restrict render_buffer,
                                                 ccl_private LightSample *ls,
                                                 ccl_private VolumeIntegrateResult &result)
{
  PROFILING_INIT(kg, PROFILING_SHADE_VOLUME_INTEGRATE);

  EquiangularCoefficients equiangular_coeffs = {zero_float3(), {ray->tmin, ray->tmax}};
  VolumeIntegrateState vstate ccl_optional_struct_init;
  bool resumed = false;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  auto *checkpoint = deep_native_null_checkpoint(state);
  resumed = checkpoint && checkpoint->active;
  if (resumed && !deep_native_null_checkpoint_matches(state, ray, checkpoint)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return false;
  }
  if (resumed) {
    equiangular_coeffs = checkpoint->equiangular;
    *ls = checkpoint->light;
    vstate = checkpoint->state;
    result = checkpoint->result;
    deep_native_restore_volume_shader(sd, checkpoint->shader);
  }
#  endif
  if (!resumed) {
    const VolumeSampleMethod direct_sample_method = volume_direct_sample_method(
        kg, state, ray, sd, rng_state, &equiangular_coeffs, ls);
    volume_integrate_state_init(kg, state, direct_sample_method, rng_state, ray->tmin, vstate);
    volume_integrate_result_init(state, ray, vstate, equiangular_coeffs, result);
  }

#  if __DEEP_BOUNDED_POLICY__ > 0
  vstate.bounded = {ray->tmin,
                    deep_bounded_camera_chain(kg, state) ? BOUNDED_VOLUME_TRACKING :
                                                           BOUNDED_VOLUME_INACTIVE};
#  endif
  if (volume_is_homogeneous<false>(kg, state)) {
    volume_integrate_homogeneous(
        kg, state, ray, sd, rng_state, render_buffer, vstate, equiangular_coeffs.t_range, result);
  }
  else {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    deep_compact_save_stack(kg, state);
    if (deep_compact_camera_chain(state))
      kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)].flags |=
          DEEP_COMPACT_STOP_SHARING;
    if (deep_native_is_camera_chain(state) && !kernel_integrator_state.deep_native_full_optical) {
      deep_native_capture_error(DEEP_NATIVE_ERROR_HETEROGENEOUS);
    }
#  endif
    if (!volume_integrate_heterogeneous(
            kg, state, ray, sd, *rng_state, render_buffer, vstate, result))
    {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
      if (checkpoint && checkpoint->active) {
        checkpoint->equiangular = equiangular_coeffs;
        checkpoint->light = *ls;
      }
#  endif
      return false;
    }
  }
  /* A homogeneous cache miss has no checkpoint and restarts before all Film side effects. */
  if (sd->flag & SD_CACHE_MISS)
    return false;

  volume_direct_scatter_mis(ray, vstate, equiangular_coeffs, result);
#  if __DEEP_BOUNDED_POLICY__ > 0
  [[maybe_unused]] const bool bounded_permit = !(sd->flag & SD_CACHE_MISS) &&
      deep_bounded_commit_volume(kg,
                                 state,
                                 ray,
                                 sd,
                                 vstate.bounded,
                                 result.indirect_throughput,
                                 result.indirect_scatter,
                                 result.indirect_t,
                                 __DEEP_BOUNDED_POLICY__ >= 2 &&
                                     (!is_zero(vstate.emission) || result.direct_scatter));
#    if __DEEP_BOUNDED_POLICY__ == 3
  result.bounded_colour_permit = bounded_permit;
#    endif
#  endif

  /* Write accumulated emission. */
  if (!is_zero(vstate.emission)) {
    if (light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object)) {
      /* Deep output: emission depth. */
      float volume_depth = -1.0f;
#  ifdef __DEEP_OUTPUT__
      if (kernel_data.film.use_deep_output && (INTEGRATOR_STATE(state, path, bounce) == 0)) {
        float3 P = ray->P + ray->D * vstate.t;
        volume_depth = camera_z_depth(kg, P);
      }
#  endif
      film_write_volume_emission(kg,
                                 state,
                                 vstate.emission,
                                 render_buffer,
                                 volume_depth,
                                 object_lightgroup(kg, sd->object),
                                 vstate.deep_native_segment,
                                 vstate.deep_expression_head
#  if __DEEP_BOUNDED_POLICY__ == 3
                                 , false, bounded_permit
#  endif
                                 );
    }
  }

#  ifdef __DENOISING_FEATURES__
  /* Write denoising features. */
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DENOISING_FEATURES) {
    film_write_denoising_features_volume(
        kg, state, vstate.albedo, result.indirect_scatter, render_buffer);
  }
#  endif /* __DENOISING_FEATURES__ */

  if (INTEGRATOR_STATE(state, path, bounce) == 0) {
    INTEGRATOR_STATE_WRITE(state, path, optical_depth) += vstate.optical_depth;
  }
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (checkpoint && checkpoint->active) {
    checkpoint->active = 0;
    atomic_fetch_and_sub_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_pending,
                                1);
  }
#  endif
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Ray Marching
 * \{ */

/* Determines the next shading position. */
struct VolumeStep {
  /* Shift starting point of all segments by a random amount to avoid banding artifacts due to
   * biased ray marching with insufficient step size. */
  float offset;

  /* Step size taken at each marching step. */
  float size;

  /* Perform shading at this offset within a step, to integrate over the entire step segment. */
  float shade_offset;

  /* Maximal steps allowed between `ray->tmin` and `ray->tmax`. */
  int max_steps;

  /* Current active segment. */
  Interval<float> t;
};

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline void deep_owned_store_optical_checkpoint(
    ConstIntegratorState state,
    const ccl_private Ray *ray,
    ccl_global DeepOpticalCheckpoint *checkpoint,
    const ccl_private VolumeStep &step,
    const int next_step,
    const uint lcg_state,
    const bool cache_miss)
{
  checkpoint->active = 1;
  checkpoint->segment = kernel_integrator_state.deep_optical_ledger[state].segments;
  checkpoint->sample = INTEGRATOR_STATE(state, path, sample);
  checkpoint->ray_tmin = ray->tmin;
  checkpoint->ray_tmax = ray->tmax;
  checkpoint->next_step = next_step;
  checkpoint->lcg_state = lcg_state;
  checkpoint->step_min = step.t.min;
  checkpoint->step_max = step.t.max;
  checkpoint->size = step.size;
  checkpoint->offset = step.offset;
  checkpoint->shade_offset = step.shade_offset;
  checkpoint->max_steps = step.max_steps;
  if (checkpoint->yields == UINT32_MAX || (cache_miss && checkpoint->retries == UINT32_MAX)) {
    deep_owned_optical_error(DEEP_OPTICAL_ERROR_COMPLETION);
    return;
  }
  ++checkpoint->yields;
  checkpoint->retries += uint(cache_miss);
}
#  endif

template<const bool shadow>
ccl_device_forceinline void volume_step_init(KernelGlobals kg,
                                             const ccl_private RNGState *rng_state,
                                             const float object_step_size,
                                             const float tmin,
                                             const float tmax,
                                             ccl_private VolumeStep *vstep)
{
  vstep->t.min = vstep->t.max = tmin;

  if (object_step_size == FLT_MAX) {
    /* Homogeneous volume. */
    vstep->size = tmax - tmin;
    vstep->shade_offset = 0.0f;
    vstep->offset = 1.0f;
    vstep->max_steps = 1;
  }
  else {
    /* Heterogeneous volume. */
    vstep->max_steps = kernel_data.integrator.volume_max_steps;
    const float t = tmax - tmin;
    float step_size = min(object_step_size, t);

    if (t > vstep->max_steps * step_size) {
      /* Increase step size to cover the whole ray segment. */
      step_size = t / (float)vstep->max_steps;
    }

    vstep->size = step_size;
    vstep->shade_offset = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_SHADE_OFFSET);

    if (shadow) {
      /* For shadows we do not offset all segments, since the starting point is already a random
       * distance inside the volume. It also appears to create banding artifacts for unknown
       * reasons. */
      vstep->offset = 1.0f;
    }
    else {
      vstep->offset = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_OFFSET);
    }
  }
}

ccl_device_inline bool volume_ray_marching_advance(const int step,
                                                   const ccl_private Ray *ccl_restrict ray,
                                                   ccl_private float3 *shade_P,
                                                   ccl_private VolumeStep &vstep)
{
  if (vstep.t.max == ray->tmax) {
    /* Reached the last segment. */
    return false;
  }

  /* Advance to new position. */
  vstep.t.min = vstep.t.max;
  vstep.t.max = min(ray->tmax, ray->tmin + (step + vstep.offset) * vstep.size);
  const float shade_t = mix(vstep.t.min, vstep.t.max, vstep.shade_offset);
  *shade_P = ray->P + ray->D * shade_t;

  return step < vstep.max_steps;
}

/* Complete camera Optical includes the jittered terminal remainder even when
 * native Beauty stops at its maximum step count. Evaluate extinction there;
 * this does not add an emission/Beauty step or substitute an opacity tail. */
ccl_device_inline bool volume_owned_optical_advance(const int step,
                                                    const ccl_private Ray *ray,
                                                    ccl_private float3 *shade_P,
                                                    ccl_private VolumeStep &vstep)
{
  if (vstep.t.max == ray->tmax)
    return false;
  const bool native_step = volume_ray_marching_advance(step, ray, shade_P, vstep);
  return native_step ||
         (step == vstep.max_steps && vstep.t.min < vstep.t.max && vstep.t.max == ray->tmax);
}

ccl_device_inline bool volume_deep_warp_step(const int step,
                                             const ccl_private Ray *ccl_restrict ray,
                                             ccl_private float3 *shade_P,
                                             ccl_private VolumeStep &vstep)
{
  /* Reconstruct the previous bound using the same expression as the preceding serial step.
   *
   * This changes execution ownership, not jitter, grid spacing or the evaluated positions. */
  vstep.t.max = step == 0 ? ray->tmin :
                            min(ray->tmax, ray->tmin + ((step - 1) + vstep.offset) * vstep.size);
  return volume_ray_marching_advance(step, ray, shade_P, vstep);
}

ccl_device void volume_shadow_ray_marching(KernelGlobals kg,
                                           IntegratorShadowState state,
                                           ccl_private Ray *ccl_restrict ray,
                                           ccl_private ShaderData *ccl_restrict sd,
                                           ccl_private Spectrum *ccl_restrict throughput,
                                           const float object_step_size)
{
  /* Load random number state. */
  RNGState rng_state;
  shadow_path_state_rng_load(state, &rng_state);

  /* For stochastic texture sampling. */
  sd->lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0xd9111870);

  Spectrum tp = *throughput;

  /* Prepare for stepping. */
  VolumeStep vstep;
  volume_step_init<true>(kg, &rng_state, object_step_size, ray->tmin, ray->tmax, &vstep);

  /* compute extinction at the start */
  Spectrum sum = zero_spectrum();
  for (int step = 0; volume_ray_marching_advance(step, ray, &sd->P, vstep); step++) {
    /* compute attenuation over segment */
    const Spectrum sigma_t = volume_shader_eval_extinction<true>(
        kg, state, sd, PATH_RAY_VISIBILITY_SHADOW, PATH_RAY_FLAG_NONE);
    /* Compute `expf()` only for every Nth step, to save some calculations
     * because `exp(a)*exp(b) = exp(a+b)`, also do a quick #VOLUME_THROUGHPUT_EPSILON
     * check then. */
    sum += (-sigma_t * vstep.t.length());
    if ((step & 0x07) == 0) { /* TODO: Other interval? */
      tp = *throughput * exp(sum);

      /* stop if nearly all light is blocked */
      if (reduce_max(tp) < VOLUME_THROUGHPUT_EPSILON) {
        break;
      }
    }
  }

  if (vstep.t.max == ray->tmax) {
    /* Update throughput in case we haven't done it above. */
    tp = *throughput * exp(sum);
  }

  *throughput = tp;
}

namespace {
struct VolumeRayMarchingState {
#  if __DEEP_BOUNDED_POLICY__ > 0
  DeepBoundedBridge bounded;
#  endif
  /* Random numbers for scattering. */
  float rscatter;
  float rchannel;

  /* Multiple importance sampling. */
  VolumeSampleMethod direct_sample_method;
  bool use_mis;
  float distance_pdf;
  float equiangular_pdf;
};
}  // namespace

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
struct DeepNativeMarchCheckpoint {
  uint active, cache_miss, kind, sample, bounce;
  uint64_t domain;
  Ray ray;
  DeepNativeVolumeShaderState shader;
  EquiangularCoefficients equiangular;
  LightSample light;
  VolumeStep step;
  VolumeRayMarchingState state;
  VolumeIntegrateResult result;
  Spectrum emission, albedo;
  uint expression_head;
  int next_step;
};
static_assert(sizeof(DeepNativeMarchCheckpoint) <= DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES,
              "Native march checkpoint exceeds its charged slot");

ccl_device_inline ccl_global DeepNativeMarchCheckpoint *deep_native_march_checkpoint(
    ConstIntegratorState state)
{
  if (!kernel_integrator_state.deep_native_volume_checkpoints ||
      !deep_native_path_writes_combined(INTEGRATOR_STATE(state, path, flag)))
    return nullptr;
  const uint camera = INTEGRATOR_STATE(state, path, deep_frame_camera_index);
  if (camera >= kernel_integrator_state.deep_native_volume_checkpoint_capacity) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return nullptr;
  }
  return reinterpret_cast<ccl_global DeepNativeMarchCheckpoint *>(
      static_cast<ccl_global char *>(kernel_integrator_state.deep_native_volume_checkpoints) +
      size_t(camera) * DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES);
}

ccl_device_inline bool deep_native_march_checkpoint_matches(
    ConstIntegratorState state,
    const ccl_private Ray *ray,
    const ccl_global DeepNativeMarchCheckpoint *checkpoint)
{
  const Ray previous = checkpoint->ray;
  return checkpoint->kind == 1 &&
         checkpoint->domain == kernel_integrator_state.deep_native_domain_id &&
         checkpoint->sample == INTEGRATOR_STATE(state, path, sample) &&
         checkpoint->bounce == INTEGRATOR_STATE(state, path, bounce) && previous.P.x == ray->P.x &&
         previous.P.y == ray->P.y && previous.P.z == ray->P.z && previous.D.x == ray->D.x &&
         previous.D.y == ray->D.y && previous.D.z == ray->D.z && previous.tmin == ray->tmin &&
         previous.tmax == ray->tmax && previous.time == ray->time;
}

ccl_device_inline void deep_native_store_march_checkpoint(
    ConstIntegratorState state,
    const ccl_private Ray *ray,
    ccl_global DeepNativeMarchCheckpoint *checkpoint,
    const DeepNativeVolumeShaderState shader,
    const ccl_private EquiangularCoefficients &equiangular,
    const ccl_private LightSample &light,
    const ccl_private VolumeStep &step,
    const ccl_private VolumeRayMarchingState &vstate,
    const ccl_private VolumeIntegrateResult &result,
    Spectrum emission,
    Spectrum albedo,
    uint head,
    int next_step,
    bool cache_miss)
{
  checkpoint->kind = 1;
  checkpoint->domain = kernel_integrator_state.deep_native_domain_id;
  checkpoint->sample = INTEGRATOR_STATE(state, path, sample);
  checkpoint->bounce = INTEGRATOR_STATE(state, path, bounce);
  checkpoint->ray.P = ray->P;
  checkpoint->ray.D = ray->D;
  checkpoint->ray.tmin = ray->tmin;
  checkpoint->ray.tmax = ray->tmax;
  checkpoint->ray.time = ray->time;
#    ifdef __RAY_DIFFERENTIALS__
  checkpoint->ray.dP = ray->dP;
  checkpoint->ray.dD = ray->dD;
#    endif
  checkpoint->ray.self = {PRIM_NONE, OBJECT_NONE, PRIM_NONE, OBJECT_NONE};
  checkpoint->shader = shader;
  checkpoint->equiangular = equiangular;
  checkpoint->light = light;
  checkpoint->step = step;
  checkpoint->state = vstate;
  checkpoint->result = result;
  checkpoint->emission = emission;
  checkpoint->albedo = albedo;
  checkpoint->expression_head = head;
  checkpoint->next_step = next_step;
  checkpoint->cache_miss = cache_miss;
  if (!checkpoint->active)
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_pending,
                                1);
  checkpoint->active = 1;
  atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_yields,
                              1);
  if (cache_miss)
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_retries,
                                1);
}
#  endif

ccl_device_inline void volume_ray_marching_state_init(
    KernelGlobals kg,
    const ccl_private RNGState *rng_state,
    const VolumeSampleMethod direct_sample_method,
    ccl_private VolumeRayMarchingState &vstate)
{
  vstate.rscatter = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_SCATTER_DISTANCE);
  vstate.rchannel = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_COLOR_CHANNEL);

  /* Multiple importance sampling: pick between equiangular and distance sampling strategy. */
  vstate.direct_sample_method = direct_sample_method;
  vstate.use_mis = (direct_sample_method == VOLUME_SAMPLE_MIS);
  if (vstate.use_mis) {
    if (vstate.rscatter < 0.5f) {
      vstate.rscatter *= 2.0f;
      vstate.direct_sample_method = VOLUME_SAMPLE_DISTANCE;
    }
    else {
      vstate.rscatter = (vstate.rscatter - 0.5f) * 2.0f;
      vstate.direct_sample_method = VOLUME_SAMPLE_EQUIANGULAR;
    }
  }
  vstate.equiangular_pdf = 0.0f;
  vstate.distance_pdf = 1.0f;
}

/* Returns true if we found the indirect scatter position within the current active ray segment. */
ccl_device bool volume_sample_indirect_scatter_ray_marching(
    const Spectrum transmittance,
    const Spectrum channel_pdf,
    const int channel,
    const ccl_private ShaderData *ccl_restrict sd,
    const ccl_private VolumeShaderCoefficients &ccl_restrict coeff,
    const ccl_private Interval<float> &t,
    ccl_private VolumeRayMarchingState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result)
{
  if (result.indirect_scatter) {
    /* Already sampled indirect scatter position. */
    return false;
  }

  /* If sampled distance does not go beyond the current segment, we have found the scatter
   * position. Otherwise continue searching and accumulate the transmittance along the ray. */
  const float sample_transmittance = volume_channel_get(transmittance, channel);
  if (1.0f - vstate.rscatter >= sample_transmittance) {
    /* Pick `sigma_t` from a random channel. */
    const float sample_sigma_t = volume_channel_get(coeff.sigma_t, channel);

    /* Generate the next distance using random walk, following exponential distribution
     * p(dt) = sigma_t * exp(-sigma_t * dt). */
    const float new_dt = -logf(1.0f - vstate.rscatter) / sample_sigma_t;
    const float new_t = t.min + new_dt;

    const Spectrum new_transmittance = volume_color_transmittance(coeff.sigma_t, new_dt);
    /* PDF for density-based distance sampling is handled implicitly via
     * transmittance / pdf = exp(-sigma_t * dt) / (sigma_t * exp(-sigma_t * dt)) = 1 / sigma_t. */
    const float distance_pdf = dot(channel_pdf, coeff.sigma_t * new_transmittance);

    if (vstate.distance_pdf * distance_pdf > VOLUME_SAMPLE_PDF_CUTOFF) {
      /* Update throughput. */
      result.indirect_scatter = true;
      result.indirect_t = new_t;
      result.indirect_throughput *= coeff.sigma_s * new_transmittance / distance_pdf;
      if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
        vstate.distance_pdf *= distance_pdf;
      }

      volume_shader_copy_phases(&result.indirect_phases, sd);

      return true;
    }
  }
  else {
    /* Update throughput. */
    const float distance_pdf = dot(channel_pdf, transmittance);
    result.indirect_throughput *= transmittance / distance_pdf;
    if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
      vstate.distance_pdf *= distance_pdf;
    }

    /* Remap rscatter so we can reuse it and keep thing stratified. */
    vstate.rscatter = 1.0f - (1.0f - vstate.rscatter) / sample_transmittance;
  }

  return false;
}

/* Find direct and indirect scatter positions. */
ccl_device_forceinline void volume_ray_marching_step_scattering(
    const ccl_private ShaderData *sd,
    const ccl_private Ray *ray,
    const ccl_private EquiangularCoefficients &equiangular_coeffs,
    const ccl_private VolumeShaderCoefficients &ccl_restrict coeff,
    const Spectrum transmittance,
    const ccl_private Interval<float> &t,
    ccl_private VolumeRayMarchingState &ccl_restrict vstate,
    ccl_private VolumeIntegrateResult &ccl_restrict result)
{
  /* Pick random color channel for sampling the scatter distance. We use the Veach one-sample model
   * with balance heuristic for the channels.
   * Set `albedo` to 1 for the channel where extinction coefficient `sigma_t` is zero, to make sure
   * that we sample a distance outside the current segment when that channel is picked, meaning
   * light passes through without attenuation. */
  const Spectrum albedo = safe_divide_color(coeff.sigma_s, coeff.sigma_t, 1.0f);
  Spectrum channel_pdf;
  const int channel = volume_sample_channel(
      albedo, result.indirect_throughput, &vstate.rchannel, &channel_pdf);

  /* Equiangular sampling for direct lighting. */
  if (vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR && !result.direct_scatter) {
    if (t.contains(result.direct_t) && vstate.equiangular_pdf > VOLUME_SAMPLE_PDF_CUTOFF) {
      const float new_dt = result.direct_t - t.min;
      const Spectrum new_transmittance = volume_color_transmittance(coeff.sigma_t, new_dt);

      result.direct_scatter = true;
      result.direct_throughput *= safe_divide(coeff.sigma_s * new_transmittance,
                                              vstate.equiangular_pdf);
      volume_shader_copy_phases(&result.direct_phases, sd);

      /* Multiple importance sampling. */
      if (vstate.use_mis) {
        const float distance_pdf = vstate.distance_pdf *
                                   dot(channel_pdf, coeff.sigma_t * new_transmittance);
        const float mis_weight = 2.0f * power_heuristic(vstate.equiangular_pdf, distance_pdf);
        result.direct_throughput *= mis_weight;
      }
    }
    else {
      result.direct_throughput *= transmittance;
      vstate.distance_pdf *= dot(channel_pdf, transmittance);
    }
  }

  /* Distance sampling for indirect and optional direct lighting. */
  if (volume_sample_indirect_scatter_ray_marching(
          transmittance, channel_pdf, channel, sd, coeff, t, vstate, result))
  {
    if (vstate.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
      /* If using distance sampling for direct light, just copy parameters of indirect light
       * since we scatter at the same point. */
      result.direct_scatter = true;
      result.direct_t = result.indirect_t;
      result.direct_throughput = result.indirect_throughput;
      volume_shader_copy_phases(&result.direct_phases, sd);

      /* Multiple importance sampling. */
      if (vstate.use_mis) {
        const float equiangular_pdf = volume_equiangular_pdf(
            ray, equiangular_coeffs, result.indirect_t);
        const float mis_weight = power_heuristic(vstate.distance_pdf, equiangular_pdf);
        result.direct_throughput *= 2.0f * mis_weight;
      }
    }
  }
}

/* heterogeneous volume distance sampling: integrate stepping through the
 * volume until we reach the end, get absorbed entirely, or run out of
 * iterations. this does probabilistically scatter or get transmitted through
 * for path tracing where we don't want to branch. */
ccl_device_forceinline bool volume_integrate_ray_marching(
    KernelGlobals kg,
    const IntegratorState state,
    const ccl_private Ray *ccl_restrict ray,
    ccl_private ShaderData *ccl_restrict sd,
    const ccl_private RNGState *ccl_restrict rng_state,
    ccl_global float *ccl_restrict render_buffer,
    const float object_step_size,
    ccl_private LightSample *ls,
    ccl_private VolumeIntegrateResult &result)
{
  PROFILING_INIT(kg, PROFILING_SHADE_VOLUME_INTEGRATE);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  deep_compact_save_stack(kg, state);
#  endif

  EquiangularCoefficients equiangular_coeffs = {zero_float3(), {ray->tmin, ray->tmax}};
  VolumeStep vstep ccl_optional_struct_init;
  VolumeRayMarchingState vstate ccl_optional_struct_init;
  bool resumed = false;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  auto *checkpoint = deep_native_march_checkpoint(state);
  resumed = checkpoint && checkpoint->active;
  if (resumed && !deep_native_march_checkpoint_matches(state, ray, checkpoint)) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_IDENTITY);
    return false;
  }
  if (resumed) {
    equiangular_coeffs = checkpoint->equiangular;
    *ls = checkpoint->light;
    vstep = checkpoint->step;
    vstate = checkpoint->state;
    result = checkpoint->result;
    deep_native_restore_volume_shader(sd, checkpoint->shader);
  }
#  endif
  if (!resumed) {
    const VolumeSampleMethod direct_sample_method = volume_direct_sample_method(
        kg, state, ray, sd, rng_state, &equiangular_coeffs, ls);
    volume_step_init<false>(kg, rng_state, object_step_size, ray->tmin, ray->tmax, &vstep);
    volume_ray_marching_state_init(kg, rng_state, direct_sample_method, vstate);
#  if __DEEP_BOUNDED_POLICY__ > 0
    vstate.bounded = {ray->tmin,
                      deep_bounded_camera_chain(kg, state) ? BOUNDED_VOLUME_TRACKING :
                                                             BOUNDED_VOLUME_INACTIVE};
#  endif
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (kernel_integrator_state.deep_compact_emission && deep_native_is_camera_chain(state)) {
      DeepCompactEmissionState emission = {};
      emission.front = emission.back = ray->tmin;
      emission.flags = volume_is_homogeneous<false>(kg, state) ? 1u : 0u;
      kernel_integrator_state.deep_compact_emission[
          INTEGRATOR_STATE(state, path, deep_frame_camera_index)] = emission;
    }
#  endif
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    result.direct_throughput = (vstate.direct_sample_method == VOLUME_SAMPLE_NONE) ?
                                   zero_spectrum() :
                                   throughput;
    result.indirect_throughput = throughput;
  }
#  ifdef __DEEP_OUTPUT__
  const bool write_deep = kernel_data.film.use_deep_output == 1 &&
                          (INTEGRATOR_STATE(state, path, bounce) == 0);
  ccl_global KernelDeepSample *deep_samples =
      write_deep ? (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr : nullptr;
  ccl_global uint32_t *deep_sample_counts =
      write_deep ? (ccl_global uint32_t *)kernel_data.film.deep_sample_counts_ptr : nullptr;
  const uint32_t deep_pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  uint32_t last_deep_sample_idx = DEEP_INVALID_SAMPLE_INDEX;
#  endif

  /* Equiangular sampling: compute distance and PDF in advance. */
  if (!resumed && vstate.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) {
    result.direct_t = volume_equiangular_sample(
        ray, equiangular_coeffs, vstate.rscatter, &vstate.equiangular_pdf);
  }
#  if defined(__PATH_GUIDING__)
  result.direct_sample_method = vstate.direct_sample_method;
#  endif

#  ifdef __DENOISING_FEATURES__
  const bool write_denoising_features = (INTEGRATOR_STATE(state, path, flag) &
                                         PATH_RAY_DENOISING_FEATURES);
  Spectrum accum_albedo = zero_spectrum();
#  endif
  Spectrum accum_emission = zero_spectrum();
  uint32_t deep_expression_head = DEEP_NATIVE_INVALID_INDEX;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_native_expression_nodes && deep_native_is_camera_chain(state))
    deep_expression_head = 0;
#  endif

  int first_step = 0;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  int completed_steps = 0;
  if (resumed) {
    accum_emission = checkpoint->emission;
#    ifdef __DENOISING_FEATURES__
    accum_albedo = checkpoint->albedo;
#    endif
    deep_expression_head = checkpoint->expression_head;
    first_step = checkpoint->next_step;
  }
#  endif
  for (int step = first_step;; step++) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const VolumeStep before_step = vstep;
#  endif
    if (!volume_ray_marching_advance(step, ray, &sd->P, vstep))
      break;
    /* Evaluate before changing any emission, throughput or Film transaction. */
    VolumeShaderCoefficients coeff ccl_optional_struct_init;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const DeepNativeVolumeShaderState before_shader = deep_native_volume_shader_state(sd);
#  endif
    const bool has_coefficients = volume_shader_sample(kg, state, sd, &coeff);
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (checkpoint && (sd->flag & SD_CACHE_MISS)) {
      deep_native_store_march_checkpoint(state,
                                         ray,
                                         checkpoint,
                                         before_shader,
                                         equiangular_coeffs,
                                         *ls,
                                         before_step,
                                         vstate,
                                         result,
                                         accum_emission,
#    ifdef __DENOISING_FEATURES__
                                         accum_albedo,
#    else
                                         zero_spectrum(),
#    endif
                                         deep_expression_head,
                                         step,
                                         true);
      return false;
    }
#  endif
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (deep_compact_camera_chain(state)) {
      const uint i = deep_compact_camera_index(state);
      deep_compact_cell(kg, state, vstep.t.min, vstep.t.max,
                        has_coefficients ? (double(coeff.sigma_t.x) + coeff.sigma_t.y + coeff.sigma_t.z) / 3.0 : 0.0,
                        kernel_integrator_state.deep_compact_rays[i].reserved);
    }
#  endif
    if (has_coefficients) {
      const int closure_flag = sd->flag;

      /* Evaluate transmittance over segment. */
      const float dt = vstep.t.length();
      const Spectrum transmittance = (closure_flag & SD_EXTINCTION) ?
                                         volume_color_transmittance(coeff.sigma_t, dt) :
                                         one_spectrum();

      /* Emission. */
      Spectrum segment_emission = zero_spectrum();
      if (closure_flag & SD_EMISSION) {
        /* Only write emission before indirect light scatter position, since we terminate
         * stepping at that point if we have already found a direct light scatter position. */
        if (!result.indirect_scatter) {
          const Spectrum emission = volume_emission_integrate(&coeff, closure_flag, dt);
          segment_emission = result.indirect_throughput * emission;
          accum_emission += segment_emission;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
          if (kernel_integrator_state.deep_compact_emission && deep_native_is_camera_chain(state) &&
              !is_zero(segment_emission)) {
            auto &source = kernel_integrator_state.deep_compact_emission[
                INTEGRATOR_STATE(state, path, deep_frame_camera_index)];
            /* A previous stochastic distance-PDF update changes the step
             * weights. A material's constant sigma alone cannot certify G. */
            if (source.flags & 8u) source.flags &= ~1u;
            if (!(source.flags & 2u)) {
              source.sigma[0] = coeff.sigma_t.x; source.sigma[1] = coeff.sigma_t.y;
              source.sigma[2] = coeff.sigma_t.z;
            }
            else if (source.sigma[0] != coeff.sigma_t.x || source.sigma[1] != coeff.sigma_t.y ||
                     source.sigma[2] != coeff.sigma_t.z)
              source.flags &= ~1u;
            source.flags |= 2u;
            source.back = vstep.t.max;
            if (-expm1(-(double(coeff.sigma_t.x) + coeff.sigma_t.y + coeff.sigma_t.z) * dt / 3.0) > 0)
              source.flags |= 4u;
          }
#  endif
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
          if (deep_expression_head != DEEP_NATIVE_INVALID_INDEX) {
            const uint32_t term = deep_native_expression_term(kg,
                                                              state,
                                                              ray,
                                                              sd,
                                                              vstep.t.min,
                                                              vstep.t.max,
                                                              segment_emission,
                                                              coeff.sigma_t * dt,
                                                              false,
                                                              coeff.sigma_t);
            deep_expression_head = deep_native_expression_node(
                state, DEEP_NATIVE_EXPR_ADD, deep_expression_head, term);
          }
#  endif
          guiding_record_volume_emission(kg, state, emission);
        }
      }

      /* Deep output: ray-marched segments. */
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
      if (kernel_integrator_state.deep_volume_emission_spans) {
        const float span_alpha = 1.0f - saturatef(average(transmittance));
        volume_capture_primary_emission_span(
            kg,
            state,
            ray,
            vstep.t.min,
            vstep.t.max,
            segment_emission,
            coeff.sigma_t * dt,
            span_alpha,
            DEEP_EMISSION_MARCHED |
                (span_alpha > 1e-6f ? DEEP_EMISSION_LEGACY_OPACITY_ELIGIBLE : 0u));
      }
#  endif
#  ifdef __DEEP_OUTPUT__
      /* Write deep sample for this volume segment (alpha-only, RGB=0).
       * Volume color is applied via deep recolor in compositing.
       *
       * We write PER-SEGMENT alpha (opacity of just this segment),
       * not accumulated. Deep compositing expects each sample to represent
       * independent opacity - the compositor will accumulate front-to-back. */
      if (write_deep) {
        /* Per-segment opacity from physical transmittance.
         * This avoids throughput-ratio artifacts and lets Deep Recolor scale to match
         * the Combined alpha. */
        float segment_alpha = 1.0f - saturatef(average(transmittance));

        const float deep_segment_alpha_epsilon = 1e-6f;
        if (segment_alpha > deep_segment_alpha_epsilon) {
          if (deep_samples && deep_sample_counts) {
            /* Calculate front and back Z depths from ray positions. */
            float3 P_front = ray->P + ray->D * vstep.t.min;
            float3 P_back = ray->P + ray->D * vstep.t.max;
            float z_front = camera_z_depth(kg, P_front);
            float z_back = camera_z_depth(kg, P_back);

            /* Write per-segment alpha (RGB=0), color applied via deep recolor. */
            last_deep_sample_idx = film_write_deep_sample_volume(
                kg,
                deep_pixel_index,
                deep_samples,
                deep_sample_counts,
                segment_alpha,
                z_front,
                z_back,
                INTEGRATOR_STATE(state, path, sample),
                segment_emission,
                coeff.sigma_t * dt);
          }
        }
      }
#  endif

      if (closure_flag & SD_SCATTER) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
        if (kernel_integrator_state.deep_compact_emission && deep_native_is_camera_chain(state))
          kernel_integrator_state.deep_compact_emission[
              INTEGRATOR_STATE(state, path, deep_frame_camera_index)].flags |= 8u;
#  endif
#  ifdef __DENOISING_FEATURES__
        /* Accumulate albedo for denoising features. */
        if (write_denoising_features && (closure_flag & SD_SCATTER)) {
          const Spectrum albedo = safe_divide_color(coeff.sigma_s, coeff.sigma_t);
          accum_albedo += result.indirect_throughput * albedo * (one_spectrum() - transmittance);
        }
#  endif

        /* Scattering and absorption. */
        volume_ray_marching_step_scattering(
            sd, ray, equiangular_coeffs, coeff, transmittance, vstep.t, vstate, result);
      }
      else if (closure_flag & SD_EXTINCTION) {
        /* Absorption only. */
        result.indirect_throughput *= transmittance;
        result.direct_throughput *= transmittance;
      }

#  if __DEEP_BOUNDED_POLICY__ > 0
      if (vstate.bounded.reason == BOUNDED_VOLUME_TRACKING) {
        vstate.bounded.event_t = vstep.t.max;
#    if __DEEP_BOUNDED_POLICY__ >= 2
        vstate.bounded.has_extinction |= (closure_flag & SD_EXTINCTION) && dt > 0.0f &&
                                         !is_zero(coeff.sigma_t);
#    endif
      }
#  endif
      if (volume_integrate_should_stop(result)) {
        break;
      }
    }
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (checkpoint &&
        ++completed_steps >= int(kernel_integrator_state.deep_native_volume_step_limit) &&
        vstep.t.max != ray->tmax && step + 1 < vstep.max_steps)
    {
      deep_native_store_march_checkpoint(state,
                                         ray,
                                         checkpoint,
                                         deep_native_volume_shader_state(sd),
                                         equiangular_coeffs,
                                         *ls,
                                         vstep,
                                         vstate,
                                         result,
                                         accum_emission,
#    ifdef __DENOISING_FEATURES__
                                         accum_albedo,
#    else
                                         zero_spectrum(),
#    endif
                                         deep_expression_head,
                                         step + 1,
                                         false);
      return false;
    }
#  endif
  }

#  ifdef __DEEP_OUTPUT__
  if (last_deep_sample_idx != DEEP_INVALID_SAMPLE_INDEX) {
    uint32_t deep_ray_flags = DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE;
    if (result.indirect_scatter) {
      deep_ray_flags |= DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED;
    }
    film_mark_deep_volume_primary_ray_flag(
        kg, deep_pixel_index, last_deep_sample_idx, deep_samples, deep_ray_flags);
  }
#  endif

#  if __DEEP_BOUNDED_POLICY__ > 0
  const bool bounded_success = !(sd->flag & SD_CACHE_MISS);
  if (bounded_success && vstate.bounded.reason == BOUNDED_VOLUME_TRACKING) {
    vstate.bounded.reason = BOUNDED_VOLUME_INTERVAL;
  }
  [[maybe_unused]] const bool bounded_permit = bounded_success &&
      deep_bounded_commit_volume(kg,
                                 state,
                                 ray,
                                 sd,
                                 vstate.bounded,
                                 result.indirect_throughput,
                                 result.indirect_scatter,
                                 result.indirect_t,
                                 __DEEP_BOUNDED_POLICY__ >= 2 &&
                                     (!is_zero(accum_emission) || result.direct_scatter));
#    if __DEEP_BOUNDED_POLICY__ == 3
  result.bounded_colour_permit = bounded_permit;
#    endif
#  endif
  /* Write accumulated emission. */
  if (!is_zero(accum_emission)) {
    if (light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object)) {
      /* Capture the supported native uniform span only after its actual support closes. */
      uint32_t native_uniform = DEEP_NATIVE_INVALID_INDEX;
      bool discard_deep_emission = false;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
      if (kernel_integrator_state.deep_compact_emission && deep_native_is_camera_chain(state)) {
        const auto &source = kernel_integrator_state.deep_compact_emission[
            INTEGRATOR_STATE(state, path, deep_frame_camera_index)];
        if ((source.flags & 3u) == 3u && source.back > source.front) {
          const double sigma = (double(source.sigma[0]) + source.sigma[1] + source.sigma[2]) / 3.0;
          discard_deep_emission = -expm1(-sigma * (double(source.back) - source.front)) == 0;
          if (!discard_deep_emission) {
            Ray span = *ray;
            span.tmin = source.front; span.tmax = source.back;
            VolumeShaderCoefficients uniform = {};
            uniform.sigma_t = make_float3(source.sigma[0], source.sigma[1], source.sigma[2]);
            native_uniform = volume_capture_native_homogeneous(kg, state, &span, sd, uniform);
          }
        }
        else discard_deep_emission = !(source.flags & 4u);
      }
#  endif
      film_write_volume_emission(kg,
                                 state,
                                 accum_emission,
                                 render_buffer,
                                 -1.0f,
                                 object_lightgroup(kg, sd->object),
                                 native_uniform,
                                 deep_expression_head,
                                 discard_deep_emission
#  if __DEEP_BOUNDED_POLICY__ == 3
                                 , bounded_permit
#  endif
                                 );
    }
  }

#  ifdef __DENOISING_FEATURES__
  /* Write denoising features. */
  if (write_denoising_features) {
    film_write_denoising_features_volume(
        kg, state, accum_albedo, result.indirect_scatter, render_buffer);
  }
#  endif /* __DENOISING_FEATURES__ */
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (checkpoint && checkpoint->active) {
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_native_volume_pending,
                                uint32_t(-1));
    checkpoint->active = 0;
  }
#  endif
  return true;
}

#  ifdef __DEEP_OUTPUT__
/* Capture extinction for one geometry-bounded segment of the dedicated camera optical traversal.

 * * The integrator kernels advance that traversal across all volume boundaries before restarting

 * * the regular Beauty path. */
ccl_device_forceinline uint32_t
volume_integrate_deep_optical_profile(KernelGlobals kg,
                                      const IntegratorState state,
                                      const ccl_private Ray *ccl_restrict ray,
                                      ccl_private ShaderData *ccl_restrict sd,
                                      const ccl_private RNGState *ccl_restrict rng_state)
{
#    ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_optical_inputs) {
    const float step_size = volume_stack_step_size<false>(kg, state);
    VolumeStep vstep;
    volume_step_init<false>(kg, rng_state, step_size, ray->tmin, ray->tmax, &vstep);
    const uint segment = kernel_integrator_state.deep_optical_ledger[state].segments;
    if (kernel_integrator_state.deep_compact_rays) {
      uint hash = 2166136261u;
      for (int i = 0; i < kernel_data.volume_stack_size; i++) {
        const int shader = INTEGRATOR_STATE_ARRAY(state, volume_stack, i, shader);
        if (shader == SHADER_NONE) break;
        const int object = INTEGRATOR_STATE_ARRAY(state, volume_stack, i, object);
        if (shader != SHADER_NONE && object == OBJECT_NONE) {
          deep_owned_optical_error(DEEP_OPTICAL_ERROR_STACK);
          return DEEP_INVALID_SAMPLE_INDEX;
        }
        hash = (hash ^ uint(object)) * 16777619u;
        hash = (hash ^ uint(shader)) * 16777619u;
      }
      kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)].reserved = hash;
    }
    auto *checkpoint = kernel_integrator_state.deep_owned_optical_checkpoints ?
                           &kernel_integrator_state.deep_owned_optical_checkpoints[state] :
                           nullptr;
    int first_step = 0;
    if (checkpoint && checkpoint->active) {
      if (checkpoint->segment != segment ||
          checkpoint->sample != INTEGRATOR_STATE(state, path, sample) ||
          checkpoint->ray_tmin != ray->tmin || checkpoint->ray_tmax != ray->tmax)
      {
        deep_owned_optical_error(DEEP_OPTICAL_ERROR_COMPLETION);
        return DEEP_INVALID_SAMPLE_INDEX;
      }
      vstep.t = {checkpoint->step_min, checkpoint->step_max};
      vstep.size = checkpoint->size;
      vstep.offset = checkpoint->offset;
      vstep.shade_offset = checkpoint->shade_offset;
      vstep.max_steps = checkpoint->max_steps;
      first_step = checkpoint->next_step;
      sd->lcg_state = checkpoint->lcg_state;
    }
    if (kernel_integrator_state.deep_compact_rays) {
      auto &scalar = kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)];
      if (scalar.flags & DEEP_COMPACT_PENDING_CELL) {
        const float pending_back = scalar.pending_back;
        if (!deep_compact_cell(kg, state, scalar.pending_front, pending_back,
                               scalar.pending_sigma, scalar.pending_phase))
          return DEEP_INVALID_SAMPLE_INDEX;
        auto &coverage = kernel_integrator_state.deep_owned_optical_coverage[state];
        coverage.cursor_t = pending_back;
        coverage.z_back = deep_compact_depth(kg,
            &kernel_integrator_state.deep_compact_cameras[deep_compact_camera_index(state)], pending_back);
      }
    }
    float integrated_end = vstep.t.max;
    uint completed_steps = 0;
    /* Same native grid/extinction/RNG, independent tau sink with no alpha-threshold deletion. */
    for (int step = first_step;; ++step) {
      const VolumeStep before_step = vstep;
      const uint before_lcg = sd->lcg_state;
      if (!volume_owned_optical_advance(step, ray, &sd->P, vstep))
        break;
      const Spectrum sigma_t = volume_shader_eval_extinction<false>(
          kg,
          state,
          sd,
          INTEGRATOR_STATE(state, path, visibility),
          INTEGRATOR_STATE(state, path, flag));
      if (sd->flag & SD_CACHE_MISS) {
        if (checkpoint) {
          deep_owned_store_optical_checkpoint(
              state, ray, checkpoint, before_step, step, before_lcg, true);
          return DEEP_INVALID_SAMPLE_INDEX;
        }
        break; /* Diagnostic-only traversals have no retry checkpoint. */
      }
      if (is_zero(sigma_t) && !kernel_integrator_state.deep_owned_optical_pieces &&
          !kernel_integrator_state.deep_compact_rays) {
        continue;
      }
      const Spectrum tau = sigma_t * vstep.t.length();
      if (kernel_integrator_state.deep_owned_optical_pieces || kernel_integrator_state.deep_compact_rays) {
        if (!deep_owned_optical_piece(
                kg, state, vstep.t.min, vstep.t.max, tau, DEEP_OWNED_OPTICAL_MEDIUM,
                double(average(sigma_t))))
        {
          if (checkpoint && kernel_integrator_state.deep_compact_rays &&
              !kernel_integrator_state.queue_counter->deep_native_capture_error) {
            deep_owned_store_optical_checkpoint(
                state, ray, checkpoint, vstep, step + 1, sd->lcg_state, false);
            return DEEP_INVALID_SAMPLE_INDEX;
          }
          break;
        }
        integrated_end = vstep.t.max;
        if (checkpoint &&
            ++completed_steps >= kernel_integrator_state.deep_owned_optical_operation_steps &&
            integrated_end != ray->tmax)
        {
          deep_owned_store_optical_checkpoint(
              state, ray, checkpoint, vstep, step + 1, sd->lcg_state, false);
          return DEEP_INVALID_SAMPLE_INDEX;
        }
        continue;
      }
      const float z = camera_z_depth(kg, ray->P + ray->D * vstep.t.min);
      const float zback = camera_z_depth(kg, ray->P + ray->D * vstep.t.max);
      if (!isfinite_safe(tau.x) || !isfinite_safe(tau.y) || !isfinite_safe(tau.z) ||
          !isfinite_safe(z) || !isfinite_safe(zback) || tau.x < 0.0f || tau.y < 0.0f ||
          tau.z < 0.0f || zback < z)
      {
        atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_optical_error,
                                   DEEP_OPTICAL_ERROR_NONFINITE);
        break;
      }
      const uint index = atomic_fetch_and_add_uint32(
          &kernel_integrator_state.queue_counter->deep_optical_event_count, 1);
      if (index >= kernel_integrator_state.deep_optical_event_capacity) {
        atomic_fetch_and_or_uint32(&kernel_integrator_state.queue_counter->deep_optical_error,
                                   DEEP_OPTICAL_ERROR_CAPACITY);
        break;
      }
      DeepOpticalEvent event = {};
      event.receipt = state;
      event.segment = segment;
      event.z = z;
      event.zback = zback;
      event.tau[0] = tau.x;
      event.tau[1] = tau.y;
      event.tau[2] = tau.z;
      kernel_integrator_state.deep_optical_events[index] = event;
      ++kernel_integrator_state.deep_optical_ledger[state].events;
      integrated_end = vstep.t.max;
    }
    if (checkpoint)
      checkpoint->active = 0;
    if (integrated_end != ray->tmax && !(sd->flag & SD_CACHE_MISS) &&
        !kernel_integrator_state.queue_counter->deep_optical_error)
    {
      deep_owned_optical_bounds(
          kg, state, 6, integrated_end, ray->tmax, first_step, vstep.max_steps, zero_spectrum());
    }
    return DEEP_INVALID_SAMPLE_INDEX;
  }
#    endif
  if (kernel_data.film.use_deep_output < 2 || INTEGRATOR_STATE(state, path, bounce) != 0) {
    return DEEP_INVALID_SAMPLE_INDEX;
  }

  ccl_global KernelDeepSample *deep_samples = (ccl_global KernelDeepSample *)
                                                  kernel_data.film.deep_samples_ptr;
  ccl_global uint32_t *deep_sample_counts = (ccl_global uint32_t *)
                                                kernel_data.film.deep_sample_counts_ptr;
  if (!deep_samples || !deep_sample_counts) {
    return DEEP_INVALID_SAMPLE_INDEX;
  }

  const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  const float step_size = volume_stack_step_size<false>(kg, state);
  VolumeStep vstep;
  volume_step_init<false>(kg, rng_state, step_size, ray->tmin, ray->tmax, &vstep);

  uint32_t last_deep_sample_idx = DEEP_INVALID_SAMPLE_INDEX;
  for (int step = 0; volume_ray_marching_advance(step, ray, &sd->P, vstep); step++) {
    const Spectrum sigma_t = volume_shader_eval_extinction<false>(
        kg, state, sd, path_visibility, path_flag);
    if (is_zero(sigma_t)) {
      continue;
    }

    const Spectrum transmittance = volume_color_transmittance(sigma_t, vstep.t.length());
    const float segment_alpha = 1.0f - saturatef(average(transmittance));
    /* Store finite nonzero extinction before production compaction. A per-step opacity floor
     * can delete an arbitrarily large accumulated prefix over many weak steps. */

    const float3 P_front = ray->P + ray->D * vstep.t.min;
    const float3 P_back = ray->P + ray->D * vstep.t.max;
    last_deep_sample_idx = film_write_deep_sample_volume(kg,
                                                         pixel_index,
                                                         deep_samples,
                                                         deep_sample_counts,
                                                         segment_alpha,
                                                         camera_z_depth(kg, P_front),
                                                         camera_z_depth(kg, P_back),
                                                         INTEGRATOR_STATE(state, path, sample),
                                                         zero_spectrum(),
                                                         sigma_t * vstep.t.length());
  }

  return last_deep_sample_idx;
}

#    if defined(__KERNEL_CUDA__) && !defined(__KERNEL_OPTIX__)
ccl_device_forceinline uint32_t
volume_integrate_deep_optical_profile_warp(KernelGlobals kg,
                                           const IntegratorState state,
                                           const ccl_private Ray *ccl_restrict ray,
                                           ccl_private ShaderData *ccl_restrict sd,
                                           const ccl_private RNGState *ccl_restrict rng_state,
                                           bool &cache_miss)
{
  constexpr uint32_t mask = 0xffffffffu;
  const uint32_t lane = threadIdx.x & 31u;
  cache_miss = false;
  ccl_global KernelDeepSample *deep_samples = (ccl_global KernelDeepSample *)
                                                  kernel_data.film.deep_samples_ptr;
  ccl_global uint32_t *counts = (ccl_global uint32_t *)kernel_data.film.deep_sample_counts_ptr;
  const uint32_t pixels = kernel_data.film.deep_width * kernel_data.film.deep_height;
  const uint32_t pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  if (kernel_data.film.use_deep_output < 2 || INTEGRATOR_STATE(state, path, bounce) != 0 ||
      !deep_samples || !counts || pixel >= pixels)
  {
    return DEEP_INVALID_SAMPLE_INDEX;
  }
  const uint64_t capacity = uint64_t(pixels) * kernel_data.film.deep_max_samples;
  const PathRayVisibility visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  VolumeStep vstep;
  volume_step_init<false>(
      kg, rng_state, volume_stack_step_size<false>(kg, state), ray->tmin, ray->tmax, &vstep);

  uint32_t last_sample = DEEP_INVALID_SAMPLE_INDEX;
  for (int base = 0; base < vstep.max_steps; base += 32) {
    bool keep = false;
    Spectrum sigma_t = zero_spectrum();
    float alpha = 0.0f;
    const bool active = volume_deep_warp_step(base + int(lane), ray, &sd->P, vstep);
    if (!__any_sync(mask, active)) {
      break;
    }
    if (active) {
      sigma_t = volume_shader_eval_extinction<false>(kg, state, sd, visibility, path_flag);
      if (!is_zero(sigma_t)) {
        const Spectrum transmittance = volume_color_transmittance(sigma_t, vstep.t.length());
        alpha = 1.0f - saturatef(average(transmittance));
        keep = true;
      }
    }

    const uint32_t kept = __ballot_sync(mask, keep);
    uint32_t sample = DEEP_INVALID_SAMPLE_INDEX;
    if (kept) {
      const uint32_t length = __popc(kept);
      uint32_t first = 0;
      if (lane == 0) {
        first = atomic_fetch_and_add_uint32(counts + pixels, length);
        const uint32_t valid = uint64_t(first) < capacity ?
                                   uint32_t(min(uint64_t(length), capacity - first)) :
                                   0;
        atomic_fetch_and_add_uint32(counts + pixel, valid);
        if (valid != length) {
          atomic_fetch_and_or_uint32(counts + pixel, DEEP_SAMPLE_COUNT_OVERFLOW);
        }
      }
      first = __shfl_sync(mask, first, 0);
      if (keep) {
        const uint32_t rank = __popc(kept & ((1u << lane) - 1u));
        sample = film_write_deep_sample_volume<true>(
            kg,
            pixel,
            deep_samples,
            counts,
            alpha,
            camera_z_depth(kg, ray->P + ray->D * vstep.t.min),
            camera_z_depth(kg, ray->P + ray->D * vstep.t.max),
            INTEGRATOR_STATE(state, path, sample),
            zero_spectrum(),
            sigma_t * vstep.t.length(),
            first + rank);
      }
    }
    const uint32_t written = __ballot_sync(mask, sample != DEEP_INVALID_SAMPLE_INDEX);
    if (written) {
      last_sample = __shfl_sync(mask, sample, 31 - __clz(written));
    }
    /* Complete every group's stores before another group can be reserved or marked finished. */
    __syncwarp(mask);
  }
  cache_miss = __any_sync(mask, (sd->flag & SD_CACHE_MISS) != 0);
  return last_sample;
}
#    endif
#  endif

/** \} */

/* Path tracing: sample point on light and evaluate light shader, then
 * queue shadow ray to be traced. */
ccl_device_forceinline void integrate_volume_direct_light(
    KernelGlobals kg,
    IntegratorState state,
    const ccl_private ShaderData *ccl_restrict sd,
    const ccl_private RNGState *ccl_restrict rng_state,
    const float3 P,
    const ccl_private ShaderVolumePhases *ccl_restrict phases,
#  if defined(__PATH_GUIDING__)
    const ccl_private Spectrum unlit_throughput,
#  endif
    const ccl_private Spectrum throughput,
    ccl_private LightSample &ccl_restrict ls,
    const float deep_source_t
#  if __DEEP_BOUNDED_POLICY__ == 3
    , const bool bounded_colour_permit
#  endif
    )
{
  PROFILING_INIT(kg, PROFILING_SHADE_VOLUME_DIRECT_LIGHT);

  if (!kernel_data.integrator.use_direct_light || ls.emitter_id == EMITTER_NONE) {
    return;
  }

  /* Sample position on the same light again, now from the shading point where we scattered. */
  {
    const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
    const uint bounce = INTEGRATOR_STATE(state, path, bounce);
    const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);
    const float3 N = zero_float3();
    const int object_receiver = light_link_receiver_nee(kg, sd);
    const int shader_flags = SD_BSDF_HAS_TRANSMISSION;

    if (!light_sample<false>(
            kg, rand_light, sd->time, P, N, object_receiver, shader_flags, bounce, path_flag, &ls))
    {
      return;
    }
  }

  if (ls.shader & SHADER_EXCLUDE_SCATTER) {
    return;
  }

  /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
  Spectrum light_shader_eval ccl_optional_struct_init;
  const bool is_constant_light_shader = light_sample_shader_eval_nee_constant(
      kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

  /* Evaluate BSDF. */
  BsdfEval phase_eval ccl_optional_struct_init;
  const float phase_pdf = volume_shader_phase_eval(
      kg, state, sd, phases, ls.D, &phase_eval, ls.shader);
  const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, phase_pdf);
  bsdf_eval_mul(&phase_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);

  /* Path termination for constant light shader. */
  if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
    const float terminate = path_state_rng_light_termination(kg, rng_state);
    if (light_sample_terminate(kg, &phase_eval, terminate)) {
      return;
    }
  }
  /* For non-constant light shader, probabilistic termination happens in
   * SHADE_LIGHT_NEE when the full contribution is known. */
  else if (bsdf_eval_is_zero(&phase_eval)) {
    return;
  }

  /* Create shadow ray. */
  Ray ray ccl_optional_struct_init;
  light_sample_to_volume_shadow_ray(sd, &ls, P, &ray);

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg,
      state,
      (is_constant_light_shader) ? DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW :
                                   DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE,
      false);

#  if __DEEP_BOUNDED_POLICY__ == 3
  if (deep_bounded_active(kg) && !bounded_colour_permit) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, deep_bounded_slot_kind) =
        deep_bounded_source(0, DeepBoundedSourceKind::EXCLUDED);
  }
#  endif
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (deep_native_is_camera_chain(state)) {
    const uint root = deep_native_append_root(
        state,
        DEEP_NATIVE_ROOT_VOLUME_DIRECT,
        camera_z_depth(kg, P),
        sd->object,
        sd->prim,
        sd->shader,
        deep_source_t,
        0,
        true,
        false,
        volume_native_world_support(kg, state, deep_source_t));
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, deep_frame_root_handle) = root;
  }
#  endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, &ray);
  integrator_state_write_shadow_ray_self(shadow_state, &ray);

  /* Copy state from main path to shadow path. */
  const uint16_t bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint16_t transparent_bounce = INTEGRATOR_STATE(state, path, transparent_bounce);
  PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);
  const Spectrum phase_sum = bsdf_eval_sum(&phase_eval);
  const Spectrum throughput_phase = throughput * phase_sum;

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bsdf_eval_average) = average(phase_sum);
  }

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (shadow_flag & PATH_RAY_ANY_PASS) {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }
    else {
      /* Direct light, no diffuse/glossy distinction needed for volumes. */
      shadow_flag |= PATH_RAY_VOLUME_PASS;
      pass_diffuse_weight = one_spectrum();
      pass_glossy_weight = zero_spectrum();
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
  }

  if (bounce == 0) {
    path_visibility |= PATH_RAY_VISIBILITY_VOLUME_SCATTER;
    shadow_flag &= ~PATH_RAY_VOLUME_PRIMARY_TRANSMIT;
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = path_visibility;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, deep_surface_sample_idx) = INTEGRATOR_STATE(
      state, path, deep_surface_sample_idx);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, deep_volume_radiance_idx) = INTEGRATOR_STATE(
      state, path, deep_volume_radiance_idx);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = transparent_bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, diffuse_bounce) = INTEGRATOR_STATE(
      state, path, diffuse_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, glossy_bounce) = INTEGRATOR_STATE(
      state, path, glossy_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) = INTEGRATOR_STATE(
      state, path, transmission_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput_phase;
  if (kernel_data.integrator.light_flags & KERNEL_INTEGRATOR_SHADOW_COLOR) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = throughput_phase;
  }

  /* Write Light-group, +1 as light-group is int but we need to encode into a uint8_t. */
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, lightgroup) = ls.group + 1;

#  if defined(__PATH_GUIDING__)
  if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unlit_throughput) = unlit_throughput;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, path_segment) = INTEGRATOR_STATE(
        state, guiding, path_segment);
    INTEGRATOR_STATE(shadow_state, shadow_path, guiding_light_linking_mis_weight) = 0.0f;
  }
#  endif

  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
}

/* Path tracing: scatter in new direction using phase function */
ccl_device_forceinline bool integrate_volume_phase_scatter(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private Ray *ray,
    const ccl_private RNGState *rng_state,
    const ccl_private ShaderVolumePhases *phases)
{
  PROFILING_INIT(kg, PROFILING_SHADE_VOLUME_INDIRECT_LIGHT);

  float2 rand_phase = path_state_rng_2D(kg, rng_state, PRNG_VOLUME_PHASE);

  const ccl_private ShaderVolumeClosure *svc = volume_shader_phase_pick(phases, &rand_phase);

  /* Phase closure, sample direction. */
  float phase_pdf = 0.0f;
  float unguided_phase_pdf = 0.0f;
  BsdfEval phase_eval ccl_optional_struct_init;
  float3 phase_wo ccl_optional_struct_init;
  float sampled_roughness = 1.0f;
  int label;

#  if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
  if (kernel_data.integrator.use_guiding &&
      (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING))
  {
    label = volume_shader_phase_guided_sample(kg,
                                              state,
                                              sd,
                                              svc,
                                              rand_phase,
                                              &phase_eval,
                                              &phase_wo,
                                              &phase_pdf,
                                              &unguided_phase_pdf,
                                              &sampled_roughness);

    if (phase_pdf == 0.0f || bsdf_eval_is_zero(&phase_eval)) {
      return false;
    }

    INTEGRATOR_STATE_WRITE(state, path, unguided_throughput) *= phase_pdf / unguided_phase_pdf;
  }
  else
#  endif
  {
    label = volume_shader_phase_sample(
        sd, svc, rand_phase, &phase_eval, &phase_wo, &phase_pdf, &sampled_roughness);

    if (phase_pdf == 0.0f || bsdf_eval_is_zero(&phase_eval)) {
      return false;
    }

    unguided_phase_pdf = phase_pdf;
  }

  /* Setup ray. */
  INTEGRATOR_STATE_WRITE(state, ray, P) = sd->P;
  INTEGRATOR_STATE_WRITE(state, ray, D) = normalize(phase_wo);
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
#  ifdef __LIGHT_TREE__
  if (kernel_data.integrator.use_light_tree) {
    INTEGRATOR_STATE_WRITE(state, ray, previous_dt) = ray->tmax - ray->tmin;
  }
#  endif
  INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#  ifdef __RAY_DIFFERENTIALS__
  INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
  INTEGRATOR_STATE_WRITE(state, ray, dD) = volume_phase_widen_dD(
      kg, INTEGRATOR_STATE(state, ray, dD), sampled_roughness);
#  endif
  // Save memory by storing last hit prim and object in isect
  INTEGRATOR_STATE_WRITE(state, isect, prim) = sd->prim;
  INTEGRATOR_STATE_WRITE(state, isect, object) = sd->object;

  const Spectrum phase_weight = bsdf_eval_sum(&phase_eval) / phase_pdf;

  /* Add phase function sampling data to the path segment. */
  guiding_record_volume_bounce(
      kg, state, phase_weight, phase_pdf, normalize(phase_wo), sampled_roughness);

  /* Update throughput. */
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  const Spectrum throughput_phase = throughput * phase_weight;
  INTEGRATOR_STATE_WRITE(state, path, throughput) = throughput_phase;

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = one_spectrum();
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = zero_spectrum();
    }
  }

  /* Update path state */
  INTEGRATOR_STATE_WRITE(state, path, mis_ray_pdf) = phase_pdf;
  const float3 previous_P = ray->P + ray->D * ray->tmin;
  INTEGRATOR_STATE_WRITE(state, path, mis_origin_n) = sd->P - previous_P;
  INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = fminf(
      unguided_phase_pdf, INTEGRATOR_STATE(state, path, min_ray_pdf));

#  ifdef __LIGHT_LINKING__
  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING) {
    INTEGRATOR_STATE_WRITE(state, path, mis_ray_object) = sd->object;
  }
#  endif

  path_state_next(kg, state, label, sd->flag);
  return true;
}

ccl_device_inline VolumeIntegrateEvent
volume_integrate_event(KernelGlobals kg,
                       IntegratorState state,
                       const ccl_private Ray *ccl_restrict ray,
                       ccl_private ShaderData *sd,
                       const ccl_private RNGState *rng_state,
                       ccl_private LightSample &ls,
                       ccl_private VolumeIntegrateResult &result)
{
#  if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 1
  /* The current path throughput which is used later to calculate per-segment throughput. */
  const float3 initial_throughput = INTEGRATOR_STATE(state, path, throughput);
  /* The path throughput used to calculate the throughput for direct light. */
  float3 unlit_throughput = initial_throughput;
  /* If a new path segment is generated at the direct scatter position. */
  bool guiding_generated_new_segment = false;
  float rand_phase_guiding = 0.5f;
#  endif

  /* Perform path termination. The intersect_closest will have already marked this path
   * to be terminated. That will shading evaluating to leave out any scattering closures,
   * but emission and absorption are still handled for multiple importance sampling. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  const float continuation_probability = (path_flag & PATH_RAY_TERMINATE_IN_NEXT_VOLUME) ?
                                             0.0f :
                                             INTEGRATOR_STATE(
                                                 state, path, continuation_probability);
  if (continuation_probability == 0.0f) {
    return VOLUME_PATH_MISSED;
  }

  /* Direct light. */
  if (result.direct_scatter) {
    const float3 direct_P = ray->P + result.direct_t * ray->D;

#  if defined(__PATH_GUIDING__)
    if (kernel_data.integrator.use_guiding) {
#    if PATH_GUIDING_LEVEL >= 1
      if (result.direct_sample_method == VOLUME_SAMPLE_DISTANCE) {
        /* If the direct scatter event is generated using VOLUME_SAMPLE_DISTANCE the direct event
         * will happen at the same position as the indirect event and the direct light contribution
         * will contribute to the position of the next path segment. */
        const float3 transmittance_weight = spectrum_to_rgb(
            safe_divide_color(result.indirect_throughput, initial_throughput));
        guiding_record_volume_transmission(kg, state, transmittance_weight);
        guiding_record_volume_segment(kg, state, direct_P, sd->wi);
        guiding_generated_new_segment = true;
        unlit_throughput = result.indirect_throughput / continuation_probability;
        rand_phase_guiding = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_PHASE_GUIDING_DISTANCE);
      }
      else if (result.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) {
        /* If the direct scatter event is generated using VOLUME_SAMPLE_EQUIANGULAR the direct
         * event will happen at a separate position as the indirect event and the direct light
         * contribution will contribute to the position of the current/previous path segment. The
         * unlit_throughput has to be adjusted to include the scattering at the previous segment.
         */
        float3 scatterEval = one_float3();
        if (INTEGRATOR_STATE(state, guiding, path_segment)) {
          const pgl_vec3f scatteringWeight =
              INTEGRATOR_STATE(state, guiding, path_segment)->scatteringWeight;
          scatterEval = make_float3(scatteringWeight.x, scatteringWeight.y, scatteringWeight.z);
          unlit_throughput = safe_divide(unlit_throughput, scatterEval);
        }
        unlit_throughput *= continuation_probability;
        rand_phase_guiding = path_state_rng_1D(
            kg, rng_state, PRNG_VOLUME_PHASE_GUIDING_EQUIANGULAR);
      }
#    endif
#    if PATH_GUIDING_LEVEL >= 4
      if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
        volume_shader_prepare_guiding(
            kg, state, rand_phase_guiding, direct_P, ray->D, &result.direct_phases);
      }
#    endif
    }
#  endif

    result.direct_throughput /= continuation_probability;
    integrate_volume_direct_light(kg,
                                  state,
                                  sd,
                                  rng_state,
                                  direct_P,
                                  &result.direct_phases,
#  if defined(__PATH_GUIDING__)
                                  unlit_throughput,
#  endif
                                  result.direct_throughput,
                                  ls,
                                  result.direct_t
#  if __DEEP_BOUNDED_POLICY__ == 3
                                  , result.bounded_colour_permit
#  endif
                                  );
  }

  /* Indirect light.
   *
   * Only divide throughput by continuation_probability if we scatter. For the attenuation
   * case the next surface will already do this division. */
  if (result.indirect_scatter) {
#  if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 1
    if (!guiding_generated_new_segment) {
      const float3 transmittance_weight = spectrum_to_rgb(
          safe_divide_color(result.indirect_throughput, initial_throughput));
      guiding_record_volume_transmission(kg, state, transmittance_weight);
    }
#  endif
    result.indirect_throughput /= continuation_probability;
  }
  INTEGRATOR_STATE_WRITE(state, path, throughput) = result.indirect_throughput;

  if (result.indirect_scatter) {
#  if __DEEP_BOUNDED_POLICY__ == 3
    if (deep_bounded_active(kg) && !result.bounded_colour_permit &&
        deep_bounded_kind(INTEGRATOR_STATE(state, path, deep_bounded_slot_kind)) ==
            DeepBoundedSourceKind::UNBOUND)
    {
      /* A camera path scattered by an excluded Volume has no eligible Deep
       * anchor. Existing surface or volume anchors survive indirect scattering. */
      INTEGRATOR_STATE_WRITE(state, path, deep_bounded_slot_kind) =
          deep_bounded_source(0, DeepBoundedSourceKind::EXCLUDED);
    }
#  endif
    sd->P = ray->P + result.indirect_t * ray->D;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (deep_native_is_camera_chain(state))
      deep_native_set_root(state,
                           DEEP_NATIVE_ROOT_VOLUME_SCATTER,
                           camera_z_depth(kg, sd->P),
                           sd->object,
                           sd->prim,
                           sd->shader,
                           result.indirect_t,
                           0,
                           volume_native_world_support(kg, state, result.indirect_t));
#  endif

#  if defined(__PATH_GUIDING__)
    if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
#    if PATH_GUIDING_LEVEL >= 1
      if (!guiding_generated_new_segment) {
        guiding_record_volume_segment(kg, state, sd->P, sd->wi);
      }
#    endif
#    if PATH_GUIDING_LEVEL >= 4
      /* If the direct scatter event was generated using VOLUME_SAMPLE_EQUIANGULAR we need to
       * initialize the guiding distribution at the indirect scatter position. */
      if (result.direct_sample_method == VOLUME_SAMPLE_EQUIANGULAR) {
        rand_phase_guiding = path_state_rng_1D(kg, rng_state, PRNG_VOLUME_PHASE_GUIDING_DISTANCE);
        volume_shader_prepare_guiding(
            kg, state, rand_phase_guiding, sd->P, ray->D, &result.indirect_phases);
      }
#    endif
    }
#  endif

    if (integrate_volume_phase_scatter(kg, state, sd, ray, rng_state, &result.indirect_phases)) {
      return VOLUME_PATH_SCATTERED;
    }
    return VOLUME_PATH_MISSED;
  }
#  if defined(__PATH_GUIDING__)
  /* No guiding if we don't scatter. */
  if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
    INTEGRATOR_STATE_WRITE(state, guiding, use_volume_guiding) = false;
  }
#  endif
  return VOLUME_PATH_ATTENUATED;
}

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
ccl_device_inline void integrator_ensure_volume_radiance_receipt(IntegratorState state,
                                                                 const ccl_private Ray *ray)
{
  if (!kernel_integrator_state.deep_volume_radiance_receipts ||
      INTEGRATOR_STATE(state, path, deep_volume_radiance_idx) != 0xffffffffu)
  {
    return;
  }
  DeepVolumeRadianceReceipt receipt = {};
  receipt.pixel = INTEGRATOR_STATE(state, path, render_pixel_index);
  receipt.sample = INTEGRATOR_STATE(state, path, sample);
  receipt.rng_pixel = INTEGRATOR_STATE(state, path, rng_pixel);
  receipt.rng_offset = INTEGRATOR_STATE(state, path, rng_offset);
  receipt.path_flags = INTEGRATOR_STATE(state, path, flag);
  receipt.bounce = INTEGRATOR_STATE(state, path, bounce);
  receipt.tmin = ray->tmin;
  receipt.tmax = ray->tmax;
  receipt.time = ray->time;
  receipt.origin[0] = ray->P.x;
  receipt.origin[1] = ray->P.y;
  receipt.origin[2] = ray->P.z;
  receipt.direction[0] = ray->D.x;
  receipt.direction[1] = ray->D.y;
  receipt.direction[2] = ray->D.z;
  INTEGRATOR_STATE_WRITE(
      state, path, deep_volume_radiance_idx) = film_append_volume_radiance_receipt(receipt);
}
#  endif

/* get the volume attenuation and emission over line segment defined by
 * ray, with the assumption that there are no surfaces blocking light
 * between the endpoints. distance sampling is used to decide if we will
 * scatter or not. */
ccl_device VolumeIntegrateEvent volume_integrate(KernelGlobals kg,
                                                 IntegratorState state,
                                                 ccl_private Ray *ccl_restrict ray,
                                                 ccl_global float *ccl_restrict render_buffer)
{
  kernel_assert(!kernel_data.integrator.volume_ray_marching);

  if (integrator_state_volume_stack_is_empty(kg, state)) {
    return VOLUME_PATH_ATTENUATED;
  }

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  integrator_ensure_volume_radiance_receipt(state, ray);
#  endif

  ShaderData sd;
  /* FIXME: `object` is used for light linking. We read the bottom of the stack for simplicity, but
   * this does not work for overlapping volumes. */
  shader_setup_from_volume(&sd, ray, INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object));

  /* Load random number state. */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  /* For stochastic texture sampling. */
  sd.lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0x15b4f88d);

  LightSample ls ccl_optional_struct_init;

  /* TODO: expensive to zero closures? */
  VolumeIntegrateResult result = {};
  if (!volume_integrate_null_scattering(
          kg, state, ray, &sd, &rng_state, render_buffer, &ls, result))
  {
    if (sd.flag & SD_CACHE_MISS)
      return VOLUME_PATH_CACHE_MISS;
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const auto *checkpoint = deep_native_null_checkpoint(state);
    if (checkpoint && checkpoint->cache_miss)
      return VOLUME_PATH_CACHE_MISS;
#  endif
    return VOLUME_PATH_PAUSED;
  }

  return volume_integrate_event(kg, state, ray, &sd, &rng_state, ls, result);
}

ccl_device VolumeIntegrateEvent
volume_integrate_ray_marching(KernelGlobals kg,
                              IntegratorState state,
                              ccl_private Ray *ccl_restrict ray,
                              ccl_global float *ccl_restrict render_buffer)
{
  kernel_assert(kernel_data.integrator.volume_ray_marching);

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (deep_native_is_camera_chain(state) && !kernel_integrator_state.deep_native_full_optical) {
    deep_native_capture_error(DEEP_NATIVE_ERROR_HETEROGENEOUS);
  }
#  endif

  if (integrator_state_volume_stack_is_empty(kg, state)) {
    return VOLUME_PATH_ATTENUATED;
  }

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  integrator_ensure_volume_radiance_receipt(state, ray);
#  endif

  ShaderData sd;
  /* FIXME: `object` is used for light linking. We read the bottom of the stack for simplicity, but
   * this does not work for overlapping volumes. */
  shader_setup_from_volume(&sd, ray, INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object));

  /* Load random number state. */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  /* For stochastic texture sampling. */
  sd.lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0x15b4f88d);

  LightSample ls ccl_optional_struct_init;

  /* TODO: expensive to zero closures? */
  VolumeIntegrateResult result = {};

  const float step_size = volume_stack_step_size<false>(kg, state);
  if (!volume_integrate_ray_marching(
          kg, state, ray, &sd, &rng_state, render_buffer, step_size, &ls, result))
  {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    const auto *checkpoint = deep_native_march_checkpoint(state);
    if (checkpoint && checkpoint->cache_miss)
      return VOLUME_PATH_CACHE_MISS;
#  endif
    return VOLUME_PATH_PAUSED;
  }

  if (sd.flag & SD_CACHE_MISS) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (kernel_integrator_state.deep_native_cameras)
      deep_native_capture_error(DEEP_NATIVE_ERROR_SHADER_RETRY);
#  endif
    return VOLUME_PATH_CACHE_MISS;
  }

  return volume_integrate_event(kg, state, ray, &sd, &rng_state, ls, result);
}

#endif

#ifdef __VOLUME__
ccl_device_inline void integrator_shade_volume_setup(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_private Ray *ccl_restrict ray,
                                                     ccl_private Intersection *ccl_restrict isect)
{
  PROFILING_INIT(kg, PROFILING_SHADE_VOLUME_SETUP);

  /* Setup shader data. */
  integrator_state_read_ray(state, ray);
  integrator_state_read_isect(state, isect);

  /* Set ray length to current segment. */
  ray->tmax = (isect->prim != PRIM_NONE) ? isect->t : FLT_MAX;
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_optical_inputs) {
    ray->tmax = min(ray->tmax, kernel_integrator_state.deep_optical_inputs[state].tmax);
  }
#  endif

#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_owned_optical_coverage) {
    /* Optical covers physical boundaries; the trace-only intersection offset
     * must not punch an artificial vacuum gap in an active medium. */
    ray->tmin = kernel_integrator_state.deep_owned_optical_coverage[state].cursor_t;
    if (kernel_integrator_state.deep_owned_optical_checkpoints &&
        kernel_integrator_state.deep_owned_optical_checkpoints[state].active)
      ray->tmin = kernel_integrator_state.deep_owned_optical_checkpoints[state].ray_tmin;
  }
#  endif

  /* Clean volume stack for background rays. */
  if (isect->prim == PRIM_NONE) {
#  if defined(__DEEP_OUTPUT__) && defined(__KERNEL_GPU__)
    if (!kernel_integrator_state.deep_native_full_optical && deep_native_is_camera_chain(state) &&
        INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, shader) != SHADER_NONE)
    {
      /* Native stack cleaning cannot certify an omitted camera optical tail.
       * Complete independent Optical owns that coverage without changing native Beauty. */
      deep_native_capture_error(DEEP_NATIVE_ERROR_INCOMPLETE);
    }
#  endif
#  ifdef __KERNEL_GPU__
    if (!kernel_integrator_state.deep_optical_inputs)
#  endif
      volume_stack_clean(kg, state);
  }

  /* Assign flag to transmitted volume rays for scattering probability guiding. */
  if (INTEGRATOR_STATE(state, path, bounce) == 0) {
    INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_VOLUME_PRIMARY_TRANSMIT;
  }
}
#endif

template<DeviceKernel volume_kernel>
ccl_device_inline void integrator_next_kernel_after_shade_volume(
    KernelGlobals kg,
    const IntegratorState state,
    ccl_global float *ccl_restrict render_buffer,
    const ccl_private Intersection *ccl_restrict isect,
    const VolumeIntegrateEvent event)
{
  if (event == VOLUME_PATH_MISSED) {
    /* End path. */
    integrator_path_terminate(kg, state, render_buffer, volume_kernel);
    return;
  }

  if (event == VOLUME_PATH_ATTENUATED) {
    /* Continue to background, light or surface. */
    integrator_intersect_next_kernel_after_volume<volume_kernel>(kg, state, isect, render_buffer);
    return;
  }

#ifdef __SHADOW_LINKING__
  if (shadow_linking_schedule_intersection_kernel<volume_kernel>(kg, state)) {
    return;
  }
#endif /* __SHADOW_LINKING__ */

  kernel_assert(event == VOLUME_PATH_SCATTERED);

  /* Queue intersect_closest kernel. */
  integrator_path_next(state, volume_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
}

#if defined(__DEEP_OUTPUT__) && defined(__VOLUME__)
template<DeviceKernel volume_kernel>
ccl_device_inline void integrator_finish_deep_volume_optical_segment(
    KernelGlobals kg,
    IntegratorState state,
    ccl_global float *ccl_restrict render_buffer,
    const ccl_private Intersection *ccl_restrict isect,
    const uint32_t sample_idx,
    const bool cache_miss)
{
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_optical_inputs) {
    if (cache_miss) {
      integrator_path_cache_miss(state, volume_kernel);
    }
    else {
      ++kernel_integrator_state.deep_optical_ledger[state].segments;
      integrator_intersect_next_kernel_after_volume<volume_kernel>(kg, state, isect, nullptr);
    }
    return;
  }
#  endif
  if (cache_miss) {
    film_mark_deep_volume_primary_ray_flag(
        kg,
        INTEGRATOR_STATE(state, path, render_pixel_index),
        sample_idx,
        (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr,
        DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED);
    integrator_path_cache_miss(state, volume_kernel);
    return;
  }
  if (sample_idx != DEEP_INVALID_SAMPLE_INDEX) {
    film_mark_deep_volume_primary_ray_flag(
        kg,
        INTEGRATOR_STATE(state, path, render_pixel_index),
        sample_idx,
        (ccl_global KernelDeepSample *)kernel_data.film.deep_samples_ptr,
        DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE);
    INTEGRATOR_STATE_WRITE(state, path, deep_surface_sample_idx) = sample_idx;
  }
  integrator_intersect_next_kernel_after_volume<volume_kernel>(kg, state, isect, render_buffer);
}

template<DeviceKernel volume_kernel>
ccl_device_inline bool integrator_shade_deep_volume_optical_segment(
    KernelGlobals kg,
    IntegratorState state,
    ccl_global float *ccl_restrict render_buffer,
    const ccl_private Ray *ccl_restrict ray,
    const ccl_private Intersection *ccl_restrict isect,
    ccl_private ShaderData *ccl_restrict sd)
{
  if (!(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DEEP_VOLUME_OPTICAL)) {
    return false;
  }

  shader_setup_from_volume(sd, ray, INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object));

  RNGState rng_state;
  path_state_rng_load(state, &rng_state);
  sd->lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0x15b4f88d);

  const uint32_t sample_idx = volume_integrate_deep_optical_profile(
      kg, state, ray, sd, &rng_state);
#  if defined(__KERNEL_GPU__)
  if (kernel_integrator_state.deep_owned_optical_checkpoints &&
      kernel_integrator_state.deep_owned_optical_checkpoints[state].active &&
      !(sd->flag & SD_CACHE_MISS))
    return true; /* Keep the actual Optical queued kernel; no segment completion yet. */
#  endif

  integrator_finish_deep_volume_optical_segment<volume_kernel>(
      kg, state, render_buffer, isect, sample_idx, (sd->flag & SD_CACHE_MISS) != 0);
  return true;
}

template<DeviceKernel volume_kernel>
ccl_device_inline bool integrator_shade_deep_volume_optical_segment(
    KernelGlobals kg,
    IntegratorState state,
    ccl_global float *ccl_restrict render_buffer,
    const ccl_private Ray *ccl_restrict ray,
    const ccl_private Intersection *ccl_restrict isect)
{
  ShaderData sd;
  return integrator_shade_deep_volume_optical_segment<volume_kernel>(
      kg, state, render_buffer, ray, isect, &sd);
}
#endif

ccl_device void integrator_shade_deep_volume_optical(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
#if defined(__DEEP_OUTPUT__) && defined(__VOLUME__)
  /* Optical traversal shares exactly the original setup, RNG, extinction and retry logic.
   *
   * Keeping this entry separate avoids reserving Beauty integration's private state per ray. */
  kernel_assert(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DEEP_VOLUME_OPTICAL);
#  ifdef __KERNEL_GPU__
  if (kernel_integrator_state.deep_compact_rays) {
    auto &scalar = kernel_integrator_state.deep_compact_rays[deep_compact_camera_index(state)];
    if (scalar.flags & DEEP_COMPACT_TRANSFER_PENDING) {
      if (!deep_compact_cell(kg, state, scalar.pending_front, scalar.pending_back,
                             scalar.pending_sigma, scalar.pending_phase))
        return;
      scalar.flags &= ~DEEP_COMPACT_TRANSFER_PENDING;
      auto &coverage = kernel_integrator_state.deep_owned_optical_coverage[state];
      coverage.front_t = coverage.cursor_t = scalar.cursor;
      INTEGRATOR_STATE_WRITE(state, ray, tmin) = scalar.cursor;
      integrator_path_next(state, DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL,
                           DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
      return;
    }
  }
#  endif
  Ray ray ccl_optional_struct_init;
  Intersection isect ccl_optional_struct_init;
  integrator_shade_volume_setup(kg, state, &ray, &isect);
  /* Extinction evaluation always sets PATH_RAY_EMISSION and allocates zero closures. */
  ShaderDataTinyStorage sd_storage;
  integrator_shade_deep_volume_optical_segment<DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL>(
      kg, state, render_buffer, &ray, &isect, AS_SHADER_DATA(&sd_storage));
#endif
}

ccl_device void integrator_shade_deep_volume_optical_warp(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
#if defined(__DEEP_OUTPUT__) && defined(__VOLUME__) && defined(__KERNEL_CUDA__) && \
    !defined(__KERNEL_OPTIX__)
  constexpr uint32_t mask = 0xffffffffu;
  const uint32_t lane = threadIdx.x & 31u;
  kernel_assert(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_DEEP_VOLUME_OPTICAL);
  Ray ray ccl_optional_struct_init;
  Intersection isect ccl_optional_struct_init;
  /* Only one lane mutates the integrator state; all lanes then read its completed setup. */
  if (lane == 0) {
    integrator_shade_volume_setup(kg, state, &ray, &isect);
  }
  __syncwarp(mask);
  integrator_state_read_ray(state, &ray);
  integrator_state_read_isect(state, &isect);
  ray.tmax = (isect.prim != PRIM_NONE) ? isect.t : FLT_MAX;

  ShaderDataTinyStorage sd_storage;
  ccl_private ShaderData *sd = AS_SHADER_DATA(&sd_storage);
  shader_setup_from_volume(sd, &ray, INTEGRATOR_STATE_ARRAY(state, volume_stack, 0, object));
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);
  sd->lcg_state = lcg_state_init(
      rng_state.rng_pixel, rng_state.rng_offset, rng_state.sample, 0x15b4f88d);
  bool cache_miss = false;
  const uint32_t sample_idx = volume_integrate_deep_optical_profile_warp(
      kg, state, &ray, sd, &rng_state, cache_miss);
  if (lane == 0) {
    integrator_finish_deep_volume_optical_segment<
        DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL_WARP>(
        kg, state, render_buffer, &isect, sample_idx, cache_miss);
  }
#else
  /* This entry is never scheduled on other backends; keep their kernel registry complete. */
  integrator_shade_deep_volume_optical(kg, state, render_buffer);
#endif
}

ccl_device void integrator_shade_volume(KernelGlobals kg,
                                        IntegratorState state,
                                        ccl_global float *ccl_restrict render_buffer)
{
#ifdef __VOLUME__
  Ray ray ccl_optional_struct_init;
  Intersection isect ccl_optional_struct_init;
  integrator_shade_volume_setup(kg, state, &ray, &isect);

#  ifdef __DEEP_OUTPUT__
  if (integrator_shade_deep_volume_optical_segment<DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME>(
          kg, state, render_buffer, &ray, &isect))
  {
    return;
  }
#  endif

  const VolumeIntegrateEvent event = volume_integrate(kg, state, &ray, render_buffer);
  if (event == VOLUME_PATH_PAUSED)
    return; /* Keep the queued null-tracking operation until its checkpoint completes. */
  if (event == VOLUME_PATH_CACHE_MISS) {
    integrator_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME);
    return;
  }
  integrator_next_kernel_after_shade_volume<DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME>(
      kg, state, render_buffer, &isect, event);

#endif /* __VOLUME__ */
}

ccl_device void integrator_shade_volume_ray_marching(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
#ifdef __VOLUME__
  Ray ray ccl_optional_struct_init;
  Intersection isect ccl_optional_struct_init;
  integrator_shade_volume_setup(kg, state, &ray, &isect);

#  ifdef __DEEP_OUTPUT__
  if (integrator_shade_deep_volume_optical_segment<
          DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING>(
          kg, state, render_buffer, &ray, &isect))
  {
    return;
  }
#  endif

  const VolumeIntegrateEvent event = volume_integrate_ray_marching(kg, state, &ray, render_buffer);
  if (event == VOLUME_PATH_PAUSED)
    return; /* Preserve this real queued kernel until its checkpoint completes. */
  if (event == VOLUME_PATH_CACHE_MISS) {
    integrator_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING);
    return;
  }
  integrator_next_kernel_after_shade_volume<DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING>(
      kg, state, render_buffer, &isect, event);

#endif /* __VOLUME__ */
}
CCL_NAMESPACE_END
