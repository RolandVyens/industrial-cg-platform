/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Experimental driver-loaded module. No CUDA runtime host ABI is required. */
#include <cub/block/block_radix_sort.cuh>
#include <cub/block/block_scan.cuh>
#include <stdint.h>

struct alignas(16) RawEvent {
  float rgba[4], z, z_back;
  uint32_t metadata[4], flags, pixel;
};
static_assert(sizeof(RawEvent) == 48, "Deep capture event ABI");

extern "C" __global__ void deep_raw_keys(const RawEvent *events, uint32_t n,
    uint32_t pixels, uint64_t *keys, uint32_t *ids, uint32_t *actual, uint32_t *error)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const RawEvent e = events[i];
  ids[i] = i;
  if (e.pixel >= pixels) {
    atomicOr(error, 1u); keys[i] = ~uint64_t(0); return;
  }
  atomicAdd(actual + e.pixel, 1u);
  keys[i] = (uint64_t(e.pixel) << 32) | ((e.flags & 2u) ? e.flags >> 8 : 0xffffffffu);
}

extern "C" __global__ void deep_raw_validate(const uint32_t *counts,
    const uint32_t *actual, uint32_t n, uint32_t pixels, uint32_t *error)
{
  const uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
  if (p < pixels && ((counts[p] & 0x80000000u) || counts[p] != actual[p])) atomicOr(error, 2u);
  if (p == 0 && counts[pixels] != n) atomicOr(error, 4u);
}

/* Stable block radix sorting followed by stable parallel merges. */
extern "C" __global__ void deep_raw_sort_blocks(const uint64_t *in_keys,
    const uint32_t *in_ids, uint64_t *out_keys, uint32_t *out_ids, uint32_t n)
{
  using Sort = cub::BlockRadixSort<uint64_t, 256, 4, uint32_t>;
  __shared__ typename Sort::TempStorage temporary;
  uint64_t keys[4]; uint32_t ids[4];
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    keys[j] = i < n ? in_keys[i] : ~uint64_t(0);
    ids[j] = i < n ? in_ids[i] : 0xffffffffu;
  }
  Sort(temporary).Sort(keys, ids);
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    if (i < n) { out_keys[i] = keys[j]; out_ids[i] = ids[j]; }
  }
}

extern "C" __global__ void deep_raw_merge(const uint64_t *in_keys, const uint32_t *in_ids,
    uint64_t *out_keys, uint32_t *out_ids, uint32_t n, uint32_t width)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const uint32_t run = i / width; const bool left = (run & 1u) == 0;
  const uint32_t pair = (run & ~1u) * width;
  const uint32_t other = left ? min(n, pair + width) : pair;
  uint32_t lo = other, hi = min(n, other + width);
  const uint64_t key = in_keys[i];
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (in_keys[mid] < key || (!left && in_keys[mid] == key)) lo = mid + 1;
    else hi = mid;
  }
  const uint32_t destination = pair + (i - run * width) + (lo - other);
  out_keys[destination] = key; out_ids[destination] = in_ids[i];
}

__device__ void deep_raw_filter_group(const RawEvent *events, const uint32_t *ids,
    uint8_t *keep, uint32_t start, uint32_t end)
{
  bool reject = false;
  for (uint32_t cursor = end; cursor > start;) {
    const uint32_t index = ids[--cursor], flags = events[index].flags;
    if (flags & 2u) {
      if (flags & 128u) reject = true;
      else if (flags & 64u) reject = false;
      keep[index] = !reject;
    }
    else keep[index] = 1;
  }
}

extern "C" __global__ void deep_raw_filter(const RawEvent *events, const uint64_t *keys,
    const uint32_t *ids, uint8_t *keep, uint32_t n)
{
  const uint32_t start = blockIdx.x * blockDim.x + threadIdx.x;
  if (start >= n || (start && keys[start] == keys[start - 1])) return;
  uint32_t end = start + 1;
  while (end < n && keys[end] == keys[start]) end++;
  deep_raw_filter_group(events, ids, keep, start, end);
}

/* Compact sorted group starts without changing any order inside a camera ray. The sentinel
 * after the final group gives its exclusive end, including Surface groups and aborted attempts. */
extern "C" __global__ void deep_raw_group_counts(const uint64_t *keys, uint32_t n,
    uint32_t *counts)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  uint32_t values[4], prefix[4], total;
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    values[j] = i < n && (!i || keys[i] != keys[i - 1]);
  }
  Scan(temporary).ExclusiveSum(values, prefix, total);
  if (threadIdx.x == 0) counts[blockIdx.x + 1] = total;
}

