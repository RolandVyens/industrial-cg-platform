/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Experimental module: preserve raw validation/filtering and evaluate eligible extinction-only
 * pixels on device. Unsupported pixels are explicitly selected for complete host processing. */
#include "deep_capture.cu"
#include <math.h>
#include <math_constants.h>

struct VolumeRaySummary {
  uint32_t front, back, count, complete;
};
struct VolumePixelGrid {
  float front, back;
  uint32_t intervals, population;
};
struct VolumeDifference {
  float3 slope, intercept;
  int opaque;
};
struct VolumeProfileSample {
  float r, g, b, a, z, z_back;
};
static_assert(sizeof(VolumeProfileSample) == 24, "Deep output sample ABI");

__device__ void volume_top_two(unsigned long long *address, const uint32_t depth)
{
  unsigned long long old = atomicCAS(address, 0ull, 0ull);
  for (;;) {
    const uint32_t first = uint32_t(old >> 32), second = uint32_t(old);
    unsigned long long next = old;
    if (depth > first) next = (uint64_t(depth) << 32) | first;
    else if (depth < first && depth > second) next = (uint64_t(first) << 32) | depth;
    if (next == old) return;
    const auto observed = atomicCAS(address, old, next);
    if (observed == old) return;
    old = observed;
  }
}

extern "C" __global__ void deep_volume_initialize(VolumeRaySummary *rays,
    uint32_t slots, uint32_t *fallback, uint32_t pixels, unsigned long long *endpoints)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < slots) rays[i] = {0x7f800000u, 0, 0, 0};
  if (i < pixels) { fallback[i] = 0; endpoints[i] = 0; }
}

extern "C" __global__ void deep_volume_summarize(const RawEvent *events,
    const uint8_t *keep, uint32_t n, uint32_t samples, VolumeRaySummary *rays,
    uint32_t *fallback, unsigned long long *endpoints)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n || !keep[i]) return;
  const RawEvent e = events[i];
  const uint32_t camera = e.flags >> 8;
  if (!(e.flags & 2u) || (e.flags & 1u) || camera >= samples ||
      !isfinite(e.rgba[3]) || !isfinite(e.z) || !isfinite(e.z_back) ||
      e.z <= 0 || e.z_back <= e.z + 1e-6f ||
      ((e.flags & 32u) && (!isfinite(__uint_as_float(e.metadata[0])) ||
                          !isfinite(__uint_as_float(e.metadata[1])) ||
                          !isfinite(__uint_as_float(e.metadata[2])) ||
                          fminf(__uint_as_float(e.metadata[0]),
                                fminf(__uint_as_float(e.metadata[1]),
                                      __uint_as_float(e.metadata[2]))) < 0)) ||
      e.rgba[0] != 0 || e.rgba[1] != 0 || e.rgba[2] != 0)
  {
    atomicOr(fallback + e.pixel, 1u);
    return;
  }
  VolumeRaySummary *ray = rays + size_t(e.pixel) * samples + camera;
  atomicMin(&ray->front, __float_as_uint(e.z));
  atomicMax(&ray->back, __float_as_uint(e.z_back));
  atomicAdd(&ray->count, 1u);
  if (e.flags & 8u) atomicOr(&ray->complete, 1u);
  volume_top_two(endpoints + e.pixel, __float_as_uint(e.z));
  volume_top_two(endpoints + e.pixel, __float_as_uint(e.z_back));
}

/* Integer-only warp aggregation preserves the exact count/min/max/OR and two largest distinct
 * positive float bit patterns. No optical arithmetic or event ordering is changed. */
