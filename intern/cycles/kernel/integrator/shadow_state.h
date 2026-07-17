/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/features.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline bool shadow_state_unshadowed_throughput_is_needed(const uint kernel_features,
                                                                    const bool use_shadow_color)
{
  return (kernel_features & KERNEL_FEATURE_AO_ADDITIVE) || use_shadow_color;
}

CCL_NAMESPACE_END
