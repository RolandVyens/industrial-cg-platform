/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <cmath>
#include <limits>

#include "session/deep_camera_depth.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
DeepCameraDepthSpec radial()
{
  DeepCameraDepthSpec spec;
  spec.kind = DeepCameraDepthKind::Radial;
  spec.origin[0] = 3;
  spec.origin[1] = 4;
  spec.direction[2] = 2;
  spec.tmax = 8;
  return spec;
}
void contains(const DeepDepthParameterBounds &bounds, double reference)
{
  EXPECT_LE(bounds.lower, reference);
  EXPECT_GE(bounds.upper, reference);
  EXPECT_GE(bounds.value, bounds.lower);
  EXPECT_LE(bounds.value, bounds.upper);
}
}  // namespace

TEST(DeepCameraDepth, affine_frozen_slope_preserves_physical_parameter)
{
  DeepCameraDepthSpec spec;
  spec.affine_origin = .25;
  spec.affine_slope = .5;
  spec.tmax = 20;
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(spec));
  double z;
  ASSERT_TRUE(map.depth(6, z));
  EXPECT_EQ(z, 3.25);
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(3.25, t));
  contains(t, 6);
  DeepDepthDerivativeBounds derivative;
  ASSERT_TRUE(map.derivatives(.25, 10, derivative));
  EXPECT_GE(derivative.first_abs, 2);
  EXPECT_EQ(derivative.second_abs, 0);
}

TEST(DeepCameraDepth, nonunit_radial_345_geometry_does_not_normalize_the_ray)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  double z;
  ASSERT_TRUE(map.depth(6, z));
  EXPECT_EQ(z, 13);
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(13, t));
  contains(t, 6);
  EXPECT_NEAR(t.value, 6, 2e-13);
  ASSERT_TRUE(map.parameter(5, t));
  contains(t, 0);
}

TEST(DeepCameraDepth, radial_derivatives_enclose_the_whole_offset_interval)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  DeepDepthDerivativeBounds bounds;
  ASSERT_TRUE(map.derivatives(5.125, 13, bounds));
  ASSERT_TRUE(std::isfinite(bounds.first_abs));
  ASSERT_TRUE(std::isfinite(bounds.second_abs));
  for (int i = 0; i <= 512; i++) {
    const double z = 5.125 + (13 - 5.125) * i / 512;
    const double distance = std::sqrt(z * z - 25);
    EXPECT_LE(z / (2 * distance), bounds.first_abs);
    EXPECT_LE(25 / (2 * distance * distance * distance), bounds.second_abs);
  }
}

TEST(DeepCameraDepth, tangent_requests_total_variation_fallback)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  DeepDepthDerivativeBounds bounds;
  ASSERT_TRUE(map.derivatives(5, 6, bounds));
  EXPECT_TRUE(std::isinf(bounds.first_abs));
  EXPECT_TRUE(std::isinf(bounds.second_abs));
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(5, t));
  contains(t, 0);
}

TEST(DeepCameraDepth, collinear_origin_has_finite_right_derivative_at_zero)
{
  auto spec = radial();
  spec.origin[0] = spec.origin[1] = 0;
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(spec));
  DeepDepthDerivativeBounds bounds;
  ASSERT_TRUE(map.derivatives(0, 20, bounds));
  EXPECT_GE(bounds.first_abs, .5);
  EXPECT_LT(bounds.first_abs, .500000000001);
  EXPECT_EQ(bounds.second_abs, 0);
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(0, t));
  contains(t, 0);
}

TEST(DeepCameraDepth, decreasing_branch_is_rejected_but_outward_branch_after_turn_is_valid)
{
  auto spec = radial();
  spec.origin[0] = 1;
  spec.origin[1] = 0;
  spec.direction[0] = -1;
  spec.direction[2] = 0;
  DeepCameraDepthMap map;
  const auto status = map.initialize(spec);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message.find("NonMonotonicCameraDepth"), std::string::npos);
  EXPECT_FALSE(map.valid());
  spec.tmin = 1;
  ASSERT_TRUE(map.initialize(spec));
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(.5, t));
  contains(t, 1.5);
  double z;
  EXPECT_FALSE(map.depth(.5, z));
}

