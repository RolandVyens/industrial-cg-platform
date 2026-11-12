/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <cub/block/block_scan.cuh>
#include <cub/block/block_reduce.cuh>
#include <float.h>
#include "device/cuda/deep_compact_fit.cuh"

namespace {
__device__ double compact_logadd(const double a, const double b)
{
  if (a == -INFINITY) return b;
  if (b == -INFINITY) return a;
  return fmax(a, b) + log1p(exp(fmin(a, b) - fmax(a, b)));
}
__device__ double compact_logmass(const double mass)
{
  return mass > 0 ? log(mass) : -INFINITY;
}
struct CompactLogAdd {
  __device__ double operator()(const double a, const double b) const
  {
    return compact_logadd(a, b);
  }
};
struct CompactTail {
  double log_tail;
  uint32_t representative;
};
struct CompactTailAdd {
  __device__ CompactTail operator()(const CompactTail a, const CompactTail b) const
  {
    return {compact_logadd(a.log_tail, b.log_tail), min(a.representative, b.representative)};
  }
};
struct CompactSlope {
  double value, magnitude;
  int active;
};
struct CompactSlopeAdd {
  __device__ CompactSlope operator()(const CompactSlope a, const CompactSlope b) const
  {
    return {a.value + b.value, a.magnitude + b.magnitude, a.active + b.active};
  }
};
__device__ void compact_fail(const DeepCompactParameters &p, const uint32_t bits)
{
  atomicOr(ptr<uint32_t>(p.status), bits);
}
}  // namespace

extern "C" __global__ void deep_gpu_frame_compact_camera_pack(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *native = ptr<const unsigned char>(p.a) + uint64_t(i) * 128;
  const auto &ray = ptr<const DeepCompactRayState>(p.b)[i];
  const uint32_t disposition = native_u32(native, 36);
  const int x = int(native_u32(native, 24)), y = int(native_u32(native, 28));
  const bool core = int64_t(x) >= int32_t(p.m) && int64_t(y) >= int32_t(p.k) &&
      int64_t(x) < int64_t(int32_t(p.m)) + p.width && int64_t(y) < int64_t(int32_t(p.k)) + p.pixel;
  double tail = disposition == 2u ? -INFINITY : compact_logadd(ray.log_m, ray.log_q);
  /* A conservative operation-count bound permits only ordinary floating-point
   * roundoff of an exactly unit holdout tail, never an artistic opacity epsilon. */
  const double roundoff = 8.0 * DBL_EPSILON * fmax(1.0, double(ray.next_ordinal));
  if (tail > 0 && tail <= roundoff) tail = 0;
  if (core && (isnan(tail) || tail > 0 || native_float(native, 44) != 1.0f || !(native_u32(native, 32) & 1u) ||
      (ray.flags & (DEEP_COMPACT_PENDING_CELL | DEEP_COMPACT_PENDING_POINT | DEEP_COMPACT_HAS_CANDIDATE))))
    atomicOr(ptr<uint32_t>(p.d), DEEP_GPU_FRAME_INVALID_OPTICAL);
  ptr<DeepCompactCamera>(p.c)[i] = {
      int32_t(native_u32(native, 24)), int32_t(native_u32(native, 28)), native_u32(native, 4),
      disposition, tail, native_float(native, 116), native_float(native, 120)};
}

extern "C" __global__ void deep_gpu_frame_compact_colour_init(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < p.n) ptr<DeepCompactRootColour>(p.a)[p.base + i] = {};
}

extern "C" __global__ void deep_gpu_frame_compact_needs_extinction(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  if (p.c) {
    const auto *camera = ptr<const unsigned char>(p.c) + uint64_t(i) * 128;
    const int64_t x = int32_t(native_u32(camera, 24)), y = int32_t(native_u32(camera, 28));
    if (native_u32(camera, 36) == 2 || x < int32_t(p.m) || y < int32_t(p.k) ||
        x >= int64_t(int32_t(p.m)) + p.width || y >= int64_t(int32_t(p.k)) + p.pixel) return;
  }
  const auto &ray = ptr<const DeepCompactRayState>(p.a)[i];
  const bool pending = ray.flags & (DEEP_COMPACT_PENDING_CELL | DEEP_COMPACT_PENDING_POINT |
                                    DEEP_COMPACT_HAS_CANDIDATE | DEEP_COMPACT_FINISH_PENDING);
  if (pending || !(ray.flags & (DEEP_COMPACT_TERMINAL | DEEP_COMPACT_EXTINCTION_DONE)))
    atomicAdd(ptr<uint32_t>(p.b), 1u);
}

