/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <cuda_fp16.h>
#include <cub/block/block_scan.cuh>

#include "kernel/integrator/deep_bounded_records.h"

namespace {

constexpr int block_size = 256;

#include "kernel/integrator/deep_bounded_numeric.h"

__device__ uint32_t thread_index()
{
  return blockIdx.x * blockDim.x + threadIdx.x;
}

__device__ uint64_t logical_pixel(const DeepBoundedDispatch &p, const uint32_t local)
{
  const auto &packet = p.capture.packet;
  const uint64_t dx = uint32_t(packet.owned_x) - uint32_t(p.collector.x);
  const uint64_t dy = uint32_t(packet.owned_y) - uint32_t(p.collector.y);
  return (dy + local / packet.owned_pitch) * p.collector.width + dx + local % packet.owned_pitch;
}

struct ActivePixelCounts {
  uint32_t owned, native;

  __host__ __device__ ActivePixelCounts operator+(const ActivePixelCounts &other) const
  {
    return {owned + other.owned, native + other.native};
  }
};

__device__ void map_active_pixels(const DeepBoundedDispatch &p)
{
  const auto &packet = p.capture.packet;
  const uint32_t pixel = thread_index();
  const uint32_t x = pixel % p.native_width;
  const uint32_t y = pixel / p.native_width;
  const float *film = deep_bounded_ptr<const float>(p.film);
  const bool active = pixel < p.size &&
      film[p.film_first + (uint64_t(y) * p.film_row_stride + x) * p.film_pass_stride] == 0.0f;
  const uint32_t dx = uint32_t(p.native_x) + x - uint32_t(packet.owned_x);
  const uint32_t dy = uint32_t(p.native_y) + y - uint32_t(packet.owned_y);
  const bool owned = pixel < p.size && dx < packet.owned_w && dy < packet.owned_h;

  using Scan = cub::BlockScan<ActivePixelCounts, block_size>;
  __shared__ typename Scan::TempStorage scan;
  __shared__ ActivePixelCounts first;
  ActivePixelCounts rank, total;
  Scan(scan).ExclusiveSum(ActivePixelCounts{uint32_t(active && owned), uint32_t(active)}, rank, total);
  uint32_t *map = deep_bounded_ptr<uint32_t>(p.capture.active_map);
  if (threadIdx.x == 0) {
    first.owned = total.owned ? atomicAdd(map, total.owned) : 0;
    first.native = total.native ? atomicAdd(map + 1, total.native) : 0;
  }
  __syncthreads();
  if (owned) {
    const uint32_t local = dy * packet.owned_pitch + dx;
    map[2 + local] = active ? first.owned + rank.owned : UINT32_MAX;
    if (active) {
      map[2 + uint64_t(packet.plane_pitch) + first.owned + rank.owned] = local;
    }
  }
  if (active) {
    deep_bounded_ptr<uint32_t>(p.capture.native_active_map)[first.native + rank.native] = pixel;
  }
}

constexpr uint32_t sample_next_mask = (1u << 19) - 1u;
constexpr uint32_t sample_next_shift = 12u;

/* Camera producers have joined. Use only otherwise idle header bits, and
 * prepend in reverse camera order to keep every bucket's original fold. */
__device__ void index_bucket_samples(uint32_t *headers, const uint32_t samples,
                                     uint32_t *heads, const uint32_t pitch,
                                     const uint32_t rank)
{
  uint32_t first[DEEP_BOUNDED_BUCKETS] = {};
  for (uint32_t sample = samples; sample > 0; --sample) {
    uint32_t &header = headers[uint64_t(rank) * samples + sample - 1];
    const uint32_t original = header;
    if (!(original & DEEP_BOUNDED_VALID)) continue;
    const uint32_t bucket = (original >> DEEP_BOUNDED_BUCKET_SHIFT) & 15u;
    header = (original & ~(sample_next_mask << sample_next_shift)) |
             (first[bucket] << sample_next_shift);
    first[bucket] = sample;
  }
  for (uint32_t b = 0; b < DEEP_BOUNDED_BUCKETS; ++b) {
    heads[uint64_t(b) * pitch + rank] = first[b];
  }
}

template<bool Physical> __device__ void begin_packet(const DeepBoundedDispatch &p)
{
  if (p.first & DeepBoundedBeginFlags::BUCKET_INDEX) {
    const uint32_t rank = thread_index();
    if (rank < p.root_pitch) {
      index_bucket_samples(deep_bounded_ptr<uint32_t>(p.capture.headers),
                            p.capture.packet.num_samples,
                            deep_bounded_ptr<uint32_t>(p.scan_input), p.root_pitch, rank);
    }
    return;
  }

  const uint32_t i = thread_index();
  if (i == 0) {
    auto &control = *deep_bounded_ptr<DeepBoundedPacketControl>(p.capture.packet_device);
    control.packet = p.capture.packet;
    control.root_pitch = p.root_pitch;
    control.track_camera = p.track_camera;
    control.native_width = p.native_width;
    control.native_count = p.native_count;
    control.native_columns = p.native_columns;
    control.native_x = p.native_x;
    control.native_y = p.native_y;
    control.native_offset = p.native_offset;
    control.native_stride = p.film_row_stride;
    if (p.next_main_index) {
      *deep_bounded_ptr<uint32_t>(p.next_main_index) = 0;
    }
    if (p.next_shadow_index) {
      *deep_bounded_ptr<uint32_t>(p.next_shadow_index) = 0;
    }
    if (p.first & DeepBoundedBeginFlags::FRAME) {
      *deep_bounded_ptr<uint32_t>(p.capture.packet.status_ptr) = 0;
    }
  }
  if (i < p.size) {
    deep_bounded_ptr<uint32_t>(p.capture.headers)[i] = 0;
    if (p.capture.holdout_mq) {
      float *mq = deep_bounded_ptr<float>(p.capture.holdout_mq) + uint64_t(i) * 2;
      mq[0] = 1.0f;
      mq[1] = 0.0f;
    }
    if constexpr (Physical) {
      float *pending = deep_bounded_ptr<float>(p.capture.no_anchor) + uint64_t(i) * 3;
      pending[0] = pending[1] = pending[2] = 0.0f;
    }
  }
  if ((p.first & DeepBoundedBeginFlags::TILE) && i < p.capture.packet.plane_pitch) {
    const uint64_t pixel = logical_pixel(p, i);
    for (uint32_t b = 0; b < DEEP_BOUNDED_BUCKETS; ++b) {
      const uint64_t h = pixel + uint64_t(b) * p.collector.capacity_pixels;
      deep_bounded_ptr<uint32_t>(p.collector.counts)[h] = 0;
      deep_bounded_ptr<uint32_t>(p.collector.populations)[h] = 0;
    }
  }
}

template<bool Physical>
__device__ __forceinline__ bool pending_source_valid(
    const DeepBoundedDispatch &p, const uint32_t root, const uint32_t pixel,
    const uint32_t sample, const uint32_t count)
{
  if constexpr (Physical) {
    const float *pending = deep_bounded_ptr<const float>(p.capture.no_anchor) + uint64_t(root) * 3;
    if (pending[0] != 0.0f || pending[1] != 0.0f || pending[2] != 0.0f) {
      if (!(atomicOr(deep_bounded_ptr<uint32_t>(p.capture.packet.status_ptr),
                      DEEP_BOUNDED_NO_SOURCE) & DEEP_BOUNDED_NO_SOURCE))
      {
        printf("DEEP_BOUNDED_NO_SOURCE pixel=(%d,%d) sample=%d records=%u rgb=(%g,%g,%g)\n",
               p.capture.packet.owned_x + int(pixel % p.capture.packet.owned_pitch),
               p.capture.packet.owned_y + int(pixel / p.capture.packet.owned_pitch),
               p.capture.packet.start_sample + int(sample), count,
               pending[0], pending[1], pending[2]);
      }
      return false;
    }
  }
  return true;
}

template<bool Physical> __device__ void accumulate(const DeepBoundedDispatch &p)
{
  const auto &packet = p.capture.packet;
  const uint32_t work = thread_index();
  const bool parallel_buckets = (p.first & DeepBoundedAccumulateFlags::PARALLEL_BUCKETS) != 0;
  if (work >= uint64_t(p.root_pitch) * (parallel_buckets ? DEEP_BOUNDED_BUCKETS : 1)) {
    return;
  }
  const uint32_t rank = parallel_buckets ? work % p.root_pitch : work;
  const uint32_t owned_bucket = parallel_buckets ? work / p.root_pitch : UINT32_MAX;
  const uint32_t pixel = p.root_pitch == packet.plane_pitch ?
                             rank :
                             deep_bounded_ptr<const uint32_t>(p.capture.active_map)[
                                 2 + uint64_t(packet.plane_pitch) + rank];
  const uint64_t collector_pixel = logical_pixel(p, pixel);
  const uint64_t stride = p.collector.capacity_pixels;
  const bool indexed = (p.first & DeepBoundedAccumulateFlags::INDEXED) != 0;
  const uint64_t heads = p.scan_totals ? p.scan_totals : p.scan_input;
  uint32_t next_sample = indexed ? deep_bounded_ptr<const uint32_t>(heads)[
      uint64_t(owned_bucket) * p.root_pitch + rank] - 1u : 0u;
  while (next_sample < packet.num_samples) {
    const uint32_t s = next_sample;
    const uint32_t root = rank * packet.num_samples + s;
    const uint32_t h = deep_bounded_ptr<const uint32_t>(p.capture.headers)[root];
    next_sample = indexed ? ((h >> sample_next_shift) & sample_next_mask) - 1u : s + 1u;
    if (!(h & DEEP_BOUNDED_VALID)) {
      continue;
    }
    const uint32_t count = h & DEEP_BOUNDED_COUNT_MASK;
    const uint32_t bucket = (h >> DEEP_BOUNDED_BUCKET_SHIFT) & 15u;
    if (parallel_buckets && bucket != owned_bucket) {
      continue;
    }
    const uint64_t index = uint64_t(bucket) * stride + collector_pixel;
    uint32_t &population = deep_bounded_ptr<uint32_t>(p.collector.populations)[index];
    uint32_t &old_count = deep_bounded_ptr<uint32_t>(p.collector.counts)[index];
    if (count > DEEP_BOUNDED_K || old_count > DEEP_BOUNDED_K || population == UINT32_MAX) {
      deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_POPULATION);
      return;
    }
    if (!pending_source_valid<Physical>(p, root, pixel, s, count)) {
      return;
    }
    if ((p.first & DeepBoundedAccumulateFlags::WARP) && old_count && count && old_count + count > 4) {
      const uint32_t task = atomicAdd(deep_bounded_ptr<uint32_t>(p.scan_output), 1u);
      if (task >= p.size) {
        deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_CAPACITY);
        return;
      }
      uint32_t *tasks = deep_bounded_ptr<uint32_t>(p.scan_input) + uint64_t(task) * 2;
      tasks[0] = work;
      tasks[1] = s;
      return;
    }
    const float inverse = 1.0f / float(population + 1);
    const Chain dst{p.collector.planes, uint64_t(bucket) * DEEP_BOUNDED_K * stride + collector_pixel,
                    stride, old_count};
    const Chain src{p.capture.raw, root, packet.root_capacity, count, true};
    old_count = merge_chains<Physical>(dst, src, dst, float(population) * inverse, inverse,
                                       p.merge, packet.status_ptr);
    ++population;
  }
}

