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

/**
 * Deep sample data structure for kernel use.
 * Matches DeepSampleData in session/deep_buffers.h.
 */
struct ccl_align(32) KernelDeepSample {
  float r, g, b, a;
  float z;
  float z_back;
};

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
ccl_device_inline void film_write_deep_sample(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float z,
    const float z_back)
{
  /* Bounds check: ensure pixel index is within allocated buffer. */
  const uint32_t num_pixels = kernel_data.film.deep_width * kernel_data.film.deep_height;
  if (pixel_index >= num_pixels) {
    return;
  }

  /* Atomically increment sample count and get our slot index. */
  const uint32_t sample_idx = atomic_fetch_and_add_uint32(&sample_counts[pixel_index], 1);

  /* Check against maximum samples per pixel. */
  if (sample_idx >= kernel_data.film.deep_max_samples) {
    /* Decrement count since we couldn't store the sample. */
    atomic_fetch_and_add_uint32(&sample_counts[pixel_index], -1);
    return;
  }

  /* Calculate offset into sample data buffer
   * Layout: pixel[0].samples[0..max], pixel[1].samples[0..max], ... */
  const uint64_t offset = uint64_t(pixel_index) * kernel_data.film.deep_max_samples + sample_idx;

  /* Alpha-only deep samples (RGB=0). Color is applied via Deep Recolor post-processing. */
  deep_samples[offset].r = 0.0f;
  deep_samples[offset].g = 0.0f;
  deep_samples[offset].b = 0.0f;
  deep_samples[offset].a = 1.0f; /* Solid alpha for opaque surfaces. */
  deep_samples[offset].z = z;
  deep_samples[offset].z_back = z_back;
}

/**
 * Write a deep sample with transparency (for transparent surfaces).
 *
 * \param alpha: Explicit alpha value (0 = fully transparent, 1 = fully opaque)
 */
ccl_device_inline void film_write_deep_sample_transparent(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z,
    const float z_back)
{
  /* Bounds check: ensure pixel index is within allocated buffer. */
  const uint32_t num_pixels = kernel_data.film.deep_width * kernel_data.film.deep_height;
  if (pixel_index >= num_pixels) {
    return;
  }

  const uint32_t sample_idx = atomic_fetch_and_add_uint32(&sample_counts[pixel_index], 1);

  if (sample_idx >= kernel_data.film.deep_max_samples) {
    atomic_fetch_and_add_uint32(&sample_counts[pixel_index], -1);
    return;
  }

  const uint64_t offset = uint64_t(pixel_index) * kernel_data.film.deep_max_samples + sample_idx;

  /* Alpha-only deep samples (RGB=0). Color is applied via Deep Recolor post-processing. */
  deep_samples[offset].r = 0.0f;
  deep_samples[offset].g = 0.0f;
  deep_samples[offset].b = 0.0f;
  deep_samples[offset].a = alpha;
  deep_samples[offset].z = z;
  deep_samples[offset].z_back = z_back;
}

/**
 * Write a volumetric deep sample.
 *
 * Volumes have a front and back depth representing the entry and exit points.
 *
 * \param z_entry: Depth where ray enters the volume.
 * \param z_exit: Depth where ray exits the volume.
 */
ccl_device_inline void film_write_deep_sample_volume(
    KernelGlobals kg,
    const uint32_t pixel_index,
    ccl_global KernelDeepSample *ccl_restrict deep_samples,
    ccl_global uint32_t *ccl_restrict sample_counts,
    const float alpha,
    const float z_entry,
    const float z_exit)
{
  /* Volumes use z_back to represent the volume thickness. */
  film_write_deep_sample_transparent(
      kg, pixel_index, deep_samples, sample_counts, alpha, z_entry, z_exit);
}

/** \} */

#endif /* __DEEP_OUTPUT__ */

CCL_NAMESPACE_END