extern "C" __global__ void deep_volume_summarize_warp(const RawEvent *events,
    const uint8_t *keep, uint32_t n, uint32_t samples, VolumeRaySummary *rays,
    uint32_t *fallback, unsigned long long *endpoints)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  const unsigned active = __activemask();
  const bool retained = i < n && keep[i];
  RawEvent e = {};
  if (retained) e = events[i];
  const uint32_t camera = e.flags >> 8;
  const bool valid = retained && (e.flags & 2u) && !(e.flags & 1u) && camera < samples &&
      isfinite(e.rgba[3]) && isfinite(e.z) && isfinite(e.z_back) &&
      e.z > 0 && e.z_back > e.z + 1e-6f &&
      (!(e.flags & 32u) || (isfinite(__uint_as_float(e.metadata[0])) &&
                            isfinite(__uint_as_float(e.metadata[1])) &&
                            isfinite(__uint_as_float(e.metadata[2])) &&
                            fminf(__uint_as_float(e.metadata[0]),
                                  fminf(__uint_as_float(e.metadata[1]),
                                        __uint_as_float(e.metadata[2]))) >= 0)) &&
      e.rgba[0] == 0 && e.rgba[1] == 0 && e.rgba[2] == 0;
  const unsigned participants = __ballot_sync(active, valid);
  if (retained && !valid) atomicOr(fallback + e.pixel, 1u);
  if (!valid) return;

  const unsigned group = __match_any_sync(participants, (uint64_t(e.pixel) << 32) | camera);
  const uint32_t front = __float_as_uint(e.z), back = __float_as_uint(e.z_back);
  const uint32_t first = __reduce_max_sync(group, back);
  const uint32_t second = __reduce_max_sync(group,
      max(front < first ? front : 0u, back < first ? back : 0u));
  const uint32_t minimum = __reduce_min_sync(group, front);
  const uint32_t complete = __reduce_or_sync(group, e.flags & 8u);
  if ((threadIdx.x & 31) != uint32_t(__ffs(group) - 1)) return;

  VolumeRaySummary *ray = rays + size_t(e.pixel) * samples + camera;
  atomicMin(&ray->front, minimum);
  atomicMax(&ray->back, first);
  atomicAdd(&ray->count, uint32_t(__popc(group)));
  if (complete) atomicOr(&ray->complete, 1u);
  volume_top_two(endpoints + e.pixel, first);
  if (second) volume_top_two(endpoints + e.pixel, second);
}

extern "C" __global__ void deep_volume_grid(const VolumeRaySummary *rays,
    uint32_t pixels, uint32_t samples, uint32_t maximum_intervals, const float *population,
    const unsigned long long *endpoints, uint32_t *fallback, uint32_t *ray_counts,
    uint32_t *ordered_cameras, VolumePixelGrid *grids)
{
  const uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
  if (p >= pixels) return;
  uint32_t *order = ordered_cameras + size_t(p) * samples;
  const VolumeRaySummary *pixel = rays + size_t(p) * samples;
  uint32_t count = 0, segments = 0;
  float first = CUDART_INF_F;
  for (uint32_t s = 0; s < samples; s++) {
    const auto r = pixel[s];
    if (!r.count) continue;
    if (!r.complete) fallback[p] = 1;
    uint32_t j = count;
    // Camera ID breaks equal-front ties deterministically; differential gates cover summation.
    while (j && pixel[order[j - 1]].front > r.front) {
      order[j] = order[j - 1]; j--;
    }
    order[j] = s; count++; segments += r.count;
    first = fminf(first, __uint_as_float(r.front));
  }
  const float last = __uint_as_float(uint32_t(endpoints[p] >> 32));
  const float second = __uint_as_float(uint32_t(endpoints[p]));
  if (count && (last - second <= 1e-6f || (count == 1 && segments <= maximum_intervals)))
    fallback[p] = 1;
  if (!isfinite(population[p]) || population[p] < 1 || maximum_intervals == 0 ||
      maximum_intervals > 256 || samples == 0 || float(count) > population[p]) fallback[p] = 1;
  ray_counts[p + 1] = fallback[p] ? 0 : count;
  if (p == 0) ray_counts[0] = 0;
  float span = 0;
  for (uint32_t j = 0; j < count; j++) {
    const auto r = pixel[order[j]];
    span += __uint_as_float(r.back) - __uint_as_float(r.front);
  }
  uint32_t intervals = 0;
  if (count && !fallback[p]) {
    const float ratio = (last - first) / fmaxf(span / float(segments), 1e-6f);
    /* Equal-depth ray ordering may differ from the host's unstable std::sort. Preserve the
     * complete host path near an integer grid transition instead of choosing another grid. */
    if (ratio < float(maximum_intervals) &&
        fabsf(ratio - roundf(ratio)) <= fmaxf(1e-5f, ratio * float(count) * 2.3841858e-7f))
      fallback[p] = 1;
    intervals = min(maximum_intervals, max(1u, uint32_t(ceilf(ratio))));
  }
  ray_counts[p + 1] = fallback[p] ? 0 : count;
  grids[p] = {first, last, intervals, uint32_t(population[p])};
}