extern "C" __global__ void deep_gpu_frame_compact_root_radiance(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  DeepCompactColourSource out{};
  out.target = UINT32_MAX;
  uint32_t camera;
  if (i < p.m) {
    const auto *root = ptr<const unsigned char>(p.a) + uint64_t(i) * 48;
    const auto &bucket = ptr<const DeepCompactRootColour>(p.b)[i];
    camera = native_u32(root, 36);
    out.z = out.z_back = native_float(root, 0);
    out.flags = native_u32(root, 4); out.ordinal = native_u32(root, 8);
    out.object = native_u32(root, 16); out.prim = native_u32(root, 20);
    out.shader = native_u32(root, 24);
    for (int c = 0; c < 3; c++) out.rgb[c] = bucket.rgb[c];
  }
  else {
    const auto *span = ptr<const unsigned char>(p.c) + uint64_t(i - p.m) * 112;
    camera = native_u32(span, 0);
    out.kind = 1;
    out.z = native_float(span, 32); out.z_back = native_float(span, 36);
    out.physical_front = native_float(span, 40); out.physical_back = native_float(span, 44);
    out.ordinal = native_u32(span, 12);
    out.object = native_u32(span, 84); out.shader = native_u32(span, 88);
    const auto *tau = reinterpret_cast<const double *>(span + 48);
    const double length = double(out.physical_back) - out.physical_front;
    for (int c = 0; c < 3; c++) {
      out.sigma[c] = length > 0 ? float(tau[c] / length) : 0;
      out.rgb[c] = native_float(span, 72 + 4 * c);
    }
  }
  out.camera = p.base + camera;
  if (camera >= p.width || !isfinite(out.z) || !isfinite(out.z_back) || out.z < 0 || out.z_back < out.z) {
    atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  const auto *receipt = ptr<const unsigned char>(p.d) + uint64_t(camera) * 128;
  if (native_u32(receipt, 32) & 2u) {
    out.map_a = native_float(receipt, 104); out.map_b = native_float(receipt, 108);
  }
  else if (out.kind == 1) {
    const double x = double(native_float(receipt, 48)) - p.p;
    const double y = double(native_float(receipt, 52)) - p.q;
    const double z = double(native_float(receipt, 56)) - p.r;
    const double along = x * native_float(receipt, 60) + y * native_float(receipt, 64) + z * native_float(receipt, 68);
    out.map_a = -along;
    out.map_b = fmax(0.0, x * x + y * y + z * z - along * along);
    out.flags |= 0x80000000u;
  }
  for (int c = 0; c < 3; c++)
    if (!isfinite(out.rgb[c]) || !(out.sigma[c] >= 0) || !isfinite(out.sigma[c]))
      atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_INVALID_RADIANCE);
  ptr<DeepCompactColourSource>(p.e)[i] = out;
}