template<bool Physical> __device__ void finalize(const DeepBoundedDispatch &p)
{
  const uint32_t pixel = thread_index();
  if (pixel >= p.model.pixels) {
    return;
  }
  const uint64_t cp = logical_pixel(p, pixel);
  const uint64_t stride = p.collector.capacity_pixels;
  if (p.first & DeepBoundedAccumulateFlags::WARP) {
    bool complex = false;
    for (uint32_t b = 0; b < DEEP_BOUNDED_BUCKETS; ++b) {
      complex |= deep_bounded_ptr<const uint32_t>(p.collector.counts)[uint64_t(b) * stride + cp] > 1;
    }
    const uint32_t lanes = __activemask();
    const uint32_t selected = __ballot_sync(lanes, complex);
    if (selected) {
      const uint32_t lane = threadIdx.x & 31u;
      const uint32_t leader = uint32_t(__ffs(selected) - 1);
      uint32_t base = 0;
      if (lane == leader) base = atomicAdd(deep_bounded_ptr<uint32_t>(p.scan_output), __popc(selected));
      base = __shfl_sync(lanes, base, leader);
      if (complex) {
        const uint32_t rank = __popc(selected & ((1u << lane) - 1u));
        deep_bounded_ptr<uint32_t>(p.scan_input)[base + rank] = pixel;
      }
    }
    if (complex) return;
  }
  uint32_t population = 0;
  Chain model{p.model.planes, pixel, p.model.pixels, 0};
  for (uint32_t b = 0; b < DEEP_BOUNDED_BUCKETS; ++b) {
    const uint64_t h = uint64_t(b) * stride + cp;
    const uint32_t n = deep_bounded_ptr<const uint32_t>(p.collector.populations)[h];
    if (!n) {
      continue;
    }
    const uint32_t count = deep_bounded_ptr<const uint32_t>(p.collector.counts)[h];
    if (count > DEEP_BOUNDED_K || n > UINT32_MAX - population) {
      deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_POPULATION);
      break;
    }
    const float inverse = 1.0f / float(population + n);
    const Chain src{p.collector.planes, uint64_t(b) * DEEP_BOUNDED_K * stride + cp, stride, count};
    model.count = merge_chains<Physical>(model, src, model, float(population) * inverse,
                                         float(n) * inverse, p.merge, p.capture.packet.status_ptr);
    population += n;
  }
  if (!population) {
    deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_POPULATION);
  }
  deep_bounded_ptr<uint32_t>(p.model.counts)[pixel] = model.count;
}