extern "C" __global__ void deep_volume_assign_rays(const uint32_t *ordered_cameras,
    const uint32_t *offsets, uint32_t pixels, uint32_t samples, uint32_t *ray_map,
    uint32_t *ray_pixels, const uint32_t *pixel_prefix_offsets, uint32_t *ray_prefix_offsets,
    const VolumePixelGrid *grids)
{
  const uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
  if (p >= pixels) return;
  for (uint32_t j = offsets[p]; j < offsets[p + 1]; j++) {
    const uint32_t camera = ordered_cameras[size_t(p) * samples + j - offsets[p]];
    ray_map[size_t(p) * samples + camera] = j;
    ray_pixels[j] = p;
    ray_prefix_offsets[j] = pixel_prefix_offsets[p] + (j - offsets[p]) * (grids[p].intervals + 2);
  }
}

__device__ float volume_boundary(const VolumePixelGrid grid, const uint32_t i)
{
  /* Match the invariant reciprocal used by the current /fp:fast host grid builder. */
  const float fraction = float(i) * (1.0f / float(grid.intervals));
  return grid.front + (grid.back - grid.front) * fraction;
}

__device__ uint32_t volume_lower(const VolumePixelGrid grid, const float z, const bool upper)
{
  uint32_t lo = 0, hi = grid.intervals + 1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const float depth = volume_boundary(grid, mid);
    if (depth < z || (upper && depth == z)) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

__device__ void deep_volume_difference_group(const RawEvent *events,
    const uint64_t *keys, const uint32_t *ids, const uint8_t *keep, uint32_t start,
    uint32_t end, uint32_t p, uint32_t camera,
    uint32_t samples, const uint32_t *ray_map, const VolumePixelGrid *grids,
    VolumeDifference *differences, uint32_t *fallback,
    const uint32_t *initial_fallback, float *last_back, const uint32_t *ray_prefix_offsets)
{
  if (initial_fallback[p] || camera >= samples) return;
  // Aborted-only groups have no retained ray and therefore no initialized ray-map slot.
  // Filter them before reading any per-ray storage, including the preceding-depth state.
  uint32_t first = start;
  while (first < end && (!keys || keys[first] == keys[start]) && !keep[ids[first]]) first++;
  if (first == end || (keys && keys[first] != keys[start])) return;
  const auto grid = grids[p];
  const uint32_t ray = ray_map[size_t(p) * samples + camera];
  auto *difference = differences + ray_prefix_offsets[ray];
  float previous = last_back[ray];
  for (uint32_t i = first; i < end && (!keys || keys[i] == keys[start]); i++) {
    const uint32_t id = ids[i]; if (!keep[id]) continue;
    const RawEvent e = events[id];
    // Retain a complete host fallback for reordered or overlapping input segments.
    if (e.z < previous) { atomicOr(fallback + p, 1u); return; }
    previous = e.z_back;
    const uint32_t partial = volume_lower(grid, e.z, true), size = grid.intervals + 1;
    if (partial == size) continue;
    float3 log_t;
    if (e.flags & 32u) {
      const float x = __uint_as_float(e.metadata[0]), y = __uint_as_float(e.metadata[1]),
                  z = __uint_as_float(e.metadata[2]);
      if (!isfinite(x) || !isfinite(y) || !isfinite(z) || fminf(x, fminf(y,z)) < 0) {
        atomicOr(fallback + p, 1u); return;
      }
      log_t = make_float3(-x,-y,-z);
    }
    else {
      const float t = 1 - fminf(fmaxf(e.rgba[3],0),1);
      if (t <= 1e-8f) {
        difference[partial].opaque++; difference[size].opaque--; continue;
      }
      log_t = make_float3(logf(t),logf(t),logf(t));
    }
    // A finite endpoint tau cannot make the entire interval opaque at its first inner boundary.
    const uint32_t full = volume_lower(grid,e.z_back,false);
    float *tau = &log_t.x;
    for (int c=0;c<3;c++) {
      float *sp=&difference[partial].slope.x, *sf=&difference[full].slope.x;
      float *ip=&difference[partial].intercept.x, *ifull=&difference[full].intercept.x;
      float *iend=&difference[size].intercept.x;
      if (partial < full) {
        const float slope=tau[c]/(e.z_back-e.z), intercept=-slope*e.z;
        if (!isfinite(slope) || !isfinite(intercept)) {
          atomicOr(fallback + p, 1u); return;
        }
        sp[c]+=slope; sf[c]-=slope; ip[c]+=intercept; ifull[c]-=intercept;
      }
      ifull[c]+=tau[c]; iend[c]-=tau[c];
    }
  }
  last_back[ray] = previous;
}

extern "C" __global__ void deep_volume_difference(const RawEvent *events,
    const uint64_t *keys, const uint32_t *ids, const uint8_t *keep, uint32_t n,
    uint32_t samples, const uint32_t *ray_map, const VolumePixelGrid *grids,
    VolumeDifference *differences, uint32_t *fallback,
    const uint32_t *initial_fallback, float *last_back, const uint32_t *ray_prefix_offsets)
{
  const uint32_t start = blockIdx.x * blockDim.x + threadIdx.x;
  if (start >= n || (start && keys[start] == keys[start - 1])) return;
  deep_volume_difference_group(events, keys, ids, keep, start, n,
      uint32_t(keys[start] >> 32), uint32_t(keys[start]), samples, ray_map, grids,
      differences, fallback, initial_fallback, last_back, ray_prefix_offsets);
}

extern "C" __global__ void deep_volume_difference_group_spread(const RawEvent *events,
    const uint32_t *ids, const uint8_t *keep, const uint2 *heads, uint32_t groups,
    uint32_t samples, const uint32_t *ray_map, const VolumePixelGrid *grids,
    VolumeDifference *differences, uint32_t *fallback,
    const uint32_t *initial_fallback, float *last_back, const uint32_t *ray_prefix_offsets,
    uint32_t lanes)
{
  const uint64_t index = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index & (lanes - 1)) return;
  const uint32_t group = uint32_t(index >> (__ffs(lanes) - 1));
  if (group >= groups) return;
  const uint2 head = heads[group];
  if (initial_fallback[head.y]) return;
  const uint32_t start = head.x;
  const RawEvent event = events[ids[start]];
  const uint32_t camera = (event.flags & 2u) ? event.flags >> 8 : 0xffffffffu;
  deep_volume_difference_group(events, nullptr, ids, keep, start, heads[group + 1].x,
      event.pixel, camera, samples, ray_map, grids, differences, fallback,
      initial_fallback, last_back, ray_prefix_offsets);
}

