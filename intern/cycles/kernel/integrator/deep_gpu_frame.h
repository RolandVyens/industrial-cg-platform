/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>
#include "kernel/integrator/deep_compact_types.h"

/* Device-only numerical interface. Host code may submit addresses and dimensions,
 * but must not materialize a reference curve to satisfy this interface. */
constexpr uint32_t DEEP_GPU_FRAME_ABI = 0x44474603u;
enum DeepGPUFrameError : uint32_t {
  DEEP_GPU_FRAME_INVALID_RANGE = 1u,
  DEEP_GPU_FRAME_INVALID_OPTICAL = 2u,
  DEEP_GPU_FRAME_INVALID_VISIBILITY = 4u,
  DEEP_GPU_FRAME_INVALID_POPULATION = 8u,
  DEEP_GPU_FRAME_INVALID_GRID = 16u,
  DEEP_GPU_FRAME_INVALID_RADIANCE = 32u,
  DEEP_GPU_FRAME_INVALID_ENCODING = 64u,
  DEEP_GPU_FRAME_CAPACITY = 128u,
  DEEP_GPU_FRAME_SIMPLE_ZERO_ALPHA_COLOUR = 256u,
  DEEP_GPU_FRAME_FORMAT_RANGE = 512u,
  DEEP_GPU_FRAME_NUMERIC_MASS_RANGE = 1024u,
};

struct DeepGPUFrameFloat3 {
  float x, y, z;
};
struct DeepGPUFrameOptical {
  float z, z_back;
  DeepGPUFrameFloat3 tau;
};
struct DeepGPUFrameVisibility {
  float z;
  DeepGPUFrameFloat3 transmission, holdout;
};
struct DeepGPUFrameRay {
  uint32_t optical_begin, optical_count, visibility_begin, visibility_count;
  uint32_t pixel, sample;
  uint32_t radiance_begin = 0, radiance_count = 0;
};
struct DeepGPUFrameRadiance {
  float z, z_back;
  DeepGPUFrameFloat3 tau, rgb;
};
struct DeepGPUFrameValue {
  float t, r, g, b;
};
struct DeepGPUFrameKnot {
  DeepGPUFrameValue before, after;
};
struct DeepGPUFramePixel {
  uint32_t ray_begin, ray_count, cut_begin, cut_count, population;
};
struct DeepGPUFrameTransmission {
  float before, after;
};
struct DeepGPUFrameReduceParameters {
  uint64_t rays, optical, visibility, pixels, cuts;
  uint64_t optical_prefix, transmission, status;
  uint32_t ray_count, optical_count, visibility_count, pixel_count, cut_count;
  uint32_t abi = DEEP_GPU_FRAME_ABI;
  uint64_t radiance = 0, values = 0;
  uint32_t radiance_count = 0;
  float exposure = 1;
  uint32_t flags = 0;
};

struct DeepGPUFrameConsumer {
  uint32_t half, alpha_only;
  float alpha_tolerance, depth_tolerance, rgb_absolute, rgb_relative;
  uint32_t reuse = UINT32_MAX;
};
struct DeepGPUFrameSample {
  float r, g, b, a, z, z_back;
};
struct DeepGPUFrameRange {
  uint32_t offset, count, source, consumer;
};
struct DeepGPUFrameFinalRange {
  uint32_t offset, count, population, consumer;
};
struct DeepGPUFrameEncodeParameters {
  uint64_t pixels, cuts, values, consumers, ranges, sparse, packed;
  uint64_t scan_counts, scan_offsets, output_count, status;
  uint32_t pixel_count, cut_count, consumer_count, sample_capacity;
  uint32_t abi = DEEP_GPU_FRAME_ABI;
};

/* Private device-stage dispatch. Addresses refer only to independently owned
 * device allocations; dimension metadata may be prepared by the host owner. */