/* One owner per complex pixel. Reuse the existing shared candidates for
 * ordinary compaction and one bounded pressure pass, without trial rescans. */
template<bool Physical>
__device__ __forceinline__ uint32_t merge_warp_chains(const Chain &a,
                                                      const Chain &b,
                                                      const Chain &output,
                                                      const float wa,
                                                      const float wb,
                                                      const DeepBoundedMergePolicy merge,
                                                      const uint64_t status,
                                                      DeepBoundedRecord<Physical> *candidate,
                                                      const uint32_t lane)
{
  uint32_t result = 0;
  if (lane == 0) {
    if (!a.count || !b.count) {
      result = !a.count ? scale_chain<Physical>(b, output, wb, status) :
                          scale_chain<Physical>(a, output, wa, status);
    }
    else {
      const uint32_t count = generate_candidates(
          a, b, wa, wb, candidate, candidate_capacity, status);
      if (count != UINT32_MAX) {
        result = reduce_numeric(candidate,
                                count,
                                output.planes,
                                output.first,
                                output.stride,
                                DEEP_BOUNDED_K,
                                merge,
                                true,
                                status);
      }
    }
  }
  /* The candidate workspace and output above have a single writer/reader,
   * lane 0 of this warp. No cross-lane memory handoff needs a barrier here.
   * Broadcast the count; the caller retains its own synchronization. */
  result = __shfl_sync(0xffffffffu, result, 0);
  return result;
}

