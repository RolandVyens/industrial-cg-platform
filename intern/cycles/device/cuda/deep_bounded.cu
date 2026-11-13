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

template<bool Physical> __device__ void begin_packet(const DeepBoundedDispatch &p)
{
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
    deep_bounded_ptr<uint32_t>(p.collector.counts)[pixel] = 0;
    deep_bounded_ptr<uint32_t>(p.collector.populations)[pixel] = 0;
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
  const uint32_t rank = thread_index();
  if (rank >= p.root_pitch) {
    return;
  }
  const uint32_t pixel = p.root_pitch == packet.plane_pitch ?
                             rank :
                             deep_bounded_ptr<const uint32_t>(p.capture.active_map)[
                                 2 + uint64_t(packet.plane_pitch) + rank];
  const uint64_t collector_pixel = logical_pixel(p, pixel);
  const uint64_t stride = p.collector.capacity_pixels;
  for (uint32_t s = 0; s < packet.num_samples; ++s) {
    const uint32_t root = rank * packet.num_samples + s;
    const uint32_t h = deep_bounded_ptr<const uint32_t>(p.capture.headers)[root];
    if (!(h & DEEP_BOUNDED_VALID)) {
      continue;
    }
    const uint32_t count = h & DEEP_BOUNDED_COUNT_MASK;
    const uint64_t index = collector_pixel;
    uint32_t &population = deep_bounded_ptr<uint32_t>(p.collector.populations)[index];
    uint32_t &old_count = deep_bounded_ptr<uint32_t>(p.collector.counts)[index];
    if (count > DEEP_BOUNDED_K || old_count > DEEP_BOUNDED_K || population == UINT32_MAX) {
      deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_POPULATION);
      return;
    }
    if (!pending_source_valid<Physical>(p, root, pixel, s, count)) {
      return;
    }
    const float inverse = 1.0f / float(population + 1);
    const Chain dst{p.collector.planes, collector_pixel,
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
  const uint32_t population = deep_bounded_ptr<const uint32_t>(p.collector.populations)[cp];
  const uint32_t count = deep_bounded_ptr<const uint32_t>(p.collector.counts)[cp];
  uint32_t result = 0;
  if (!population || count > DEEP_BOUNDED_K) {
    deep_bounded_error(p.capture.packet.status_ptr, DEEP_BOUNDED_POPULATION);
  }
  else {
    /* The collector is already normalized. Publish into the reusable phase
     * arena without a second population fold or pressure-compaction pass. */
    const Chain src{p.collector.planes, cp, stride, count};
    const Chain dst{p.model.planes, pixel, p.model.pixels, 0};
    result = scale_chain<Physical>(src, dst, 1.0f, p.capture.packet.status_ptr);
  }
  deep_bounded_ptr<uint32_t>(p.model.counts)[pixel] = result;
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
  const uint64_t records = uint64_t(p.size) * DEEP_BOUNDED_K;
  const uint64_t i = thread_index();
  if (i >= records) {
    return;
  }
  const uint64_t pixel = i % p.size;
  const uint64_t record_slot = i / p.size;
  const uint64_t slot = record_slot % DEEP_BOUNDED_K;
  const uint64_t n = p.collector.capacity_pixels;
  const uint64_t original_h = p.first + pixel;
  const uint64_t staged_h = pixel;
  const uint64_t original_i = record_slot * n + p.first + pixel;
  DeepBoundedPlanes staged;
  staged.z = p.migration;
  staged.z_back = staged.z + records * 4;
  staged.rgba = staged.z_back + records * 4;
  staged.object = staged.rgba + records * (Physical ? 16 : 4);
  staged.shader = staged.object + records * 4;
  staged.normal = staged.shader + records * 4;
  uint32_t *counts = deep_bounded_ptr<uint32_t>(staged.normal + records * 4);
  uint32_t *population = counts + p.size;
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
