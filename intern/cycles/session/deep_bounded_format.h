/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Host payload consists of FLOAT Z/ZBack, A or RGBA, then optional summary.
 * GPU plane traversal and EXR channel order are deliberately not unified. */
inline size_t deep_bounded_record_bytes(const bool half, const bool physical_rgb)
{
  return 8 + (half ? 2 : 4) * (physical_rgb ? 4 : 1);
}
inline size_t deep_bounded_summary_bytes(const bool simple, const bool dense,
                                         const size_t pixels, const size_t samples)
{
  return simple ? 4 * (dense ? pixels : samples) : 0;
}
inline size_t deep_bounded_summary_offset(const size_t samples, const bool half,
                                          const bool physical_rgb = false)
{
  return samples * deep_bounded_record_bytes(half, physical_rgb);
}

CCL_NAMESPACE_END