/* Real heads are sorted by (pixel,camera); the terminal {n,0} is an end pointer only,
 * never a member of the pixel search. Return one compact range per window endpoint. */
extern "C" __global__ void deep_volume_group_window_bounds(const uint2 *heads,
    uint32_t groups, const uint32_t *pixels, uint32_t endpoints, uint32_t *bounds)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= endpoints) return;
  uint32_t first = 0, last = groups;
  while (first < last) {
    const uint32_t middle = first + (last - first) / 2;
    if (heads[middle].y < pixels[i]) first = middle + 1;
    else last = middle;
  }
  bounds[i] = first;
}

extern "C" __global__ void deep_volume_transmittance_checked(const VolumeDifference *differences,
    const uint32_t *ray_pixels, const VolumePixelGrid *grids, uint32_t rays,
    float *transmittance, const uint32_t *ray_prefix_offsets, uint32_t *fallback)
{
  const uint32_t ray=blockIdx.x*blockDim.x+threadIdx.x;if(ray>=rays)return;
  const auto grid=grids[ray_pixels[ray]];
  const auto *difference=differences+ray_prefix_offsets[ray];
  float3 slope=make_float3(0,0,0),intercept=make_float3(0,0,0);int opaque=0;
  for(uint32_t i=0;i<=grid.intervals;i++) {
    for(int c=0;c<3;c++) {
      (&slope.x)[c]+=(&difference[i].slope.x)[c];
      (&intercept.x)[c]+=(&difference[i].intercept.x)[c];
    }
    opaque+=difference[i].opaque;
    const float depth=volume_boundary(grid,i);
    const float x=slope.x*depth+intercept.x,y=slope.y*depth+intercept.y,z=slope.z*depth+intercept.z;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
      atomicOr(fallback + ray_pixels[ray], 1u); return;
    }
    float t=0;
    if(!opaque) {
      if(x==y&&x==z){const float value=expf(x);t=(value+value+value)*(1.0f/3.0f);}
      else t=(expf(x)+expf(y)+expf(z))*(1.0f/3.0f);
    }
    transmittance[ray_prefix_offsets[ray]+i]=fminf(fmaxf(t,0),1);
  }
}

