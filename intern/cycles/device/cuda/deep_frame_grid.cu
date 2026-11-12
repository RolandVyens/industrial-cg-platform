/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

extern "C" __global__ void deep_gpu_frame_build_rays(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const uint64_t key = ptr<const uint64_t>(p.b)[i];
  const uint32_t original = ptr<const uint32_t>(p.c)[i];
  if (original >= p.n) { atomicOr(ptr<uint32_t>(p.h), DEEP_GPU_FRAME_INVALID_RANGE); return; }
  if (key != UINT64_MAX && i && ptr<const uint64_t>(p.b)[i - 1] == key)
    atomicOr(ptr<uint32_t>(p.h), DEEP_GPU_FRAME_INVALID_POPULATION);
  if (p.d)
    ptr<uint32_t>(p.d)[original] = key == UINT64_MAX ? UINT32_MAX : i;
  if (!p.a)
    return;
  if (key == UINT64_MAX) {
    ptr<DeepGPUFrameRay>(p.a)[i] = {};
    ptr<DeepGPUFrameRay>(p.a)[i].pixel = UINT32_MAX;
    return;
  }
  const auto optical = ptr<const DeepGPUFrameRange>(p.e)[i];
  const auto visibility = ptr<const DeepGPUFrameRange>(p.f)[i];
  const auto radiance = ptr<const DeepGPUFrameRange>(p.g)[i];
  ptr<DeepGPUFrameRay>(p.a)[i] = {optical.offset, optical.count, visibility.offset,
                                 visibility.count, uint32_t(key >> 32), uint32_t(key),
                                 radiance.offset, radiance.count};
  /* Original->sorted mapping is a separate dispatch before primitive sorting.
   * Here p.d holds the mapping's independently owned buffer. */
}

extern "C" __global__ void deep_gpu_frame_build_pixels(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto range = ptr<const DeepGPUFrameRange>(p.a)[i];
  const uint32_t x = i % p.m, y = i / p.m;
  const uint32_t window_x = uint32_t(p.e >> 32), window_y = uint32_t(p.e);
  const uint64_t offset = (uint64_t(y + window_y) * p.width + x + window_x) * p.base + p.pixel;
  const uint32_t population = __float_as_uint(ptr<const float>(p.c)[offset]);
  if (!population || range.count != population || y >= p.k)
    atomicOr(ptr<uint32_t>(p.d), DEEP_GPU_FRAME_INVALID_POPULATION);
  ptr<DeepGPUFramePixel>(p.b)[i] = {range.offset, range.count, 0, 0, population};
}

extern "C" __global__ void deep_gpu_frame_coalesce_radiance(const DeepGPUFrameDispatch p)
{
  const uint32_t ray_index = blockIdx.x, lane = threadIdx.x;
  if (ray_index >= p.n) return;
  const auto ray = ptr<const DeepGPUFrameRay>(p.a)[ray_index];
  const auto *source = ptr<const DeepGPUFrameRadiance>(p.b);
  auto *destination = ptr<DeepGPUFrameRadiance>(p.c);
  uint32_t first = 0, written = 0;
  while (first < ray.radiance_count) {
    const auto basis = source[ray.radiance_begin + first];
    uint32_t end = first + 1;
    if (!lane) {
      while (end < ray.radiance_count) {
        const auto next = source[ray.radiance_begin + end];
        if (next.z != basis.z || next.z_back != basis.z_back ||
            next.tau.x != basis.tau.x || next.tau.y != basis.tau.y || next.tau.z != basis.tau.z)
          break;
        end++;
      }
    }
    end = __shfl_sync(0xffffffffu, end, 0);
    V rgb{0, 0, 0};
    for (uint32_t i = first + lane; i < end; i += 32)
      rgb = Add{}(rgb, source[ray.radiance_begin + i].rgb);
    for (int shift = 16; shift; shift >>= 1) {
      rgb.x += __shfl_down_sync(0xffffffffu, rgb.x, shift);
      rgb.y += __shfl_down_sync(0xffffffffu, rgb.y, shift);
      rgb.z += __shfl_down_sync(0xffffffffu, rgb.z, shift);
    }
    if (!lane) {
      auto value = basis; value.rgb = rgb;
      if (!isfinite(rgb.x) || !isfinite(rgb.y) || !isfinite(rgb.z))
        atomicOr(ptr<uint32_t>(p.e), DEEP_GPU_FRAME_INVALID_RADIANCE);
      destination[ray.radiance_begin + written] = value;
    }
    written++; first = end;
  }
  if (!lane) ptr<uint32_t>(p.d)[ray_index] = written;
}

