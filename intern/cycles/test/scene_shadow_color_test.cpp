/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "kernel/integrator/shadow_state.h"
#include "scene/light.h"

CCL_NAMESPACE_BEGIN

TEST(ShadowColorPolicy, unshadowed_throughput_state_is_opt_in)
{
  EXPECT_FALSE(shadow_state_unshadowed_throughput_is_needed(0, false));
  EXPECT_TRUE(shadow_state_unshadowed_throughput_is_needed(KERNEL_FEATURE_AO_ADDITIVE, false));
  EXPECT_TRUE(shadow_state_unshadowed_throughput_is_needed(0, true));
}

TEST(ShadowColorPolicy, only_enabled_shadow_casting_colored_lights_activate)
{
  PointLight light;
  light.set_is_enabled(true);
  light.set_cast_shadow(true);

  EXPECT_FALSE(light_uses_shadow_color(&light));

  light.set_shadow_color(make_float3(0.25f, 0.0f, 0.0f));
  EXPECT_TRUE(light_uses_shadow_color(&light));

  light.set_cast_shadow(false);
  EXPECT_FALSE(light_uses_shadow_color(&light));

  light.set_cast_shadow(true);
  light.set_is_enabled(false);
  EXPECT_FALSE(light_uses_shadow_color(&light));
}

CCL_NAMESPACE_END