TEST(DeepCameraDepth, positive_parallel_offset_keeps_short_inverse_under_cancellation)
{
  auto spec = radial();
  spec.origin[0] = 1e8;
  spec.origin[1] = 1;
  spec.direction[0] = 1;
  spec.direction[2] = 0;
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(spec));
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(1e8 + .125, t));
  EXPECT_NEAR(t.value, .125, 1e-7);
  EXPECT_LT(t.upper - t.lower, 1e-5);
  contains(t, .125);
}

TEST(DeepCameraDepth, invalid_input_cannot_reuse_a_previous_valid_map)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  auto spec = radial();
  spec.direction[2] = 0;
  EXPECT_FALSE(map.initialize(spec));
  EXPECT_FALSE(map.valid());
  double z = 123;
  EXPECT_FALSE(map.depth(1, z));
  EXPECT_EQ(z, 0);
  spec = radial();
  spec.origin[0] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(map.initialize(spec));
  spec = radial();
  spec.tmin = 9;
  EXPECT_FALSE(map.initialize(spec));
}

TEST(DeepCameraDepth, radial_distance_uses_the_frozen_camera_origin)
{
  auto spec = radial();
  spec.camera_origin[0] = 100;
  spec.camera_origin[1] = 200;
  spec.camera_origin[2] = -300;
  spec.origin[0] = 103;
  spec.origin[1] = 204;
  spec.origin[2] = -300;
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(spec));
  double z;
  ASSERT_TRUE(map.depth(6, z));
  EXPECT_EQ(z, 13);
  DeepDepthParameterBounds t;
  ASSERT_TRUE(map.parameter(13, t));
  contains(t, 6);
}

TEST(DeepCameraDepth, absent_radial_preimage_is_an_error_not_a_curvature_fallback)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  DeepDepthParameterBounds t;
  EXPECT_FALSE(map.parameter(4, t));
  DeepDepthDerivativeBounds bounds;
  EXPECT_FALSE(map.derivatives(4, 6, bounds));
  EXPECT_EQ(bounds.first_abs, 0);
  EXPECT_EQ(bounds.second_abs, 0);
}

TEST(DeepCameraDepth, physical_span_bounds_endpoint_rounding_and_nonlinear_fractions)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  DeepCameraDepthSpan span;
  const double front = double(float(std::sqrt(34.))), back = 13;
  ASSERT_TRUE(span.initialize(map, 1.5, 6, front, back));
  DeepDepthParameterBounds fraction, delta;
  for (int i = 0; i <= 1024; i++) {
    const double z = front + (back - front) * i / 1024;
    ASSERT_TRUE(span.fraction(map, z, fraction));
    const long double t = std::sqrt(static_cast<long double>(z) * z - 25) / 2;
    const double ideal = double(std::clamp((t - 1.5L) / 4.5L, 0.L, 1.L));
    EXPECT_LE(fraction.lower, ideal);
    EXPECT_GE(fraction.upper, ideal);
  }
  ASSERT_TRUE(span.fraction(map, front, fraction));
  EXPECT_EQ(fraction.value, 0);
  ASSERT_TRUE(span.delta(map, front, back, delta));
  EXPECT_EQ(delta.value, 1);
  ASSERT_TRUE(span.delta(map, front, front, delta));
  EXPECT_EQ(delta.lower, 0);
  EXPECT_EQ(delta.upper, 0);
  DeepDepthDerivativeBounds derivative;
  ASSERT_TRUE(span.derivatives(map, 7, 12, derivative));
  EXPECT_GT(derivative.second_abs, 0);
  EXPECT_FALSE(span.initialize(map, 1.5, 6, 1, 13));
}

TEST(DeepCameraDepth, forward_enclosures_certify_tangent_adjacent_inverse_clamps)
{
  DeepCameraDepthMap map;
  ASSERT_TRUE(map.initialize(radial()));
  DeepDepthParameterBounds forward;
  ASSERT_TRUE(map.depth_bounds(6, forward));
  contains(forward, 13);
  ASSERT_TRUE(map.depth_bounds(0, forward));
  contains(forward, 5);
  DeepCameraDepthSpan span;
  ASSERT_TRUE(span.initialize(map, 0, 6, 5, 13));
  double z = 5;
  for (int i = 0; i < 64; i++) {
    DeepDepthParameterBounds f;
    ASSERT_TRUE(span.fraction(map, z, f));
    const double reference = std::sqrt((z - 5) * (z + 5)) / 12;
    EXPECT_LE(f.lower, reference);
    EXPECT_GE(f.upper, reference);
    z = std::nextafter(z, std::numeric_limits<double>::infinity());
  }
}

CCL_NAMESPACE_END