extern "C" __global__ void deep_volume_finish(const uint32_t *ray_offsets,
    const float *ray_transmittance, const VolumePixelGrid *grids, const float *beauty,
    uint32_t pixels, const uint32_t *fallback, VolumeProfileSample *output, uint32_t *counts,
    uint32_t maximum_intervals, const uint32_t *ray_prefix_offsets)
{
  const uint32_t p=blockIdx.x*blockDim.x+threadIdx.x;if(p>=pixels)return;
  counts[p+1]=0;if(p==0)counts[0]=0;
  const auto grid=grids[p];if(fallback[p]||!grid.intervals)return;
  float t[257];
  for(uint32_t i=0;i<=grid.intervals;i++) {
    float sum=float(grid.population-(ray_offsets[p+1]-ray_offsets[p]));
    for(uint32_t ray=ray_offsets[p];ray<ray_offsets[p+1];ray++)sum+=ray_transmittance[ray_prefix_offsets[ray]+i];
    t[i]=fminf(fmaxf(sum/float(grid.population),0),1);
    if(i)t[i]=fminf(t[i],t[i-1]);
  }
  const float target_alpha=1-t[grid.intervals];
  /* Only adjacent radiance values are needed for an output interval. Keep their original
   * expressions, but avoid the 3084-byte per-thread radiance array in local memory. */
  uint32_t count=0;
  for(uint32_t i=0;i<grid.intervals;i++) {
    if(t[i]<=1e-8f)break;
    const float back=fminf(t[i+1],t[i]);
    const float alpha=fminf(fmaxf(1-back/t[i],0),1);
    if(alpha>1e-8f) {
      float front_radiance[3], back_radiance[3];
      for(int c=0;c<3;c++) {
        const float target=beauty[size_t(p)*4+c];
        const bool distribute=fabsf(target)>1e-8f&&target_alpha>1e-8f;
        front_radiance[c]=distribute?target*(1-t[i])/target_alpha:0;
        back_radiance[c]=distribute?target*(1-t[i+1])/target_alpha:0;
      }
      output[size_t(p)*maximum_intervals+count++]={(back_radiance[0]-front_radiance[0])/t[i],
        (back_radiance[1]-front_radiance[1])/t[i],(back_radiance[2]-front_radiance[2])/t[i],
        alpha,volume_boundary(grid,i),volume_boundary(grid,i+1)};
    }
  }
  counts[p+1]=count;
}

