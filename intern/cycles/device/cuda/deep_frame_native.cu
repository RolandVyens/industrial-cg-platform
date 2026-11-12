/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cub/block/block_radix_sort.cuh>

extern "C" __global__ void deep_gpu_frame_compact_rebase(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  auto &primitive = ptr<DeepCompactPrimitive>(p.a)[i];
  if (primitive.camera >= p.m || uint64_t(primitive.camera) + p.base > UINT32_MAX) {
    atomicOr(ptr<uint32_t>(p.b), DEEP_GPU_FRAME_INVALID_POPULATION);
    return;
  }
  primitive.camera += p.base;
}

namespace {
/* The native POD offsets are checked against offsetof/sizeof in the owning
 * renderer module. Reading resident POD fields does not create a host schema. */
__device__ uint32_t native_u32(const unsigned char *record, const uint32_t offset)
{
  return *reinterpret_cast<const uint32_t *>(record + offset);
}
__device__ float native_float(const unsigned char *record, const uint32_t offset)
{
  return *reinterpret_cast<const float *>(record + offset);
}
__device__ V native_rgb(const unsigned char *record, const uint32_t offset)
{
  return {native_float(record, offset), native_float(record, offset + 4),
          native_float(record, offset + 8)};
}
__device__ V native_tau(const unsigned char *record, const uint32_t offset)
{
  const auto *value = reinterpret_cast<const double *>(record + offset);
  return {float(value[0]), float(value[1]), float(value[2])};
}
__device__ void native_error(const DeepGPUFrameNativeParameters &p, const uint32_t bits)
{
  atomicOr(ptr<uint32_t>(p.status), bits);
}
}  // namespace

/* Seed all actual Combined Film calls, then reverse each camera's independent
 * DAG once. Linearity permits aggregation of call gains before reversal. */