template<bool Physical>
__device__ __forceinline__ void accumulate_warp_task(
    const DeepBoundedDispatch &p, const uint32_t work, const uint32_t first_sample,
    DeepBoundedRecord<Physical> *staged, const uint32_t lane)
{
  const auto &packet = p.capture.packet;
  const bool parallel_buckets = (p.first & DeepBoundedAccumulateFlags::PARALLEL_BUCKETS) != 0;
  const uint32_t rank = parallel_buckets ? work % p.root_pitch : work;
  const uint32_t owned_bucket = parallel_buckets ? work / p.root_pitch : UINT32_MAX;
  const uint32_t pixel = p.root_pitch == packet.plane_pitch ? rank :
      deep_bounded_ptr<const uint32_t>(p.capture.active_map)[
          2 + uint64_t(packet.plane_pitch) + rank];
  const uint64_t cp = logical_pixel(p, pixel);
  const uint64_t stride = p.collector.capacity_pixels;
  uint32_t next_sample = first_sample;
  while (next_sample < packet.num_samples) {
    const uint32_t s = next_sample;
    const uint32_t root = rank * packet.num_samples + s;
    uint32_t header = 0;
    if (lane == 0) header = deep_bounded_ptr<const uint32_t>(p.capture.headers)[root];
    header = __shfl_sync(0xffffffffu, header, 0);
    next_sample = (p.first & DeepBoundedAccumulateFlags::INDEXED) ? ((header >> sample_next_shift) & sample_next_mask) - 1u : s + 1u;
    if (!(header & DEEP_BOUNDED_VALID)) continue;
    const uint32_t count = header & DEEP_BOUNDED_COUNT_MASK;
    const uint32_t bucket = (header >> DEEP_BOUNDED_BUCKET_SHIFT) & 15u;
    if (parallel_buckets && bucket != owned_bucket) continue;
    const uint64_t h = uint64_t(bucket) * stride + cp;
    uint32_t population = 0, old_count = 0;
    if (lane == 0) {
      population = deep_bounded_ptr<const uint32_t>(p.collector.populations)[h];
      old_count = deep_bounded_ptr<const uint32_t>(p.collector.counts)[h];
    }
    population = __shfl_sync(0xffffffffu, population, 0);
    old_count = __shfl_sync(0xffffffffu, old_count, 0);
    if (count > DEEP_BOUNDED_K || old_count > DEEP_BOUNDED_K || population == UINT32_MAX) {
      if (lane == 0) deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_POPULATION);
      return;
    }
    if constexpr (Physical) {
      /* The scalar producer already checked the first continuation sample. */
      if (s != first_sample) {
        uint32_t valid = lane == 0 ? pending_source_valid<true>(p, root, pixel, s, count) : 0u;
        valid = __shfl_sync(0xffffffffu, valid, 0);
        if (!valid) return;
      }
    }
    float wa = 0, wb = 0;
    if (lane == 0) {
      const float inverse = 1.0f / float(population + 1);
      wa = float(population) * inverse;
      wb = inverse;
    }
    wa = __shfl_sync(0xffffffffu, wa, 0);
    wb = __shfl_sync(0xffffffffu, wb, 0);
    const Chain dst{p.collector.planes, uint64_t(bucket) * DEEP_BOUNDED_K * stride + cp,
                     stride, old_count};
    const Chain input{p.capture.raw, root, packet.root_capacity, count, true};
    const uint32_t result = merge_warp_chains<Physical>(dst, input, dst, wa, wb, p.merge,
                                               packet.status_ptr, staged, lane);
    if (lane == 0) {
      deep_bounded_ptr<uint32_t>(p.collector.counts)[h] = result;
      deep_bounded_ptr<uint32_t>(p.collector.populations)[h] = population + 1;
    }
    __syncwarp(0xffffffffu);
  }
}

