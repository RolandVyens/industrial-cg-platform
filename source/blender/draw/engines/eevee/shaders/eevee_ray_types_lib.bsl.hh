/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_math_geom_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_math_matrix_transform_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_ray_lib.glsl"

/* Screen-space ray ([0..1] "uv" range) where direction is normalize to be as small as one
 * full-resolution pixel. The ray is also clipped to all frustum sides.
 * Z component is device normalized Z (aka. depth buffer value).
 * W component is device normalized Z + Thickness.
 */
struct ScreenSpaceRay {
  float4 origin;
  float4 direction;
  float max_time;

  static ScreenSpaceRay create(Ray ray, float2 pixel_size)
  {
    ScreenSpaceRay ssray;
    ssray.origin.xyz = drw_point_view_to_ndc(ray.origin);
    ssray.direction.xyz = drw_point_view_to_ndc(ray.origin + ray.direction * ray.max_time);

    ssray.ray_finalize(pixel_size);
    return ssray;
  }

  static ScreenSpaceRay create(Ray ray, float4x4 winmat, float2 pixel_size)
  {
    ScreenSpaceRay ssray;
    ssray.origin.xyz = project_point(winmat, ray.origin);
    ssray.direction.xyz = project_point(winmat, ray.origin + ray.direction * ray.max_time);

    ssray.ray_finalize(pixel_size);
    return ssray;
  }

  static ScreenSpaceRay create(Ray ray, float2 pixel_size, float thickness)
  {
    ScreenSpaceRay ssray;
    ssray.origin.xyz = drw_point_view_to_ndc(ray.origin);
    ssray.direction.xyz = drw_point_view_to_ndc(ray.origin + ray.direction * ray.max_time);
    /* Interpolate thickness in screen space.
     * Calculate thickness further away to avoid near plane clipping issues. */
    ssray.origin.w = drw_depth_view_to_screen(ray.origin.z - thickness);
    ssray.direction.w = drw_depth_view_to_screen(ray.origin.z + ray.direction.z - thickness);
    ssray.origin.w = ssray.origin.w * 2.0f - 1.0f;
    ssray.direction.w = ssray.direction.w * 2.0f - 1.0f;

    ssray.ray_finalize(pixel_size);
    return ssray;
  }

 private:
  void ray_finalize(float2 pixel_size)
  {
    /* Constant bias (due to depth buffer precision). Helps with self intersection. */
    /* Magic numbers for 24bits of precision.
     * From http://terathon.com/gdc07_lengyel.pdf (slide 26) */
    constexpr float bias = -2.4e-7f * 2.0f;
    origin.zw += bias;
    direction.zw += bias;

    direction -= origin;
    /* If the line is degenerate, make it cover at least one pixel
     * to not have to handle zero-pixel extent as a special case later */
    if (length_squared(direction.xy) < 0.00001f) {
      direction.xy = float2(0.0f, 0.00001f);
    }
    float ray_len_sqr = length_squared(direction.xyz);
    /* Make direction cover one pixel. */
    bool is_more_vertical = abs(direction.x / pixel_size.x) < abs(direction.y / pixel_size.y);
    direction /= (is_more_vertical) ? abs(direction.y) : abs(direction.x);
    direction *= (is_more_vertical) ? pixel_size.y : pixel_size.x;
    /* Clip to segment's end. */
    max_time = sqrt(ray_len_sqr * safe_rcp(length_squared(direction.xyz)));
    /* Clipping to frustum sides. */
    float clip_dist = line_unit_box_intersect_dist_safe(origin.xyz, direction.xyz);
    max_time = min(max_time, clip_dist);
    /* Convert to texture coords [0..1] range. */
    origin = origin * 0.5f + 0.5f;
    direction *= 0.5f;
  }
};