extern "C" __global__ void deep_gpu_frame_native_seed(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameNativeParameters>(dispatch.a);
  const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= p.call_count)
    return;
  const auto *call = ptr<const unsigned char>(p.calls) + uint64_t(index) * 64;
  const uint32_t camera = native_u32(call, 0), head = native_u32(call, 8);
  const float gain = native_float(call, 40);
  if (camera >= p.camera_count || head > p.node_count || !isfinite(gain) || gain < 0 || gain > 1) {
    native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  if (!(native_u32(call, 44) & 1u) || !head)
    return;
  const auto *node = ptr<const unsigned char>(p.nodes) + uint64_t(head - 1) * 48;
  if (native_u32(node, 0) != camera) {
    native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  V *weights = ptr<V>(p.node_weights);
  atomicAdd(&weights[head - 1].x, gain);
  atomicAdd(&weights[head - 1].y, gain);
  atomicAdd(&weights[head - 1].z, gain);
}

extern "C" __global__ void deep_gpu_frame_native_reverse(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameNativeParameters>(dispatch.a);
  const uint32_t camera = blockIdx.x * blockDim.x + threadIdx.x;
  if (camera >= p.camera_count)
    return;
  const auto range = ptr<const DeepGPUFrameRange>(p.node_ranges)[camera];
  if (!::range(range.offset, range.count, p.node_count)) {
    native_error(p, DEEP_GPU_FRAME_INVALID_RANGE); return;
  }
  const auto *order = ptr<const uint32_t>(p.node_order);
  V *weights = ptr<V>(p.node_weights), *source_weights = ptr<V>(p.source_weights);
  for (uint32_t cursor = range.count; cursor;) {
    const uint32_t index = order[range.offset + --cursor];
    if (index >= p.node_count) {
      native_error(p, DEEP_GPU_FRAME_INVALID_RANGE); return;
    }
    const auto *node = ptr<const unsigned char>(p.nodes) + uint64_t(index) * 48;
    const uint32_t op = native_u32(node, 8), a = native_u32(node, 12),
                   b = native_u32(node, 16), source = native_u32(node, 20);
    if (native_u32(node, 0) != camera || native_u32(node, 4) != index + 1 || op > 6 ||
        ((op == 2 || op == 3 || op == 4 || op == 5 || op == 6) && a > index) ||
        ((op == 2 || op == 5 || op == 6) && b > index))
    {
      native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
    }
    const V value = weights[index];
    if (op == 0)
      continue;
    if (op == 1) {
      if (!source || source > p.source_count ||
          native_u32(ptr<const unsigned char>(p.sources) + uint64_t(source - 1) * 96, 0) != camera)
      {
        native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
      }
      source_weights[source - 1] = Add{}(source_weights[source - 1], value);
      continue;
    }
    const V scale = native_rgb(node, 28);
    const float mix = scale.x;
    for (int side = 0; side < 2; side++) {
      uint32_t target = side ? b : a;
      V factor{1, 1, 1};
      if (op == 3 && side)
        continue;
      if (op == 4) {
        if (side) continue;
        factor = scale;
      }
      if (op == 5) {
        const float f = side ? mix : 1 - mix;
        factor = {f, f, f};
      }
      if (op == 6) {
        if (side) continue;
        target = native_u32(node, 24) ? b : a;
      }
      if (!target)
        continue;
      const auto *prior = ptr<const unsigned char>(p.nodes) + uint64_t(target - 1) * 48;
      if (native_u32(prior, 0) != camera || target > index ||
          !isfinite(factor.x) || !isfinite(factor.y) || !isfinite(factor.z))
      {
        native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
      }
      const V contribution{value.x * factor.x, value.y * factor.y, value.z * factor.z};
      weights[target - 1] = Add{}(weights[target - 1], contribution);
    }
  }
}

extern "C" __global__ void deep_gpu_frame_native_radiance(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameNativeParameters>(dispatch.a);
  const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  const uint64_t count = uint64_t(p.homogeneous_count) + p.point_count + p.source_count;
  if (index >= count)
    return;
  DeepGPUFrameRadiance out{};
  uint32_t camera;
  if (index < p.homogeneous_count) {
    const auto *record = ptr<const unsigned char>(p.homogeneous) + uint64_t(index) * 112;
    camera = native_u32(record, 0);
    out = {native_float(record, 32), native_float(record, 36),
           native_tau(record, 48), native_rgb(record, 72)};
  }
  else if (index - p.homogeneous_count < p.point_count) {
    const auto *record = ptr<const unsigned char>(p.points) +
                         uint64_t(index - p.homogeneous_count) * 96;
    camera = native_u32(record, 0);
    out.z = out.z_back = native_float(record, 32);
    if (native_u32(record, 28) & 1u)
      out.rgb = native_rgb(record, 60);
  }
  else {
    const uint32_t source = index - p.homogeneous_count - p.point_count;
    const auto *record = ptr<const unsigned char>(p.sources) + uint64_t(source) * 96;
    camera = native_u32(record, 0);
    out = {native_float(record, 16), native_float(record, 20),
           native_tau(record, 48), native_rgb(record, 32)};
    const V weight = ptr<const V>(p.source_weights)[source];
    out.rgb = {out.rgb.x * weight.x, out.rgb.y * weight.y, out.rgb.z * weight.z};
  }
  if (camera >= p.camera_count || !isfinite(out.z) || !isfinite(out.z_back) ||
      out.z < 0 || out.z_back < out.z || !finite_nonnegative(out.tau) ||
      !isfinite(out.rgb.x) || !isfinite(out.rgb.y) || !isfinite(out.rgb.z))
  {
    native_error(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  ptr<DeepGPUFrameRadiance>(p.radiance)[index] = out;
  ptr<uint32_t>(p.radiance_owners)[index] = p.camera_base + camera;
}

extern "C" __global__ void deep_gpu_frame_native_camera_input(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n)
    return;
  const auto *camera = ptr<const unsigned char>(p.a) + uint64_t(p.base + i) * 128;
  auto *receipt = ptr<unsigned char>(p.b) + uint64_t(i) * 88;
  const int x = int(native_u32(camera, 24)), y = int(native_u32(camera, 28));
  const bool core = int64_t(x) >= int32_t(p.m) && int64_t(y) >= int32_t(p.k) &&
                    int64_t(x) < int64_t(int32_t(p.m)) + p.width &&
                    int64_t(y) < int64_t(int32_t(p.k)) + p.pixel;
  for (int c = 0; c < 4; c++)
    reinterpret_cast<uint32_t *>(receipt)[c] = native_u32(camera, c * 4 + (c > 1 ? 8 : 0));
  *reinterpret_cast<uint32_t *>(receipt + 16) = core && native_u32(camera, 36) != 2 ? 1 : 2;
  *reinterpret_cast<int32_t *>(receipt + 20) = x;
  *reinterpret_cast<int32_t *>(receipt + 24) = y;
  *reinterpret_cast<float *>(receipt + 28) = native_float(camera, 44);
  for (int c = 0; c < 14; c++)
    reinterpret_cast<float *>(receipt + 32)[c] = native_float(camera, 48 + c * 4);
  if (p.c) {
    const auto &scalar = ptr<const DeepCompactRayState>(p.c)[p.base + i];
    if (scalar.flags & (DEEP_COMPACT_TERMINAL | DEEP_COMPACT_EXTINCTION_DONE))
      *reinterpret_cast<uint32_t *>(receipt + 16) = 2u;
    else
      *reinterpret_cast<float *>(receipt + 56) = scalar.cursor;
  }
}

extern "C" __global__ void deep_gpu_frame_owned_optical(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *record = ptr<const unsigned char>(p.a) + uint64_t(i) * 48;
  const uint32_t camera = native_u32(record, 0);
  if (camera >= p.m) {
    atomicOr(ptr<uint32_t>(p.d), DEEP_GPU_FRAME_INVALID_POPULATION); return;
  }
  ptr<DeepGPUFrameOptical>(p.b)[i] = {native_float(record, 16), native_float(record, 20),
                                    native_rgb(record, 24)};
  ptr<uint32_t>(p.c)[i] = p.base + camera;
}

extern "C" __global__ void deep_gpu_frame_owned_visibility(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *record = ptr<const unsigned char>(p.a) + uint64_t(i) * 64;
  const uint32_t camera = native_u32(record, 0);
  if (camera >= p.m) {
    atomicOr(ptr<uint32_t>(p.d), DEEP_GPU_FRAME_INVALID_POPULATION); return;
  }
  ptr<DeepGPUFrameVisibility>(p.b)[i] = {native_float(record, 8), native_rgb(record, 16),
                                       native_rgb(record, 28)};
  ptr<uint32_t>(p.c)[i] = p.base + camera;
}

extern "C" __global__ void deep_gpu_frame_make_keys(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *record = ptr<const unsigned char>(p.a) + uint64_t(i) * p.m;
  ptr<uint64_t>(p.b)[i] = UINT64_MAX;
  ptr<uint32_t>(p.c)[i] = i;
  uint32_t owner = p.e ? ptr<const uint32_t>(p.e)[i] : native_u32(record, p.k);
  if (p.d) {
    if (owner >= p.pixel) {
      atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_INVALID_POPULATION); return;
    }
    owner = ptr<const uint32_t>(p.d)[owner];
  }
  const uint32_t order = p.kind ? native_u32(record, p.width) :
                                 __float_as_uint(native_float(record, p.width));
  ptr<uint64_t>(p.b)[i] = owner == UINT32_MAX ? UINT64_MAX : (uint64_t(owner) << 32) | order;
  ptr<uint32_t>(p.c)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_sort_blocks(const DeepGPUFrameDispatch p)
{
  using Sort = cub::BlockRadixSort<uint64_t, 256, 4, uint32_t>;
  __shared__ typename Sort::TempStorage temporary;
  uint64_t keys[4]; uint32_t ids[4];
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    keys[j] = i < p.n ? ptr<const uint64_t>(p.a)[i] : UINT64_MAX;
    ids[j] = i < p.n ? ptr<const uint32_t>(p.b)[i] : UINT32_MAX;
  }
  Sort(temporary).Sort(keys, ids);
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    if (i < p.n) { ptr<uint64_t>(p.c)[i] = keys[j]; ptr<uint32_t>(p.d)[i] = ids[j]; }
  }
}

