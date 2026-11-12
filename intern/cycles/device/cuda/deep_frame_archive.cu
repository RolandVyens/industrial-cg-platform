/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

extern "C" __global__ void deep_gpu_frame_compact_archive_keys(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  const auto *record = ptr<const unsigned char>(d.a) + uint64_t(i) * d.m;
  const uint32_t camera = native_u32(record, d.k);
  uint64_t key = UINT64_MAX;
  if (camera >= d.width) atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_POPULATION);
  else {
    const uint32_t rank = ptr<const uint32_t>(d.d)[camera];
    if (rank != UINT32_MAX) key =
        (ptr<const uint64_t>(d.e)[rank] & 0xffffffff00000000ull) | rank;
  }
  ptr<uint64_t>(d.b)[i] = key;
  ptr<uint32_t>(d.c)[i] = i;
}

extern "C" __global__ void deep_gpu_frame_compact_archive_gather(const DeepGPUFrameDispatch d)
{
  const uint64_t byte = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (byte >= uint64_t(d.n) * d.m) return;
  const uint32_t record = uint32_t(byte / d.m), field = uint32_t(byte % d.m);
  const uint32_t source = ptr<const uint32_t>(d.b)[record];
  if (source >= d.n) { atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_RANGE); return; }
  const auto *input = ptr<const unsigned char>(d.a) + uint64_t(source) * d.m;
  if (field < d.k || field >= d.k + 4) ptr<unsigned char>(d.c)[byte] = input[field];
  else if (field == d.k) {
    const uint32_t camera = native_u32(input, d.k);
    if (camera >= d.width) atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_POPULATION);
    else {
      const uint32_t rank = ptr<const uint32_t>(d.d)[camera];
      const uint64_t key = ptr<const uint64_t>(d.f)[record];
      if (key != UINT64_MAX && rank != uint32_t(key)) {
        if (atomicCAS(ptr<uint32_t>(d.h) + 3, 0u, 5501u) == 0)
          printf("DEEP_ARCHIVE_GATHER_INVALID record=%u camera=%u rank=%u key=%llu source=%u\n",
                 record, camera, rank, static_cast<unsigned long long>(key), source);
        atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_POPULATION); return;
      }
      *reinterpret_cast<uint32_t *>(ptr<unsigned char>(d.c) + uint64_t(record) * d.m + d.k) = rank;
    }
  }
}

extern "C" __global__ void deep_gpu_frame_compact_archive_rebase(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  if (d.kind == 1) {
    const auto cameras = ptr<const DeepGPUFrameRange>(d.b)[i];
    const auto records = ptr<const DeepGPUFrameRange>(d.c)[i];
    for (uint32_t at = 0; at < records.count; at++) {
      const uint32_t camera = native_u32(ptr<const unsigned char>(d.a) +
          uint64_t(records.offset + at) * d.m, d.k);
      if (camera < cameras.offset || uint64_t(camera) >= uint64_t(cameras.offset) + cameras.count) {
        if (atomicCAS(ptr<uint32_t>(d.h) + 3, 0u, 5502u) == 0)
          printf("DEEP_ARCHIVE_CHECK_INVALID pixel=%u record=%u camera=%u base=%u count=%u\n",
                 i, records.offset + at, camera, cameras.offset, cameras.count);
        atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_POPULATION); return;
      }
    }
    return;
  }
  auto *camera = reinterpret_cast<uint32_t *>(ptr<unsigned char>(d.a) + uint64_t(i) * d.m + d.k);
  if (*camera < d.base || uint64_t(*camera) >= uint64_t(d.base) + d.width ||
      uint64_t(*camera - d.base) + d.pixel > UINT32_MAX) {
    if (atomicCAS(ptr<uint32_t>(d.h) + 3, 0u, 5601u) == 0)
      printf("DEEP_ARCHIVE_REBASE_INVALID record=%u camera=%u base=%u count=%u target=%u stride=%u field=%u\n",
             i, *camera, d.base, d.width, d.pixel, d.m, d.k);
    atomicOr(ptr<uint32_t>(d.h), DEEP_GPU_FRAME_INVALID_POPULATION); return;
  }
  *camera = *camera - d.base + d.pixel;
}
