/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/film/write.h"
#include "util/atomic.h"

CCL_NAMESPACE_BEGIN

/* -------------------------------------------------------------------- */
/** \name Deep Sample Writing
 *
 * Functions for writing deep samples during path tracing.
 * Each sample stores RGBA color and depth information.
 * \{ */

#ifdef __DEEP_OUTPUT__

#  ifdef __KERNEL_GPU__
ccl_device_inline void film_append_volume_emission_span(DeepVolumeEmissionSpan span)
{
  if (!kernel_integrator_state.deep_volume_emission_spans) {
    return;
  }
  const auto *roots = kernel_integrator_state.deep_volume_radiance_receipts;
  if (!roots || span.root >= kernel_integrator_state.deep_volume_radiance_receipt_capacity ||
      roots[span.root].pixel != span.pixel || roots[span.root].sample != span.sample)
  {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_emission_span_error, 2u);
    return;
  }
  bool finite = isfinite_safe(span.z) && isfinite_safe(span.zback) &&
                isfinite_safe(span.alpha) && isfinite_safe(span.time);
  for (int c = 0; c < 3; ++c) {
    finite &= isfinite_safe(span.rgb[c]) && isfinite_safe(span.tau[c]) && span.tau[c] >= 0;
  }
  if (!finite || span.zback <= span.z || span.alpha < 0 || span.alpha > 1) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_emission_span_error, 4u);
    return;
  }
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_volume_emission_span_count, 1u);
  if (index >= kernel_integrator_state.deep_volume_emission_span_capacity) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_emission_span_error, 1u);
    return;
  }
  /* Global append order is immutable within this complete diagnostic domain. A primary
   * path produces its spans sequentially; fork/retry ownership remains a separate gate. */
  span.ordinal = index;
  kernel_integrator_state.deep_volume_emission_spans[index] = span;
}

ccl_device_inline uint32_t film_append_volume_radiance_receipt(
    const DeepVolumeRadianceReceipt &receipt)
{
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_volume_radiance_receipt_count, 1u);
  if (index >= kernel_integrator_state.deep_volume_radiance_receipt_capacity) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_radiance_receipt_error, 1u);
    return 0xffffffffu;
  }
  kernel_integrator_state.deep_volume_radiance_receipts[index] = receipt;
  return index;
}

enum DeepVolumeRadianceKind { DEEP_VOLUME_EMISSION, DEEP_VOLUME_PATH, DEEP_VOLUME_SHADOW };

ccl_device_inline void film_accumulate_volume_radiance_receipt(
    const uint32_t pixel,
    const uint32_t sample,
    const uint32_t index,
    const Spectrum contribution,
    const DeepVolumeRadianceKind kind)
{
  if (!kernel_integrator_state.deep_volume_radiance_receipts) {
    return;
  }
  /* Call only for a contribution with a declared Volume owner. An invalid owner after append
   * overflow is already a hard domain error, never a successful transparent fallback. */
  if (index == 0xffffffffu) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_radiance_receipt_error, 2u);
    return;
  }
  if (index >= kernel_integrator_state.deep_volume_radiance_receipt_capacity ||
      kernel_integrator_state.deep_volume_radiance_receipts[index].pixel != pixel ||
      kernel_integrator_state.deep_volume_radiance_receipts[index].sample != sample)
  {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_radiance_receipt_error, 2u);
    return;
  }
  const float3 rgb = spectrum_to_rgb(contribution);
  if (!isfinite_safe(rgb.x) || !isfinite_safe(rgb.y) || !isfinite_safe(rgb.z)) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_volume_radiance_receipt_error, 4u);
    return;
  }
  ccl_global DeepVolumeRadianceReceipt *receipt =
      &kernel_integrator_state.deep_volume_radiance_receipts[index];
  ccl_global float *target = kind == DEEP_VOLUME_EMISSION ? receipt->emission :
                            kind == DEEP_VOLUME_PATH ? receipt->unowned_path :
                                                       receipt->unowned_shadow;
  atomic_add_and_fetch_float(target, rgb.x);
  atomic_add_and_fetch_float(target + 1, rgb.y);
  atomic_add_and_fetch_float(target + 2, rgb.z);
}

ccl_device_inline uint32_t film_append_surface_receipt(const DeepSurfaceReceipt &receipt)
{
  const uint32_t index = atomic_fetch_and_add_uint32(
      &kernel_integrator_state.queue_counter->deep_surface_receipt_count, 1u);
  if (index >= kernel_integrator_state.deep_surface_receipt_capacity) {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_surface_receipt_overflow, 1u);
    return 0xffffffffu;
  }
  kernel_integrator_state.deep_surface_receipts[index] = receipt;
  return index;
}