extern "C" __global__ void deep_gpu_frame_pack_radiance(const DeepGPUFrameDispatch p)
{
  const uint32_t ray_index = blockIdx.x;
  if (ray_index >= p.n) return;
  auto *rays = ptr<DeepGPUFrameRay>(p.a);
  const auto ray = rays[ray_index];
  const uint32_t count = ptr<const uint32_t>(p.d)[ray_index];
  const uint32_t offset = ptr<const uint32_t>(p.e)[ray_index];
  for (uint32_t i = threadIdx.x; i < count; i += blockDim.x)
    ptr<DeepGPUFrameRadiance>(p.c)[offset + i] =
        ptr<const DeepGPUFrameRadiance>(p.b)[ray.radiance_begin + i];
  __syncthreads();
  if (!threadIdx.x) {
    rays[ray_index].radiance_begin = offset;
    rays[ray_index].radiance_count = count;
  }
}

extern "C" __global__ void deep_gpu_frame_grid_count(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameGridParameters>(dispatch.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.ray_count) return;
  const auto ray = ptr<const DeepGPUFrameRay>(p.rays)[i];
  if (ray.pixel >= p.pixel_count) { ptr<uint32_t>(p.ray_counts)[i] = 0; return; }
  const uint64_t count = 2ull + ray.visibility_count + 2ull * ray.radiance_count;
  if (count > UINT32_MAX) {
    atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_CAPACITY); return;
  }
  ptr<uint32_t>(p.ray_counts)[i] = uint32_t(count);
}

extern "C" __global__ void deep_gpu_frame_grid_fill(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameGridParameters>(dispatch.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.ray_count) return;
  const auto ray = ptr<const DeepGPUFrameRay>(p.rays)[i];
  if (ray.pixel >= p.pixel_count) return;
  uint32_t offset = ptr<const uint32_t>(p.ray_offsets)[i];
  const uint32_t count = ptr<const uint32_t>(p.ray_counts)[i];
  if (!range(offset, count, p.capacity)) {
    atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_CAPACITY); return;
  }
  auto *keys = ptr<uint64_t>(p.keys);
  auto *ids = ptr<uint32_t>(p.ids);
  float front = INFINITY, back = 0;
  if (ray.optical_count) {
    front = ptr<const DeepGPUFrameOptical>(p.optical)[ray.optical_begin].z;
    back = ptr<const DeepGPUFrameOptical>(p.optical)[ray.optical_begin + ray.optical_count - 1].z_back;
  }
  const auto add = [&](const float z) {
    if (!isfinite(z) || z < 0) {
      atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_GRID); return;
    }
    keys[offset] = (uint64_t(ray.pixel) << 32) | __float_as_uint(z == 0 ? 0.0f : z);
    ids[offset] = offset;
    offset++;
  };
  /* Reserve the first two keys for optical/empty-domain extents. Radiance and
   * real visibility boundaries remain explicit even under signed cancellation. */
  const uint32_t extent = offset;
  offset += 2;
  const auto *radiance = ptr<const DeepGPUFrameRadiance>(p.radiance);
  for (uint32_t r = 0; r < ray.radiance_count; r++) {
    const auto part = radiance[ray.radiance_begin + r];
    front = fminf(front, part.z); back = fmaxf(back, part.z_back);
    add(part.z); add(part.z_back);
  }
  const auto *visibility = ptr<const DeepGPUFrameVisibility>(p.visibility);
  for (uint32_t v = 0; v < ray.visibility_count; v++) {
    const float z = visibility[ray.visibility_begin + v].z;
    front = fminf(front, z); back = fmaxf(back, z);
    add(z);
  }
  if (!isfinite(front)) { front = 0; back = 1; }
  offset = extent;
  add(front); add(back);
}

extern "C" __global__ void deep_gpu_frame_unique_flags(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto *keys = ptr<const uint64_t>(p.a);
  ptr<uint32_t>(p.b)[i] = keys[i] != UINT64_MAX && (!i || keys[i] != keys[i - 1]);
}

extern "C" __global__ void deep_gpu_frame_unique_scatter(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n || !ptr<const uint32_t>(p.b)[i]) return;
  const uint32_t index = ptr<const uint32_t>(p.c)[i];
  if (index >= p.m) { atomicOr(ptr<uint32_t>(p.f), DEEP_GPU_FRAME_CAPACITY); return; }
  const uint64_t key = ptr<const uint64_t>(p.a)[i];
  ptr<uint64_t>(p.d)[index] = key;
  ptr<float>(p.e)[index] = __uint_as_float(uint32_t(key));
}