extern "C" __global__ void deep_raw_group_scatter_pixels(const uint64_t *keys, uint32_t n,
    const uint32_t *offsets, uint2 *heads, uint32_t groups)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  uint32_t values[4], prefix[4];
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    values[j] = i < n && (!i || keys[i] != keys[i - 1]);
  }
  Scan(temporary).ExclusiveSum(values, prefix);
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    if (values[j]) heads[offsets[blockIdx.x] + prefix[j]] = make_uint2(i, uint32_t(keys[i] >> 32));
  }
  if (blockIdx.x == 0 && threadIdx.x == 0) heads[groups] = make_uint2(n, 0);
}

extern "C" __global__ void deep_raw_filter_group_spread(const RawEvent *events,
    const uint32_t *ids, uint8_t *keep, const uint2 *heads, uint32_t groups, uint32_t lanes)
{
  const uint64_t index = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index & (lanes - 1)) return;
  const uint32_t group = uint32_t(index >> (__ffs(lanes) - 1));
  if (group < groups) deep_raw_filter_group(events, ids, keep, heads[group].x, heads[group + 1].x);
}

extern "C" __global__ void deep_raw_survivor_keys(const RawEvent *events,
    const uint8_t *keep, uint64_t *keys, uint32_t *ids, uint32_t n)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  keys[i] = keep[i] ? uint64_t(events[i].pixel) << 32 : ~uint64_t(0); ids[i] = i;
}

extern "C" __global__ void deep_raw_gather(const RawEvent *events, const uint64_t *keys,
    const uint32_t *ids, RawEvent *output, uint32_t *kept_counts, uint32_t n)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n || keys[i] == ~uint64_t(0)) return;
  const RawEvent event = events[ids[i]];
  output[i] = event; atomicAdd(kept_counts + event.pixel + 1, 1u);
}

extern "C" __global__ void deep_raw_scan(const uint32_t *counts, uint32_t *offsets,
    uint32_t *block_totals, uint32_t size)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  const uint32_t i = blockIdx.x * 256 + threadIdx.x;
  uint32_t value = i < size ? counts[i] : 0, total;
  Scan(temporary).InclusiveSum(value, value, total);
  if (i < size) offsets[i] = value;
  if (threadIdx.x == 0) block_totals[blockIdx.x] = total;
}

extern "C" __global__ void deep_raw_scan_add(uint32_t *offsets,
    const uint32_t *block_totals, uint32_t size)
{
  const uint32_t i = blockIdx.x * 256 + threadIdx.x;
  if (i >= size) return;
  uint32_t prefix = 0;
  for (uint32_t block = 0; block < blockIdx.x; block++) prefix += block_totals[block];
  offsets[i] += prefix;
}

/* Select complete pixels before grouping/filtering. Ownership/count validation still runs on
 * the complete raw stream, including pixels excluded from the final host fallback. */
extern "C" __global__ void deep_raw_selected_counts(const RawEvent *events,
    const uint32_t *selection, uint32_t n, uint32_t *counts)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  uint32_t values[4], prefix[4], total;
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    values[j] = i < n && selection[events[i].pixel] != 0;
  }
  Scan(temporary).ExclusiveSum(values, prefix, total);
  if (threadIdx.x == 0) counts[blockIdx.x + 1] = total;
}

extern "C" __global__ void deep_raw_selected_scatter(const RawEvent *events,
    const uint32_t *selection, const uint64_t *keys, const uint32_t *offsets,
    uint32_t n, uint64_t *selected_keys, uint32_t *selected_ids)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  uint32_t values[4], prefix[4];
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    values[j] = i < n && selection[events[i].pixel] != 0;
  }
  Scan(temporary).ExclusiveSum(values, prefix);
  for (int j = 0; j < 4; j++) {
    const uint32_t i = blockIdx.x * 1024 + threadIdx.x * 4 + j;
    if (values[j]) {
      const uint32_t out = offsets[blockIdx.x] + prefix[j];
      selected_keys[out] = keys[i]; selected_ids[out] = i;
    }
  }
}

extern "C" __global__ void deep_raw_selected_survivor_keys(const RawEvent *events,
    const uint8_t *keep, const uint32_t *selected_ids, uint32_t n,
    uint64_t *keys, uint32_t *ids)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const uint32_t id = selected_ids[i];
  // Restore original append order inside each pixel after camera-group abort filtering.
  keys[i] = keep[id] ? (uint64_t(events[id].pixel) << 32) | id : ~uint64_t(0);
  ids[i] = id;
}
