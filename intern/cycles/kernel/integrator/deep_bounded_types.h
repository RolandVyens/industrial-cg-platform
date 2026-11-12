/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/deep_bounded_schema.h"

#ifndef __DEEP_BOUNDED_POLICY__
#  define __DEEP_BOUNDED_POLICY__ 0
#endif

constexpr uint32_t DEEP_BOUNDED_K = 32;
constexpr uint32_t DEEP_BOUNDED_BUCKETS = 16;
constexpr uint32_t DEEP_BOUNDED_SLOT_MASK = 0x3fffffffu;
constexpr uint32_t DEEP_BOUNDED_ELIGIBLE = 0x80000000u;
constexpr uint32_t DEEP_BOUNDED_NONSURFACE = 0x7fffffffu;
constexpr uint32_t DEEP_BOUNDED_VALID = 1u << 31;
constexpr uint32_t DEEP_BOUNDED_COUNT_MASK = 63u;
constexpr uint32_t DEEP_BOUNDED_BUCKET_SHIFT = 6;
/* Raw-only proof bit; count/bucket and the later AA links leave bit 10 idle. */
constexpr uint32_t DEEP_BOUNDED_ORDERED_ENDS = 1u << 10;
static_assert((DEEP_BOUNDED_ORDERED_ENDS & (DEEP_BOUNDED_VALID | DEEP_BOUNDED_COUNT_MASK |
                                            (15u << DEEP_BOUNDED_BUCKET_SHIFT))) == 0);

enum DeepBoundedError : uint32_t {
  DEEP_BOUNDED_RANGE = 1u,
  DEEP_BOUNDED_NUMERIC = 2u,
  DEEP_BOUNDED_CAPACITY = 4u,
  DEEP_BOUNDED_POPULATION = 8u,
  DEEP_BOUNDED_NO_SOURCE = 16u,
  DEEP_BOUNDED_LAYOUT = 32u,
};

/* Plain addresses make the same descriptor usable by the renderer, numeric
 * module, and host. Payload planes are never reinterpreted as an aligned AoS. */
struct DeepBoundedPlanes {
  uint64_t z, z_back, rgba;
  uint64_t object, shader, normal;
};
static_assert(sizeof(DeepBoundedPlanes) == 48);

struct alignas(16) DeepBoundedRGBA {
  float r, g, b, a;
};
static_assert(sizeof(DeepBoundedRGBA) == 16);

struct DeepBoundedMergePolicy {
  float alpha_tolerance;
  float depth_tolerance;
  float colour_absolute;
  float colour_relative;
  float projection_scale;
};

struct DeepBoundedPacketControl {
  DeepBoundedPacket packet;
  uint32_t status;
  uint32_t root_pitch;
  uint32_t native_width, native_count, native_columns;
  int32_t native_x, native_y, native_offset;
  uint32_t native_stride;
  uint32_t track_camera;
};
static_assert(sizeof(DeepBoundedPacketControl) == 104);

struct DeepBoundedBinding {
  DeepBoundedPacket packet;
  /* One descriptor shared by OptiX and compute. Its address is stable across
   * packets, so the native launch-parameter blocks need no packet upload. */
  uint64_t packet_device;
  uint64_t active_map;
  uint64_t native_active_map;
  DeepBoundedPlanes raw;
  DeepBoundedMergePolicy merge;
  uint64_t headers;
  uint64_t holdout_mq;
  uint64_t no_anchor;
  uint32_t policy;
  uint32_t has_holdout;
};

struct DeepBoundedCollector {
  DeepBoundedPlanes planes = {};
  uint64_t counts = 0, populations = 0;
  uint32_t capacity_pixels = 0;
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

struct DeepBoundedModel {
  DeepBoundedPlanes planes = {};
  uint64_t counts = 0, offsets = 0;
  uint32_t pixels = 0;
};

struct DeepBoundedEncoding {
  uint64_t header = 0, counts = 0, z = 0, z_back = 0, rgba = 0, summary = 0;
  uint64_t capacity_records = 0;
  uint32_t half = 1, alpha_only = 0;
  uint32_t inline_summary = 0, layout = 0;
};

/* Numeric operations do not extend the native DeviceKernel/counter enum. */
enum class DeepBoundedOperation : uint32_t {
  BEGIN_PACKET,
  ACCUMULATE,
  FINALIZE,
  COARSEN,
  SCAN,
  SCAN_ADD,
  ENCODE,
  MIGRATION_PACK,
  MIGRATION_UNPACK,
  MAP_ACTIVE_PIXELS,
  COUNT,
};

/* first is operation-local: BEGIN_PACKET flags, ACCUMULATE flags,
 * FINALIZE flags, or a numeric first pixel for MIGRATION. Never combine ops. */
namespace DeepBoundedBeginFlags {
constexpr uint32_t TILE = 1u, FRAME = 2u, BUCKET_INDEX = 32u;
}
namespace DeepBoundedAccumulateFlags {
constexpr uint32_t PARALLEL_BUCKETS = 1u, WARP = 4u, INDEXED = 64u;
}
namespace DeepBoundedFinalizeFlags {
constexpr uint32_t WARP = 4u;
}

struct DeepBoundedDispatch {
  DeepBoundedBinding capture = {};
  DeepBoundedCollector collector = {};
  DeepBoundedModel model = {}, input_model = {};
  DeepBoundedEncoding encoding = {};
  DeepBoundedMergePolicy merge = {};
  uint64_t scan_input = 0, scan_output = 0, scan_totals = 0;
  uint64_t migration = 0;
  uint64_t next_main_index = 0, next_shadow_index = 0;
  uint32_t first = 0, size = 0, scan_u64 = 0;
  uint32_t root_pitch = 0;
  uint32_t track_camera = 0;
  uint64_t film = 0;
  int64_t film_first = 0;
  uint32_t film_row_stride = 0, film_pass_stride = 0;
  uint32_t native_width = 0, native_count = 0, native_columns = 0;
  int32_t native_x = 0, native_y = 0, native_offset = 0;
};

/* The tagged summary is a colour routing value, never an optical Alpha. */
struct DeepBoundedSimpleSummary {
  float value;
};
static_assert(sizeof(DeepBoundedSimpleSummary) == 4);

#ifdef __CUDACC__
__host__ __device__
#endif
constexpr uint32_t deep_bounded_source(const uint32_t slot, const DeepBoundedSourceKind kind)
{
  return (slot & DEEP_BOUNDED_SLOT_MASK) | (uint32_t(kind) << 30);
}