template<bool Physical> __device__ void accumulate_warp(const DeepBoundedDispatch &p)
{
  const uint32_t lane = threadIdx.x & 31u;
  const uint32_t warp = threadIdx.x >> 5;
  const uint32_t first = blockIdx.x * (blockDim.x >> 5) + warp;
  const uint32_t step = gridDim.x * (blockDim.x >> 5);
  const uint32_t tasks = *deep_bounded_ptr<const uint32_t>(p.scan_output);
  if (tasks > p.size) {
    if (lane == 0) deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_RANGE);
    return;
  }
  __shared__ DeepBoundedRecord<Physical> staged[Physical ? 4 : 8][candidate_capacity];
  for (uint32_t task = first; task < tasks; task += step) {
    const uint32_t *item = deep_bounded_ptr<const uint32_t>(p.scan_input) + uint64_t(task) * 2;
    const uint32_t work = item[0], sample = item[1];
    if (work >= p.size || sample >= p.capture.packet.num_samples) {
      if (lane == 0) deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_RANGE);
      return;
    }
    accumulate_warp_task<Physical>(p, work, sample, staged[warp], lane);
  }
}

template<bool Physical>
__device__ __forceinline__ void finalize_warp_pixel(
    const DeepBoundedDispatch &p, const uint32_t pixel,
    DeepBoundedRecord<Physical> *staged, const uint32_t lane)
{
  const uint64_t cp = logical_pixel(p, pixel);
  const uint64_t stride = p.collector.capacity_pixels;
  Chain model{p.model.planes, pixel, p.model.pixels, 0};
  uint32_t population = 0;
  for (uint32_t b = 0; b < DEEP_BOUNDED_BUCKETS; ++b) {
    const uint64_t h = uint64_t(b) * stride + cp;
    uint32_t n = 0, count = 0;
    if (lane == 0) {
      n = deep_bounded_ptr<const uint32_t>(p.collector.populations)[h];
      count = deep_bounded_ptr<const uint32_t>(p.collector.counts)[h];
    }
    n = __shfl_sync(0xffffffffu, n, 0);
    count = __shfl_sync(0xffffffffu, count, 0);
    if (!n) continue;
    if (count > DEEP_BOUNDED_K || n > UINT32_MAX - population) {
      if (lane == 0) deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_POPULATION);
      return;
    }
    float wa = 0, wb = 0;
    if (lane == 0) {
      const float inverse = 1.0f / float(population + n);
      wa = float(population) * inverse;
      wb = float(n) * inverse;
    }
    wa = __shfl_sync(0xffffffffu, wa, 0);
    wb = __shfl_sync(0xffffffffu, wb, 0);
    const Chain input{p.collector.planes, uint64_t(b) * DEEP_BOUNDED_K * stride + cp,
                       stride, count};
    model.count = merge_warp_chains<Physical>(model, input, model, wa, wb, p.merge,
                                     p.capture.packet.status_ptr, staged, lane);
    population += n;
  }
  if (lane == 0) {
    if (!population) deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_POPULATION);
    deep_bounded_ptr<uint32_t>(p.model.counts)[pixel] = model.count;
  }
}

