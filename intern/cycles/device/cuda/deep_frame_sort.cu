/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

extern "C" __global__ void deep_gpu_frame_compact_sort_pack(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < d.n)
    ptr<DeepCompactSortItem>(d.c)[i] = {ptr<const uint64_t>(d.a)[i], ptr<const uint32_t>(d.b)[i], 0};
}

extern "C" __global__ void deep_gpu_frame_compact_sort_unpack(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < d.n) {
    const auto item = ptr<const DeepCompactSortItem>(d.a)[i];
    ptr<uint64_t>(d.b)[d.base + i] = item.key;
    ptr<uint32_t>(d.c)[d.base + i] = item.id;
  }
}

/* Stable merge-path over two reload windows. The diagonal receipt is control
 * metadata; only this device kernel compares keys or decides their order. */
extern "C" __global__ void deep_gpu_frame_compact_sort_merge(const DeepGPUFrameDispatch d)
{
  const uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k > d.k) return;
  const auto *a = ptr<const DeepCompactSortItem>(d.a), *b = ptr<const DeepCompactSortItem>(d.b);
  uint32_t lo = k > d.m ? k - d.m : 0, hi = k < d.n ? k : d.n;
  uint32_t i = lo, j = k - lo;
  while (lo <= hi) {
    i = lo + (hi - lo) / 2; j = k - i;
    if (i && j < d.m && a[i - 1].key > b[j].key) hi = i - 1;
    else if (j && i < d.n && b[j - 1].key >= a[i].key) lo = i + 1;
    else break;
  }
  if (k == d.k) {
    ptr<uint32_t>(d.d)[0] = i; ptr<uint32_t>(d.d)[1] = j;
  }
  else ptr<DeepCompactSortItem>(d.c)[k] =
      i < d.n && (j >= d.m || a[i].key <= b[j].key) ? a[i] : b[j];
}
