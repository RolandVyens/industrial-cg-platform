/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Scalar backing ABI, distinct from the old packed 48-byte film event.
 * The path flags distinguish split paths; shader alpha is raw compatibility metadata,
 * not a certified final opacity (in particular for holdouts/shadow catchers). */
struct DeepSurfaceReceipt {
  float rgba[4];
  float z, zback;
  uint32_t pixel, sample, rng_pixel, rng_offset, ordinal;
  uint32_t object, prim, shader, packed_normal, path_flags;
  float alpha_rgb[3], throughput[3], time, reserved;
};
static_assert(sizeof(DeepSurfaceReceipt) == 96, "Surface receipt backing ABI changed");

CCL_NAMESPACE_END