template<bool Physical> __device__ void finalize_warp(const DeepBoundedDispatch &p)
{
  const uint32_t lane = threadIdx.x & 31u;
  const uint32_t warp = threadIdx.x >> 5;
  const uint32_t first = blockIdx.x * (blockDim.x >> 5) + warp;
  const uint32_t step = gridDim.x * (blockDim.x >> 5);
  const uint32_t tasks = *deep_bounded_ptr<const uint32_t>(p.scan_output);
  if (tasks > p.model.pixels) {
    if (lane == 0) deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_RANGE);
    return;
  }
  __shared__ DeepBoundedRecord<Physical> staged[Physical ? 4 : 8][candidate_capacity];
  for (uint32_t task = first; task < tasks; task += step) {
    const uint32_t pixel = deep_bounded_ptr<const uint32_t>(p.scan_input)[task];
    finalize_warp_pixel<Physical>(p, pixel, staged[warp], lane);
  }
}

template<bool Physical> __device__ void coarsen(const DeepBoundedDispatch &p)
{
  const uint32_t pixel = thread_index();
  if (pixel >= p.model.pixels) {
    return;
  }
  const uint32_t count = deep_bounded_ptr<const uint32_t>(p.input_model.counts)[pixel];
  DeepBoundedRecord<Physical> candidate[DEEP_BOUNDED_K];
  float prefix = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    auto &r = candidate[i];
    r = deep_bounded_load<Physical>(p.input_model.planes, pixel + uint64_t(i) * p.model.pixels);
    const float q = (1.0f - prefix) * r.alpha;
    prefix = deep_bounded_unit(prefix + q);
    r.alpha = q;
  }
  deep_bounded_ptr<uint32_t>(p.model.counts)[pixel] = reduce_numeric(
      candidate, count, p.model.planes, pixel, p.model.pixels, DEEP_BOUNDED_K, p.merge, true,
      p.capture.packet.status_ptr);
}

__device__ void scan(const DeepBoundedDispatch &p)
{
  using Block = cub::BlockScan<uint64_t, block_size>;
  __shared__ typename Block::TempStorage scratch;
  const uint32_t i = thread_index();
  uint64_t value = 0;
  if (i < p.size) {
    value = p.scan_u64 ? deep_bounded_ptr<const uint64_t>(p.scan_input)[i] :
                         deep_bounded_ptr<const uint32_t>(p.scan_input)[i];
  }
  uint64_t prefix, total;
  Block(scratch).ExclusiveSum(value, prefix, total);
  if (i < p.size) {
    deep_bounded_ptr<uint64_t>(p.scan_output)[i] = prefix;
  }
  if (threadIdx.x == 0 && p.scan_totals) {
    deep_bounded_ptr<uint64_t>(p.scan_totals)[blockIdx.x] = total;
  }
}

__device__ void scan_add(const DeepBoundedDispatch &p)
{
  const uint32_t i = thread_index();
  if (i < p.size) {
    deep_bounded_ptr<uint64_t>(p.scan_output)[i] +=
        deep_bounded_ptr<const uint64_t>(p.scan_input)[blockIdx.x];
  }
}

__device__ float encoded_value(const uint64_t address,
                               const uint64_t index,
                               const float value,
                               const bool half)
{
  const float limit = half ? 65504.0f : FLT_MAX;
  const float finite_value = fminf(fmaxf(value, -limit), limit);
  if (half) {
    const __half encoded = __float2half_rn(finite_value);
    deep_bounded_ptr<uint16_t>(address)[index] = __half_as_ushort(encoded);
    return __half2float(encoded);
  }
  deep_bounded_ptr<float>(address)[index] = finite_value;
  return finite_value;
}

