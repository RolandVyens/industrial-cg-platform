/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Raw legacy-compatible primary emission payload, separate from authoritative post-clamp
 * Volume radiance totals. Never assign NEE/scatter totals an invented source depth. */
struct DeepVolumeEmissionSpan {
  uint32_t root, pixel, sample, ordinal, kind, path_flags;
  float z, zback, alpha, time, rgb[3], tau[3];
};
static_assert(sizeof(DeepVolumeEmissionSpan) == 64, "Volume emission span ABI changed");
constexpr uint32_t DEEP_EMISSION_HOMOGENEOUS = 1u;
constexpr uint32_t DEEP_EMISSION_MARCHED = 2u;
constexpr uint32_t DEEP_EMISSION_LEGACY_OPACITY_ELIGIBLE = 4u;

CCL_NAMESPACE_END
