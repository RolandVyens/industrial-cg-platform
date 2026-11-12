/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Production schemas. Frame identity belongs to the owner, never to a recycled integrator slot.
 * Payloads contain handles/values only; no host or device pointer is serialized. */
struct DeepFrameKey {
  uint64_t invocation = 0, scene_revision = 0, camera = 0, policy_revision = 0;
  int32_t frame = 0;
  uint32_t subframe_bits = 0, view = 0, view_layer = 0;
  bool simple_mode = true;

  bool operator==(const DeepFrameKey &other) const
  {
    return invocation == other.invocation && scene_revision == other.scene_revision &&
           camera == other.camera && policy_revision == other.policy_revision &&
           frame == other.frame && subframe_bits == other.subframe_bits && view == other.view &&
           view_layer == other.view_layer && simple_mode == other.simple_mode;
  }
};
static_assert(sizeof(DeepFrameKey) == 56);

enum class DeepCameraDisposition : uint32_t { Ray, ClearTransparent, InvalidCameraBlack };

/* Scoped by DeepFrameKey; absolute raster coordinates include negative overscan. */
struct DeepSampleKey {
  int32_t x = 0, y = 0;
  uint32_t sample = 0, subset_offset = 0, rng_pixel = 0, rng_offset = 0;
  DeepCameraDisposition disposition = DeepCameraDisposition::Ray;
  uint32_t scheduled_sample = 0;

  bool operator==(const DeepSampleKey &other) const
  {
    return x == other.x && y == other.y && sample == other.sample &&
           subset_offset == other.subset_offset && rng_pixel == other.rng_pixel &&
           rng_offset == other.rng_offset && disposition == other.disposition &&
           scheduled_sample == other.scheduled_sample;
  }
};
static_assert(sizeof(DeepSampleKey) == 32);

enum class DeepRecordKind : uint16_t {
  OpticalPiece,
  VisibilityPoint,
  RadianceTerm,
  NativeCurve,
  DepthKnot,
  NativeExpressionSource,
  NativeExpressionNode,
  NativeExpressionCall,
  DepthMap,
  PhysicalSupport,
};

struct DeepRecordTag {
  uint64_t ray = 0, root = 0;
  uint32_t attempt = 0, ordinal = 0, kind = 0, flags = 0;
};
static_assert(sizeof(DeepRecordTag) == 32);

struct DeepOpticalPiece {
  float z, z_back, tau[3];
  uint32_t medium_stack, coverage_segment, flags;
};
static_assert(sizeof(DeepOpticalPiece) == 32);

struct DeepVisibilityPoint {
  float z, transmission[3], holdout[3];
  uint32_t object, primitive, shader, packed_normal, source_ordinal;
};
static_assert(sizeof(DeepVisibilityPoint) == 48);

struct DeepRadianceTerm {
  float z, z_back, rgb[3];
  double tau[3];
  uint64_t emission_call;
  uint32_t basis, pass_mask;
};
static_assert(sizeof(DeepRadianceTerm) == 64);

struct DeepNativeCurveHeader {
  uint64_t ray, depth_map;
  double population_weight;
  uint64_t knot_begin, knot_count, representation_error;
  uint32_t flags, reserved;
  uint64_t provenance;
};
static_assert(sizeof(DeepNativeCurveHeader) == 64);

enum class DeepFrozenDepthKind : uint32_t { Affine, Radial };
struct DeepFrozenDepthMap {
  uint64_t id = 0;
  DeepFrozenDepthKind kind = DeepFrozenDepthKind::Affine;
  uint32_t flags = 0;
  float transform[12] = {};
  float origin[3] = {}, direction[3] = {};
  float tmin = 0, tmax = 0, affine_origin = 0, affine_slope = 0;
};
static_assert(sizeof(DeepFrozenDepthMap) == 104);
enum class DeepPhysicalSourceKind : uint32_t { Optical, Visibility, Radiance };
enum DeepPhysicalSupportFlag : uint32_t {
  DEEP_PHYSICAL_POINT = 1u << 0,
  DEEP_PHYSICAL_INFINITY = 1u << 1,
  DEEP_PHYSICAL_WORLD_RADIANCE = 1u << 2,
};
struct DeepPhysicalSourceSupport {
  uint64_t depth_map = 0, source = 0;
  double front = 0, back = 0, z = 0, z_back = 0;
  DeepPhysicalSourceKind kind = DeepPhysicalSourceKind::Optical;
  uint32_t ordinal = 0, flags = 0, reserved = 0;
};
static_assert(sizeof(DeepPhysicalSourceSupport) == 64);

enum DeepDepthKnotFlag : uint32_t {
  DEEP_KNOT_PROTECTED_CUT = 1u << 0,
  DEEP_KNOT_INTERVAL_BOUNDARY = 1u << 1,
  DEEP_KNOT_POINT_RADIANCE = 1u << 2,
  DEEP_KNOT_WORLD_TERMINAL = 1u << 3,
  DEEP_KNOT_INVALID_CAMERA_BLACK = 1u << 4,
  /* Radiance at the depth sentinel grants no World visibility/opacity event. */
  DEEP_KNOT_INFINITY_RADIANCE = 1u << 5,
  DEEP_KNOT_NATIVE_WORLD_RADIANCE = 1u << 6,
};

/* Components are transmission, premultiplied R/G/B. Same-z jumps have explicit two sides. */
struct DeepDepthKnot {
  double z, before[4], after[4], slope_right[4];
  uint64_t source_group, error_bound;
  uint32_t flags, reserved;
};
static_assert(sizeof(DeepDepthKnot) == 128);

/* Each count has a distinct authority. Offsets are explicit, not reconstructed from valid. */
struct DeepPageDescriptor {
  uint32_t page_id;
  uint16_t generation;
  DeepRecordKind kind;
  uint32_t capacity, reserved, written, committed, valid, first_error;
  uint64_t tag_offset, payload_offset, payload_bytes, frame_invocation;
};
static_assert(sizeof(DeepPageDescriptor) == 64);
static_assert(std::is_standard_layout_v<DeepPageDescriptor>);
static_assert(std::is_trivially_copyable_v<DeepDepthKnot>);
static_assert(offsetof(DeepPageDescriptor, reserved) == 12);
static_assert(offsetof(DeepPageDescriptor, valid) == 24);
static_assert(offsetof(DeepPageDescriptor, tag_offset) == 32);
static_assert(offsetof(DeepPageDescriptor, payload_offset) == 40);
static_assert(offsetof(DeepDepthKnot, before) == 8);
static_assert(offsetof(DeepDepthKnot, after) == 40);
static_assert(offsetof(DeepDepthKnot, slope_right) == 72);
static_assert(offsetof(DeepNativeCurveHeader, population_weight) == 16);
static_assert(offsetof(DeepRadianceTerm, emission_call) == 48);

constexpr uint64_t deep_page_handle(const uint32_t page,
                                    const uint16_t slot,
                                    const uint16_t generation)
{
  return (uint64_t(page) << 32) | (uint64_t(generation) << 16) | slot;
}

constexpr uint32_t deep_handle_page(const uint64_t handle)
{
  return uint32_t(handle >> 32);
}
constexpr uint16_t deep_handle_slot(const uint64_t handle)
{
  return uint16_t(handle);
}
constexpr uint16_t deep_handle_generation(const uint64_t handle)
{
  return uint16_t(handle >> 16);
}

CCL_NAMESPACE_END