template<bool Physical, bool Simple> __device__ void encode(const DeepBoundedDispatch &p)
{
  const uint32_t pixel = thread_index();
  const uint32_t pixels = p.model.pixels;
  if (pixel >= pixels) {
    return;
  }
  const auto &e = p.encoding;
  const uint32_t *counts = deep_bounded_ptr<const uint32_t>(p.model.counts);
  const uint64_t *offsets = deep_bounded_ptr<const uint64_t>(p.model.offsets);
  const uint64_t m = offsets[pixels - 1] + counts[pixels - 1];
  const bool with_summary = Simple && !e.alpha_only;
  const bool dense = with_summary && m >= pixels;
  const bool rgb = Physical && !e.alpha_only;
  auto &header = *deep_bounded_ptr<DeepBoundedEncodedHeader>(e.header);
  const uint64_t header_status = e.header + offsetof(DeepBoundedEncodedHeader, status);
  if (pixel == 0) {
    header.M = m;
    header.layout = e.layout | (dense ? 1u << 16 : 0u);
    atomicOr(&header.status, *deep_bounded_ptr<const uint32_t>(p.capture.packet.status_ptr));
  }
  const uint32_t count = counts[pixel];
  const uint64_t offset = offsets[pixel];
  if (m > e.capacity_records || count > DEEP_BOUNDED_K || offset > m || count > m - offset) {
    deep_bounded_error(header_status, DEEP_BOUNDED_CAPACITY);
    return;
  }
  deep_bounded_ptr<uint32_t>(e.counts)[pixel] = count;
  float prefix = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    const auto r = deep_bounded_load<Physical>(p.model.planes, pixel + uint64_t(i) * pixels);
    if (!valid_record(r)) {
      deep_bounded_error(header_status, DEEP_BOUNDED_NUMERIC);
      return;
    }
    const uint64_t j = offset + i;
    deep_bounded_ptr<float>(e.z)[j] = r.z;
    deep_bounded_ptr<float>(e.z_back)[j] = r.z_back;
    const float alpha = encoded_value(e.rgba, j * (rgb ? 4 : 1) + (rgb ? 3 : 0), r.alpha, e.half);
    if constexpr (Physical) {
      if (rgb) {
        const float inverse = 1.0f / fmaxf(1.0f - prefix, 0x1p-23f);
        for (uint32_t c = 0; c < 3; ++c) {
          encoded_value(e.rgba, j * 4 + c, r.rgb[c] * inverse, e.half);
        }
      }
    }
    if (with_summary && !dense && i) {
      deep_bounded_ptr<DeepBoundedSimpleSummary>(e.summary)[j].value = 0.0f;
    }
    if constexpr (Simple || Physical) {
      prefix = fmaf(1.0f - prefix, alpha, prefix);
    }
  }
  if (with_summary && (dense || count)) {
    float value = prefix >= FLT_MIN ? prefix : 0.0f;
    if (value == 0.0f) {
      for (uint32_t i = 0; i < count; ++i) {
        if (deep_bounded_mature_eligible(
                deep_bounded_load_object(p.model.planes, pixel + uint64_t(i) * pixels))) {
          value = -float(i + 1);
          break;
        }
      }
    }
    deep_bounded_ptr<DeepBoundedSimpleSummary>(e.summary)[dense ? pixel : offset].value = value;
  }
}

template<bool Physical, bool Pack> __device__ void migrate(const DeepBoundedDispatch &p)
{
  const uint64_t records = uint64_t(p.size) * DEEP_BOUNDED_K * DEEP_BOUNDED_BUCKETS;
  const uint64_t i = thread_index();
  if (i >= records) {
    return;
  }
  const uint64_t pixel = i % p.size;
  const uint64_t record_slot = i / p.size;
  const uint64_t bucket = record_slot / DEEP_BOUNDED_K;
  const uint64_t slot = record_slot % DEEP_BOUNDED_K;
  const uint64_t n = p.collector.capacity_pixels;
  const uint64_t original_h = bucket * n + p.first + pixel;
  const uint64_t staged_h = bucket * p.size + pixel;
  const uint64_t original_i = record_slot * n + p.first + pixel;
  DeepBoundedPlanes staged;
  staged.z = p.migration;
  staged.z_back = staged.z + records * 4;
  staged.rgba = staged.z_back + records * 4;
  staged.object = staged.rgba + records * (Physical ? 16 : 4);
  staged.shader = staged.object + records * 4;
  staged.normal = staged.shader + records * 4;
  uint32_t *counts = deep_bounded_ptr<uint32_t>(staged.normal + records * 4);
  uint32_t *population = counts + uint64_t(p.size) * DEEP_BOUNDED_BUCKETS;
  uint32_t *original_counts = deep_bounded_ptr<uint32_t>(p.collector.counts);
  uint32_t *original_population = deep_bounded_ptr<uint32_t>(p.collector.populations);
  const uint32_t count = Pack ? original_counts[original_h] : counts[staged_h];
  if (count > DEEP_BOUNDED_K) {
    deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_RANGE);
    return;
  }
  if (slot == 0) {
    if constexpr (Pack) {
      counts[staged_h] = count;
      population[staged_h] = original_population[original_h];
    }
    else {
      original_counts[original_h] = count;
      original_population[original_h] = population[staged_h];
    }
  }
  if constexpr (Pack) {
    const auto value = slot < count ? deep_bounded_load<Physical>(p.collector.planes, original_i) :
                                      DeepBoundedRecord<Physical>{};
    deep_bounded_store(staged, i, value);
  }
  else if (slot < count) {
    deep_bounded_store(p.collector.planes, original_i, deep_bounded_load<Physical>(staged, i));
  }
}

}  // namespace