extern "C" __global__ void deep_gpu_frame_merge(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const uint32_t run = i / p.width, pair = (run & ~1u) * p.width;
  const bool left = !(run & 1u);
  const uint32_t other = left ? min(p.n, pair + p.width) : pair;
  uint32_t lo = other, hi = min(p.n, other + p.width);
  const auto *keys = ptr<const uint64_t>(p.a);
  const uint64_t key = keys[i];
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (keys[mid] < key || (!left && keys[mid] == key)) lo = mid + 1;
    else hi = mid;
  }
  const uint32_t destination = pair + (i - run * p.width) + (lo - other);
  ptr<uint64_t>(p.c)[destination] = key;
  ptr<uint32_t>(p.d)[destination] = ptr<const uint32_t>(p.b)[i];
}

extern "C" __global__ void deep_gpu_frame_gather(const DeepGPUFrameDispatch p)
{
  const uint64_t byte = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (byte >= uint64_t(p.n) * p.m) return;
  const uint32_t record = uint32_t(byte / p.m), field = uint32_t(byte % p.m);
  const uint32_t source = ptr<const uint32_t>(p.b)[record];
  if (source >= p.n) { atomicOr(ptr<uint32_t>(p.d), DEEP_GPU_FRAME_INVALID_RANGE); return; }
  ptr<unsigned char>(p.c)[byte] = ptr<const unsigned char>(p.a)[uint64_t(source) * p.m + field];
}

