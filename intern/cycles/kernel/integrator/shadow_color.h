/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"
#include "kernel/types.h"

#include "kernel/integrator/state.h"

CCL_NAMESPACE_BEGIN

ccl_device_forceinline Spectrum integrator_shadow_color(KernelGlobals kg,
                                                        ConstIntegratorShadowState state,
                                                        const uint32_t path_flag,
                                                        const bool use_shadow_color)
{
  if (!use_shadow_color || (path_flag & PATH_RAY_SHADOW_FOR_AO)) {
    return zero_spectrum();
  }

  const int light_object = INTEGRATOR_STATE(state, shadow_ray, self_light_object);
  const int light_prim = INTEGRATOR_STATE(state, shadow_ray, self_light_prim);
  if (light_object == OBJECT_NONE || light_prim == PRIM_NONE) {
    return zero_spectrum();
  }

  const KernelObject &kobject = kernel_data_fetch(objects, light_object);
  if (kobject.primitive_type != PRIMITIVE_LAMP) {
    return zero_spectrum();
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, light_prim);
  const Spectrum shadow_color = make_float3(
      klight->shadow_color[0], klight->shadow_color[1], klight->shadow_color[2]);
  return clamp(shadow_color, zero_spectrum(), one_spectrum());
}

CCL_NAMESPACE_END