extern "C" __global__ void deep_gpu_frame_compact_camera_keys(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto camera = ptr<const DeepCompactCamera>(p.a)[i];
  const int64_t x = int64_t(camera.x) - int32_t(p.base);
  const int64_t y = int64_t(camera.y) - int32_t(p.pixel);
  uint64_t key = UINT64_MAX;
  if (x >= 0 && y >= 0 && x < p.m && y < p.k)
    key = (uint64_t(y * p.m + x) << 32) | camera.sample;
  ptr<uint64_t>(p.b)[i] = key;
  ptr<uint32_t>(p.c)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_compact_primitive_keys(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.primitive_count) return;
  const auto &primitive = ptr<const DeepCompactPrimitive>(p.primitives)[i];
  uint64_t key = UINT64_MAX;
  if (primitive.camera >= p.camera_count) compact_fail(p, DEEP_GPU_FRAME_INVALID_POPULATION);
  else {
    const uint32_t camera = ptr<const uint32_t>(p.camera_map)[primitive.camera];
    if (camera != UINT32_MAX) key = (uint64_t(camera) << 32) | primitive.ordinal;
  }
  ptr<uint64_t>(dispatch.b)[i] = key;
  ptr<uint32_t>(p.primitive_ids)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_compact_events(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.event_count) return;
  DeepCompactEvent event{};
  event.pixel = UINT32_MAX;
  uint32_t order = 0;
  if (uint64_t(i) < 2ull * p.primitive_count) {
    const uint32_t source = ptr<const uint32_t>(p.primitive_ids)[i / 2];
    const auto &primitive = ptr<const DeepCompactPrimitive>(p.primitives)[source];
    if (primitive.camera >= p.camera_count) {
      compact_fail(p, DEEP_GPU_FRAME_INVALID_POPULATION); return;
    }
    const uint32_t camera = ptr<const uint32_t>(p.camera_map)[primitive.camera];
    if (camera != UINT32_MAX) {
      event.pixel = uint32_t(ptr<const uint64_t>(p.camera_keys)[camera] >> 32);
      event.source = source;
      event.kind = primitive.kind;
      event.z = i & 1u ? primitive.back : primitive.front;
      if (!(event.z >= 0) || !isfinite(event.z) || !(primitive.q >= 0) || !isfinite(primitive.q))
        compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      if (event.z == 0) event.z = 0; /* canonical +0 key */
      if (primitive.kind == DEEP_COMPACT_VOLUME && primitive.back > primitive.front) {
        const double slope = primitive.q / (double(primitive.back) - primitive.front);
        if (!(slope > 0) || !isfinite(slope)) compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
        event.slope = i & 1u ? -slope : slope;
        event.active = i & 1u ? -1 : 1;
      }
      else if (!(i & 1u)) event.jump = primitive.q;
      else event.pixel = UINT32_MAX;
      order = event.kind == DEEP_COMPACT_BACKGROUND ? 3u : event.kind == DEEP_COMPACT_VOLUME ? 0u : 2u;
    }
  }
  else {
    const uint32_t source = i - 2u * p.primitive_count;
    const auto &camera = ptr<const DeepCompactCamera>(p.cameras)[source];
    const uint32_t rank = ptr<const uint32_t>(p.camera_map)[source];
    if (rank != UINT32_MAX && camera.disposition == 2u) {
      event.pixel = uint32_t(ptr<const uint64_t>(p.camera_keys)[rank] >> 32);
      event.source = source;
      event.kind = DEEP_COMPACT_BLACK;
      event.jump = 1.0;
      event.z = 0;
      order = 1u;
    }
  }
  ptr<DeepCompactEvent>(p.events)[i] = event;
  ptr<uint64_t>(p.event_keys)[i] = event.pixel == UINT32_MAX ? UINT64_MAX :
      (uint64_t(order) << 32) | (i < 2u * p.primitive_count ? i : i - 2u * p.primitive_count);
  ptr<uint32_t>(p.event_ids)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_compact_event_keys(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.event_count) return;
  const auto &event = ptr<const DeepCompactEvent>(p.events)[ptr<const uint32_t>(p.event_ids)[i]];
  ptr<uint64_t>(p.event_keys)[i] = event.pixel == UINT32_MAX ? UINT64_MAX :
      (uint64_t(event.pixel) << 32) | __float_as_uint(event.z);
}

/* One CTA per active pixel; chunks carry a fixed state. Each event is consumed
 * once in each direction; no cut queries the camera/ray population. */
extern "C" __global__ void deep_gpu_frame_compact_sweep(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t pixel = blockIdx.x;
  if (pixel >= p.pixel_count) return;
  using Scan = cub::BlockScan<CompactSlope, 256>;
  __shared__ typename Scan::TempStorage storage;
  __shared__ CompactSlope carry;
  if (!threadIdx.x) carry = {0, 0, 0};
  __syncthreads();
  const auto range = ptr<const DeepGPUFrameRange>(p.event_ranges)[pixel];
  for (uint32_t start = 0; start < range.count; start += 256) {
    const uint32_t local = start + threadIdx.x, at = range.offset + local;
    DeepCompactEvent event{};
    if (local < range.count)
      event = ptr<const DeepCompactEvent>(p.events)[ptr<const uint32_t>(p.event_ids)[at]];
    CompactSlope prefix;
    Scan(storage).InclusiveScan(CompactSlope{event.slope, fabs(event.slope), event.active},
                                prefix, CompactSlopeAdd{});
    prefix = CompactSlopeAdd{}(carry, prefix);
    if (local < range.count) {
      const double bound = 8.0 * DBL_EPSILON * double(range.count) * prefix.magnitude;
      double slope = prefix.active == 0 ? 0.0 : prefix.value;
      if (prefix.active < 0 || slope < -bound || !isfinite(slope))
        compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      if (slope < 0 && slope >= -bound) slope = 0;
      double distance = 0;
      if (local + 1 < range.count) {
        const auto &next = ptr<const DeepCompactEvent>(p.events)[ptr<const uint32_t>(p.event_ids)[at + 1]];
        distance = double(next.z) - event.z;
      }
      const double area = slope * distance;
      if (!(area >= 0) || !isfinite(area) || (slope > 0 && distance > 0 && area == 0))
        compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      ptr<double>(p.areas)[at] = area;
    }
    __syncthreads();
    if (threadIdx.x == 255) {
      carry = prefix;
      if (!carry.active) carry.value = 0;
    }
    __syncthreads();
  }
  if (!threadIdx.x && carry.active) compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
}