extern "C" __global__ void deep_gpu_frame_ranges(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *keys = ptr<const uint64_t>(p.a);
  const uint32_t owner = uint32_t(keys[i] >> 32);
  if (owner == UINT32_MAX || (i && uint32_t(keys[i - 1] >> 32) == owner)) return;
  if (owner >= p.m) { atomicOr(ptr<uint32_t>(p.c), DEEP_GPU_FRAME_INVALID_POPULATION); return; }
  uint32_t lo = i + 1, hi = p.n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (uint32_t(keys[mid] >> 32) == owner) lo = mid + 1;
    else hi = mid;
  }
  ptr<DeepGPUFrameRange>(p.b)[owner] = {i, lo - i, 0, 0};
}

extern "C" __global__ void deep_gpu_frame_camera_keys(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *camera = ptr<const unsigned char>(p.a) + uint64_t(i) * 128;
  const int64_t x = int32_t(native_u32(camera, 24)) - int64_t(int32_t(p.base));
  const int64_t y = int32_t(native_u32(camera, 28)) - int64_t(int32_t(p.pixel));
  uint64_t key = UINT64_MAX;
  if (x >= 0 && y >= 0 && x < p.m && y < p.k)
    key = (uint64_t(y * p.m + x) << 32) | native_u32(camera, 4);
  ptr<uint64_t>(p.b)[i] = key;
  ptr<uint32_t>(p.c)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_validate_optical(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *ledger = ptr<const unsigned char>(p.a) + uint64_t(i) * 16;
  const auto *checkpoint = ptr<const unsigned char>(p.b) + uint64_t(i) * 64;
  const auto *input = ptr<const unsigned char>(p.c) + uint64_t(i) * 88;
  const auto *coverage = ptr<const unsigned char>(p.d) + uint64_t(i) * 32;
  const bool zero = native_u32(input, 16) & 2u;
  const uint32_t events = native_u32(ledger, 8);
  if (native_u32(ledger, 0) != (zero ? 2u : 1u) || native_u32(ledger, 12) ||
      native_u32(checkpoint, 0) || native_u32(checkpoint, 60) ||
      (zero && (events || native_u32(ledger, 4))) ||
      !isfinite(native_float(coverage, 0)) || !isfinite(native_float(coverage, 4)) ||
      native_float(coverage, 4) < native_float(coverage, 0))
    atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_INVALID_OPTICAL);
  atomicAdd(reinterpret_cast<unsigned long long *>(p.e), static_cast<unsigned long long>(events));
}

extern "C" __global__ void deep_gpu_frame_validate_optical_finish(const DeepGPUFrameDispatch p)
{
  if (!blockIdx.x && !threadIdx.x && *ptr<const uint64_t>(p.e) != p.g)
    atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_INVALID_OPTICAL);
}
