/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* One CTA owns one already encoded pixel/family. The alpha encoder accumulates
 * the shared denominator as it writes actual format alpha, excluding black
 * mask packets. This stage never recalibrates or re-encodes alpha. */
extern "C" __global__ void deep_gpu_frame_compact_simple_colour(const DeepGPUFrameDispatch d)
{
  const uint32_t pixel = blockIdx.x;
  if (pixel >= d.n) return;
  const auto range = ptr<const DeepGPUFrameFinalRange>(d.c)[pixel];
  const auto summary = ptr<const DeepCompactOutputSummary>(d.a)[pixel];
  const uint32_t population = ptr<const DeepGPUFramePixel>(d.e)[pixel].population;
  const uint32_t window_x = uint32_t(d.h >> 32), window_y = uint32_t(d.h);
  const uint64_t film_pixel = uint64_t(window_y + pixel / d.m) * d.width + window_x + pixel % d.m;
  if (!population || uint64_t(range.offset) + range.count > d.base || !isfinite(d.p) || d.p < 0) {
    if (!threadIdx.x) atomicOr(ptr<uint32_t>(d.f), DEEP_GPU_FRAME_INVALID_ENCODING);
    return;
  }
  double beauty[3];
  bool nonzero = false;
  for (int c = 0; c < 3; c++) {
    beauty[c] = double(ptr<const float>(d.d)[film_pixel * d.k + d.g + c]) * double(d.p) / population;
    nonzero |= beauty[c] != 0;
    if (!isfinite(beauty[c]) && !threadIdx.x)
      atomicOr(ptr<uint32_t>(d.f), DEEP_GPU_FRAME_INVALID_RADIANCE);
  }
  if (summary.log_denominator == -INFINITY) {
    if (nonzero && summary.surface && !threadIdx.x)
      atomicOr(ptr<uint32_t>(d.f), DEEP_GPU_FRAME_SIMPLE_ZERO_ALPHA_COLOUR);
    if (nonzero && summary.surface) return;
  }
  else if (!isfinite(summary.log_denominator)) {
    if (!threadIdx.x) atomicOr(ptr<uint32_t>(d.f), DEEP_GPU_FRAME_NUMERIC_MASS_RANGE);
    return;
  }
  for (uint32_t i = threadIdx.x; i < range.count; i += blockDim.x) {
    const uint32_t at = range.offset + i;
    auto &sample = ptr<DeepGPUFrameSample>(d.b)[at];
    /* A negative marker is private device metadata until all colour is written;
     * final payload contains only the public six fields. */
    const bool black = sample.r < 0;
    float *rgb = &sample.r;
    for (int c = 0; c < 3; c++) {
      double colour = 0;
      if (!black && sample.a > 0 && beauty[c] != 0)
        colour = copysign(exp(log(fabs(beauty[c])) + log(double(sample.a)) - summary.log_denominator), beauty[c]);
      const double limit = d.kind ? 65504.0 : double(FLT_MAX);
      if (!isfinite(colour) || fabs(colour) > limit) {
        atomicOr(ptr<uint32_t>(d.f), DEEP_GPU_FRAME_FORMAT_RANGE); colour = 0;
      }
      rgb[c] = d.kind ? __half2float(__float2half_rn(float(colour))) : float(colour);
    }
  }
}

extern "C" __global__ void deep_gpu_frame_compact_pending_ranges(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  auto range = ptr<const DeepGPUFrameFinalRange>(d.a)[i];
  if (range.count && (range.offset < d.base || uint64_t(range.offset - d.base) + range.count > d.m)) {
    atomicOr(ptr<uint32_t>(d.d), DEEP_GPU_FRAME_INVALID_RANGE); return;
  }
  range.offset = range.count ? range.offset - d.base : 0;
  ptr<DeepGPUFrameFinalRange>(d.b)[i] = range;
  ptr<DeepGPUFramePixel>(d.c)[i] = {0, 0, 0, 0, range.population};
}
