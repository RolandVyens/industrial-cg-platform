/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Native Beauty radiance ownership, independent of Surface opacity and Optical tau.
 * All RGB is the final, throughput-weighted post-clamp contribution written to film.
 * A root remains stable across main-state reuse, splits and late shadow completion. */
struct DeepVolumeRadianceReceipt {
  float emission[3], unowned_path[3], unowned_shadow[3];
  uint32_t pixel, sample, rng_pixel, rng_offset, path_flags, bounce;
  float tmin, tmax, time, origin[3], direction[3];
};
static_assert(sizeof(DeepVolumeRadianceReceipt) == 96, "Volume radiance receipt ABI changed");

CCL_NAMESPACE_END
