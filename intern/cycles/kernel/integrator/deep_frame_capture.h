/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cfloat>

#include "kernel/types.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Physical support and native totals are unchanged. The host span certificate
 * must pay any normalization-sized overflow quantized to the last finite Z. */
ccl_device_inline bool deep_native_world_span_depth(double z, bool radial, float &output)
{
  output = 0;
  const double maximum = double(FLT_MAX);
  const double limit = radial ? maximum * (1 + 8 * double(FLT_EPSILON)) : maximum;
  if (!(z >= 0) || z > limit)
    return false;
  output = z > maximum ? FLT_MAX : float(z);
  return true;
}

/* HIT alone stays on the original raw Combined path. These auxiliary branches
 * are handled by native catcher film helpers before the Combined write. */
ccl_device_inline bool deep_native_path_writes_combined(const uint32_t flags)
{
  return (flags & (PATH_RAY_SHADOW_CATCHER_PASS | PATH_RAY_SHADOW_CATCHER_BACKGROUND)) == 0;
}

/* Native M1 transport. These PODs have no ownership of integrator slots or host vectors.
 * A domain keeps its arrays alive until every native main/shadow/MNEE queue has joined. */
enum DeepNativeCaptureError : uint32_t {
  DEEP_NATIVE_ERROR_OVERFLOW = 1u << 0,
  DEEP_NATIVE_ERROR_IDENTITY = 1u << 1,
  DEEP_NATIVE_ERROR_NONFINITE = 1u << 2,
  DEEP_NATIVE_ERROR_HETEROGENEOUS = 1u << 3,
  DEEP_NATIVE_ERROR_SCATTERING = 1u << 4,
  DEEP_NATIVE_ERROR_COLORED = 1u << 5,
  DEEP_NATIVE_ERROR_SURFACE = 1u << 6,
  DEEP_NATIVE_ERROR_BACKGROUND = 1u << 7,
  DEEP_NATIVE_ERROR_EMISSION_CALL = 1u << 8,
  DEEP_NATIVE_ERROR_SHADER_RETRY = 1u << 9,
  DEEP_NATIVE_ERROR_INCOMPLETE = 1u << 10,
  DEEP_NATIVE_ERROR_CAMERA_WEIGHT = 1u << 11,
  DEEP_NATIVE_ERROR_MASS_RANGE = 1u << 12,
};

/* Explicit opt-in qualification faults. Zero is the production default. */
enum DeepNativeValidationFault : uint32_t {
  DEEP_NATIVE_FAULT_SHADOW_RETRY = 1u << 0,
  DEEP_NATIVE_FAULT_MNEE_RETRY = 1u << 1,
  DEEP_NATIVE_FAULT_BACKGROUND_RETRY = 1u << 2,
  DEEP_NATIVE_FAULT_POINT_DENY = 1u << 3,
  DEEP_NATIVE_FAULT_EXPRESSION_DENY = 1u << 4,
  DEEP_NATIVE_FAULT_HOMOGENEOUS_DENY = 1u << 5,
};

enum DeepNativeCameraFlag : uint32_t {
  DEEP_NATIVE_CAMERA_EVALUATED = 1u << 0,
  DEEP_NATIVE_CAMERA_AFFINE_DEPTH = 1u << 1,
  DEEP_NATIVE_CAMERA_BACKGROUND_REACHED = 1u << 2,
  DEEP_NATIVE_CAMERA_COMPLETE = 1u << 3,
  DEEP_NATIVE_CAMERA_CACHE_RETRY = 1u << 4,
  DEEP_NATIVE_CAMERA_RADIAL_DEPTH = 1u << 5,
};

/* Values intentionally match DeepCameraDisposition in the host production schema. */
enum DeepNativeCameraDisposition : uint32_t {
  DEEP_NATIVE_CAMERA_RAY = 0,
  DEEP_NATIVE_CAMERA_CLEAR_TRANSPARENT = 1,
  DEEP_NATIVE_CAMERA_INVALID_BLACK = 2,
};

struct DeepNativeCameraRecord {
  uint32_t pixel, sample, scheduled_sample, subset_offset, rng_pixel, rng_offset;
  int32_t x, y;
  uint32_t flags, disposition, unsupported_features;
  float population_weight;
  float origin[3], direction[3], tmin, tmax, time, dP, dD, throughput[3];
  /* The actual affine camera depth map is z(t) = z_origin + z_direction * t.
   * InvalidCameraBlack stores zeros and must never be used as a geometric Ray. */
  float z_origin, z_direction;
  /* Domain-local, one-based immutable root handle. Zero keeps the camera chain live. */
  uint32_t root_handle, reserved[3];
};
static_assert(sizeof(DeepNativeCameraRecord) == 128, "Native camera capture ABI changed");

enum DeepNativeHomogeneousFlag : uint32_t {
  DEEP_NATIVE_HOMOGENEOUS_OPTICAL = 1u << 0,
  DEEP_NATIVE_HOMOGENEOUS_EMISSION_FILM_CALL = 1u << 1,
  /* Producer compared current P/D/time with the actual captured camera, and computed both
   * endpoint depths with the same frozen native camera depth definition. */
  DEEP_NATIVE_HOMOGENEOUS_CAMERA_SUPPORT_VERIFIED = 1u << 2,
  DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT = 1u << 3,
};