ccl_device_inline void film_accumulate_surface_receipt_rgb(const uint32_t pixel,
                                                          const uint32_t index,
                                                          const Spectrum contribution)
{
  if (index == 0xffffffffu) {
    return;
  }
  if (index >= kernel_integrator_state.deep_surface_receipt_capacity ||
      kernel_integrator_state.deep_surface_receipts[index].pixel != pixel)
  {
    atomic_fetch_and_or_uint32(
        &kernel_integrator_state.queue_counter->deep_surface_receipt_overflow, 2u);
    return;
  }
  ccl_global DeepSurfaceReceipt *receipt = &kernel_integrator_state.deep_surface_receipts[index];
  const float3 rgb = spectrum_to_rgb(contribution);
  atomic_add_and_fetch_float(&receipt->rgba[0], rgb.x);
  atomic_add_and_fetch_float(&receipt->rgba[1], rgb.y);
  atomic_add_and_fetch_float(&receipt->rgba[2], rgb.z);
}
#  endif

/* Keep these flags and pack/unpack helpers in sync with session/deep_buffers.h. */
constexpr uint32_t DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA = (1u << 0);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_METADATA = (1u << 1);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_TERMINATED = (1u << 2);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE = (1u << 3);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_PROFILE = (1u << 4);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH = (1u << 5);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_SEGMENT_COMPLETE = (1u << 6);
constexpr uint32_t DEEP_SAMPLE_FLAG_VOLUME_OPTICAL_ATTEMPT_ABORTED = (1u << 7);
constexpr uint32_t DEEP_SAMPLE_INFO_FLAG_MASK = 0xffu;
constexpr uint32_t DEEP_SAMPLE_INFO_CAMERA_SAMPLE_SHIFT = 8u;
constexpr uint32_t DEEP_SAMPLE_INFO_CAMERA_SAMPLE_MASK =
    0xffffffffu & ~DEEP_SAMPLE_INFO_FLAG_MASK;
constexpr uint32_t DEEP_SAMPLE_COUNT_OVERFLOW = (1u << 31);
constexpr uint32_t DEEP_INVALID_SAMPLE_INDEX = 0xffffffffu;

/**
 * Deep sample data structure for kernel use.
 * Matches DeepSampleData in session/deep_buffers.h.
 */
struct ccl_align(16) KernelDeepSample {
  float r, g, b, a;
  float z;
  float z_back;
  uint32_t surface_object;
  uint32_t surface_prim;
  uint32_t surface_shader;
  uint32_t packed_geometric_normal;
  uint32_t flags;
  uint32_t pixel_index;
};

static_assert(sizeof(KernelDeepSample) == 48, "KernelDeepSample layout must match host.");

ccl_device_inline void deep_make_surface_key(const int object,
                                             const int prim,
                                             const int shader,
                                             uint32_t &surface_object,
                                             uint32_t &surface_prim,
                                             uint32_t &surface_shader)
{
  surface_object = uint32_t(object);
  surface_prim = uint32_t(prim);
  surface_shader = uint32_t(shader);
}

ccl_device_inline float2 deep_encode_octahedral_normal(const float3 normal)
{
  const float3 n = safe_normalize(normal);
  const float inv_l1 = 1.0f / (fabsf(n.x) + fabsf(n.y) + fabsf(n.z) + 1e-20f);
  float2 encoded = make_float2(n.x * inv_l1, n.y * inv_l1);

  if (n.z < 0.0f) {
    encoded = make_float2(copysignf(1.0f - fabsf(encoded.y), encoded.x),
                          copysignf(1.0f - fabsf(encoded.x), encoded.y));
  }

  return encoded;
}

ccl_device_inline uint32_t deep_pack_unorm_16(const float value)
{
  return (uint32_t)clamp(int((clamp(value, 0.0f, 1.0f) * 65535.0f) + 0.5f), 0, 65535);
}

ccl_device_inline uint32_t deep_pack_geometric_normal(const float3 normal)
{
  const float2 encoded = deep_encode_octahedral_normal(normal);
  const float2 unorm = encoded * 0.5f + make_float2(0.5f, 0.5f);
  return deep_pack_unorm_16(unorm.x) | (deep_pack_unorm_16(unorm.y) << 16);
}

ccl_device_inline uint32_t deep_pack_sample_info(const uint32_t flags,
                                                 const uint32_t camera_sample)
{
  return (flags & DEEP_SAMPLE_INFO_FLAG_MASK) |
         ((camera_sample << DEEP_SAMPLE_INFO_CAMERA_SAMPLE_SHIFT) &
          DEEP_SAMPLE_INFO_CAMERA_SAMPLE_MASK);
}