extern "C" __global__ void deep_gpu_frame_grid_publish(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= p.n) return;
  const auto range = ptr<const DeepGPUFrameRange>(p.a)[i];
  auto *pixels = ptr<DeepGPUFramePixel>(p.b);
  pixels[i].cut_begin = range.offset;
  pixels[i].cut_count = range.count;
  if (!range.count)
    atomicOr(ptr<uint32_t>(p.c), DEEP_GPU_FRAME_INVALID_GRID);
}

extern "C" __global__ void deep_gpu_frame_midpoints(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameGridParameters>(dispatch.a);
  const uint32_t pixel_index = blockIdx.x;
  if (pixel_index >= p.pixel_count) return;
  const auto pixel = ptr<const DeepGPUFramePixel>(p.pixels)[pixel_index];
  const auto *cuts = ptr<const float>(p.cuts);
  for (uint32_t i = threadIdx.x; i < pixel.cut_count; i += blockDim.x) {
    const uint32_t index = pixel.cut_begin + i;
    const float a = cuts[index], b = i + 1 < pixel.cut_count ? cuts[index + 1] : a;
    ptr<float>(p.midcuts)[index] = fmaf(b - a, dispatch.p, a);
  }
}

extern "C" __global__ void deep_gpu_frame_refine_flags(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameGridParameters>(dispatch.a);
  const uint32_t pixel_index = blockIdx.x;
  if (pixel_index >= p.pixel_count) return;
  const auto pixel = ptr<const DeepGPUFramePixel>(p.pixels)[pixel_index];
  const auto *knots = ptr<const DeepGPUFrameKnot>(p.values);
  const auto *midvalues = ptr<const DeepGPUFrameKnot>(p.midvalues);
  const auto *cuts = ptr<const float>(p.cuts), *midcuts = ptr<const float>(p.midcuts);
  const auto *consumers = ptr<const DeepGPUFrameConsumer>(p.consumers);
  for (uint32_t i = threadIdx.x; i + 1 < pixel.cut_count; i += blockDim.x) {
    const uint32_t index = pixel.cut_begin + i;
    /* FP32 probes can round onto a protected endpoint. Its one-sided value
     * belongs to that cut, not to the open interval being refined. There is
     * no representable interior to split when adjacent depths differ by 1 ULP. */
    if (!(midcuts[index] > cuts[index] && midcuts[index] < cuts[index + 1]))
      continue;
    const float fraction = (midcuts[index] - cuts[index]) / (cuts[index + 1] - cuts[index]);
    bool split = false;
    for (uint32_t c = 0; c < p.consumer_count; c++) {
      auto config = consumers[c];
      config.alpha_tolerance *= 0.5f;
      config.rgb_absolute *= 0.5f; config.rgb_relative *= 0.5f;
      DeepGPUFrameSample sample{};
      if (!sample_for(knots[index].after, knots[index + 1].before,
                       cuts[index], cuts[index + 1], config, sample) ||
          residual(decode(knots[index].after, sample, fraction), midvalues[index].before, config) > 1)
        split = true;
    }
    if (split) {
      const float middle = fmaf(cuts[index + 1] - cuts[index], 0.5f, cuts[index]);
      if (!(middle > cuts[index] && middle < cuts[index + 1]))
        atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_INVALID_ENCODING);
      else
        atomicOr(ptr<uint32_t>(p.flags) + index, 1u);
    }
  }
}

extern "C" __global__ void deep_gpu_frame_refine_scatter(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameGridParameters>(dispatch.a);
  const uint32_t pixel_index = blockIdx.x;
  if (pixel_index >= p.pixel_count) return;
  const auto pixel = ptr<const DeepGPUFramePixel>(p.pixels)[pixel_index];
  const auto *offsets = ptr<const uint32_t>(p.scan_offsets);
  const auto *flags = ptr<const uint32_t>(p.flags);
  const auto *cuts = ptr<const float>(p.cuts);
  auto *destination = ptr<float>(p.midcuts);
  for (uint32_t i = threadIdx.x; i < pixel.cut_count; i += blockDim.x) {
    const uint32_t index = pixel.cut_begin + i, out = index + offsets[index];
    if (out >= p.capacity || (flags[index] && out + 1 >= p.capacity)) {
      atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_CAPACITY); continue;
    }
    destination[out] = cuts[index];
    if (flags[index])
      destination[out + 1] = fmaf(cuts[index + 1] - cuts[index], 0.5f, cuts[index]);
  }
  if (!threadIdx.x && pixel.cut_count) {
    auto *next = ptr<DeepGPUFramePixel>(dispatch.b);
    const uint32_t first = pixel.cut_begin, last = first + pixel.cut_count - 1;
    next[pixel_index] = pixel;
    next[pixel_index].cut_begin = first + offsets[first];
    next[pixel_index].cut_count = pixel.cut_count + offsets[last] + flags[last] - offsets[first];
  }
}
