/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "kernel/integrator/deep_gpu_frame.h"

#include <cub/block/block_reduce.cuh>
#include <cub/block/block_scan.cuh>
#include <math.h>
#include <cuda_fp16.h>

extern "C" __device__ __constant__ uint32_t deep_gpu_frame_module_abi = DEEP_GPU_FRAME_ABI;

namespace {
using V = DeepGPUFrameFloat3;
struct Add {
  __device__ V operator()(const V a, const V b) const
  {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }
};
struct AddPair {
  __device__ DeepGPUFrameTransmission operator()(const DeepGPUFrameTransmission a,
                                                const DeepGPUFrameTransmission b) const
  {
    return {a.before + b.before, a.after + b.after};
  }
};
template<typename T> __device__ T *ptr(const uint64_t address)
{
  return reinterpret_cast<T *>(address);
}
__device__ void error(const DeepGPUFrameReduceParameters &p, const uint32_t flag)
{
  atomicOr(ptr<uint32_t>(p.status), flag);
}
__device__ bool range(const uint32_t begin, const uint32_t count, const uint32_t total)
{
  return begin <= total && count <= total - begin;
}
__device__ bool finite_nonnegative(const V a)
{
  return isfinite(a.x) && isfinite(a.y) && isfinite(a.z) && a.x >= 0 && a.y >= 0 &&
         a.z >= 0;
}

/* Prefixes are built once, so each requested depth is a binary lookup rather
 * than another scan of every optical piece. Vacuum gaps need no records. */
__device__ V tau_at(const DeepGPUFrameReduceParameters &p,
                   const DeepGPUFrameRay &ray,
                   const float z,
                   const bool before = false)
{
  const auto *optical = ptr<const DeepGPUFrameOptical>(p.optical);
  const auto *prefix = ptr<const V>(p.optical_prefix);
  uint32_t lo = 0, hi = ray.optical_count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (optical[ray.optical_begin + mid].z < z ||
        (!before && optical[ray.optical_begin + mid].z == z))
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return {0, 0, 0};
  const uint32_t index = ray.optical_begin + lo - 1;
  const auto piece = optical[index];
  const float fraction = piece.z == piece.z_back ? 1 :
                         fminf(1.0f, fmaxf(0.0f, (z - piece.z) / (piece.z_back - piece.z)));
  const V preceding = prefix[index];
  return {fmaf(fraction, piece.tau.x, preceding.x),
          fmaf(fraction, piece.tau.y, preceding.y),
          fmaf(fraction, piece.tau.z, preceding.z)};
}

__device__ float transmission_at(const DeepGPUFrameReduceParameters &p,
                                 const DeepGPUFrameRay &ray,
                                 const float z,
                                 const bool before)
{
  V q{1, 1, 1}, holes{0, 0, 0};
  const auto *visibility = ptr<const DeepGPUFrameVisibility>(p.visibility);
  for (uint32_t i = 0; i < ray.visibility_count; i++) {
    const auto point = visibility[ray.visibility_begin + i];
    if (point.z > z || (before && point.z == z))
      break;
    const V tau = tau_at(p, ray, point.z);
    holes.x = fmaf(expf(-tau.x) * q.x, point.holdout.x, holes.x);
    holes.y = fmaf(expf(-tau.y) * q.y, point.holdout.y, holes.y);
    holes.z = fmaf(expf(-tau.z) * q.z, point.holdout.z, holes.z);
    q.x *= point.transmission.x;
    q.y *= point.transmission.y;
    q.z *= point.transmission.z;
  }
  const V tau = tau_at(p, ray, z, before);
  return ((expf(-tau.x) * q.x + holes.x) + (expf(-tau.y) * q.y + holes.y) +
          (expf(-tau.z) * q.z + holes.z)) /
         3.0f;
}

__device__ V radiance_at(const DeepGPUFrameReduceParameters &p,
                        const DeepGPUFrameRay &ray,
                        const float z,
                        const bool before)
{
  V value{0, 0, 0};
  const auto *radiance = ptr<const DeepGPUFrameRadiance>(p.radiance);
  for (uint32_t i = 0; i < ray.radiance_count; i++) {
    const auto part = radiance[ray.radiance_begin + i];
    const bool point = part.z == part.z_back;
    if (z < part.z || (point && before && z == part.z))
      continue;
    if (point || z >= part.z_back) {
      value = Add{}(value, part.rgb);
      continue;
    }
    const float fraction = point ? 1 : fminf(1.0f, (z - part.z) / (part.z_back - part.z));
    const float *tau = &part.tau.x, *rgb = &part.rgb.x;
    float *out = &value.x;
    for (int c = 0; c < 3; c++) {
      /* Native contribution totals remain authoritative. The small-tau limit
       * is analytic, not a redistribution of terminal Beauty. */
      const float denominator = -expm1f(-tau[c]);
      const float weight = denominator > 0 ? -expm1f(-tau[c] * fraction) / denominator : fraction;
      out[c] = fmaf(weight, rgb[c], out[c]);
    }
  }
  return value;
}

struct AddKnot {
  __device__ DeepGPUFrameKnot operator()(const DeepGPUFrameKnot a,
                                        const DeepGPUFrameKnot b) const
  {
    return {{a.before.t + b.before.t, a.before.r + b.before.r,
             a.before.g + b.before.g, a.before.b + b.before.b},
            {a.after.t + b.after.t, a.after.r + b.after.r,
             a.after.g + b.after.g, a.after.b + b.after.b}};
  }
};
}  // namespace

