/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types.h"
#include "util/math_base.h"

CCL_NAMESPACE_BEGIN

/* Independently owned M2 diagnostic records. Receipt IDs refer to the immutable chunk,
 * whose backing header carries its domain and absolute receipt offset. Never pack sample IDs. */
struct DeepOpticalEvent {
  uint32_t receipt;
  uint32_t segment;
  float z;
  float zback;
  float tau[3];
  uint32_t reserved;
};
static_assert(sizeof(DeepOpticalEvent) == 32, "Optical event scalar ABI");

struct DeepOpticalLedger {
  uint32_t status;
  uint32_t segments;
  uint32_t events;
  uint32_t reserved;
};
static_assert(sizeof(DeepOpticalLedger) == 16, "Optical ledger scalar ABI");

/* Private state of one unfinished optical segment. The receipt input and ray are
 * immutable while active; a cache retry starts before the failed evaluation. */
struct DeepOpticalCheckpoint {
  uint32_t active, segment, sample, next_step, lcg_state;
  float ray_tmin, ray_tmax, step_min, step_max, size, offset, shade_offset;
  int32_t max_steps;
  uint32_t yields, retries, reserved;
};
static_assert(sizeof(DeepOpticalCheckpoint) == 64);

enum DeepOwnedOpticalFlag : uint32_t {
  DEEP_OWNED_OPTICAL_VACUUM = 1u << 0,
  DEEP_OWNED_OPTICAL_MEDIUM = 1u << 1,
  DEEP_OWNED_OPTICAL_COMPLETE = 1u << 2,
  DEEP_OWNED_OPTICAL_FAR = 1u << 3,
  DEEP_OWNED_OPTICAL_VISIBILITY_END = 1u << 4,
  DEEP_OWNED_OPTICAL_NO_RAY = 1u << 5,
  DEEP_OWNED_OPTICAL_CLAMPED = 1u << 6,
};
struct DeepOwnedOpticalPiece {
  uint32_t receipt, segment;
  float tmin, tmax, z, zback, tau[3];
  uint32_t flags, stack_hash[2];
};
static_assert(sizeof(DeepOwnedOpticalPiece) == 48);
struct DeepOwnedOpticalVisibility {
  uint32_t receipt, ordinal;
  float z, physical_t, transmission[3], holdout[3];
  int32_t object, prim, shader;
  uint32_t packed_normal, flags, reserved;
};
static_assert(sizeof(DeepOwnedOpticalVisibility) == 64);
struct DeepOwnedOpticalCoverage {
  float front_t, cursor_t, z_front, z_back;
  uint32_t flags, pieces, visibility, clamps;
};
static_assert(sizeof(DeepOwnedOpticalCoverage) == 32);

/* A positive physical partition can collapse under FLOAT camera-depth mapping.
 * Only exactly zero extinction is an optical identity at that projected point.
 * Callers still validate the complete physical partition and retain its record. */
ccl_device_inline bool deep_owned_optical_depth_identity(const float z,
                                                        const float zback,
                                                        const float tau_r,
                                                        const float tau_g,
                                                        const float tau_b)
{
  /* Float comparisons under FTZ/DAZ can turn nonzero captured subnormal bits
   * into an apparent zero. This is an exact identity, never a cutoff. */
  const uint32_t front = __float_as_uint(z), back = __float_as_uint(zback);
  const uint32_t tau = __float_as_uint(tau_r) | __float_as_uint(tau_g) |
                       __float_as_uint(tau_b);
  return (front <= 0x7f7fffffu || front == 0x80000000u) &&
         (back <= 0x7f7fffffu || back == 0x80000000u) &&
         ((front ^ back) & 0x7fffffffu) == 0 && (tau & 0x7fffffffu) == 0;
}

enum : uint32_t {
  DEEP_OPTICAL_COMPLETE = 1,
  DEEP_OPTICAL_ZERO_THROUGHPUT = 2,
  DEEP_OPTICAL_ERROR_CAPACITY = 1,
  DEEP_OPTICAL_ERROR_STACK = 2,
  DEEP_OPTICAL_ERROR_BOUNDS = 4,
  DEEP_OPTICAL_ERROR_NONFINITE = 8,
  DEEP_OPTICAL_ERROR_COMPLETION = 16,
};

CCL_NAMESPACE_END