#define DEEP_BOUNDED_ENTRIES(mode, physical, simple) \
  extern "C" __global__ void kernel_deep_bounded_##mode##_begin_packet(const DeepBoundedDispatch p) \
  { begin_packet<physical>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_accumulate(const DeepBoundedDispatch p) \
  { accumulate<physical>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_finalize(const DeepBoundedDispatch p) \
  { finalize<physical>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_coarsen(const DeepBoundedDispatch p) \
  { coarsen<physical>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_scan(const DeepBoundedDispatch p) \
  { scan(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_scan_add(const DeepBoundedDispatch p) \
  { scan_add(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_encode(const DeepBoundedDispatch p) \
  { encode<physical, simple>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_migration_pack(const DeepBoundedDispatch p) \
  { migrate<physical, true>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_migration_unpack(const DeepBoundedDispatch p) \
  { migrate<physical, false>(p); } \
  extern "C" __global__ void kernel_deep_bounded_##mode##_map_active_pixels(const DeepBoundedDispatch p) \
  { map_active_pixels(p); }

DEEP_BOUNDED_ENTRIES(alpha, false, false)
DEEP_BOUNDED_ENTRIES(simple, false, true)
DEEP_BOUNDED_ENTRIES(physical, true, false)

#undef DEEP_BOUNDED_ENTRIES

extern "C" __global__ void kernel_deep_bounded_alpha_finalize_warp(const DeepBoundedDispatch p)
{ finalize_warp<false>(p); }
extern "C" __global__ void kernel_deep_bounded_simple_finalize_warp(const DeepBoundedDispatch p)
{ finalize_warp<false>(p); }

extern "C" __global__ void kernel_deep_bounded_alpha_accumulate_warp(const DeepBoundedDispatch p)
{ accumulate_warp<false>(p); }
extern "C" __global__ void kernel_deep_bounded_simple_accumulate_warp(const DeepBoundedDispatch p)
{ accumulate_warp<false>(p); }

extern "C" __global__ __launch_bounds__(128) void kernel_deep_bounded_physical_finalize_warp(
    const DeepBoundedDispatch p)
{ finalize_warp<true>(p); }
extern "C" __global__ __launch_bounds__(128) void kernel_deep_bounded_physical_accumulate_warp(
    const DeepBoundedDispatch p)
{ accumulate_warp<true>(p); }

/* Same integer shader buckets as the native serial prefix. This entry keeps
 * one launch and the original sorted-path pass; only the scan is parallel. */
extern "C" __global__ __launch_bounds__(256) void kernel_deep_shader_prefix_sum(
    uint32_t *counter, uint32_t *prefix_sum, const int num_values)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage scratch;
  if (num_values <= 0) return;
  const uint32_t count = uint32_t(num_values);
  uint32_t base = 0;
  for (uint32_t first = 0; first < count; first += 256) {
    const uint32_t i = first + threadIdx.x;
    const uint32_t value = i < count ? counter[i] : 0;
    uint32_t rank, total;
    Scan(scratch).ExclusiveSum(value, rank, total);
    if (i < count) {
      prefix_sum[i] = base + rank;
      counter[i] = 0;
    }
    base += total;
    __syncthreads(); /* The next chunk reuses the same scan workspace. */
  }
}