extern "C" __global__ void deep_volume_gather(const VolumeProfileSample *source,
    const uint32_t *offsets, VolumeProfileSample *output, uint32_t pixels, uint32_t maximum_intervals)
{
  const uint32_t p=blockIdx.x*blockDim.x+threadIdx.x;if(p>=pixels)return;
  for(uint32_t i=offsets[p];i<offsets[p+1];i++)output[i]=source[size_t(p)*maximum_intervals+i-offsets[p]];
}

/* One block owns one pixel. Grid points retain their original ordered ray sums, while
 * shared transmittance replaces the large private array and interval writes run together. */
extern "C" __global__ void deep_volume_finish_cooperative(const uint32_t *ray_offsets,
    const float *ray_transmittance, const VolumePixelGrid *grids, const float *beauty,
    uint32_t pixels, const uint32_t *fallback, VolumeProfileSample *output, uint32_t *counts,
    uint32_t maximum_intervals, const uint32_t *ray_prefix_offsets)
{
  const uint32_t p = blockIdx.x, lane = threadIdx.x;
  if (p >= pixels) return;
  if (lane == 0) { counts[p + 1] = 0; if (p == 0) counts[0] = 0; }
  const auto grid = grids[p];
  if (fallback[p] || !grid.intervals) return;
  __shared__ float t[257];
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage scan;
  for (uint32_t i = lane; i <= grid.intervals; i += 256) {
    float sum = float(grid.population - (ray_offsets[p + 1] - ray_offsets[p]));
    for (uint32_t ray = ray_offsets[p]; ray < ray_offsets[p + 1]; ray++)
      sum += ray_transmittance[ray_prefix_offsets[ray] + i];
    t[i] = fminf(fmaxf(sum / float(grid.population), 0), 1);
  }
  __syncthreads();
  if (lane == 0)
    for (uint32_t i = 1; i <= grid.intervals; i++) t[i] = fminf(t[i], t[i - 1]);
  __syncthreads();
  float alpha = 0;
  if (lane < grid.intervals && t[lane] > 1e-8f)
    alpha = fminf(fmaxf(1 - fminf(t[lane + 1], t[lane]) / t[lane], 0), 1);
  const uint32_t keep = alpha > 1e-8f;
  uint32_t index, total;
  Scan(scan).ExclusiveSum(keep, index, total);
  if (keep) {
    const float target_alpha = 1 - t[grid.intervals];
    float rgb[3];
    for (int c = 0; c < 3; c++) {
      const float target = beauty[size_t(p) * 4 + c];
      const bool distribute = fabsf(target) > 1e-8f && target_alpha > 1e-8f;
      const float front = distribute ? target * (1 - t[lane]) / target_alpha : 0;
      const float back = distribute ? target * (1 - t[lane + 1]) / target_alpha : 0;
      rgb[c] = (back - front) / t[lane];
    }
    output[size_t(p) * maximum_intervals + index] = {rgb[0], rgb[1], rgb[2], alpha,
                                                   volume_boundary(grid, lane),
                                                   volume_boundary(grid, lane + 1)};
  }
  if (lane == 0) counts[p + 1] = total;
}

extern "C" __global__ void deep_raw_select(const RawEvent *events, uint8_t *keep,
    const uint32_t *pixel_selection, uint32_t n)
{
  const uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;
  if(i<n&&!pixel_selection[events[i].pixel])keep[i]=0;
}
