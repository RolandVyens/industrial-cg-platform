/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

namespace {
__device__ double witness_logadd(const double a, const double b)
{
  if (a == -INFINITY) return b;
  if (b == -INFINITY) return a;
  return fmax(a, b) + log1p(exp(fmin(a, b) - fmax(a, b)));
}
__device__ void witness_atomic_log(double *address, const double value)
{
  auto *bits = reinterpret_cast<unsigned long long *>(address);
  unsigned long long old = atomicCAS(bits, 0ull, 0ull), assumed;
  do {
    assumed = old;
    old = atomicCAS(bits, assumed, __double_as_longlong(witness_logadd(__longlong_as_double(assumed), value)));
  } while (old != assumed);
}
__device__ uint32_t witness_pixel(const DeepCompactWitnessParameters &p, const uint32_t camera)
{
  const auto *receipt = ptr<const unsigned char>(p.cameras) + uint64_t(camera) * 128;
  const int64_t x = int32_t(native_u32(receipt, 24)) - int64_t(p.x);
  const int64_t y = int32_t(native_u32(receipt, 28)) - int64_t(p.y);
  return x < 0 || y < 0 || x >= p.width || y >= p.height ? UINT32_MAX : uint32_t(y * p.width + x);
}
}  // namespace

extern "C" __global__ void deep_gpu_frame_compact_witness_init(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < d.n) ptr<double>(d.a)[i] = -INFINITY;
}

extern "C" __global__ void deep_gpu_frame_compact_witness_seal(const DeepGPUFrameDispatch d)
{
  const auto p = *ptr<const DeepCompactWitnessParameters>(d.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.camera_count) return;
  auto &ray = ptr<DeepCompactWitnessRay>(p.rays)[i];
  const uint32_t pixel = witness_pixel(p, i);
  if (pixel == UINT32_MAX || ray.done) return;
  if (p.has_volume) { atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_OPTICAL); return; }
  for (; ray.next_cut < p.cut_count; ray.next_cut++)
    witness_atomic_log(ptr<double>(p.log_t) + uint64_t(pixel) * p.cut_count + ray.next_cut,
                        witness_logadd(ray.log_m, ray.log_q));
  ray.done = 1;
}

/* Direct post-clamp inputs precede any colour discretization or pixel fitting.
 * Both requested alpha formats have independent zero-opacity exclusion. */
extern "C" __global__ void deep_gpu_frame_compact_witness_sources(const DeepGPUFrameDispatch d)
{
  const auto p = *ptr<const DeepCompactWitnessParameters>(d.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n || !p.rgb) return;
  const auto &source = ptr<const DeepCompactColourSource>(d.b)[i];
  if (source.camera < p.camera_base || source.camera - p.camera_base >= p.camera_count) {
    atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  const uint32_t pixel = witness_pixel(p, source.camera - p.camera_base);
  if (pixel == UINT32_MAX) return;
  if (d.c && (source.rgb[0] != 0 || source.rgb[1] != 0 || source.rgb[2] != 0)) {
    const auto trace = *ptr<const DeepCompactTraceParameters>(d.c);
    const auto *camera = ptr<const unsigned char>(p.cameras) + uint64_t(source.camera - p.camera_base) * 128;
    const int32_t x = int32_t(native_u32(camera, 24)), y = int32_t(native_u32(camera, 28));
    const uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
    bool selected = false;
    for (uint32_t j = 0; j < trace.probe_count; j++) selected |= ptr<const uint64_t>(trace.probes)[j] == key;
    if (selected && trace.colour_records) {
      const uint32_t at = atomicAdd(ptr<uint32_t>(trace.colour_counter), 1u);
      if (at >= trace.capacity) atomicAdd(ptr<uint32_t>(trace.colour_counter) + 1, 1u);
      else ptr<DeepCompactColourTrace>(trace.colour_records)[at] =
          {source, x, y, native_u32(camera, 4), 0};
    }
  }
  const double length = double(source.physical_back) - source.physical_front;
  const double opacity = source.kind == 1 ?
      -expm1(-(double(source.sigma[0]) + source.sigma[1] + source.sigma[2]) * length / 3.0) : 1.0;
  const bool half = __half2float(__float2half_rn(float(opacity))) != 0;
  const bool full = float(opacity) != 0;
  for (uint32_t cut = 0; cut < p.cut_count; cut++) {
    const double z = ptr<const double>(p.cuts)[cut];
    double distance = 0;
    if (source.kind == 1 && z >= source.z_back) distance = length;
    else if (source.kind == 1 && z > source.z) {
      const double physical = source.flags & 0x80000000u ?
          source.map_a + sqrt(fmax(0.0, z * z - source.map_b)) :
          (z - source.map_a) / source.map_b;
      distance = z >= source.z_back ? length :
          fmin(length, fmax(0.0, physical - source.physical_front));
    }
    for (int channel = 0; channel < 3; channel++) {
      const double k = source.sigma[channel];
      const double fraction = source.kind == 0 ? double(z >= source.z) :
          k > 0 ? -expm1(-k * distance) / -expm1(-k * length) : distance / length;
      const double colour = double(source.rgb[channel]) * fraction;
      if (!isfinite(colour)) { atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_RADIANCE); return; }
      const uint64_t at = (uint64_t(pixel) * p.cut_count + cut) * 6;
      if (half) atomicAdd(ptr<double>(p.rgb) + at + channel, colour);
      if (full) atomicAdd(ptr<double>(p.rgb) + at + 3 + channel, colour);
    }
  }
}

extern "C" __global__ void deep_gpu_frame_compact_witness_output(const DeepGPUFrameDispatch d)
{
  const auto p = *ptr<const DeepCompactWitnessParameters>(d.a);
  const uint64_t i = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  const uint64_t size = uint64_t(p.width) * p.height * p.cut_count;
  if (i >= size) return;
  const uint32_t pixel = uint32_t(i / p.cut_count);
  const uint64_t film_pixel = uint64_t(p.window_y + pixel / p.width) * p.film_stride + p.window_x + pixel % p.width;
  const uint32_t population = __float_as_uint(ptr<const float>(p.film)[film_pixel * p.pass_stride + p.sample_pass]);
  if (!population) { atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_POPULATION); return; }
  const uint32_t channels = p.rgb ? 7 : 1;
  ptr<double>(p.output)[i * channels] = ptr<const double>(p.log_t)[i] - log(double(population));
  if (p.rgb) for (int c = 0; c < 6; c++) ptr<double>(p.output)[i * channels + 1 + c] =
      ptr<const double>(p.rgb)[i * 6 + c] * double(p.exposure) / population;
}