#ifdef __KERNEL_CUDA__
ccl_device_inline uint32_t film_deep_warp_append(ccl_global uint32_t *global_sample_count)
{
  /* Opportunistic grouping works inside the divergent volume march. Only currently active
   * lanes participate; each receives a distinct slot, and each lane's attempt order is retained. */
  const uint32_t active = __activemask();
  const int leader = __ffs(active) - 1;
  const uint32_t lane = threadIdx.x & 31u;
  const uint32_t rank = __popc(active & ((1u << lane) - 1u));
  uint32_t first = 0;
  if (int(lane) == leader) {
    first = atomic_fetch_and_add_uint32(global_sample_count, __popc(active));
  }
  first = __shfl_sync(active, first, leader);
  return first + rank;
}
#endif

template<bool reserved = false>
ccl_device_inline uint32_t film_write_deep_sample_with_metadata(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z,
    const float z_back,
    const uint32_t surface_object,
    const uint32_t surface_prim,
    const uint32_t surface_shader,
    const uint32_t packed_geometric_normal,
    const uint32_t sample_info,
    const uint32_t reserved_index = 0)
{
  /* Bounds check: ensure pixel index is within allocated buffer. */
  const uint32_t num_pixels = kernel_data.film.deep_width * kernel_data.film.deep_height;
  if (pixel_index >= num_pixels) {
    return DEEP_INVALID_SAMPLE_INDEX;
  }

  uint32_t sample_idx;
  uint64_t offset;
  if (kernel_data.film.use_deep_output >= 2) {
    ccl_global uint32_t *global_sample_count = sample_counts + num_pixels;
    if constexpr (reserved) {
      /* The cooperative producer reserved this ordered group and counted its valid slots. */
      sample_idx = reserved_index;
    }
    else {
#ifdef __KERNEL_CUDA__
    sample_idx = kernel_data.film.use_deep_output == 3 ?
                     film_deep_warp_append(global_sample_count) :
                     atomic_fetch_and_add_uint32(global_sample_count, 1);
#else
    sample_idx = atomic_fetch_and_add_uint32(global_sample_count, 1);
#endif
    }
    const uint64_t global_capacity = uint64_t(num_pixels) * kernel_data.film.deep_max_samples;
    if (uint64_t(sample_idx) >= global_capacity) {
      atomic_fetch_and_or_uint32(&sample_counts[pixel_index], DEEP_SAMPLE_COUNT_OVERFLOW);
      return DEEP_INVALID_SAMPLE_INDEX;
    }
    if constexpr (!reserved) {
      atomic_fetch_and_add_uint32(&sample_counts[pixel_index], 1);
    }
    offset = sample_idx;
  }
  else {
    sample_idx = atomic_fetch_and_add_uint32(&sample_counts[pixel_index], 1);
    if (sample_idx >= kernel_data.film.deep_max_samples) {
      atomic_fetch_and_add_uint32(&sample_counts[pixel_index], -1);
      atomic_fetch_and_or_uint32(&sample_counts[pixel_index], DEEP_SAMPLE_COUNT_OVERFLOW);
      return DEEP_INVALID_SAMPLE_INDEX;
    }
    offset = uint64_t(pixel_index) * kernel_data.film.deep_max_samples + sample_idx;
  }

  deep_samples[offset].r = 0.0f;
  deep_samples[offset].g = 0.0f;
  deep_samples[offset].b = 0.0f;
  deep_samples[offset].a = alpha;
  deep_samples[offset].z = z;
  deep_samples[offset].z_back = z_back;
  deep_samples[offset].surface_object = surface_object;
  deep_samples[offset].surface_prim = surface_prim;
  deep_samples[offset].surface_shader = surface_shader;
  deep_samples[offset].packed_geometric_normal = packed_geometric_normal;
  deep_samples[offset].flags = sample_info;
  deep_samples[offset].pixel_index = pixel_index;

  return sample_idx;
}

/**
 * Write a deep sample for the current pixel.
 *
 * This appends a new sample to the pixel's sample list. If the maximum
 * number of samples has been reached, the sample is dropped.
 *
 * \param kg: Kernel globals
 * \param pixel_index: Pixel index for the current sample
 * \param deep_samples: Pointer to deep sample buffer
 * \param sample_counts: Pointer to per-pixel sample counts
 * \param z: Front depth (distance from camera)
 * \param z_back: Back depth (same as z for surfaces, different for volumes)
 */