struct DeepNativeHomogeneousRecord {
  uint32_t camera_index, pixel, sample, ordinal;
  uint32_t path_flags, flags, unsupported_features, emission_calls;
  float z, zback, tmin, tmax;
  double tau[3];
  float post_clamp_rgb[3];
  int32_t source_object, source_shader;
  /* Frozen input and the gain computed by the same native film clamp. Never infer
   * a gain by dividing captured RGB, nor clamp individual marched source terms. */
  float pre_clamp_rgb[3], clamp_gain;
};
static_assert(sizeof(DeepNativeHomogeneousRecord) == 112,
              "Native homogeneous capture ABI changed");

constexpr uint32_t DEEP_NATIVE_INVALID_INDEX = 0xffffffffu;
constexpr uint32_t DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES = 8192;

enum DeepNativeRootKind : uint32_t {
  DEEP_NATIVE_ROOT_NONE = 0,
  DEEP_NATIVE_ROOT_SURFACE = 1,
  DEEP_NATIVE_ROOT_WORLD = 2,
  DEEP_NATIVE_ROOT_VOLUME_SCATTER = 3,
  DEEP_NATIVE_ROOT_VOLUME_DIRECT = 4,
  DEEP_NATIVE_ROOT_LIGHT = 5,
  /* Camera-chain distant radiance, independent of the World visibility terminal. */
  DEEP_NATIVE_ROOT_DISTANT = 6,
};
enum DeepNativeRootPin : uint32_t {
  DEEP_NATIVE_ROOT_PIN_CAMERA = 1u << 0,
  DEEP_NATIVE_ROOT_PIN_MAIN = 1u << 1,
  DEEP_NATIVE_ROOT_PIN_SHADOW = 1u << 2,
  DEEP_NATIVE_ROOT_PIN_MNEE = 1u << 3,
};

enum DeepNativeRootFlag : uint32_t {
  DEEP_NATIVE_ROOT_CAMERA_SUPPORT = 1u << 0,
  DEEP_NATIVE_ROOT_OPAQUE_SURFACE = 1u << 1,
  DEEP_NATIVE_ROOT_WORLD_TERMINAL = 1u << 2,
  DEEP_NATIVE_ROOT_RADIANCE_ONLY = 1u << 3,
  /* Candidate was admitted at a real successful MNEE pending-shadow site. */
  DEEP_NATIVE_ROOT_MNEE_ADMISSION = 1u << 4,
  DEEP_NATIVE_ROOT_WORLD_SUPPORT = 1u << 5,
};
/* Immutable metadata survives in the host journal through native domain join.
 * GPU descriptors retain the current append window and actual live references.
 * Ordinal is its domain-global one-based handle; camera_index binds its camera. */
struct DeepNativeRootRecord {
  float z;
  uint32_t kind, ordinal, flags;
  int32_t object, prim, shader;
  float physical_t;
  uint32_t packed_normal, camera_index, reserved[2];
};
static_assert(sizeof(DeepNativeRootRecord) == 48, "Native root capture ABI changed");

/* One actual Combined film contribution at a known root. Routing scale is
 * separate from the call's native clamp (AO applies its weight afterwards). */
struct DeepNativePointRecord {
  uint32_t camera_index, pixel, sample, ordinal;
  uint32_t root_kind, root_ordinal, path_flags, routing;
  float z, pre_clamp_rgb[3], clamp_rgb[3], final_rgb[3], clamp_gain, routing_scale[3];
  uint32_t root_handle, reserved;
};
static_assert(sizeof(DeepNativePointRecord) == 96, "Native point film capture ABI changed");

enum DeepNativeExpressionOperation : uint32_t {
  DEEP_NATIVE_EXPR_ZERO,
  DEEP_NATIVE_EXPR_TERM,
  DEEP_NATIVE_EXPR_ADD,
  DEEP_NATIVE_EXPR_SNAPSHOT,
  DEEP_NATIVE_EXPR_SCALE,
  DEEP_NATIVE_EXPR_MIX,
  DEEP_NATIVE_EXPR_SELECT,
};
enum DeepNativeExpressionSourceFlag : uint32_t {
  DEEP_NATIVE_EXPR_CAMERA_SPAN = 1u << 0,
  DEEP_NATIVE_EXPR_INHERITED_POINT = 1u << 1,
  DEEP_NATIVE_EXPR_CAMERA_POINT = 1u << 2,
  DEEP_NATIVE_EXPR_WORLD_SUPPORT = 1u << 3,
};
/* Domain-local one-based handles; zero denotes the implicit Zero expression.
 * All vertices are append-only and preserve the actual native float operands. */
struct DeepNativeExpressionSource {
  uint32_t camera_index, ordinal, flags, root_handle;
  float z, zback, tmin, tmax, raw_rgb[3];
  double tau[3];
  int32_t object, prim, shader;
  uint32_t packed_normal, reserved[2];
};
static_assert(sizeof(DeepNativeExpressionSource) == 96, "Native expression source ABI changed");
struct DeepNativeExpressionNode {
  uint32_t camera_index, ordinal, operation, a, b, source, select_b;
  float weight[3];
  uint32_t reserved[2];
};
static_assert(sizeof(DeepNativeExpressionNode) == 48, "Native expression node ABI changed");
struct DeepNativeExpressionCall {
  uint32_t camera_index, ordinal, head, path_flags;
  float pre_clamp_rgb[3], post_clamp_rgb[3], clamp_gain;
  uint32_t routing, pixel, sample, root_handle, reserved;
};
static_assert(sizeof(DeepNativeExpressionCall) == 64, "Native expression call ABI changed");

constexpr uint32_t DEEP_NATIVE_POINT_COMBINED = 1u;

CCL_NAMESPACE_END