extern "C" __global__ void deep_gpu_frame_compact_tail(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t pixel = blockIdx.x;
  if (pixel >= p.pixel_count) return;
  using Reduce = cub::BlockReduce<CompactTail, 256>;
  __shared__ typename Reduce::TempStorage storage;
  const auto population = ptr<const DeepGPUFramePixel>(p.pixels)[pixel];
  CompactTail tail{-INFINITY, UINT32_MAX};
  for (uint32_t i = threadIdx.x; i < population.ray_count; i += 256) {
    const uint32_t source = ptr<const uint32_t>(p.camera_ids)[population.ray_begin + i];
    const auto &camera = ptr<const DeepCompactCamera>(p.cameras)[source];
    if (isnan(camera.log_tail) || camera.log_tail > 0)
      compact_fail(p, DEEP_GPU_FRAME_INVALID_POPULATION);
    tail.log_tail = compact_logadd(tail.log_tail, camera.log_tail);
    if (camera.disposition == 1u && camera.projection != 0 && isfinite(camera.projection))
      tail.representative = min(tail.representative, i);
  }
  const CompactTail total = Reduce(storage).Reduce(tail, CompactTailAdd{});
  if (!threadIdx.x) {
    ptr<double>(p.tail)[pixel] = total.log_tail;
    float2 projection = make_float2(0, 0);
    if (total.representative != UINT32_MAX) {
      const auto &camera = ptr<const DeepCompactCamera>(p.cameras)[
          ptr<const uint32_t>(p.camera_ids)[population.ray_begin + total.representative]];
      projection = make_float2(camera.projection, camera.depth_origin);
    }
    ptr<float2>(p.projection)[pixel] = projection;
  }
}

extern "C" __global__ void deep_gpu_frame_compact_reverse(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepCompactParameters>(dispatch.a);
  const uint32_t pixel = blockIdx.x;
  if (pixel >= p.pixel_count) return;
  using Scan = cub::BlockScan<double, 256>;
  __shared__ typename Scan::TempStorage storage;
  __shared__ double carry;
  if (!threadIdx.x) carry = 0;
  __syncthreads();
  const auto range = ptr<const DeepGPUFrameRange>(p.event_ranges)[pixel];
  const double log_tail = ptr<const double>(p.tail)[pixel];
  const uint32_t population = ptr<const DeepGPUFramePixel>(p.pixels)[pixel].population;
  const double log_population = log(double(population));
  for (uint32_t start = 0; start < range.count; start += 256) {
    const uint32_t reverse = start + threadIdx.x;
    const bool valid = reverse < range.count;
    const uint32_t at = valid ? range.offset + range.count - reverse - 1 : range.offset;
    DeepCompactEvent event{};
    double area = 0;
    if (valid) {
      event = ptr<const DeepCompactEvent>(p.events)[ptr<const uint32_t>(p.event_ids)[at]];
      area = ptr<const double>(p.areas)[at];
    }
    const double mass = area + event.jump;
    double suffix, aggregate;
    Scan(storage).ExclusiveSum(mass, suffix, aggregate);
    suffix += carry;
    if (valid) {
      DeepCompactKnot knot{};
      knot.z = event.z; knot.next_z = event.z;
      if (at + 1 < range.offset + range.count)
        knot.next_z = ptr<const DeepCompactEvent>(p.events)[ptr<const uint32_t>(p.event_ids)[at + 1]].z;
      knot.log_next = compact_logadd(log_tail, compact_logmass(suffix)) - log_population;
      knot.log_after = compact_logadd(log_tail, compact_logmass(suffix + area)) - log_population;
      knot.log_before = compact_logadd(log_tail, compact_logmass(suffix + mass)) - log_population;
      knot.point_q = event.jump / population;
      knot.interval_q = area / population;
      knot.kind = event.kind; knot.source = event.source;
      if ((event.jump > 0 && !(knot.point_q > 0)) || (area > 0 && !(knot.interval_q > 0)))
        compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      ptr<DeepCompactKnot>(p.knots)[at] = knot;
    }
    __syncthreads();
    if (!threadIdx.x) carry += aggregate;
    __syncthreads();
  }
}