ccl_device_inline uint32_t film_write_deep_sample(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float z,
    const float z_back)
{
  return film_write_deep_sample_with_metadata(
      kg, pixel_index, deep_samples, sample_counts, 1.0f, z, z_back, 0, 0, 0, 0, 0);
}

/**
 * Write a deep sample with transparency (for transparent surfaces).
 *
 * \param alpha: Explicit alpha value (0 = fully transparent, 1 = fully opaque)
 */
ccl_device_inline uint32_t film_write_deep_sample_transparent(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z,
    const float z_back)
{
  return film_write_deep_sample_with_metadata(
      kg, pixel_index, deep_samples, sample_counts, alpha, z, z_back, 0, 0, 0, 0, 0);
}

ccl_device_inline uint32_t film_write_deep_surface_sample_transparent(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z,
    const float z_back,
    const uint32_t surface_object,
    const uint32_t surface_prim,
    const uint32_t surface_shader,
    const uint32_t packed_geometric_normal,
    const uint32_t camera_sample)
{
  return film_write_deep_sample_with_metadata(
      kg,
      pixel_index,
      deep_samples,
      sample_counts,
      alpha,
      z,
      z_back,
      surface_object,
      surface_prim,
      surface_shader,
      packed_geometric_normal,
      deep_pack_sample_info(DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA, camera_sample));
}

ccl_device_inline uint64_t film_deep_sample_offset(KernelGlobals kg,
                                                   const uint32_t pixel_index,
                                                   const uint32_t sample_idx)
{
  return kernel_data.film.use_deep_output >= 2 ?
             uint64_t(sample_idx) :
             uint64_t(pixel_index) * kernel_data.film.deep_max_samples + sample_idx;
}

/**
 * Write a volumetric deep sample.
 *
 * Volumes have a front and back depth representing the entry and exit points.
 *
 * \param z_entry: Depth where ray enters the volume.
 * \param z_exit: Depth where ray exits the volume.
 */
template<bool reserved = false>
ccl_device_inline uint32_t film_write_deep_sample_volume(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z_entry,
    const float z_exit,
    const uint32_t camera_sample,
    const Spectrum radiance,
    const Spectrum optical_depth,
    const uint32_t reserved_index = 0)
{
  const float3 tau = spectrum_to_rgb(optical_depth);
  /* Volumes use z_back to represent the volume thickness. */
  const uint32_t sample_idx = film_write_deep_sample_with_metadata<reserved>(
      kg,
      pixel_index,
      deep_samples,
      sample_counts,
      alpha,
      z_entry,
      z_exit,
      __float_as_uint(tau.x),
      __float_as_uint(tau.y),
      __float_as_uint(tau.z),
      0,
      deep_pack_sample_info(DEEP_SAMPLE_FLAG_VOLUME_METADATA |
                                DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH,
                            camera_sample),
      reserved_index);
  if (sample_idx != DEEP_INVALID_SAMPLE_INDEX) {
    const uint64_t offset = film_deep_sample_offset(kg, pixel_index, sample_idx);
    const float3 rgb = spectrum_to_rgb(radiance);
    deep_samples[offset].r = rgb.x;
    deep_samples[offset].g = rgb.y;
    deep_samples[offset].b = rgb.z;
  }
  return sample_idx;
}

ccl_device_inline void film_mark_deep_volume_primary_ray_flag(
    KernelGlobals kg,
    const uint32_t pixel_index,
    const uint32_t sample_idx,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    const uint32_t flag)
{
  if (deep_samples == nullptr || sample_idx == DEEP_INVALID_SAMPLE_INDEX) {
    return;
  }

  const uint64_t offset = film_deep_sample_offset(kg, pixel_index, sample_idx);
  deep_samples[offset].flags |= flag;
}

ccl_device_inline void film_accumulate_deep_surface_rgb(
    KernelGlobals kg,
    const uint32_t pixel_index,
    const uint32_t sample_idx,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    const Spectrum contribution)
{
  if (!kernel_data.film.use_deep_output || deep_samples == nullptr ||
      sample_idx == DEEP_INVALID_SAMPLE_INDEX)
  {
    return;
  }

  const uint64_t offset = film_deep_sample_offset(kg, pixel_index, sample_idx);
  const float3 contribution_rgb = spectrum_to_rgb(contribution);
  atomic_add_and_fetch_float(&deep_samples[offset].r, contribution_rgb.x);
  atomic_add_and_fetch_float(&deep_samples[offset].g, contribution_rgb.y);
  atomic_add_and_fetch_float(&deep_samples[offset].b, contribution_rgb.z);
}

/** \} */

#endif /* __DEEP_OUTPUT__ */

CCL_NAMESPACE_END