enum class DeepGPUFrameOperation : uint32_t {
  Fit,
  Scan,
  ScanAdd,
  Pack,
  NativeSeed,
  NativeReverse,
  NativeRadiance,
  NativeCameraInput,
  OwnedOptical,
  OwnedVisibility,
  MakeKeys,
  SortBlocks,
  Merge,
  Gather,
  Ranges,
  CameraKeys,
  BuildRays,
  GridCount,
  GridFill,
  UniqueFlags,
  UniqueScatter,
  GridPublish,
  Midpoints,
  RefineFlags,
  RefineScatter,
  BuildPixels,
  ValidateOptical,
  ValidateOpticalFinish,
  CoalesceRadiance,
  PackRadiance,
  CompactRebase,
  CompactCameraPack,
  CompactCameraKeys,
  CompactPrimitiveKeys,
  CompactEvents,
  CompactEventKeys,
  CompactSweep,
  CompactTail,
  CompactReverse,
  CompactColourInit,
  CompactRootRadiance,
  CompactColourHash,
  CompactColourPage,
  CompactJoinEvents,
  CompactJoinKeys,
  CompactJoinSweep,
  CompactSimpleColour,
  CompactEncodeInit,
  CompactEncodePage,
  CompactFinalRanges,
  CompactPayloadIds,
  CompactSortPack,
  CompactSortUnpack,
  CompactSortMerge,
  CompactArchiveKeys,
  CompactArchiveGather,
  CompactArchiveRebase,
  CompactWitnessInit,
  CompactWitnessSeal,
  CompactWitnessSources,
  CompactWitnessOutput,
  CompactNeedsExtinction,
  CompactPendingRanges,
  CompactQualificationCases,
  Count,
};
struct DeepGPUFrameDispatch {
  uint64_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
  uint32_t n = 0, m = 0, k = 0, width = 0, base = 0, kind = 0, pixel = 0;
  uint32_t abi = DEEP_GPU_FRAME_ABI;
  float p = 0, q = 0, r = 0, s = 0;
};

/* Complete resident native domain. The renderer appends whole joined pages by
 * D2D copy; global source/node handles retain their original append identity. */
struct DeepGPUFrameNativeParameters {
  uint64_t cameras, homogeneous, points, sources, nodes, calls;
  uint64_t node_order, node_ranges, node_weights, source_weights;
  uint64_t radiance, radiance_owners, status;
  uint32_t camera_count, homogeneous_count, point_count, source_count;
  uint32_t node_count, call_count, camera_base, reserved;
};

struct DeepGPUFrameGridParameters {
  uint64_t rays, optical, radiance, visibility, pixels;
  uint64_t ray_counts, ray_offsets, keys, ids, source_keys, unique_offsets;
  uint64_t cuts, values, midcuts, midvalues, consumers, status;
  uint64_t flags, scan_offsets;
  uint32_t ray_count, pixel_count, cut_count, consumer_count, capacity;
};

static_assert(sizeof(DeepGPUFrameFloat3) == 12);
static_assert(sizeof(DeepGPUFrameOptical) == 20);
static_assert(sizeof(DeepGPUFrameVisibility) == 28);
static_assert(sizeof(DeepGPUFrameRay) == 32);
static_assert(sizeof(DeepGPUFramePixel) == 20);
static_assert(sizeof(DeepGPUFrameTransmission) == 8);
static_assert(sizeof(DeepGPUFrameReduceParameters) == 120);
static_assert(sizeof(DeepGPUFrameRadiance) == 32);
static_assert(sizeof(DeepGPUFrameValue) == 16);
static_assert(sizeof(DeepGPUFrameKnot) == 32);
static_assert(sizeof(DeepGPUFrameConsumer) == 28);
static_assert(sizeof(DeepGPUFrameSample) == 24);
static_assert(sizeof(DeepGPUFrameRange) == 16);
static_assert(sizeof(DeepGPUFrameFinalRange) == 16);
static_assert(sizeof(DeepGPUFrameEncodeParameters) == 112);
static_assert(sizeof(DeepGPUFrameDispatch) == 112);
static_assert(sizeof(DeepGPUFrameNativeParameters) == 136);
static_assert(sizeof(DeepGPUFrameGridParameters) == 176);

/* Qualification storage never aliases real pixels, Film, RNG or status. */
struct DeepGPUFrameQualificationCase {
  uint32_t id, count, error, phase;
  DeepCompactOutputSummary summary;
  DeepGPUFrameRange range;
  DeepGPUFrameConsumer policy;
  uint32_t padding;
  DeepCompactEncodeParameters encode;
  DeepCompactParameters base;
  alignas(8) uint8_t state[240];
  alignas(8) uint8_t joined[176];
  DeepCompactKnot knot;
  DeepGPUFrameSample samples[256];
  uint64_t keys[256];
  uint32_t controls[4];
  float projection[2];
};