extern "C" __global__ void deep_gpu_frame_optical_prefix(const DeepGPUFrameReduceParameters p)
{
  using Scan = cub::BlockScan<V, 128>;
  __shared__ typename Scan::TempStorage storage;
  __shared__ V block_total;
  if (blockIdx.x >= p.ray_count)
    return;
  const auto ray = ptr<const DeepGPUFrameRay>(p.rays)[blockIdx.x];
  if (!range(ray.optical_begin, ray.optical_count, p.optical_count) ||
      !range(ray.visibility_begin, ray.visibility_count, p.visibility_count) ||
      !range(ray.radiance_begin, ray.radiance_count, p.radiance_count))
  {
    if (!threadIdx.x)
      error(p, DEEP_GPU_FRAME_INVALID_RANGE);
    return;
  }
  const auto *optical = ptr<const DeepGPUFrameOptical>(p.optical);
  auto *prefix = ptr<V>(p.optical_prefix);
  V carry{0, 0, 0};
  for (uint32_t start = 0; start < ray.optical_count; start += 128) {
    const uint32_t offset = start + threadIdx.x;
    V value{0, 0, 0};
    if (offset < ray.optical_count) {
      const uint32_t index = ray.optical_begin + offset;
      const auto piece = optical[index];
      if (!isfinite(piece.z) || !isfinite(piece.z_back) || piece.z < 0 ||
          piece.z > piece.z_back || !finite_nonnegative(piece.tau) ||
          (offset && optical[index - 1].z_back > piece.z))
      {
        error(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      }
      else
        value = piece.tau;
    }
    V exclusive, total;
    Scan(storage).ExclusiveScan(value, exclusive, V{0, 0, 0}, Add{}, total);
    if (offset < ray.optical_count)
      prefix[ray.optical_begin + offset] = Add{}(carry, exclusive);
    if (!threadIdx.x) block_total = total;
    __syncthreads();
    carry = Add{}(carry, block_total);
    __syncthreads();
  }
  const auto *visibility = ptr<const DeepGPUFrameVisibility>(p.visibility);
  for (uint32_t i = threadIdx.x; i < ray.visibility_count; i += 128) {
    const auto point = visibility[ray.visibility_begin + i];
    if (!isfinite(point.z) || point.z < 0 || !finite_nonnegative(point.transmission) ||
        !finite_nonnegative(point.holdout) ||
        point.transmission.x + point.holdout.x > 1 ||
        point.transmission.y + point.holdout.y > 1 ||
        point.transmission.z + point.holdout.z > 1 ||
        (i && visibility[ray.visibility_begin + i - 1].z > point.z))
    {
      error(p, DEEP_GPU_FRAME_INVALID_VISIBILITY);
    }
  }
  const auto *radiance = ptr<const DeepGPUFrameRadiance>(p.radiance);
  for (uint32_t i = threadIdx.x; i < ray.radiance_count; i += 128) {
    const auto part = radiance[ray.radiance_begin + i];
    if (!isfinite(part.z) || !isfinite(part.z_back) || part.z < 0 ||
        part.z_back < part.z || !finite_nonnegative(part.tau) ||
        !isfinite(part.rgb.x) || !isfinite(part.rgb.y) || !isfinite(part.rgb.z))
      error(p, DEEP_GPU_FRAME_INVALID_RADIANCE);
  }
}

/* Independent pixels run concurrently; lanes sum independent camera samples.
 * Both sides of a protected cut remain separate. No host reference polyline,
 * per-cut transfer, endpoint calibration or surviving-ray normalization. */
extern "C" __global__ void deep_gpu_frame_transmission(const DeepGPUFrameReduceParameters p)
{
  using Reduce = cub::BlockReduce<DeepGPUFrameKnot, 32>;
  __shared__ typename Reduce::TempStorage storage;
  if (blockIdx.x >= p.pixel_count)
    return;
  const auto pixel = ptr<const DeepGPUFramePixel>(p.pixels)[blockIdx.x];
  if (!range(pixel.ray_begin, pixel.ray_count, p.ray_count) ||
      !range(pixel.cut_begin, pixel.cut_count, p.cut_count))
  {
    if (!threadIdx.x)
      error(p, DEEP_GPU_FRAME_INVALID_RANGE);
    return;
  }
  if (!pixel.population || pixel.ray_count != pixel.population) {
    if (!threadIdx.x)
      error(p, DEEP_GPU_FRAME_INVALID_POPULATION);
    return;
  }
  const auto *rays = ptr<const DeepGPUFrameRay>(p.rays);
  for (uint32_t i = threadIdx.x; i < pixel.ray_count; i += 32) {
    const auto ray = rays[pixel.ray_begin + i];
    if (ray.pixel != blockIdx.x ||
        (i && rays[pixel.ray_begin + i - 1].sample >= ray.sample))
      error(p, DEEP_GPU_FRAME_INVALID_POPULATION);
  }
  const auto *cuts = ptr<const float>(p.cuts);
  auto *output = ptr<DeepGPUFrameTransmission>(p.transmission);
  auto *values = ptr<DeepGPUFrameKnot>(p.values);
  /* Independent cut stripes remove long per-pixel serial tails. Each cut keeps
   * the same lane/ray order and reduction, so this changes only scheduling. */
  for (uint32_t cut = blockIdx.y; cut < pixel.cut_count; cut += gridDim.y) {
    const uint32_t index = pixel.cut_begin + cut;
    const float z = cuts[index];
    if (!isfinite(z) || z < 0 ||
        (cut && ((p.flags & 1u) ? cuts[index - 1] > z : !(cuts[index - 1] < z)))) {
      if (!threadIdx.x)
        error(p, DEEP_GPU_FRAME_INVALID_GRID);
      continue;
    }
    DeepGPUFrameKnot sum{};
    for (uint32_t i = threadIdx.x; i < pixel.ray_count; i += 32) {
      const auto ray = rays[pixel.ray_begin + i];
      /* Failed ranges have already latched status; do not dereference them. */
      if (!range(ray.optical_begin, ray.optical_count, p.optical_count) ||
          !range(ray.visibility_begin, ray.visibility_count, p.visibility_count) ||
          !range(ray.radiance_begin, ray.radiance_count, p.radiance_count))
        continue;
      sum.before.t += transmission_at(p, ray, z, true);
      sum.after.t += transmission_at(p, ray, z, false);
      if (p.values) {
        const V front = radiance_at(p, ray, z, true), back = radiance_at(p, ray, z, false);
        sum.before.r += front.x; sum.before.g += front.y; sum.before.b += front.z;
        sum.after.r += back.x; sum.after.g += back.y; sum.after.b += back.z;
      }
    }
    auto total = Reduce(storage).Reduce(sum, AddKnot{});
    if (!threadIdx.x) {
      const float population = float(pixel.population);
      total.before.t /= population; total.after.t /= population;
      const float scale = p.exposure / population;
      total.before.r *= scale; total.before.g *= scale; total.before.b *= scale;
      total.after.r *= scale; total.after.g *= scale; total.after.b *= scale;
      if (output)
        output[index] = {total.before.t, total.after.t};
      if (values)
        values[index] = total;
    }
    __syncthreads();
  }
}

#include "device/cuda/deep_frame_encode.cu"
#include "device/cuda/deep_frame_native.cu"
#include "device/cuda/deep_frame_grid.cu"
#include "device/cuda/deep_frame_compact.cu"
#include "device/cuda/deep_frame_colour.cu"
#include "device/cuda/deep_frame_simple.cu"
#include "device/cuda/deep_frame_compact_encode.cu"
#include "device/cuda/deep_frame_sort.cu"
#include "device/cuda/deep_frame_archive.cu"
#include "device/cuda/deep_frame_witness.cu"
#include "device/cuda/deep_frame_cases.cu"
