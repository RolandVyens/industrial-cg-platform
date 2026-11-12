/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64)
#  include <xmmintrin.h>
#endif

#include "integrator/deep_control_journal.h"
#include "integrator/deep_expression_journal.h"
#include "integrator/deep_native_capacity.h"
#include "session/deep_frame_production.h"
#include "session/deep_homogeneous_curve.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {

bool begin_reference_capture(DeepFrameProduction &production, const DeepNativeCaptureDomain &domain)
{
  auto *reference = production.reference_capture();
  return reference && reference->begin_native_capture(domain);
}

bool complete_reference_capture(DeepFrameProduction &production, DeepNativeCaptureBatch &&batch)
{
  auto *reference = production.reference_capture();
  return reference && reference->complete_native_capture(std::move(batch));
}

DeepFrameKey homogeneous_frame()
{
  return {701, 2, 3, 4, 2, 0, 5, 6};
}

DeepHomogeneousRay admitted_ray(
    const uint64_t ray,
    const uint32_t sample,
    const std::span<const DeepHomogeneousSegment> supports = {},
    const DeepCameraDisposition disposition = DeepCameraDisposition::Ray)
{
  DeepHomogeneousRay result;
  result.ray = ray;
  result.sample = {-96, -17, sample, 17, 99, 47, disposition, sample + 17};
  result.native_complete = true;
  result.coverage_complete = true;
  result.homogeneous_certified = true;
  result.affine_camera_depth = true;
  result.segments = supports;
  if (!supports.empty()) {
    result.coverage_front = supports.front().z;
    result.coverage_back = supports.back().z_back;
  }
  return result;
}

DeepHomogeneousPixelInput complete_pixel(const std::span<const DeepHomogeneousRay> rays)
{
  DeepHomogeneousPixelInput input;
  input.frame = homogeneous_frame();
  input.x = -96;
  input.y = -17;
  input.native_population = rays.size();
  input.population_complete = true;
  input.rays = rays;
  return input;
}

template<typename Curve> std::vector<DeepDepthKnot> read_knots(const Curve &curve)
{
  std::vector<DeepDepthKnot> knots(curve.knot_count());
  for (size_t i = 0; i < knots.size(); i++)
    EXPECT_TRUE(curve.read_knot(i, knots[i]));
  return knots;
}

std::array<double, 4> prefix(const std::vector<DeepDepthKnot> &knots,
                             const double z,
                             const bool before = false)
{
  std::array<double, 4> value{1, 0, 0, 0};
  for (size_t i = 0; i < knots.size(); i++) {
    if (knots[i].z > z)
      break;
    for (int c = 0; c < 4; c++) {
      if (knots[i].z == z)
        value[c] = before ? knots[i].before[c] : knots[i].after[c];
      else
        value[c] = knots[i].after[c] + knots[i].slope_right[c] * (z - knots[i].z);
    }
  }
  return value;
}

/* Independent closed-form oracle for the ordinary tau used by these tests. It does
 * not call the builder's fraction, curvature/refinement, event or sweep implementation. */
double analytic_fraction(const double tau, const double u)
{
  return tau == 0 ? u : (1 - std::exp(-tau * u)) / (1 - std::exp(-tau));
}

void expect_bound(const DeepHomogeneousCurve &curve)
{
  for (const double error : curve.representation_error()) {
    EXPECT_GE(error, 0);
    EXPECT_LE(error, 1e-4);
  }
}

DeepNativeCaptureBatch native_packet(const DeepMemoryBudget &budget,
                                     const uint64_t id = 1,
                                     const uint32_t scheduled = 0,
                                     const float tau = 2,
                                     const float red = 4)
{
  DeepNativeCaptureBatch packet(budget);
  auto &domain = packet.domain;
  domain.id = id;
  domain.device_generation = 8;
  domain.layout_generation = id;
  domain.start_sample = scheduled;
  domain.samples_num = 1;
  domain.sample_offset = 17;
  domain.x = -96;
  domain.y = -17;
  domain.width = 1;
  domain.height = 1;
  domain.scheduled_admissions = 1;
  domain.camera_reserved = domain.camera_written = 1;
  domain.homogeneous_reserved = domain.homogeneous_written = 1;
  domain.emission_calls = 1;
  domain.main_shadow_mnee_joined = true;
  DeepNativeCameraRecord camera = {};
  camera.pixel = 0;
  camera.sample = scheduled + 17;
  camera.scheduled_sample = scheduled;
  camera.subset_offset = 17;
  camera.rng_pixel = 77;
  camera.rng_offset = 47;
  camera.x = -96;
  camera.y = -17;
  camera.flags = DEEP_NATIVE_CAMERA_EVALUATED | DEEP_NATIVE_CAMERA_AFFINE_DEPTH |
                 DEEP_NATIVE_CAMERA_BACKGROUND_REACHED | DEEP_NATIVE_CAMERA_COMPLETE;
  camera.disposition = DEEP_NATIVE_CAMERA_RAY;
  camera.population_weight = 1;
  camera.direction[2] = 1;
  camera.tmax = 1000;
  camera.time = 0.5f;
  camera.throughput[0] = camera.throughput[1] = camera.throughput[2] = 1;
  camera.z_direction = 1;
  packet.cameras.push_back(camera);
  DeepNativeHomogeneousRecord segment = {};
  segment.camera_index = 0;
  segment.pixel = 0;
  segment.sample = camera.sample;
  segment.ordinal = 7;
  segment.flags = DEEP_NATIVE_HOMOGENEOUS_OPTICAL | DEEP_NATIVE_HOMOGENEOUS_EMISSION_FILM_CALL |
                  DEEP_NATIVE_HOMOGENEOUS_CAMERA_SUPPORT_VERIFIED;
  segment.emission_calls = 1;
  segment.z = segment.tmin = 2;
  segment.zback = segment.tmax = 4;
  segment.tau[0] = segment.tau[1] = segment.tau[2] = tau;
  segment.post_clamp_rgb[0] = red;
  segment.post_clamp_rgb[1] = -3;
  segment.post_clamp_rgb[2] = 1;
  segment.pre_clamp_rgb[0] = red;
  segment.pre_clamp_rgb[1] = -3;
  segment.pre_clamp_rgb[2] = 1;
  segment.clamp_gain = 1;
  segment.source_object = 2;
  segment.source_shader = 3;
  packet.homogeneous.push_back(segment);
  return packet;
}

bool admit_packet(DeepFrameProduction &production, DeepNativeCaptureBatch &&packet)
{
  auto begin = packet.domain;
  begin.main_shadow_mnee_joined = false;
  begin.camera_reserved = begin.camera_written = 0;
  begin.homogeneous_reserved = begin.homogeneous_written = begin.emission_calls = 0;
  begin.point_reserved = begin.point_written = begin.root_reserved = begin.root_written = 0;
  return begin_reference_capture(production, begin) &&
         complete_reference_capture(production, std::move(packet));
}

std::array<double, 4> decode_encoded(const DeepCanonicalEncoded &encoded, const double z)
{
  std::array<double, 4> value{1, 0, 0, 0};
  for (const auto &sample : encoded.samples()) {
    if (z < sample.z)
      break;
    const double fraction = sample.z == sample.z_back ?
                                1 :
                                std::clamp((z - sample.z) / (sample.z_back - sample.z), 0.0, 1.0);
    const double alpha = fraction == 0 ? 0 : 1 - std::pow(1 - double(sample.a), fraction);
    const double rgb_fraction = sample.a == 0 ? fraction : alpha / sample.a;
    value[1] += value[0] * sample.r * rgb_fraction;
    value[2] += value[0] * sample.g * rgb_fraction;
    value[3] += value[0] * sample.b * rgb_fraction;
    value[0] *= 1 - alpha;
    if (z < sample.z_back)
      break;
  }
  return value;
}
}  // namespace

TEST(DeepHomogeneousCurve, native_float_grid_preserves_aa_supports_without_sub_ulp_cuts)
{
  std::array<DeepHomogeneousSegment, 1> first{{{3, 5, 0.2, {0.2, 0.2, 0.2}, 1}}};
  std::array<DeepHomogeneousSegment, 1> second{
      {{double(std::nextafter(3.0f, 4.0f)), 5, 0.2, {0.2, 0.2, 0.2}, 2}}};
  const std::array<DeepHomogeneousRay, 2> rays{admitted_ray(1, 0, first),
                                               admitted_ray(2, 1, second)};
  auto input = complete_pixel(rays);
  DeepPageStore store(input.frame, 1 << 24);
  DeepHomogeneousCurvePolicy policy;
  policy.float_depth_grid = true;
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve, policy));
  EXPECT_EQ(curve.sources().size(), 2u);
  EXPECT_EQ(curve.sources()[1].support.z, second[0].z);
  for (const auto &knot : read_knots(curve))
    EXPECT_EQ(double(float(knot.z)), knot.z);
  expect_bound(curve);
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_curve(store.memory_budget(), curve, {}, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_TRUE(encoded.report()->quantized_checked);
  EXPECT_LE(encoded.report()->total_error[0], 0.1);
  EXPECT_LE(encoded.report()->total_error[1], 0.006);
}

TEST(DeepHomogeneousCurve, analytic_prefix_retains_native_signed_rgb_without_double_attenuation)
{
  const std::array<DeepHomogeneousSegment, 1> supports{{{2, 6, 2, {4, -3, 0.5}, 11}}};
  const auto ray = admitted_ray(0x100000001ULL, 0xf0000000u, supports);
  auto input = complete_pixel({&ray, 1});
  input.exposure = 3;
  DeepPageStore store(input.frame, 16 << 20);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  ASSERT_TRUE(curve);
  EXPECT_EQ(curve.native_population(), 1u);
  EXPECT_EQ(curve.population_weight(), 1);
  ASSERT_EQ(curve.identities().size(), 1u);
  EXPECT_EQ(curve.identities()[0].sample.sample, 0xf0000000u);
  const auto knots = read_knots(curve);
  for (int i = 0; i <= 256; i++) {
    const double u = double(i) / 256;
    const auto value = prefix(knots, 2 + 4 * u);
    EXPECT_NEAR(value[0], std::exp(-2 * u), 1e-4);
    const double fraction = analytic_fraction(2, u);
    EXPECT_NEAR(value[1], 12 * fraction, 1e-4);
    EXPECT_NEAR(value[2], -9 * fraction, 1e-4);
    EXPECT_NEAR(value[3], 1.5 * fraction, 1e-4);
  }
  const auto end = prefix(knots, 6);
  EXPECT_DOUBLE_EQ(end[1], 12);
  EXPECT_DOUBLE_EQ(end[2], -9);
  EXPECT_DOUBLE_EQ(end[3], 1.5);
  expect_bound(curve);
}

TEST(DeepHomogeneousCurve, ray_first_prefix_preserves_aa_correlation_and_complete_denominator)
{
  const std::array<DeepHomogeneousSegment, 2> a{
      {{0, 1, 0, {2, 0, 0}, 11}, {1, 2, 4, {-1, 0, 0}, 12}}};
  const std::array<DeepHomogeneousSegment, 2> b{
      {{0, 1, 4, {-3, 0, 0}, 21}, {1, 2, 0, {4, 0, 0}, 22}}};
  const std::array<DeepHomogeneousRay, 3> rays{
      admitted_ray(8, 17, a),
      admitted_ray(9, 18, b),
      admitted_ray(10, 19, {}, DeepCameraDisposition::ClearTransparent)};
  const auto input = complete_pixel(rays);
  DeepPageStore store(input.frame, 16 << 20);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  const auto knots = read_knots(curve);
  for (int i = 0; i <= 256; i++) {
    const double z = double(i) / 128;
    const double first = std::min(z, 1.0), second = std::max(z - 1, 0.0);
    const double expected_t = (std::exp(-4 * second) + std::exp(-4 * first) + 1) / 3;
    const double expected_r = (2 * first - analytic_fraction(4, second) -
                               3 * analytic_fraction(4, first) + 4 * second) /
                              3;
    const auto value = prefix(knots, z);
    EXPECT_NEAR(value[0], expected_t, 1e-4);
    EXPECT_NEAR(value[1], expected_r, 1e-4);
  }
  EXPECT_GT(std::abs(prefix(knots, 1.5)[0] - (2 * std::exp(-3.0) + 1) / 3), 0.01);
  const auto boundary = std::find_if(
      knots.begin(), knots.end(), [](const auto &knot) { return knot.z == 1; });
  ASSERT_NE(boundary, knots.end());
  EXPECT_TRUE(boundary->flags & DEEP_KNOT_PROTECTED_CUT);
  EXPECT_TRUE(boundary->flags & DEEP_KNOT_INTERVAL_BOUNDARY);
}

TEST(DeepHomogeneousCurve, zero_tau_emission_is_linear_and_all_clear_is_owned)
{
  const std::array<DeepHomogeneousSegment, 1> supports{{{3, 9, 0, {-7, 3, 9}, 42}}};
  const std::array<DeepHomogeneousRay, 2> rays{
      admitted_ray(7, 1, supports),
      admitted_ray(8, 2, {}, DeepCameraDisposition::ClearTransparent)};
  const auto input = complete_pixel(rays);
  DeepPageStore store(input.frame, 1 << 20, 512);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  EXPECT_EQ(curve.knot_count(), 3u);
  const auto value = prefix(read_knots(curve), 6);
  EXPECT_DOUBLE_EQ(value[0], 1);
  EXPECT_DOUBLE_EQ(value[1], -1.75);
  EXPECT_DOUBLE_EQ(value[2], 0.75);
  EXPECT_DOUBLE_EQ(value[3], 2.25);
  const auto clear = admitted_ray(9, 3, {}, DeepCameraDisposition::ClearTransparent);
  ASSERT_TRUE(deep_build_homogeneous_curve(store, complete_pixel({&clear, 1}), curve));
  EXPECT_EQ(curve.knot_count(), 1u);
  ASSERT_EQ(curve.pages().size(), 1u);
  const auto constant = prefix(read_knots(curve), 100);
  EXPECT_DOUBLE_EQ(constant[0], 1);
  EXPECT_DOUBLE_EQ(constant[1], 0);
  EXPECT_TRUE(store.publication_status());
}

TEST(DeepHomogeneousCurve, invalid_camera_black_uses_protected_sentinel_without_camera_support)
{
  auto black = admitted_ray(21, 1, {}, DeepCameraDisposition::InvalidCameraBlack);
  black.coverage_front = std::numeric_limits<double>::quiet_NaN();
  black.coverage_back = std::numeric_limits<double>::infinity();
  black.coverage_complete = false;
  black.homogeneous_certified = false;
  black.affine_camera_depth = false;
  const auto clear = admitted_ray(22, 2, {}, DeepCameraDisposition::ClearTransparent);
  const std::array<DeepHomogeneousRay, 2> rays{black, clear};
  const auto input = complete_pixel(rays);
  DeepPageStore store(input.frame, 1 << 20);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  const auto knots = read_knots(curve);
  ASSERT_EQ(knots.size(), 2u);
  const auto &terminal = knots.back();
  EXPECT_DOUBLE_EQ(terminal.z, double(std::numeric_limits<float>::max()));
  EXPECT_DOUBLE_EQ(terminal.before[0], 1);
  EXPECT_DOUBLE_EQ(terminal.after[0], 0.5);
  EXPECT_TRUE(terminal.flags & DEEP_KNOT_PROTECTED_CUT);
  EXPECT_TRUE(terminal.flags & DEEP_KNOT_INVALID_CAMERA_BLACK);
  EXPECT_DOUBLE_EQ(prefix(knots, 100)[0], 1);
  EXPECT_EQ(curve.native_population(), 2u);
}

TEST(DeepHomogeneousCurve, weak_tau_many_layers_and_more_than_64_supports_are_retained)
{
  std::array<DeepHomogeneousSegment, 200> supports;
  for (size_t i = 0; i < supports.size(); i++)
    supports[i] = {double(i), double(i + 1), 1e-10, {0, 0, 0}, i + 1};
  const auto ray = admitted_ray(31, 17, supports);
  const auto input = complete_pixel({&ray, 1});
  DeepPageStore store(input.frame, 4 << 20, 512);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  EXPECT_EQ(curve.sources().size(), 200u);
  EXPECT_GT(curve.knot_count(), 64u);
  EXPECT_GT(curve.pages().size(), 1u);
  const auto knots = read_knots(curve);
  EXPECT_NEAR(prefix(knots, 100.5)[0], std::exp(-100.5e-10), 1e-12);
  EXPECT_NEAR(knots.back().after[0], std::exp(-200e-10), 1e-15);
  for (const auto &source : curve.sources())
    EXPECT_DOUBLE_EQ(source.support.tau, 1e-10);
  for (const auto &knot : knots)
    EXPECT_TRUE(knot.flags & DEEP_KNOT_PROTECTED_CUT);
}

TEST(DeepHomogeneousCurve, finite_tau_survives_endpoint_underflow_and_early_cuts)
{
  const std::array<DeepHomogeneousSegment, 1> supports{{{0, 1, 1000, {1, -2, 0}, 51}}};
  const auto ray = admitted_ray(41, 1, supports);
  const auto input = complete_pixel({&ray, 1});
  DeepPageStore store(input.frame, 16 << 20);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  ASSERT_EQ(curve.sources().size(), 1u);
  EXPECT_DOUBLE_EQ(curve.sources()[0].support.tau, 1000);
  const auto knots = read_knots(curve);
  for (const double z : {0.0001, 0.0005, 0.001, 0.002, 0.01}) {
    const auto value = prefix(knots, z);
    EXPECT_NEAR(value[0], std::exp(-1000 * z), 1e-4);
    EXPECT_NEAR(value[1], 1 - std::exp(-1000 * z), 1e-4);
    EXPECT_NEAR(value[2], -2 * (1 - std::exp(-1000 * z)), 1e-4);
  }
  EXPECT_DOUBLE_EQ(knots.back().after[0], 0);
  EXPECT_DOUBLE_EQ(knots.back().after[1], 1);
}

TEST(DeepHomogeneousCurve, signed_hdr_aa_cancellation_uses_absolute_error)
{
  const std::array<DeepHomogeneousSegment, 1> a{{{0, 1, 3, {10000, -5000, 500}, 61}}};
  const std::array<DeepHomogeneousSegment, 1> b{{{0, 1, 7, {-10000, 5000, -500}, 62}}};
  const std::array<DeepHomogeneousRay, 2> rays{admitted_ray(51, 1, a), admitted_ray(52, 2, b)};
  const auto input = complete_pixel(rays);
  DeepPageStore store(input.frame, 128 << 20);
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  const auto knots = read_knots(curve);
  for (int i = 0; i <= 128; i++) {
    const double z = double(i) / 128;
    const double difference = analytic_fraction(3, z) - analytic_fraction(7, z);
    const auto value = prefix(knots, z);
    EXPECT_NEAR(value[1], 5000 * difference, 1e-4);
    EXPECT_NEAR(value[2], -2500 * difference, 1e-4);
    EXPECT_NEAR(value[3], 250 * difference, 1e-4);
  }
  EXPECT_DOUBLE_EQ(knots.back().after[1], 0);
  EXPECT_GT(curve.knot_count(), 64u);
  expect_bound(curve);
}

TEST(DeepHomogeneousCurve, population_identity_and_incomplete_accounting_fail_closed)
{
  auto ray = admitted_ray(71, 17, {}, DeepCameraDisposition::ClearTransparent);
  DeepPageStore store(homogeneous_frame(), 1 << 20);
  DeepHomogeneousCurve curve;
  auto input = complete_pixel({&ray, 1});
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  input.native_population = 0;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code,
            DeepErrorCode::PopulationMismatch);
  EXPECT_FALSE(curve);
  input.native_population = UINT64_MAX;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code,
            DeepErrorCode::PopulationMismatch);
  input = complete_pixel({&ray, 1});
  input.population_complete = false;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::Incomplete);
  input.population_complete = true;
  ray.native_complete = false;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::Incomplete);
  ray.native_complete = true;
  std::array<DeepHomogeneousRay, 2> duplicate{ray, ray};
  duplicate[1].ray++;
  duplicate[1].sample.disposition = DeepCameraDisposition::InvalidCameraBlack;
  EXPECT_EQ(deep_build_homogeneous_curve(store, complete_pixel(duplicate), curve).code,
            DeepErrorCode::DuplicateSample);
  duplicate[1].sample.sample++;
  duplicate[1].ray = duplicate[0].ray;
  EXPECT_EQ(deep_build_homogeneous_curve(store, complete_pixel(duplicate), curve).code,
            DeepErrorCode::InvalidReference);
  input = complete_pixel({&ray, 1});
  input.frame.invocation++;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::InvalidFrame);
}

TEST(DeepHomogeneousCurve, malformed_tau_coverage_source_and_m2_features_fail_closed)
{
  std::array<DeepHomogeneousSegment, 2> supports{
      {{0, 1, 1, {1, 0, 0}, 81}, {1, 2, 1, {1, 0, 0}, 82}}};
  auto ray = admitted_ray(81, 1, supports);
  const auto input = complete_pixel({&ray, 1});
  DeepPageStore store(input.frame, 1 << 20);
  DeepHomogeneousCurve curve;
  supports[0].tau = -1;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::InvalidPayload);
  supports[0].tau = 1;
  supports[1].z = 1.25;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::Incomplete);
  supports[1].z = 0.75;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::Incomplete);
  supports[1].z = 1;
  supports[1].source = supports[0].source;
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code,
            DeepErrorCode::InvalidReference);
  supports[1].source = 82;
  supports[1].post_clamp_rgb[0] = std::numeric_limits<double>::infinity();
  EXPECT_EQ(deep_build_homogeneous_curve(store, input, curve).code, DeepErrorCode::InvalidPayload);
  supports[1].post_clamp_rgb[0] = 1;
  ray.affine_camera_depth = false;
  const auto radial = deep_build_homogeneous_curve(store, input, curve);
  EXPECT_FALSE(radial);
  EXPECT_NE(radial.message.find("M2"), std::string::npos);
  ray.affine_camera_depth = true;
  for (uint32_t feature = 1; feature <= DEEP_HOMOGENEOUS_BACKGROUND; feature <<= 1) {
    ray.unsupported_features = feature;
    const auto status = deep_build_homogeneous_curve(store, input, curve);
    EXPECT_FALSE(status);
    EXPECT_NE(status.message.find("M2"), std::string::npos);
  }
  EXPECT_FALSE(curve);
}

TEST(DeepHomogeneousCurve, unrepresentable_depth_and_physical_tau_sum_fail_without_truncation)
{
  const double front = 1e20;
  std::array<DeepHomogeneousSegment, 1> thin{
      {{front,
        std::nextafter(front, std::numeric_limits<double>::infinity()),
        10,
        {1, 0, 0},
        91}}};
  auto ray = admitted_ray(91, 1, thin);
  auto input = complete_pixel({&ray, 1});
  DeepPageStore store(input.frame, 1 << 20);
  DeepHomogeneousCurve curve;
  const auto status = deep_build_homogeneous_curve(store, input, curve);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message.find("DepthNotRepresentable"), std::string::npos);
  EXPECT_FALSE(curve);
  std::array<DeepHomogeneousSegment, 2> huge{
      {{0, 1, std::numeric_limits<double>::max(), {0, 0, 0}, 92},
       {1, 2, std::numeric_limits<double>::max(), {0, 0, 0}, 93}}};
  ray = admitted_ray(92, 2, huge);
  input = complete_pixel({&ray, 1});
  EXPECT_FALSE(deep_build_homogeneous_curve(store, input, curve));
  EXPECT_FALSE(curve);
}

TEST(DeepHomogeneousCurve, shared_budget_pressure_is_explicit_and_leases_own_result_bytes)
{
  const auto clear = admitted_ray(101, 1, {}, DeepCameraDisposition::ClearTransparent);
  const auto input = complete_pixel({&clear, 1});
  DeepMemoryBudget budget(2 << 20);
  const size_t empty = budget.used();
  DeepHomogeneousCurve retained;
  {
    DeepPageStore store(input.frame, budget, 512);
    ASSERT_TRUE(deep_build_homogeneous_curve(store, input, retained));
    ASSERT_FALSE(retained.pages().empty());
    EXPECT_TRUE(retained.pages()[0].is_charged_to(budget));
    EXPECT_GT(budget.used(), empty);
  }
  DeepDepthKnot knot;
  ASSERT_TRUE(retained.read_knot(0, knot));
  EXPECT_DOUBLE_EQ(knot.after[0], 1);
  auto copy = retained;
  retained = {};
  EXPECT_GT(budget.used(), empty);
  copy = {};
  EXPECT_EQ(budget.used(), empty);
  DeepPageStore scarce(input.frame, 8192, 65536);
  const size_t before = scarce.bytes_used();
  EXPECT_EQ(deep_build_homogeneous_curve(scarce, input, retained).code,
            DeepErrorCode::BudgetExceeded);
  EXPECT_FALSE(retained);
  EXPECT_EQ(scarce.bytes_used(), before);
}

TEST(DeepHomogeneousCurve, tighter_absolute_policy_adds_knots_and_never_uses_relative_hdr_budget)
{
  const std::array<DeepHomogeneousSegment, 1> supports{{{0, 1, 1, {10, -20, 30}, 111}}};
  const auto ray = admitted_ray(111, 1, supports);
  const auto input = complete_pixel({&ray, 1});
  DeepPageStore store(input.frame, 16 << 20);
  DeepHomogeneousCurve loose, tight;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, loose));
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, tight, {1e-6, 1e-6}));
  EXPECT_GT(tight.knot_count(), loose.knot_count());
  for (const double error : tight.representation_error())
    EXPECT_LE(error, 1e-6);
  EXPECT_FALSE(deep_build_homogeneous_curve(store, input, tight, {1e-3, 1e-3}));
  EXPECT_FALSE(tight);
}

TEST(DeepFrameProduction,
     two_native_domains_close_actual_population_then_encode_independent_prefix)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
  ASSERT_TRUE(production.error());
  ASSERT_TRUE(admit_packet(production, native_packet(production.memory_budget(), 301, 0, 2, 4)));
  ASSERT_TRUE(admit_packet(production, native_packet(production.memory_budget(), 302, 1, 5, 6)));
  ASSERT_EQ(production.curves().size(), 1u);
  EXPECT_FALSE(production.curves()[0]); /* Joined domains alone are not final pixel closure. */
  const std::array<uint32_t, 1> native_counts{2};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, native_counts, 2));
  ASSERT_TRUE(production.finish_inputs());
  ASSERT_TRUE(production.curves()[0]);
  EXPECT_EQ(production.curves()[0].native_population(), 2u);
  const auto knots = read_knots(production.curves()[0]);
  for (int i = 0; i <= 128; i++) {
    const double u = double(i) / 128;
    const auto value = prefix(knots, 2 + 2 * u);
    EXPECT_NEAR(value[0], (std::exp(-2 * u) + std::exp(-5 * u)) / 2, 1e-4);
    EXPECT_NEAR(value[1], 4 * analytic_fraction(2, u) + 6 * analytic_fraction(5, u), 1e-4);
  }
  DeepCanonicalEncoded encoded;
  DeepCanonicalEncodeConfig config;
  config.precision = DeepCanonicalPrecision::Float;
  config.alpha_tolerance = 0.01;
  ASSERT_TRUE(production.encode_pixel(0, config, encoded));
  ASSERT_TRUE(encoded);
  for (int i = 0; i <= 128; i++) {
    const double u = double(i) / 128;
    const auto decoded = decode_encoded(encoded, 2 + 2 * u);
    EXPECT_NEAR(decoded[0], (std::exp(-2 * u) + std::exp(-5 * u)) / 2, 0.01);
    EXPECT_NEAR(decoded[1], 4 * analytic_fraction(2, u) + 6 * analytic_fraction(5, u), 0.006);
  }
  EXPECT_NE(production.pipeline().state(), DeepFrameState::Complete);
  /* Real sinks and checked-close belong to the parent's actual consumer integration. */
}

TEST(DeepFrameProduction, closed_tile_retires_native_packets_and_later_tile_preserves_curve)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 2, 1}, 4 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 401);
  /* Retained native staging capacity must not become permanent frame storage. */
  packet.cameras.reserve(4096);
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const size_t captured = production.memory_budget().used();
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  EXPECT_LT(production.memory_budget().used(), captured / 4);
  const auto first = read_knots(production.curves()[0]);
  auto next = native_packet(production.memory_budget(), 402);
  next.domain.x++;
  next.cameras[0].x++;
  ASSERT_TRUE(admit_packet(production, std::move(next)));
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-95, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto retained = read_knots(production.curves()[0]);
  ASSERT_EQ(first.size(), retained.size());
  EXPECT_EQ(std::memcmp(first.data(), retained.data(), first.size() * sizeof(DeepDepthKnot)), 0);
  for (size_t pixel = 0; pixel < 2; ++pixel) {
    DeepCanonicalEncoded encoded;
    ASSERT_TRUE(production.encode_pixel(pixel, {}, encoded));
    EXPECT_NEAR(decode_encoded(encoded, 4)[0], std::exp(-2.0), .01);
    EXPECT_NEAR(decode_encoded(encoded, 4)[1], 4, .006);
  }
}

TEST(DeepFrameProduction, canonical_only_publication_keeps_exact_knots_and_both_consumer_payloads)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction reference(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
  DeepFrameProduction compact(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers, false);
  const std::array<uint32_t, 1> counts{2};
  for (auto *owner : {&reference, &compact}) {
    ASSERT_TRUE(admit_packet(*owner, native_packet(owner->memory_budget(), 410, 0, 2, 4)));
    ASSERT_TRUE(admit_packet(*owner, native_packet(owner->memory_budget(), 411, 1, 5, 6)));
    ASSERT_TRUE(owner->reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 2));
    ASSERT_TRUE(owner->finish_inputs());
  }
  const auto &a = reference.curves()[0], &b = compact.curves()[0];
  EXPECT_FALSE(a.optical_sources().empty());
  EXPECT_FALSE(a.radiance_sources().empty());
  EXPECT_TRUE(b.optical_sources().empty());
  EXPECT_TRUE(b.visibility_sources().empty());
  EXPECT_TRUE(b.radiance_sources().empty());
  EXPECT_EQ(a.native_population(), b.native_population());
  const auto expected = read_knots(a), actual = read_knots(b);
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(expected[i].z, actual[i].z);
    EXPECT_EQ(expected[i].flags, actual[i].flags);
    EXPECT_EQ(expected[i].source_group, actual[i].source_group);
    for (int c = 0; c < 4; ++c) {
      EXPECT_EQ(expected[i].before[c], actual[i].before[c]);
      EXPECT_EQ(expected[i].after[c], actual[i].after[c]);
      EXPECT_EQ(expected[i].slope_right[c], actual[i].slope_right[c]);
      EXPECT_EQ(a.representation_error()[c], b.representation_error()[c]);
    }
  }
  for (const auto precision : {DeepCanonicalPrecision::Half, DeepCanonicalPrecision::Float})
    for (const bool alpha_only : {false, true}) {
      DeepCanonicalEncodeConfig config;
      config.precision = precision;
      config.alpha_only = alpha_only;
      config.alpha_tolerance = .01;
      DeepCanonicalEncoded encoded_a, encoded_b;
      ASSERT_TRUE(reference.encode_pixel(0, config, encoded_a));
      ASSERT_TRUE(compact.encode_pixel(0, config, encoded_b));
      ASSERT_EQ(encoded_a.samples().size(), encoded_b.samples().size());
      for (size_t i = 0; i < encoded_a.samples().size(); ++i) {
        const auto &left = encoded_a.samples()[i], &right = encoded_b.samples()[i];
        EXPECT_EQ(left.r, right.r);
        EXPECT_EQ(left.g, right.g);
        EXPECT_EQ(left.b, right.b);
        EXPECT_EQ(left.a, right.a);
        EXPECT_EQ(left.z, right.z);
        EXPECT_EQ(left.z_back, right.z_back);
      }
      for (int i = 0; i <= 128; ++i) {
        const double u = double(i) / 128;
        const auto decoded = decode_encoded(encoded_b, 2 + 2 * u);
        EXPECT_NEAR(decoded[0], (std::exp(-2 * u) + std::exp(-5 * u)) / 2, .01);
        if (!alpha_only)
          EXPECT_NEAR(decoded[1], 4 * analytic_fraction(2, u) + 6 * analytic_fraction(5, u), .006);
      }
    }
  EXPECT_LT(compact.memory_budget().used(), reference.memory_budget().used());
}

TEST(DeepFrameProduction, native_colored_capture_uses_separate_optical_and_emission_channels)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 450, 0);
  const float tau[3] = {0.2f, 0.7f, 1.2f};
  const float rgb[3] = {0.2f, 0.4f, 0.1f};
  for (int c = 0; c < 3; c++) {
    packet.homogeneous[0].tau[c] = tau[c];
    packet.homogeneous[0].post_clamp_rgb[c] = rgb[c];
    packet.homogeneous[0].pre_clamp_rgb[c] = rgb[c];
  }
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto knots = read_knots(production.curves()[0]);
  for (int i = 0; i <= 200; i++) {
    const double u = double(i) / 200;
    const auto value = prefix(knots, 2 + 2 * u);
    EXPECT_NEAR(value[0],
                (std::exp(-tau[0] * u) + std::exp(-tau[1] * u) + std::exp(-tau[2] * u)) / 3,
                1e-4);
    for (int c = 0; c < 3; c++)
      EXPECT_NEAR(value[c + 1], rgb[c] * analytic_fraction(tau[c], u), 1e-4);
  }
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_TRUE(encoded.report()->quantized_checked);
  EXPECT_EQ(production.curves()[0].optical_sources()[0].support.tau[1], double(tau[1]));
}

TEST(DeepFrameProduction, raw_film_count_mismatch_is_a_hard_failure)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  ASSERT_TRUE(admit_packet(production, native_packet(production.memory_budget(), 311)));
  const std::array<uint32_t, 1> wrong_counts{2};
  EXPECT_FALSE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, wrong_counts, 1));
  EXPECT_EQ(production.error().code, DeepErrorCode::PopulationMismatch);
  EXPECT_EQ(production.pipeline().state(), DeepFrameState::Failed);
  EXPECT_FALSE(production.curves()[0]);
  EXPECT_FALSE(production.finish_inputs());
}

TEST(DeepFrameProduction, native_film_clamp_gain_is_applied_to_real_source_without_calibration)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 451, 0, 2, 100);
  auto &segment = packet.homogeneous[0];
  segment.clamp_gain = 0.13f;
  for (int c = 0; c < 3; c++)
    segment.post_clamp_rgb[c] = segment.pre_clamp_rgb[c] * segment.clamp_gain;
  const double red = double(segment.pre_clamp_rgb[0]) * segment.clamp_gain;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_NEAR(prefix(knots, 3)[1], red * analytic_fraction(2, 0.5), 1e-4);
  EXPECT_EQ(production.curves()[0].radiance_sources()[0].support.post_clamp_rgb[0], red);
}

TEST(DeepFrameProduction, inconsistent_or_nonfinite_native_clamp_fails_before_curve_publication)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int kind = 0; kind < 2; kind++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 452 + kind);
    if (kind == 0)
      packet.homogeneous[0].clamp_gain = 0.5f;
    else
      packet.homogeneous[0].pre_clamp_rgb[0] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
    EXPECT_EQ(production.error().code, DeepErrorCode::InvalidPayload);
    EXPECT_FALSE(production.curves()[0]);
  }
}

TEST(DeepFrameProduction, incomplete_unbound_and_foreign_quota_native_packets_are_rejected)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction unjoined(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  auto packet = native_packet(unjoined.memory_budget(), 321);
  packet.domain.main_shadow_mnee_joined = false;
  ASSERT_TRUE(begin_reference_capture(unjoined, packet.domain));
  EXPECT_FALSE(complete_reference_capture(unjoined, std::move(packet)));
  EXPECT_EQ(unjoined.pipeline().state(), DeepFrameState::Failed);
  DeepFrameProduction foreign(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  EXPECT_FALSE(complete_reference_capture(foreign, native_packet(foreign.memory_budget(), 322)));
  EXPECT_EQ(foreign.error().code, DeepErrorCode::InvalidReference);
  DeepFrameProduction wrong_budget(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  DeepMemoryBudget external(1 << 20);
  auto external_packet = native_packet(external, 323);
  ASSERT_TRUE(begin_reference_capture(wrong_budget, external_packet.domain));
  EXPECT_FALSE(complete_reference_capture(wrong_budget, std::move(external_packet)));
  EXPECT_EQ(wrong_budget.error().code, DeepErrorCode::InvalidReference);
}

TEST(DeepFrameProduction, cancelled_native_domain_is_not_a_completed_output)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  const auto packet = native_packet(production.memory_budget(), 324);
  ASSERT_TRUE(begin_reference_capture(production, packet.domain));
  production.cancel_native_capture(packet.domain.id);
  EXPECT_TRUE(production.capture_cancelled());
  EXPECT_EQ(production.error().code, DeepErrorCode::Cancelled);
  EXPECT_EQ(production.pipeline().state(), DeepFrameState::Cancelled);
  const std::array<uint32_t, 1> counts{1};
  EXPECT_FALSE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  EXPECT_FALSE(production.finish_inputs());
  EXPECT_FALSE(production.curves()[0]);
  EXPECT_EQ(production.error().code, DeepErrorCode::Cancelled);
}

TEST(DeepFrameProduction, native_cancellation_cannot_replace_an_earlier_failure)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  const auto packet = native_packet(production.memory_budget(), 325);
  ASSERT_TRUE(begin_reference_capture(production, packet.domain));
  production.abort_native_capture(packet.domain.id, "First native producer failure");
  production.cancel_native_capture(packet.domain.id);
  EXPECT_FALSE(production.capture_cancelled());
  EXPECT_EQ(production.error().code, DeepErrorCode::Incomplete);
  EXPECT_EQ(production.capture_error_message(), "First native producer failure");
}

TEST(DeepFrameProduction, unknown_or_completed_domains_cannot_be_cancelled)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const bool completed : {false, true}) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
    if (completed)
      ASSERT_TRUE(admit_packet(production, native_packet(production.memory_budget(), 326)));
    production.cancel_native_capture(326);
    EXPECT_FALSE(production.capture_cancelled());
    EXPECT_EQ(production.error().code, DeepErrorCode::InvalidReference);
  }
}

TEST(DeepFrameProduction, pending_domain_prevents_finality_and_multi_page_curve_is_owned)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction pending(homogeneous_frame(), {-96, -17, 1, 1}, 4 << 20, consumers);
  const auto pending_packet = native_packet(pending.memory_budget(), 331);
  ASSERT_TRUE(begin_reference_capture(pending, pending_packet.domain));
  const std::array<uint32_t, 1> counts{1};
  EXPECT_FALSE(pending.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  EXPECT_EQ(pending.error().code, DeepErrorCode::Incomplete);

  DeepMemoryBudget budget(16 << 20);
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, budget, consumers);
  auto packet = native_packet(budget, 332);
  packet.homogeneous.clear();
  for (uint32_t i = 0; i < 600; i++) {
    DeepNativeHomogeneousRecord segment = {};
    segment.camera_index = 0;
    segment.sample = packet.cameras[0].sample;
    segment.ordinal = i;
    segment.flags = DEEP_NATIVE_HOMOGENEOUS_OPTICAL |
                    DEEP_NATIVE_HOMOGENEOUS_CAMERA_SUPPORT_VERIFIED;
    segment.z = segment.tmin = float(i);
    segment.zback = segment.tmax = float(i + 1);
    segment.tau[0] = segment.tau[1] = segment.tau[2] = 1e-10f;
    packet.homogeneous.push_back(segment);
  }
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written = 600;
  packet.domain.emission_calls = 0;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  EXPECT_EQ(curve.optical_sources().size(), 600u);
  EXPECT_GT(curve.pages().size(), 1u);
  DeepCanonicalRowLease row;
  ASSERT_TRUE(production.pipeline().canonical_view(row));
  ASSERT_EQ(row.pixels().size(), 1u);
  ASSERT_TRUE(row.pixels()[0].additional_knots);
  EXPECT_FALSE(row.pixels()[0].additional_knots->pages().empty());
  for (const auto &page : row.pixels()[0].additional_knots->pages())
    EXPECT_TRUE(page.is_charged_to(budget));
  EXPECT_NEAR(prefix(read_knots(curve), 300.5)[0], std::exp(-300.5 * double(1e-10f)), 1e-12);
}

namespace {
DeepNativeCaptureBatch point_packet(const DeepMemoryBudget &budget,
                                    uint64_t domain,
                                    bool world,
                                    uint32_t call_count = 2)
{
  auto packet = native_packet(budget, domain);
  packet.homogeneous.clear();
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
      packet.domain.emission_calls = 0;
  packet.roots.resize(1);
  packet.cameras[0].root_handle = 1;
  auto &root = packet.roots[0];
  root.kind = world ? DEEP_NATIVE_ROOT_WORLD : DEEP_NATIVE_ROOT_SURFACE;
  root.z = world ? std::numeric_limits<float>::max() : 2.0f;
  root.physical_t = world ? -1.0f : 2.0f;
  root.ordinal = 1;
  root.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT |
               (world ? DEEP_NATIVE_ROOT_WORLD_TERMINAL : DEEP_NATIVE_ROOT_OPAQUE_SURFACE);
  if (!world)
    packet.cameras[0].flags &= ~DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
  for (uint32_t i = 0; i < call_count; i++) {
    DeepNativePointRecord point{};
    point.camera_index = 0;
    point.sample = packet.cameras[0].sample;
    point.ordinal = i;
    point.root_handle = 1;
    point.root_kind = root.kind;
    point.root_ordinal = root.ordinal;
    point.routing = DEEP_NATIVE_POINT_COMBINED;
    point.z = root.z;
    point.clamp_gain = 0.25f;
    const float raw[3] = {float(i + 1), -2, 4};
    for (int c = 0; c < 3; c++) {
      point.pre_clamp_rgb[c] = raw[c];
      point.clamp_rgb[c] = raw[c] * point.clamp_gain;
      point.routing_scale[c] = i == 0 ? 0.5f : 1.0f;
      point.final_rgb[c] = point.clamp_rgb[c] * point.routing_scale[c];
    }
    packet.points.push_back(point);
  }
  packet.domain.root_reserved = packet.domain.root_written = 1;
  packet.domain.point_reserved = packet.domain.point_written = call_count;
  return packet;
}
}  // namespace

TEST(DeepFrameProduction, native_light_source_keeps_its_depth_before_a_later_opaque_owner)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 631, false);
  auto surface = packet.roots[0];
  surface.ordinal = 2;
  packet.roots.push_back(surface);
  packet.roots[0].kind = DEEP_NATIVE_ROOT_LIGHT;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  packet.roots[0].z = packet.roots[0].physical_t = 1;
  packet.cameras[0].root_handle = 2;
  packet.points[0].root_kind = DEEP_NATIVE_ROOT_LIGHT;
  packet.points[0].z = 1;
  packet.points[1].root_handle = packet.points[1].root_ordinal = 2;
  packet.domain.root_reserved = packet.domain.root_written = 2;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  const auto knots = read_knots(curve);
  EXPECT_EQ(prefix(knots, 1.5)[0], 1);
  EXPECT_EQ(prefix(knots, 1.5)[1], 0.125);
  EXPECT_EQ(prefix(knots, 2.5)[0], 0);
  EXPECT_EQ(prefix(knots, 2.5)[1], 0.625);
  EXPECT_EQ(curve.visibility_sources().size(), 1u);
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(decode_encoded(encoded, 1.5)[0], 1);
  EXPECT_EQ(decode_encoded(encoded, 1.5)[1], 0.125);
}

TEST(DeepFrameProduction, inherited_point_preserves_distinct_hdr_signed_film_call_clamps)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 633, false);
  const float raw[3] = {8192, -4096, 2048};
  for (uint32_t i = 0; i < 2; i++) {
    auto &point = packet.points[i];
    point.clamp_gain = i == 0 ? 1.0f / 32768 : 1.0f / 16384;
    for (int c = 0; c < 3; c++) {
      point.pre_clamp_rgb[c] = i == 0 ? raw[c] : -raw[c];
      point.routing_scale[c] = 1;
      point.final_rgb[c] = point.clamp_rgb[c] = point.pre_clamp_rgb[c] * point.clamp_gain;
    }
  }
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  ASSERT_EQ(curve.radiance_sources().size(), 2u);
  EXPECT_NE(curve.radiance_sources()[0].support.emission_call,
            curve.radiance_sources()[1].support.emission_call);
  const auto knots = read_knots(curve);
  EXPECT_EQ(prefix(knots, 1.99)[1], 0);
  const std::array<double, 3> expected{-0.25, 0.125, -0.0625};
  for (int c = 0; c < 3; c++) {
    EXPECT_EQ(prefix(knots, 2)[c + 1], expected[c]);
    EXPECT_EQ(prefix(knots, 3)[c + 1], expected[c]);
  }
  /* The raw vectors cancel to zero, but independently clamped calls do not. */
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(production.encode_pixel(0, {}, encoded));
  for (int c = 0; c < 3; c++)
    EXPECT_NEAR(decode_encoded(encoded, 2)[c + 1], expected[c], 1e-6);
}

TEST(DeepFrameProduction, camera_emission_span_keeps_support_before_inherited_point)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 634, false, 1);
  auto camera = native_packet(production.memory_budget(), 634);
  auto span = camera.homogeneous[0];
  span.z = span.tmin = 0;
  span.zback = span.tmax = 2;
  span.tau[0] = span.tau[1] = span.tau[2] = 0;
  packet.homogeneous.push_back(span);
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written = 1;
  packet.domain.emission_calls = 1;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  const auto knots = read_knots(curve);
  EXPECT_EQ(prefix(knots, 1)[1], 2);
  EXPECT_EQ(prefix(knots, 2, true)[1], 4);
  EXPECT_EQ(prefix(knots, 2)[1], 4.125);
  ASSERT_EQ(curve.radiance_sources().size(), 2u);
  EXPECT_EQ(curve.radiance_sources()[0].support.z_back, 2);
}

TEST(DeepFrameProduction, native_light_only_camera_preserves_zero_alpha_radiance)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 632, false);
  packet.cameras[0].root_handle = 0;
  packet.cameras[0].flags |= DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
  packet.roots[0].kind = DEEP_NATIVE_ROOT_LIGHT;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  for (auto &point : packet.points)
    point.root_kind = DEEP_NATIVE_ROOT_LIGHT;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  const auto knots = read_knots(curve);
  EXPECT_EQ(prefix(knots, 1000)[0], 1);
  EXPECT_EQ(prefix(knots, 1000)[1], 0.625);
  EXPECT_TRUE(curve.visibility_sources().empty());
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(decode_encoded(encoded, 1000)[0], 1);
  EXPECT_EQ(decode_encoded(encoded, 1000)[1], 0.625);
}

TEST(DeepFrameProduction, native_root_arena_rejects_bad_domain_handles_and_reservations)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 5; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = point_packet(production.memory_budget(), 640 + mutation, false);
    if (mutation == 0)
      packet.cameras[0].root_handle = 2;
    if (mutation == 1)
      packet.roots[0].camera_index = 1;
    if (mutation == 2)
      packet.points[0].root_handle = 0;
    if (mutation == 3)
      packet.domain.root_reserved++;
    if (mutation == 4)
      packet.roots[0].ordinal++;
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
    ASSERT_EQ(production.curves().size(), 1u);
    EXPECT_FALSE(production.curves()[0]);
  }
}

namespace {
DeepNativeCaptureBatch distant_packet(const DeepMemoryBudget &budget, const uint64_t domain)
{
  auto packet = point_packet(budget, domain, true, 1);
  packet.cameras[0].root_handle = 0;
  packet.roots[0].kind = DEEP_NATIVE_ROOT_DISTANT;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  packet.points[0].root_kind = DEEP_NATIVE_ROOT_DISTANT;
  packet.domain.require_full_optical = true;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 1000, 0, 1000, {.5f, 1, 2}, DEEP_OWNED_OPTICAL_MEDIUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  return packet;
}
}  // namespace

TEST(DeepFrameProduction, distant_radiance_preserves_finite_medium_and_independent_world_opacity)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  const double infinity = double(std::numeric_limits<float>::max());
  const double transmission = (std::exp(-.5) + std::exp(-1.) + std::exp(-2.)) / 3;
  for (const bool opaque_world : {false, true}) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = distant_packet(production.memory_budget(), 645 + int(opaque_world));
    packet.domain.opaque_world = opaque_world;
    ASSERT_TRUE(admit_packet(production, std::move(packet)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    ASSERT_TRUE(production.finish_inputs());
    const auto &curve = production.curves()[0];
    const auto knots = read_knots(curve);
    EXPECT_NEAR(prefix(knots, 1000)[0], transmission, 1e-12);
    EXPECT_EQ(prefix(knots, 1000)[1], 0);
    EXPECT_NEAR(prefix(knots, infinity, true)[0], transmission, 1e-12);
    EXPECT_NEAR(prefix(knots, infinity)[0], opaque_world ? 0 : transmission, 1e-12);
    EXPECT_EQ(prefix(knots, infinity)[1], .125);
    EXPECT_EQ(curve.visibility_sources().size(), opaque_world ? 1u : 0u);
    ASSERT_EQ(curve.radiance_sources().size(), 1u);
    EXPECT_TRUE(curve.radiance_sources()[0].support.flags & DEEP_KNOT_INFINITY_RADIANCE);
    EXPECT_FALSE(curve.radiance_sources()[0].support.flags & DEEP_KNOT_WORLD_TERMINAL);
    DeepCanonicalEncoded encoded;
    const auto status = production.encode_pixel(0, {}, encoded);
    ASSERT_TRUE(status) << status.message;
    EXPECT_NEAR(decode_encoded(encoded, infinity)[1], .125, .008);
    EXPECT_NEAR(decode_encoded(encoded, infinity)[0], opaque_world ? 0 : transmission, .01);
  }
}

TEST(DeepFrameProduction, distant_radiance_cannot_claim_finite_or_opacity_ownership)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 6; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = distant_packet(production.memory_budget(), 648 + mutation);
    if (mutation == 0)
      packet.cameras[0].root_handle = 1;
    if (mutation == 1)
      packet.roots[0].z = packet.points[0].z = 999;
    if (mutation == 2)
      packet.roots[0].physical_t = 999;
    if (mutation == 3)
      packet.roots[0].flags |= DEEP_NATIVE_ROOT_WORLD_TERMINAL;
    if (mutation == 4)
      packet.cameras[0].flags &= ~DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
    if (mutation == 5)
      packet.domain.require_full_optical = false;
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
  }
}

TEST(DeepFrameProduction, lamp_split_medium_requires_independent_complete_optical_coverage)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 651);
  auto point = point_packet(production.memory_budget(), 651, false, 1);
  packet.roots = std::move(point.roots);
  packet.points = std::move(point.points);
  packet.roots[0].kind = DEEP_NATIVE_ROOT_LIGHT;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  packet.roots[0].z = packet.roots[0].physical_t = 3;
  packet.points[0].root_kind = DEEP_NATIVE_ROOT_LIGHT;
  packet.points[0].z = 3;
  packet.domain.root_reserved = packet.domain.root_written = 1;
  packet.domain.point_reserved = packet.domain.point_written = 1;
  EXPECT_FALSE(admit_packet(production, std::move(packet)));
  EXPECT_NE(production.error().message.find("complete independent Optical"), std::string::npos);
}

namespace {
DeepNativeCaptureBatch native_world_packet(const DeepMemoryBudget &budget,
                                           const uint64_t id,
                                           const float sigma,
                                           const uint32_t scheduled = 0)
{
  auto packet = native_packet(budget, id, scheduled);
  auto &camera = packet.cameras[0];
  camera.tmax = 3.9f;
  camera.origin[2] = camera.z_origin = .1f;
  auto &span = packet.homogeneous[0];
  span.z = .1f;
  span.zback = span.tmax = std::numeric_limits<float>::max();
  span.tmin = 0;
  span.flags |= DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT;
  for (int c = 0; c < 3; c++) {
    span.tau[c] = double(sigma) * double(span.tmax);
    span.pre_clamp_rgb[c] = span.post_clamp_rgb[c] = 1.6f / float(1 << c);
  }
  packet.domain.world_volume_shader = span.source_shader;
  packet.domain.world_volume_object = span.source_object;
  packet.domain.require_full_optical = true;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  const float optical_tau = sigma * camera.tmax;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back({0,
                                   0,
                                   0,
                                   camera.tmax,
                                   .1f,
                                   4,
                                   {optical_tau, optical_tau, optical_tau},
                                   DEEP_OWNED_OPTICAL_MEDIUM,
                                   {0, 0}});
  packet.optical_coverage.push_back(
      {0, camera.tmax, .1f, 4, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  return packet;
}
}  // namespace

TEST(DeepFrameProduction, native_world_beer_radiance_continues_after_finite_optical_far)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const float sigma : {.05f, 2.f}) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
    auto packet = native_world_packet(production.memory_budget(), 720, sigma);
    const double front = packet.homogeneous[0].z;
    const double optical_tau = packet.optical_pieces[0].tau[0];
    if (sigma == 2)
      EXPECT_GT(packet.homogeneous[0].tau[0], double(std::numeric_limits<float>::max()));
    ASSERT_TRUE(admit_packet(production, std::move(packet)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    ASSERT_TRUE(production.finish_inputs());
    const auto &curve = production.curves()[0];
    const auto knots = read_knots(curve);
    for (const double z : {.2, .5, 1., 2., 4., 8., 20., 100.}) {
      const auto value = prefix(knots, z);
      const double optical_fraction = std::clamp((z - front) / (4 - front), 0., 1.);
      EXPECT_NEAR(value[0], std::exp(-optical_tau * optical_fraction), 1e-4);
      for (int c = 0; c < 3; c++)
        EXPECT_NEAR(value[c + 1],
                    double(1.6f / float(1 << c)) * -std::expm1(-double(sigma) * (z - front)),
                    1e-4);
    }
    const double infinity = double(std::numeric_limits<float>::max());
    EXPECT_NEAR(prefix(knots, infinity)[0], std::exp(-optical_tau), 1e-12);
    EXPECT_NEAR(prefix(knots, infinity)[1], double(1.6f), 1e-12);
    DeepCanonicalEncoded encoded;
    const auto status = production.encode_pixel(0, {}, encoded);
    ASSERT_TRUE(status) << status.message;
    EXPECT_NEAR(decode_encoded(encoded, infinity)[0], std::exp(-optical_tau), .01);
    EXPECT_NEAR(decode_encoded(encoded, infinity)[1], double(1.6f), .01);
  }
}

namespace {
DeepNativeCaptureBatch native_world_expression_packet(const DeepMemoryBudget &budget,
                                                      const uint64_t id,
                                                      const uint32_t kind)
{
  auto packet = native_world_packet(budget, id, 2);
  packet.homogeneous.clear();
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
      packet.domain.emission_calls = 0;
  DeepNativeExpressionSource source{};
  source.ordinal = 1;
  source.object = packet.domain.world_volume_object;
  source.shader = packet.domain.world_volume_shader;
  source.prim = PRIM_NONE;
  source.flags = kind | DEEP_NATIVE_EXPR_WORLD_SUPPORT;
  source.tmin = kind == DEEP_NATIVE_EXPR_CAMERA_SPAN ? 0 : 8;
  source.tmax = kind == DEEP_NATIVE_EXPR_CAMERA_SPAN ? std::numeric_limits<float>::max() : 8;
  const auto &camera = packet.cameras[0];
  source.z = float(double(camera.z_origin) + double(camera.z_direction) * source.tmin);
  source.zback = float(double(camera.z_origin) + double(camera.z_direction) * source.tmax);
  for (int c = 0; c < 3; c++) {
    source.raw_rgb[c] = .04f / float(1 << c);
    source.tau[c] = kind == DEEP_NATIVE_EXPR_CAMERA_SPAN ? 2 * double(source.tmax) : 0;
  }
  if (kind == DEEP_NATIVE_EXPR_INHERITED_POINT) {
    source.flags = kind;
    source.root_handle = packet.cameras[0].root_handle = 1;
    DeepNativeRootRecord root{};
    root.ordinal = 1;
    root.kind = DEEP_NATIVE_ROOT_VOLUME_SCATTER;
    root.z = source.z;
    root.physical_t = source.tmin;
    root.object = source.object;
    root.prim = PRIM_NONE;
    root.shader = source.shader;
    root.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY |
                 DEEP_NATIVE_ROOT_WORLD_SUPPORT;
    packet.roots.push_back(root);
    packet.domain.root_reserved = packet.domain.root_written = 1;
  }
  packet.expression_sources.push_back(source);
  packet.expression_nodes.push_back({0, 1, DEEP_NATIVE_EXPR_TERM, 0, 0, 1, 0, {1, 1, 1}, {0, 0}});
  DeepNativeExpressionCall call{};
  call.ordinal = call.head = 1;
  call.routing = DEEP_NATIVE_POINT_COMBINED;
  call.pixel = camera.pixel;
  call.sample = camera.sample;
  call.clamp_gain = 1;
  for (int c = 0; c < 3; c++)
    call.pre_clamp_rgb[c] = call.post_clamp_rgb[c] = source.raw_rgb[c];
  packet.expression_calls.push_back(call);
  packet.domain.expression_source_reserved = packet.domain.expression_source_written = 1;
  packet.domain.expression_node_reserved = packet.domain.expression_node_written = 1;
  packet.domain.expression_call_reserved = packet.domain.expression_call_written = 1;
  return packet;
}
}  // namespace

TEST(DeepFrameProduction, native_world_expression_keeps_real_span_null_and_inherited_support)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const uint32_t kind : {DEEP_NATIVE_EXPR_CAMERA_SPAN,
                              DEEP_NATIVE_EXPR_CAMERA_POINT,
                              DEEP_NATIVE_EXPR_INHERITED_POINT})
  {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
    auto packet = native_world_expression_packet(production.memory_budget(), 750 + kind, kind);
    ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.capture_error_message();
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
        << production.capture_error_message();
    const auto knots = read_knots(production.curves()[0]);
    for (const double z : {.2, .5, 1., 4., 7., 9., 20., 100.}) {
      const auto value = prefix(knots, z);
      EXPECT_NEAR(value[0], std::exp(-2 * std::min(z - double(.1f), double(3.9f))), 1e-4);
      for (int c = 0; c < 3; c++) {
        const double fraction = kind == DEEP_NATIVE_EXPR_CAMERA_SPAN ?
                                    -std::expm1(-2 * (z - double(.1f))) :
                                    (z > 8.1 ? 1 : 0);
        EXPECT_NEAR(value[c + 1], double(.04f / float(1 << c)) * fraction, 1e-4);
      }
    }
    DeepCanonicalEncoded encoded;
    ASSERT_TRUE(production.encode_pixel(0, {}, encoded));
    EXPECT_NEAR(decode_encoded(encoded, 100)[1], double(.04f), .01);
  }
}

TEST(DeepFrameProduction, native_world_expression_rejects_unbound_or_changed_physical_support)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 7; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_world_expression_packet(
        production.memory_budget(), 770 + mutation, DEEP_NATIVE_EXPR_CAMERA_SPAN);
    auto &source = packet.expression_sources[0];
    if (mutation == 0)
      source.flags &= ~DEEP_NATIVE_EXPR_WORLD_SUPPORT;
    if (mutation == 1)
      source.shader++;
    if (mutation == 2)
      source.object++;
    if (mutation == 3)
      packet.domain.require_full_optical = false;
    if (mutation == 4)
      source.z += 1;
    if (mutation == 5)
      source.flags |= 1u << 20;
    if (mutation == 6)
      source.prim = 0;
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
  }
}

TEST(DeepFrameProduction, dense_world_four_ray_production_consumers)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepNativeCurvePolicy policy;
  ASSERT_TRUE(deep_native_curve_policy_for_budget(.01, .006, policy));
  DeepFrameProduction production(
      homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, ids, false, policy);
  const auto rgb = deep_production_encode_config(.1, .1, DeepCanonicalPrecision::Half, false);
  const auto alpha = deep_production_encode_config(.01, .01, DeepCanonicalPrecision::Half, true);
  const std::array<DeepFrozenConsumer, 2> consumers{
      {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 5, rgb}, {2, 12345, 1, 5, alpha}}};
  ASSERT_TRUE(production.configure_consumers(consumers));
  for (uint32_t sample = 0; sample < 4; sample++) {
    auto packet = native_world_packet(production.memory_budget(), 730 + sample, 2, sample);
    const float front = .09999990463256836f;
    packet.cameras[0].z_origin = packet.cameras[0].origin[2] = front;
    packet.homogeneous[0].z = packet.optical_pieces[0].z = packet.optical_coverage[0].z_front =
        front;
    for (int c = 0; c < 3; c++)
      packet.homogeneous[0].pre_clamp_rgb[c] = packet.homogeneous[0].post_clamp_rgb[c] =
          .04f / float(1 << c);
    ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.capture_error_message();
  }
  const std::array<uint32_t, 1> counts{4};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
      << production.capture_error_message();
  ASSERT_TRUE(production.finish_inputs()) << production.capture_error_message();
  DeepConsumerToken token;
  ASSERT_TRUE(production.consumer_token(DEEP_DIRECT_CONSUMER_KEY, rgb, 5, token));
  DeepEncodedPixelLease pixel;
  ASSERT_TRUE(production.encoded_pixel(token, 0, pixel));
  EXPECT_FALSE(pixel.samples().empty());
}

TEST(DeepFrameProduction, native_world_volume_points_keep_actual_depth_beyond_optical_clip)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const uint32_t kind : {DEEP_NATIVE_ROOT_VOLUME_DIRECT, DEEP_NATIVE_ROOT_VOLUME_SCATTER}) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = point_packet(production.memory_budget(), 721, false, 1);
    auto &root = packet.roots[0];
    root.kind = kind;
    root.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY |
                 DEEP_NATIVE_ROOT_WORLD_SUPPORT;
    root.z = root.physical_t = 8;
    root.object = 2;
    root.prim = PRIM_NONE;
    root.shader = 3;
    packet.points[0].root_kind = kind;
    packet.points[0].z = 8;
    packet.cameras[0].root_handle = kind == DEEP_NATIVE_ROOT_VOLUME_SCATTER ? 1 : 0;
    packet.cameras[0].tmax = 4;
    packet.cameras[0].flags |= DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
    packet.domain.world_volume_shader = 3;
    packet.domain.world_volume_object = 2;
    packet.domain.require_full_optical = true;
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 4, 0, 4, {.2f, .2f, .2f}, DEEP_OWNED_OPTICAL_MEDIUM, {0, 0}});
    packet.optical_coverage.push_back(
        {0, 4, 0, 4, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
    ASSERT_TRUE(admit_packet(production, std::move(packet)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    const auto knots = read_knots(production.curves()[0]);
    EXPECT_EQ(prefix(knots, 7)[1], 0);
    EXPECT_EQ(prefix(knots, 9)[1], .125);
    EXPECT_NEAR(prefix(knots, 100)[0], std::exp(-double(.2f)), 1e-12);
  }
}

TEST(DeepFrameProduction, native_world_support_requires_bound_shader_object_and_full_optical)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 5; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_world_packet(production.memory_budget(), 723 + mutation, .05f);
    if (mutation == 0)
      packet.domain.world_volume_shader = SHADER_NONE;
    if (mutation == 1)
      packet.domain.world_volume_object++;
    if (mutation == 2)
      packet.homogeneous[0].source_shader++;
    if (mutation == 3)
      packet.domain.require_full_optical = false;
    if (mutation == 4)
      packet.homogeneous[0].flags &= ~DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT;
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
    const auto first_error = production.capture_error_message();
    EXPECT_FALSE(first_error.empty());
    production.abort_native_capture(723 + mutation,
                                    "Later cancellation must preserve first cause");
    EXPECT_EQ(production.capture_error_message(), first_error);
  }
}

TEST(DeepFrameProduction, owned_optical_proof_is_bound_to_domain_and_actual_camera_counts)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 5; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 660 + mutation);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 1, 0});
    packet.optical_events.push_back({0, 0, 2, 4, {2, 2, 2}, 0});
    if (mutation == 1)
      packet.optical_replay_domain++;
    if (mutation == 2)
      packet.optical_ledger[0].events++;
    if (mutation == 3)
      packet.optical_events[0].segment++;
    if (mutation == 4)
      packet.optical_events[0].tau[0] = -1;
    EXPECT_EQ(admit_packet(production, std::move(packet)), mutation == 0);
  }
}

TEST(DeepFrameProduction, owned_complete_optical_rejects_gaps_and_unbacked_terminals)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 5; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 670 + mutation);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 3, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 2, 0, 2, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 0, 2, 4, 2, 4, {2, 2, 2}, DEEP_OWNED_OPTICAL_MEDIUM, {1, 0}});
    packet.optical_pieces.push_back(
        {0, 1, 4, 1000, 4, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_coverage.push_back(
        {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 3, 0, 0});
    if (mutation == 1)
      packet.optical_pieces[1].tmin = 2.01f;
    if (mutation == 2)
      packet.optical_pieces[0].tau[0] = 1;
    if (mutation == 3)
      packet.optical_coverage[0].flags = DEEP_OWNED_OPTICAL_COMPLETE |
                                         DEEP_OWNED_OPTICAL_VISIBILITY_END;
    if (mutation == 4)
      packet.optical_coverage[0].cursor_t = 999;
    EXPECT_EQ(admit_packet(production, std::move(packet)), mutation == 0);
  }
}

TEST(DeepFrameProduction, canonical_consumes_owned_tau_and_preserves_independent_native_radiance)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 681);
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 3, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 2, 0, 2, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_pieces.push_back(
      {0, 0, 2, 4, 2, 4, {1, 1, 1}, DEEP_OWNED_OPTICAL_MEDIUM, {1, 0}});
  packet.optical_pieces.push_back(
      {0, 1, 4, 1000, 4, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 3, 0, 0});
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  const auto &curve = production.curves()[0];
  EXPECT_EQ(curve.optical_sources().size(), 3u);
  const auto knots = read_knots(curve);
  EXPECT_NEAR(prefix(knots, 3)[0], std::exp(-0.5), 1e-4);
  EXPECT_NEAR(prefix(knots, 3)[1], 4 * analytic_fraction(2, 0.5), 1e-4);
  EXPECT_NEAR(prefix(knots, 1000)[0], std::exp(-1), 1e-4);
  EXPECT_NEAR(prefix(knots, 1000)[1], 4, 1e-4);
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
}

TEST(DeepFrameProduction, zero_tau_projected_depth_identity_keeps_complete_physical_partition)
{
  /* The actual original-scene first-fault interval: positive physical extent,
   * exactly zero captured extinction, equal FLOAT camera-depth endpoints. */
  constexpr float a = 8.48691368f, b = 8.48691463f, z = 8.26139832f;
  ASSERT_GT(b, a);
  constexpr float slope = 0.25f, origin = 6.139669895172119f;
  const auto depth = [](const float t) { return std::fma(slope, t, origin); };
  ASSERT_EQ(depth(a), z);
  ASSERT_EQ(depth(b), z);
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const uint32_t flags :
       {uint32_t(DEEP_OWNED_OPTICAL_VACUUM), uint32_t(DEEP_OWNED_OPTICAL_MEDIUM)})
  {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 685 + flags);
    packet.cameras[0].z_origin = origin;
    packet.cameras[0].z_direction = slope;
    packet.homogeneous[0].z = depth(2);
    packet.homogeneous[0].zback = depth(4);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 5, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 2, depth(0), depth(2), {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 0, 2, 4, depth(2), depth(4), {1, 1, 1}, DEEP_OWNED_OPTICAL_MEDIUM, {1, 0}});
    packet.optical_pieces.push_back(
        {0, 0, 4, a, depth(4), z, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back({0, 0, a, b, z, z, {0, 0, 0}, flags, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 1, b, 1000, z, depth(1000), {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_coverage.push_back({0,
                                       1000,
                                       depth(0),
                                       depth(1000),
                                       DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR,
                                       5,
                                       0,
                                       0});
    std::string error;
    ASSERT_TRUE(deep_native_capture_validate(packet, error)) << error;
    EXPECT_EQ(packet.optical_pieces.size(), 5u); /* Validation preserves raw identities. */
    EXPECT_EQ(packet.optical_coverage[0].pieces, 5u);
    ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.error().message;
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
        << production.error().message;
    const auto &curve = production.curves()[0];
    EXPECT_EQ(curve.optical_sources().size(), 4u);
    const auto knots = read_knots(curve);
    EXPECT_NEAR(prefix(knots, depth(3))[0], std::exp(-0.5), 1e-4);
    EXPECT_NEAR(prefix(knots, depth(3))[1], 4 * analytic_fraction(2, 0.5), 1e-4);
    for (const double depth : {double(std::nextafter(z, 0.0f)),
                               double(z),
                               double(std::nextafter(z, 1000.0f)),
                               double(depth(1000))})
    {
      const auto value = prefix(knots, depth);
      EXPECT_NEAR(value[0], std::exp(-1), 1e-4);
      EXPECT_NEAR(value[1], 4, 1e-4);
      EXPECT_NEAR(value[2], -3, 1e-4);
      EXPECT_NEAR(value[3], 1, 1e-4);
    }
    DeepCanonicalEncoded encoded;
    const auto status = production.encode_pixel(0, {}, encoded);
    ASSERT_TRUE(status) << status.message;
  }
}

TEST(DeepFrameProduction, nonzero_collapsed_optical_span_is_certified_without_losing_extinction)
{
  constexpr float a = 9.56226921f, b = 9.56227016f, z = 9.40139771f;
  constexpr float slope = 0.25f;
  const float origin = z - slope * a;
  const auto depth = [&](const float t) { return std::fma(slope, t, origin); };
  ASSERT_GT(b, a);
  ASSERT_EQ(depth(a), z);
  ASSERT_EQ(depth(b), z);
  constexpr float tau = 2.75083471e-6f;
  const std::array<uint32_t, 2> consumers{1, 2};
  for (const bool affordable : {true, false}) {
    DeepNativeCurvePolicy policy;
    policy.transmission_error = affordable ? 1e-4 : 1e-7;
    DeepFrameProduction production(
        homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers, true, policy);
    auto packet = native_packet(production.memory_budget(), 710 + affordable);
    packet.cameras[0].z_origin = origin;
    packet.cameras[0].z_direction = slope;
    packet.homogeneous[0].z = depth(2);
    packet.homogeneous[0].zback = depth(4);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 5, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 2, depth(0), depth(2), {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 0, 2, 4, depth(2), depth(4), {1, 1, 1}, DEEP_OWNED_OPTICAL_MEDIUM, {1, 0}});
    packet.optical_pieces.push_back(
        {0, 0, 4, a, depth(4), z, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 0, a, b, z, z, {tau, 2 * tau, 3 * tau}, DEEP_OWNED_OPTICAL_MEDIUM, {1, 0}});
    packet.optical_pieces.push_back(
        {0, 1, b, 1000, z, depth(1000), {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_coverage.push_back({0,
                                       1000,
                                       depth(0),
                                       depth(1000),
                                       DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR,
                                       5,
                                       0,
                                       0});
    std::string error;
    ASSERT_TRUE(deep_native_capture_validate(packet, error)) << error;
    EXPECT_EQ(packet.optical_pieces.size(), 5u);
    EXPECT_EQ(packet.optical_coverage[0].pieces, 5u);
    ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.error().message;
    const std::array<uint32_t, 1> counts{1};
    const bool closed = production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1);
    if (!affordable) {
      EXPECT_FALSE(closed);
      for (const auto &curve : production.curves())
        EXPECT_FALSE(curve);
      continue;
    }
    ASSERT_TRUE(closed) << production.error().message;
    const auto &curve = production.curves()[0];
    EXPECT_EQ(curve.optical_sources().size(), 4u);
    EXPECT_GE(curve.representation_error()[0], 3 * double(tau));
    const auto knots = read_knots(curve);
    std::vector<double> queries;
    for (int i = 0; i <= 2000; ++i)
      queries.push_back(1000. * i / 2000);
    for (int i = 0; i <= 16; ++i)
      queries.push_back(double(a) + (double(b) - a) * i / 16);
    for (const double t : queries) {
      double reference = 0;
      for (int c = 1; c <= 3; ++c)
        reference += std::exp(-std::clamp((t - 2) / 2, 0., 1.) -
                              c * double(tau) * std::clamp((t - a) / (b - a), 0., 1.)) /
                     3;
      const auto value = prefix(knots, std::fma(double(slope), t, double(origin)));
      EXPECT_LE(std::abs(value[0] - reference), curve.representation_error()[0]);
    }
    const double terminal = (std::exp(-1 - double(tau)) + std::exp(-1 - 2 * double(tau)) +
                             std::exp(-1 - 3 * double(tau))) /
                            3;
    EXPECT_NEAR(prefix(knots, depth(1000))[0], terminal, 1e-12);
    EXPECT_NEAR(prefix(knots, depth(1000))[1], 4, 1e-12);
    EXPECT_NEAR(prefix(knots, depth(1000))[2], -3, 1e-12);
    DeepCanonicalEncoded encoded;
    const auto status = production.encode_pixel(0, {}, encoded);
    ASSERT_TRUE(status) << status.message;
  }
}

TEST(DeepFrameProduction, optical_projection_groups_cover_leading_trailing_and_holdout_boundaries)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  constexpr float slope = .25f, tau = 2e-6f;
  for (int pattern = 0; pattern < 4; pattern++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 720 + pattern);
    const float origin = pattern == 0 ? 8.f : 7.f;
    const auto depth = [&](const float t) { return std::fma(slope, t, origin); };
    const float a = pattern == 0 ? 0.f : 8.f;
    const float b = pattern == 0 ? 1e-7f : std::nextafter(a, 1000.f);
    const float c = pattern == 0 ? 2e-7f : std::nextafter(b, 1000.f);
    const float end = pattern == 1 ? c : 1000.f;
    ASSERT_EQ(depth(a), depth(b));
    ASSERT_EQ(depth(a), depth(c));
    packet.cameras[0].z_origin = origin;
    packet.cameras[0].z_direction = slope;
    packet.cameras[0].tmax = end;
    packet.homogeneous[0].z = depth(2);
    packet.homogeneous[0].zback = depth(4);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    const auto piece = [&](const float t0, const float t1, const float extinction) {
      packet.optical_pieces.push_back(
          {0,
           0,
           t0,
           t1,
           depth(t0),
           depth(t1),
           {extinction, extinction, extinction},
           extinction == 0 ? DEEP_OWNED_OPTICAL_VACUUM : DEEP_OWNED_OPTICAL_MEDIUM,
           {0, 0}});
    };
    if (pattern != 0) {
      piece(0, 2, 0);
      piece(2, 4, 1);
      piece(4, a, 0);
    }
    piece(a, b, tau);
    piece(b, c, tau);
    if (pattern == 0) {
      piece(c, 2, 0);
      piece(2, 4, 1);
      piece(4, end, 0);
    }
    else if (pattern != 1)
      piece(c, end, 0);
    if (pattern == 3) {
      packet.optical_visibility.push_back(
          {0, 0, depth(a), b, {.5f, .5f, .5f}, {.25f, .25f, .25f}, 1, 2, 3, 0, 0, 0});
    }
    const uint32_t pieces = uint32_t(packet.optical_pieces.size());
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, pieces, 0});
    packet.optical_coverage.push_back({0,
                                       end,
                                       depth(0),
                                       depth(end),
                                       DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR,
                                       pieces,
                                       uint32_t(packet.optical_visibility.size()),
                                       0});
    ASSERT_TRUE(admit_packet(production, std::move(packet)))
        << pattern << production.error().message;
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
        << pattern << production.error().message;
    const auto &curve = production.curves()[0];
    EXPECT_GE(curve.representation_error()[0], 2 * double(tau));
    const auto knots = read_knots(curve);
    for (int i = 0; i <= 2000; i++) {
      const double t = double(end) * i / 2000;
      const double base = std::exp(-std::clamp((t - 2) / 2, 0., 1.));
      const double moved = double(tau) * (std::clamp((t - a) / (b - a), 0., 1.) +
                                          std::clamp((t - b) / (c - b), 0., 1.));
      const double reference = pattern == 3 && t >= a ?
                                   base * (.25 * std::exp(-double(tau)) + .5 * std::exp(-moved)) :
                                   base * std::exp(-moved);
      const auto value = prefix(knots, std::fma(double(slope), t, double(origin)));
      EXPECT_LE(std::abs(value[0] - reference), curve.representation_error()[0]) << pattern << t;
    }
    if (pattern != 3)
      EXPECT_NEAR(prefix(knots, depth(end))[0], std::exp(-1 - 2 * double(tau)), 1e-12);
    EXPECT_NEAR(prefix(knots, depth(end))[1], 4, 1e-12);
    EXPECT_NEAR(prefix(knots, depth(end))[2], -3, 1e-12);
    for (const auto precision : {DeepCanonicalPrecision::Half, DeepCanonicalPrecision::Float}) {
      DeepCanonicalEncodeConfig config;
      config.precision = precision;
      DeepCanonicalEncoded encoded;
      const auto status = production.encode_pixel(0, config, encoded);
      ASSERT_TRUE(status) << pattern << status.message;
    }
  }
}

TEST(DeepFrameProduction, all_collapsed_nonzero_optical_coverage_never_becomes_a_surface_point)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 725);
  packet.homogeneous.clear();
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
      packet.domain.emission_calls = 0;
  packet.cameras[0].z_origin = 8;
  packet.cameras[0].z_direction = .25f;
  packet.cameras[0].tmax = 2e-7f;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 2e-7f, 8, 8, {1e-6f, 1e-6f, 1e-6f}, DEEP_OWNED_OPTICAL_MEDIUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 2e-7f, 8, 8, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.error().message;
  const std::array<uint32_t, 1> counts{1};
  EXPECT_FALSE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  EXPECT_NE(production.error().message.find("DepthNotRepresentable"), std::string::npos);
  for (const auto &curve : production.curves())
    EXPECT_FALSE(curve);
}

TEST(DeepFrameProduction, zero_depth_rule_cannot_hide_invalid_extinction_or_physical_coverage)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 6; ++mutation) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 695 + mutation);
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 3, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 8, 0, 8, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back({0,
                                     0,
                                     8,
                                     std::nextafter(8.0f, 1000.0f),
                                     8,
                                     8,
                                     {0, 0, 0},
                                     DEEP_OWNED_OPTICAL_MEDIUM,
                                     {0, 0}});
    packet.optical_pieces.push_back({0,
                                     1,
                                     packet.optical_pieces[1].tmax,
                                     1000,
                                     8,
                                     1000,
                                     {0, 0, 0},
                                     DEEP_OWNED_OPTICAL_VACUUM,
                                     {0, 0}});
    packet.optical_coverage.push_back(
        {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 3, 0, 0});
    if (mutation == 0)
      packet.optical_pieces[1].tau[0] = -1e-30f;
    if (mutation == 1)
      packet.optical_pieces[1].zback = std::nextafter(8.0f, 0.0f);
    if (mutation == 2)
      packet.optical_pieces[1].z = std::numeric_limits<float>::quiet_NaN();
    if (mutation == 3)
      packet.optical_pieces[1].tmin = packet.optical_pieces[1].tmax;
    if (mutation == 4)
      packet.optical_pieces[1].tmax = std::nextafter(packet.optical_pieces[1].tmax, 1000.0f);
    if (mutation == 5) {
      packet.optical_pieces[1].z = packet.optical_pieces[1].zback = 7;
      packet.optical_pieces[1].tau[0] = 1e-6f;
    }
    std::string error;
    EXPECT_FALSE(deep_native_capture_validate(packet, error)) << mutation;
  }
}

TEST(DeepFrameProduction, projected_identity_rejects_nonzero_tau_bits_under_flush_to_zero)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 705);
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 1, 3, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 8, 0, 8, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_pieces.push_back({0,
                                   0,
                                   8,
                                   std::nextafter(8.0f, 1000.0f),
                                   8,
                                   8,
                                   {std::bit_cast<float>(uint32_t(1)), 0, 0},
                                   DEEP_OWNED_OPTICAL_MEDIUM,
                                   {0, 0}});
  packet.optical_pieces.push_back({0,
                                   1,
                                   packet.optical_pieces[1].tmax,
                                   1000,
                                   8,
                                   1000,
                                   {0, 0, 0},
                                   DEEP_OWNED_OPTICAL_VACUUM,
                                   {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 3, 0, 0});
#if defined(__SSE2__) || defined(_M_X64)
  struct RestoreFloatingPointMode {
    unsigned previous = _mm_getcsr();
    ~RestoreFloatingPointMode()
    {
      _mm_setcsr(previous);
    }
  } restore;
  _mm_setcsr(restore.previous | 0x8040u); /* Scoped FTZ and DAZ, like GPU fast math. */
#endif
  EXPECT_EQ(std::bit_cast<uint32_t>(packet.optical_pieces[1].tau[0]), 1u);
  std::string error;
  EXPECT_FALSE(deep_native_capture_validate(packet, error));
  EXPECT_EQ(std::bit_cast<uint32_t>(packet.optical_pieces[1].tau[0]), 1u);
}

TEST(DeepFrameProduction, late_volume_direct_call_keeps_its_root_before_main_scatter_owner)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 690, false);
  auto scatter = packet.roots[0];
  scatter.kind = DEEP_NATIVE_ROOT_VOLUME_SCATTER;
  scatter.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  scatter.ordinal = 2;
  packet.roots.push_back(scatter);
  packet.roots[0].kind = DEEP_NATIVE_ROOT_VOLUME_DIRECT;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  packet.roots[0].z = packet.roots[0].physical_t = 1;
  packet.cameras[0].root_handle = 2;
  packet.points[0].root_kind = DEEP_NATIVE_ROOT_VOLUME_DIRECT;
  packet.points[0].z = 1;
  packet.points[1].root_kind = DEEP_NATIVE_ROOT_VOLUME_SCATTER;
  packet.points[1].root_handle = packet.points[1].root_ordinal = 2;
  packet.domain.root_reserved = packet.domain.root_written = 2;
  packet.domain.require_full_optical = true;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 1000, 0, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_EQ(prefix(knots, 1.5)[0], 1);
  EXPECT_EQ(prefix(knots, 1.5)[1], 0.125);
  EXPECT_EQ(prefix(knots, 2.5)[0], 1);
  EXPECT_EQ(prefix(knots, 2.5)[1], 0.625);
  EXPECT_TRUE(production.curves()[0].visibility_sources().empty());
}

TEST(DeepFrameProduction, native_emission_graph_keeps_step_support_and_one_actual_film_clamp)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int mutation = 0; mutation < 6; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 695 + mutation);
    packet.homogeneous.clear();
    packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
        packet.domain.emission_calls = 0;
    packet.domain.require_full_optical = true;
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 1000, 0, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_coverage.push_back(
        {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
    packet.expression_sources.push_back({0,
                                         1,
                                         DEEP_NATIVE_EXPR_CAMERA_SPAN,
                                         0,
                                         1,
                                         2,
                                         1,
                                         2,
                                         {.5f, .25f, -.125f},
                                         {0, 0, 0},
                                         1,
                                         2,
                                         3,
                                         0,
                                         {0, 0}});
    packet.expression_sources.push_back({0,
                                         2,
                                         DEEP_NATIVE_EXPR_CAMERA_SPAN,
                                         0,
                                         2,
                                         3,
                                         2,
                                         3,
                                         {.25f, .5f, -.125f},
                                         {2, 2, 2},
                                         1,
                                         2,
                                         3,
                                         0,
                                         {0, 0}});
    packet.expression_nodes.push_back(
        {0, 1, DEEP_NATIVE_EXPR_TERM, 0, 0, 1, 0, {1, 1, 1}, {0, 0}});
    packet.expression_nodes.push_back({0, 2, DEEP_NATIVE_EXPR_ADD, 0, 1, 0, 0, {1, 1, 1}, {0, 0}});
    packet.expression_nodes.push_back(
        {0, 3, DEEP_NATIVE_EXPR_TERM, 0, 0, 2, 0, {1, 1, 1}, {0, 0}});
    packet.expression_nodes.push_back({0, 4, DEEP_NATIVE_EXPR_ADD, 2, 3, 0, 0, {1, 1, 1}, {0, 0}});
    DeepNativeExpressionCall call{};
    call.ordinal = 1;
    call.head = 4;
    call.routing = DEEP_NATIVE_POINT_COMBINED;
    call.pixel = packet.cameras[0].pixel;
    call.sample = packet.cameras[0].sample;
    call.pre_clamp_rgb[0] = call.pre_clamp_rgb[1] = .75f;
    call.pre_clamp_rgb[2] = -.25f;
    call.post_clamp_rgb[0] = call.post_clamp_rgb[1] = .1875f;
    call.post_clamp_rgb[2] = -.0625f;
    call.clamp_gain = .25f;
    packet.expression_calls.push_back(call);
    packet.domain.expression_source_reserved = packet.domain.expression_source_written = 2;
    packet.domain.expression_node_reserved = packet.domain.expression_node_written = 4;
    packet.domain.expression_call_reserved = packet.domain.expression_call_written = 1;
    if (mutation == 1)
      packet.expression_nodes[3].a = 4;
    if (mutation == 2)
      packet.expression_nodes[1].b = 5;
    if (mutation == 3) {
      packet.expression_calls[0].pre_clamp_rgb[0] = 1;
      packet.expression_calls[0].post_clamp_rgb[0] = .25f;
    }
    if (mutation == 4)
      packet.expression_sources[0].flags = 0;
    if (mutation == 5)
      packet.domain.expression_node_written = 3;
    const bool admitted = admit_packet(production, std::move(packet));
    EXPECT_EQ(admitted, mutation == 0 || mutation == 3);
    if (!admitted)
      continue;
    const std::array<uint32_t, 1> counts{1};
    const bool closed = production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1);
    EXPECT_EQ(closed, mutation == 0);
    if (!closed)
      continue;
    const auto knots = read_knots(production.curves()[0]);
    EXPECT_EQ(prefix(knots, 1.5)[0], 1);
    EXPECT_EQ(prefix(knots, 1.5)[1], .0625);
    EXPECT_NEAR(prefix(knots, 2.5)[1], .125 + .0625 * analytic_fraction(2, .5), 1e-4);
    EXPECT_EQ(prefix(knots, 3.5)[1], .1875);
    EXPECT_EQ(prefix(knots, 3.5)[3], -.0625);
  }
}

TEST(DeepFrameProduction, surface_candidate_call_survives_transparent_continuation_and_holdout)
{
  for (bool pending_mnee : {false, true}) {
    const std::array<uint32_t, 2> consumers{1, 2};
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = point_packet(production.memory_budget(), 692, false);
    auto later = packet.roots[0];
    later.ordinal = 2;
    packet.roots.push_back(later);
    packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
    if (pending_mnee)
      packet.roots[0].flags |= DEEP_NATIVE_ROOT_MNEE_ADMISSION;
    packet.roots[0].z = packet.roots[0].physical_t = 1;
    packet.cameras[0].root_handle = 2;
    packet.points[0].z = 1;
    packet.points[1].root_handle = packet.points[1].root_ordinal = 2;
    packet.domain.root_reserved = packet.domain.root_written = 2;
    packet.domain.require_full_optical = true;
    packet.optical_replay_domain = packet.domain.id;
    packet.optical_replay_joined = packet.optical_full_coverage = true;
    packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 2, 0});
    packet.optical_pieces.push_back(
        {0, 0, 0, 1, 0, 1, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_pieces.push_back(
        {0, 1, 1, 2, 1, 2, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
    packet.optical_visibility.push_back(
        {0, 0, 1, 1, {.5f, .5f, .5f}, {.25f, .25f, .25f}, 1, 2, 3, 0, 0, 0});
    packet.optical_visibility.push_back({0, 1, 2, 2, {0, 0, 0}, {0, 0, 0}, 1, 2, 3, 0, 0, 0});
    packet.optical_coverage.push_back(
        {0, 2, 0, 2, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_VISIBILITY_END, 2, 2, 0});
    ASSERT_TRUE(admit_packet(production, std::move(packet)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    const auto knots = read_knots(production.curves()[0]);
    EXPECT_EQ(prefix(knots, 1.5)[0], .75);
    EXPECT_EQ(prefix(knots, 1.5)[1], .125);
    EXPECT_EQ(prefix(knots, 2.5)[0], .25);
    EXPECT_EQ(prefix(knots, 2.5)[1], .625);
    EXPECT_EQ(production.curves()[0].visibility_sources().size(), 2u);
  }
}

TEST(DeepFrameProduction, opaque_world_terminal_survives_selected_volume_scatter_owner)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = point_packet(production.memory_budget(), 691, false);
  packet.roots[0].kind = DEEP_NATIVE_ROOT_VOLUME_SCATTER;
  packet.roots[0].flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  for (auto &point : packet.points)
    point.root_kind = DEEP_NATIVE_ROOT_VOLUME_SCATTER;
  packet.domain.require_full_optical = packet.domain.opaque_world = true;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 1000, 0, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_EQ(prefix(knots, 2.5)[0], 1);
  EXPECT_EQ(prefix(knots, 2.5)[1], 0.625);
  EXPECT_EQ(prefix(knots, double(std::numeric_limits<float>::max()))[0], 0);
  ASSERT_EQ(production.curves()[0].visibility_sources().size(), 1u);
  EXPECT_EQ(production.curves()[0].visibility_sources()[0].support.z,
            double(std::numeric_limits<float>::max()));
}

TEST(DeepFrameProduction,
     native_reservoir_snapshot_selection_keeps_past_emission_and_no_point_opacity)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 709);
  packet.homogeneous.clear();
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
      packet.domain.emission_calls = 0;
  packet.domain.require_full_optical = true;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 0, 1000, 0, 1000, {0, 0, 0}, DEEP_OWNED_OPTICAL_VACUUM, {0, 0}});
  packet.optical_coverage.push_back(
      {0, 1000, 0, 1000, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  packet.expression_sources.push_back({0,
                                       1,
                                       DEEP_NATIVE_EXPR_CAMERA_POINT,
                                       0,
                                       1,
                                       1,
                                       1,
                                       1,
                                       {.25f, 0, 0},
                                       {0, 0, 0},
                                       1,
                                       2,
                                       3,
                                       0,
                                       {0, 0}});
  packet.expression_sources.push_back({0,
                                       2,
                                       DEEP_NATIVE_EXPR_CAMERA_POINT,
                                       0,
                                       2,
                                       2,
                                       2,
                                       2,
                                       {.5f, 0, 0},
                                       {0, 0, 0},
                                       1,
                                       2,
                                       3,
                                       0,
                                       {0, 0}});
  packet.expression_nodes.push_back({0, 1, DEEP_NATIVE_EXPR_TERM, 0, 0, 1, 0, {1, 1, 1}, {0, 0}});
  packet.expression_nodes.push_back(
      {0, 2, DEEP_NATIVE_EXPR_SNAPSHOT, 1, 0, 0, 0, {1, 1, 1}, {0, 0}});
  packet.expression_nodes.push_back({0, 3, DEEP_NATIVE_EXPR_TERM, 0, 0, 2, 0, {1, 1, 1}, {0, 0}});
  packet.expression_nodes.push_back({0, 4, DEEP_NATIVE_EXPR_ADD, 1, 3, 0, 0, {1, 1, 1}, {0, 0}});
  packet.expression_nodes.push_back(
      {0, 5, DEEP_NATIVE_EXPR_SELECT, 2, 4, 0, 0, {1, 1, 1}, {0, 0}});
  packet.expression_nodes.push_back(
      {0, 6, DEEP_NATIVE_EXPR_MIX, 4, 5, 0, 0, {.5f, .5f, .5f}, {0, 0}});
  DeepNativeExpressionCall call{};
  call.ordinal = 1;
  call.head = 6;
  call.routing = DEEP_NATIVE_POINT_COMBINED;
  call.pixel = packet.cameras[0].pixel;
  call.sample = packet.cameras[0].sample;
  call.pre_clamp_rgb[0] = .5f;
  call.post_clamp_rgb[0] = .25f;
  call.clamp_gain = .5f;
  packet.expression_calls.push_back(call);
  packet.domain.expression_source_reserved = packet.domain.expression_source_written = 2;
  packet.domain.expression_node_reserved = packet.domain.expression_node_written = 6;
  packet.domain.expression_call_reserved = packet.domain.expression_call_written = 1;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_EQ(prefix(knots, 1.5)[0], 1);
  EXPECT_EQ(prefix(knots, 1.5)[1], .125);
  EXPECT_EQ(prefix(knots, 2.5)[0], 1);
  EXPECT_EQ(prefix(knots, 2.5)[1], .25);
  EXPECT_TRUE(production.curves()[0].visibility_sources().empty());
}

TEST(DeepFrameProduction, native_surface_root_owns_all_point_calls_and_zero_rgb_opacity)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (uint32_t calls : {0u, 2u}) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    ASSERT_TRUE(admit_packet(production,
                             point_packet(production.memory_budget(), 600 + calls, false, calls)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 2));
    ASSERT_TRUE(production.finish_inputs());
    const auto &curve = production.curves()[0];
    const auto knots = read_knots(curve);
    EXPECT_EQ(prefix(knots, 1.99)[0], 1);
    EXPECT_EQ(prefix(knots, 2.01)[0], 0);
    EXPECT_EQ(prefix(knots, 2.01)[1], calls ? 1.25 : 0);
    EXPECT_EQ(curve.visibility_sources().size(), 1u);
    EXPECT_EQ(curve.radiance_sources().size(), calls);
    DeepCanonicalEncoded encoded;
    const auto status = production.encode_pixel(0, {}, encoded);
    ASSERT_TRUE(status) << status.message;
    EXPECT_TRUE(encoded.report()->quantized_checked);
    EXPECT_EQ(decode_encoded(encoded, 2.01)[0], 0);
  }
}

TEST(DeepFrameProduction, native_world_at_infinity_uses_finite_coverage_and_exact_final_rgb)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
  ASSERT_TRUE(admit_packet(production, point_packet(production.memory_budget(), 611, true)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(production.finish_inputs());
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_EQ(prefix(knots, 1001)[0], 1);
  EXPECT_EQ(prefix(knots, double(std::numeric_limits<float>::max()))[0], 0);
  EXPECT_EQ(prefix(knots, double(std::numeric_limits<float>::max()))[1], 0.625);
  EXPECT_EQ(production.curves()[0].optical_sources()[0].support.z_back, 1000);
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(encoded.samples().back().z, std::numeric_limits<float>::max());
}

TEST(DeepFrameProduction, mixed_homogeneous_prefix_then_opaque_root_keeps_ray_correlation)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers);
  auto packet = native_packet(production.memory_budget(), 612);
  auto point = point_packet(production.memory_budget(), 612, false, 1);
  point.roots[0].z = point.roots[0].physical_t = 5;
  point.points[0].z = 5;
  packet.roots = std::move(point.roots);
  packet.points = std::move(point.points);
  packet.domain.root_reserved = packet.domain.root_written = packet.domain.point_reserved =
      packet.domain.point_written = 1;
  packet.cameras[0].root_handle = 1;
  packet.cameras[0].flags &= ~DEEP_NATIVE_CAMERA_BACKGROUND_REACHED;
  ASSERT_TRUE(admit_packet(production, std::move(packet)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  const auto knots = read_knots(production.curves()[0]);
  EXPECT_NEAR(prefix(knots, 3)[0], std::exp(-1.0), 1e-4);
  EXPECT_NEAR(prefix(knots, 4.5)[0], std::exp(-2.0), 1e-4);
  EXPECT_EQ(prefix(knots, 5.01)[0], 0);
  EXPECT_NEAR(prefix(knots, 5.01)[1], 4.125, 1e-4);
  DeepCanonicalEncoded encoded;
  const auto status = production.encode_pixel(0, {}, encoded);
  ASSERT_TRUE(status) << status.message;
}

TEST(DeepFrameProduction, catcher_auxiliary_paths_cannot_publish_combined_sources)
{
  EXPECT_TRUE(deep_native_path_writes_combined(0));
  EXPECT_TRUE(deep_native_path_writes_combined(PATH_RAY_SHADOW_CATCHER_HIT));
  for (const uint32_t flag :
       {uint32_t(PATH_RAY_SHADOW_CATCHER_PASS), uint32_t(PATH_RAY_SHADOW_CATCHER_BACKGROUND)})
  {
    EXPECT_FALSE(deep_native_path_writes_combined(PATH_RAY_SHADOW_CATCHER_HIT | flag));
    const std::array<uint32_t, 2> consumers{1, 2};
    DeepFrameProduction owner(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = point_packet(owner.memory_budget(), 699, false);
    packet.points[0].path_flags = PATH_RAY_SHADOW_CATCHER_HIT | flag;
    EXPECT_FALSE(admit_packet(owner, std::move(packet)));
    EXPECT_EQ(owner.error().code, DeepErrorCode::InvalidPayload);
    EXPECT_FALSE(owner.curves()[0]);
  }
}

TEST(DeepFrameProduction, point_root_identity_counts_and_clamp_routing_fail_closed)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  for (int kind = 0; kind < 5; kind++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = point_packet(production.memory_budget(), 620 + kind, false);
    if (kind == 0)
      packet.points[0].root_ordinal++;
    if (kind == 1)
      packet.domain.root_written = 0;
    if (kind == 2)
      packet.points[0].final_rgb[0] += 1;
    if (kind == 3)
      packet.points[1].ordinal = packet.points[0].ordinal;
    if (kind == 4)
      packet.roots[0].flags &= ~DEEP_NATIVE_ROOT_OPAQUE_SURFACE;
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
    EXPECT_EQ(production.error().code, DeepErrorCode::InvalidPayload);
    EXPECT_FALSE(production.curves()[0]);
  }
}

TEST(DeepFrameProduction, native_raster_mapping_preserves_prefix_payload_and_sample_identity)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepMemoryBudget budget(32 << 20);
  DeepFrameProduction reference(homogeneous_frame(), {-96, -17, 1, 1}, budget, consumers);
  DeepFrameProduction shifted(
      homogeneous_frame(), {-98, -18, 1, 1}, budget, budget, consumers, true, make_int2(-2, -1));
  ASSERT_TRUE(admit_packet(reference, native_packet(budget, 881, 0, 2, 4)));
  ASSERT_TRUE(admit_packet(shifted, native_packet(budget, 881, 0, 2, 4)));
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(reference.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(shifted.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
  ASSERT_TRUE(reference.finish_inputs());
  ASSERT_TRUE(shifted.finish_inputs());
  const auto a = read_knots(reference.curves()[0]), b = read_knots(shifted.curves()[0]);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].z, b[i].z);
    EXPECT_EQ(a[i].flags, b[i].flags);
    for (int c = 0; c < 4; ++c) {
      EXPECT_EQ(a[i].before[c], b[i].before[c]);
      EXPECT_EQ(a[i].after[c], b[i].after[c]);
      EXPECT_EQ(a[i].slope_right[c], b[i].slope_right[c]);
    }
  }
  DeepCanonicalRowLease rows;
  ASSERT_TRUE(shifted.pipeline().acquire_closed_rows(1, -18, 1, rows));
  ASSERT_EQ(rows.pixels().size(), 1u);
  EXPECT_EQ(rows.pixels()[0].x, -98);
  EXPECT_EQ(rows.pixels()[0].y, -18);
  EXPECT_EQ(rows.pixels()[0].native_population, 1u);
  for (bool alpha : {false, true}) {
    for (auto precision : {DeepCanonicalPrecision::Half, DeepCanonicalPrecision::Float}) {
      DeepCanonicalEncodeConfig config;
      config.alpha_only = alpha;
      config.precision = precision;
      DeepCanonicalEncoded encoded_a, encoded_b;
      ASSERT_TRUE(reference.encode_pixel(0, config, encoded_a));
      ASSERT_TRUE(shifted.encode_pixel(0, config, encoded_b));
      ASSERT_EQ(encoded_a.samples().size(), encoded_b.samples().size());
      for (size_t i = 0; i < encoded_a.samples().size(); ++i) {
        const auto &left = encoded_a.samples()[i], &right = encoded_b.samples()[i];
        EXPECT_EQ(left.r, right.r);
        EXPECT_EQ(left.g, right.g);
        EXPECT_EQ(left.b, right.b);
        EXPECT_EQ(left.a, right.a);
        EXPECT_EQ(left.z, right.z);
        EXPECT_EQ(left.z_back, right.z_back);
      }
    }
  }
}

TEST(DeepFrameProduction, native_raster_mapping_rejects_overflow_and_invalid_raw_packet)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepMemoryBudget budget(16 << 20);
  DeepFrameProduction overflow(
      homogeneous_frame(), {0, 0, 1, 1}, budget, budget, consumers, true, make_int2(1, 0));
  auto packet = native_packet(budget, 882);
  packet.domain.x = INT32_MAX;
  EXPECT_FALSE(begin_reference_capture(overflow, packet.domain));
  EXPECT_EQ(overflow.error().code, DeepErrorCode::InvalidLayout);
  EXPECT_FALSE(overflow.curves()[0]);
  DeepFrameProduction invalid(
      homogeneous_frame(), {-98, -18, 1, 1}, budget, budget, consumers, true, make_int2(-2, -1));
  auto malformed = native_packet(budget, 883);
  malformed.cameras[0].x--;
  EXPECT_FALSE(admit_packet(invalid, std::move(malformed)));
  EXPECT_EQ(invalid.error().code, DeepErrorCode::InvalidPayload);
  EXPECT_FALSE(invalid.curves()[0]);
  DeepFrameProduction final_tile(
      homogeneous_frame(), {-95, -17, 1, 1}, budget, budget, consumers, true, make_int2(1, 0));
  ASSERT_TRUE(admit_packet(final_tile, native_packet(budget, 884)));
  const std::array<uint32_t, 1> counts{1};
  EXPECT_FALSE(final_tile.reference_capture()->close_native_tile(INT32_MAX, -17, 1, 1, counts, 1));
  EXPECT_EQ(final_tile.error().code, DeepErrorCode::InvalidLayout);
  EXPECT_FALSE(final_tile.curves()[0]);
}

TEST(DeepFrameProduction, separate_device_exhaustion_preserves_owned_host_curve)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepMemoryBudget host(8 << 20), device(4096);
  const size_t host_initial = host.used(), device_initial = device.used();
  const size_t device_hold = device.quota() - device.used();
  device.acquire(device_hold);
  DeepNativeCurve lease;
  {
    DeepFrameProduction production(
        homogeneous_frame(), {-96, -17, 1, 1}, host, device, consumers, false);
    ASSERT_TRUE(production.error());
    EXPECT_TRUE(production.memory_budget() == host);
    EXPECT_TRUE(production.device_memory_budget() == device);
    EXPECT_THROW(production.device_memory_budget().acquire(1), std::bad_alloc);
    ASSERT_TRUE(admit_packet(production, native_packet(host, 901)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    ASSERT_TRUE(production.finish_inputs());
    lease = production.curves()[0];
    EXPECT_GT(lease.knot_count(), 0);
    EXPECT_EQ(device.used(), device.quota());
  }
  DeepDepthKnot knot;
  ASSERT_TRUE(lease.read_knot(0, knot));
  EXPECT_GT(host.used(), host_initial);
  lease = {};
  EXPECT_EQ(host.used(), host_initial);
  device.relinquish(device_hold);
  EXPECT_EQ(device.used(), device_initial);
}

TEST(DeepFrameProduction, idle_device_capacity_cannot_bypass_host_denial)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepMemoryBudget host(4096), device(8 << 20);
  const size_t initial = host.used(), hold = host.quota() - host.used();
  const size_t device_initial = device.used();
  host.acquire(hold);
  {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, host, device, consumers);
    EXPECT_FALSE(production.error());
    EXPECT_EQ(production.error().code, DeepErrorCode::BudgetExceeded);
    EXPECT_EQ(device.used(), device_initial);
  }
  host.relinquish(hold);
  EXPECT_EQ(host.used(), initial);
}

TEST(DeepFrameProduction, host_quota_policy_and_override_are_bounded_and_strict)
{
  size_t quota = 123;
  EXPECT_FALSE(deep_host_memory_quota(0, nullptr, quota));
  EXPECT_EQ(quota, 0);
  EXPECT_TRUE(deep_host_memory_quota(1 << 20, nullptr, quota));
  EXPECT_EQ(quota, size_t(1 << 18));
  EXPECT_TRUE(deep_host_memory_quota(size_t(64) << 30, nullptr, quota));
  EXPECT_EQ(quota, size_t(8) << 30);
  EXPECT_TRUE(deep_host_memory_quota(0, "17", quota));
  EXPECT_EQ(quota, size_t(17) << 20);
  for (const char *invalid : {"",
                              "0",
                              "-1",
                              "+1",
                              " 1",
                              "1 ",
                              "1MB",
                              "1.5",
                              "18446744073709551616",
                              "18446744073709551615"})
  {
    EXPECT_FALSE(deep_host_memory_quota(64 << 20, invalid, quota)) << invalid;
    EXPECT_EQ(quota, 0) << invalid;
  }
}

TEST(DeepFrameProduction, frozen_consumers_preserve_owned_configs_frame_and_version)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, ids);
  const auto rgb = deep_production_encode_config(.1, .1, DeepCanonicalPrecision::Half, false);
  const auto alpha = deep_production_encode_config(.01, .001, DeepCanonicalPrecision::Float, true);
  std::array<DeepFrozenConsumer, 2> consumers{
      {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, rgb}, {2, 6789, 3, 2, alpha}}};
  ASSERT_TRUE(production.configure_consumers(consumers));
  consumers[1].config.alpha_tolerance = .9;
  DeepConsumerToken token;
  ASSERT_TRUE(production.consumer_token(6789, alpha, 2, token));
  EXPECT_EQ(token.id, 2u);
  EXPECT_EQ(token.config_version, 3u);
  ASSERT_TRUE(production.validate_consumer_token(token));
  auto stale = token;
  stale.config_version++;
  EXPECT_EQ(production.validate_consumer_token(stale).code, DeepErrorCode::InvalidState);
  stale = token;
  stale.frame.invocation++;
  EXPECT_EQ(production.validate_consumer_token(stale).code, DeepErrorCode::InvalidFrame);
  stale = token;
  stale.id = 99;
  EXPECT_EQ(production.validate_consumer_token(stale).code, DeepErrorCode::InvalidReference);
  auto changed = alpha;
  changed.alpha_tolerance = 1e-9;
  EXPECT_FALSE(production.consumer_token(6789, changed, 2, stale));
  EXPECT_FALSE(production.consumer_token(6789, alpha, 3, stale));
  EXPECT_FALSE(production.consumer_token(1234, alpha, 2, stale));
  ASSERT_TRUE(production.consumer_token(6789, alpha, 2, token));
  EXPECT_DOUBLE_EQ(production.frozen_consumers()[1].config.alpha_tolerance, .001);
}

TEST(DeepFrameProduction, duplicate_or_late_consumer_registration_is_a_hard_failure)
{
  const std::array<uint32_t, 2> ids{1, 2};
  for (int mutation = 0; mutation < 4; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, ids);
    std::array<DeepFrozenConsumer, 2> consumers{
        {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, {}}, {2, 6789, 1, 2, {}}}};
    if (mutation == 0)
      consumers[1].source_key = consumers[0].source_key;
    else if (mutation == 1)
      consumers[1].id = 99;
    else if (mutation == 2)
      consumers[1].config_version = 0;
    else
      ASSERT_TRUE(admit_packet(production, native_packet(production.memory_budget(), 999)));
    EXPECT_FALSE(production.configure_consumers(consumers));
    EXPECT_FALSE(production.error());
  }
}

TEST(DeepFrameProduction, frozen_encoded_cache_matches_reference_and_outlives_retired_owner)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepMemoryBudget budget(32 << 20);
  const size_t initial = budget.used();
  DeepEncodedPixelLease held;
  const auto rgb = deep_production_encode_config(.1, .1, DeepCanonicalPrecision::Half, false);
  const auto alpha = deep_production_encode_config(.01, .001, DeepCanonicalPrecision::Float, true);
  {
    DeepFrameProduction reference(homogeneous_frame(), {-96, -17, 1, 1}, budget, ids, false);
    DeepFrameProduction cached(homogeneous_frame(), {-96, -17, 1, 1}, budget, ids, false);
    const std::array<DeepFrozenConsumer, 2> consumers{
        {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, rgb}, {2, 1234, 3, 2, alpha}}};
    ASSERT_TRUE(cached.configure_consumers(consumers));
    DeepConsumerToken token;
    ASSERT_TRUE(cached.consumer_token(1234, alpha, 2, token));
    EXPECT_FALSE(cached.encoded_pixel(token, 0, held));
    for (auto *owner : {&reference, &cached}) {
      ASSERT_TRUE(admit_packet(*owner, native_packet(budget, 1001)));
      const std::array<uint32_t, 1> counts{1};
      ASSERT_TRUE(owner->reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 2));
      ASSERT_TRUE(owner->finish_inputs());
    }
    EXPECT_FALSE(cached.curves()[0]);
    DeepCanonicalRowLease retired;
    EXPECT_EQ(cached.pipeline().canonical_view(retired).code, DeepErrorCode::InvalidState);
    EXPECT_FALSE(retired);
    for (const auto &consumer : consumers) {
      ASSERT_TRUE(cached.consumer_token(consumer.source_key, consumer.config, 2, token));
      DeepCanonicalEncoded expected;
      ASSERT_TRUE(reference.encode_pixel(0, consumer.config, expected));
      ASSERT_TRUE(cached.encoded_pixel(token, 0, held));
      ASSERT_EQ(held.samples().size(), expected.samples().size());
      for (size_t i = 0; i < held.samples().size(); i++) {
        const auto &a = held.samples()[i];
        const auto &b = expected.samples()[i];
        EXPECT_EQ(a.r, b.r);
        EXPECT_EQ(a.g, b.g);
        EXPECT_EQ(a.b, b.b);
        EXPECT_EQ(a.a, b.a);
        EXPECT_EQ(a.z, b.z);
        EXPECT_EQ(a.z_back, b.z_back);
      }
      DeepCanonicalEncodeReport report;
      ASSERT_TRUE(cached.encoding_report(token, report));
      EXPECT_EQ(report.input_knots, expected.report()->input_knots);
      EXPECT_EQ(report.output_samples, held.samples().size());
      auto stale = token;
      stale.config_version++;
      DeepEncodedPixelLease denied;
      EXPECT_FALSE(cached.encoded_pixel(stale, 0, denied));
      EXPECT_FALSE(denied);
    }
  }
  ASSERT_TRUE(held);
  EXPECT_GT(held.samples().size(), 0u);
  EXPECT_GT(budget.used(), initial);
  held = {};
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFrameProduction, consumer_encoding_failure_cannot_publish_partial_cache)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepMemoryBudget budget(16 << 20);
  const size_t initial = budget.used();
  {
    DeepFrameProduction cached(homogeneous_frame(), {-96, -17, 1, 1}, budget, ids, false);
    auto alpha = deep_production_encode_config(.01, 0, DeepCanonicalPrecision::Float, true);
    const std::array<DeepFrozenConsumer, 2> consumers{
        {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, {}}, {2, 1234, 1, 2, alpha}}};
    ASSERT_TRUE(cached.configure_consumers(consumers));
    DeepConsumerToken token;
    ASSERT_TRUE(cached.consumer_token(1234, alpha, 2, token));
    ASSERT_TRUE(admit_packet(cached, native_packet(budget, 1002)));
    const std::array<uint32_t, 1> counts{1};
    EXPECT_FALSE(cached.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
    EXPECT_FALSE(cached.finish_inputs());
    DeepEncodedPixelLease denied;
    EXPECT_FALSE(cached.encoded_pixel(token, 0, denied));
    EXPECT_FALSE(denied);
    EXPECT_EQ(cached.pipeline().state(), DeepFrameState::Failed);
  }
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFrameProduction, encoded_cache_and_control_allocations_cannot_bypass_host_quota)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepMemoryBudget host(2 << 20), device(16 << 20);
  const size_t initial = host.used();
  const size_t device_initial = device.used();
  for (const bool deny_registration : {false, true}) {
    size_t held = 0;
    {
      DeepFrameProduction cached(homogeneous_frame(), {-96, -17, 1, 1}, host, device, ids, false);
      const std::array<DeepFrozenConsumer, 2> consumers{
          {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, {}}, {2, 1234, 1, 2, {}}}};
      if (!deny_registration) {
        ASSERT_TRUE(cached.configure_consumers(consumers));
        ASSERT_TRUE(admit_packet(cached, native_packet(host, 1003)));
      }
      held = host.quota() - host.used();
      host.acquire(held);
      if (deny_registration)
        EXPECT_EQ(cached.configure_consumers(consumers).code, DeepErrorCode::BudgetExceeded);
      else {
        const std::array<uint32_t, 1> counts{1};
        EXPECT_FALSE(cached.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
        EXPECT_EQ(cached.error().code, DeepErrorCode::BudgetExceeded);
      }
      EXPECT_EQ(cached.pipeline().state(), DeepFrameState::Failed);
      EXPECT_EQ(device.used(), device_initial);
    }
    host.relinquish(held);
    EXPECT_EQ(host.used(), initial);
  }
}

TEST(DeepFrameProduction, bounded_native_capacity_uses_both_quotas_and_exact_expression_demand)
{
  DeepNativeCaptureCapacity dark, biased, null;
  ASSERT_TRUE(deep_native_capture_capacity(64, 100000, false, false, true, dark));
  ASSERT_TRUE(deep_native_capture_capacity(64, 100000, false, true, true, biased));
  ASSERT_TRUE(deep_native_capture_capacity(64, 100000, false, true, false, null));
  EXPECT_EQ(dark.expression_sources, 0u);
  EXPECT_EQ(biased.expression_nodes, 64u * 512);
  EXPECT_EQ(null.expression_nodes, 64u * 528);
  EXPECT_EQ(null.host_staging - biased.host_staging, 64u * 16 * sizeof(DeepNativeExpressionNode));
  EXPECT_EQ(deep_native_batch_samples(
                64, 99, 100000, false, true, true, biased.host_staging, biased.device),
            1);
  EXPECT_EQ(deep_native_batch_samples(
                64, 99, 100000, false, true, true, biased.host_staging - 1, biased.device),
            0);
  EXPECT_EQ(deep_native_batch_samples(
                64, 99, 100000, false, true, true, biased.host_staging, biased.device - 1),
            0);
  EXPECT_GT(deep_native_batch_samples(
                64, 99, 100000, false, false, true, biased.host_staging, biased.device),
            1);
  EXPECT_FALSE(deep_native_capture_capacity(UINT64_MAX, 1, false, true, true, null));
  EXPECT_EQ(
      deep_native_batch_samples(UINT64_MAX, INT_MAX, 1, false, true, true, SIZE_MAX, SIZE_MAX), 0);
}

TEST(DeepFrameProduction, native_adaptive_halo_is_validated_but_never_output_population)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepMemoryBudget budget(32 << 20);
  DeepFrameProduction cached(homogeneous_frame(), {-96, -17, 2, 1}, budget, ids, false);
  const std::array<DeepFrozenConsumer, 2> consumers{
      {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, {}}, {2, 1234, 1, 2, {}}}};
  ASSERT_TRUE(cached.configure_consumers(consumers));
  for (int core = 0; core < 2; core++) {
    auto packet = native_packet(budget, 1200 + core);
    packet.domain.width = 2;
    packet.domain.core_x = -96 + core;
    packet.domain.core_y = -17;
    packet.domain.core_width = packet.domain.core_height = 1;
    packet.domain.scheduled_admissions = packet.domain.camera_reserved =
        packet.domain.camera_written = 2;
    packet.domain.homogeneous_reserved = packet.domain.homogeneous_written =
        packet.domain.emission_calls = 2;
    auto camera = packet.cameras.front();
    camera.x++;
    camera.pixel++;
    packet.cameras.push_back(camera);
    auto segment = packet.homogeneous.front();
    segment.camera_index++;
    segment.pixel++;
    packet.homogeneous.push_back(segment);
    ASSERT_TRUE(admit_packet(cached, std::move(packet)));
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(cached.reference_capture()->close_native_tile(-96 + core, -17, 1, 1, counts, 1));
  }
  ASSERT_TRUE(cached.finish_inputs());
  DeepConsumerToken token;
  ASSERT_TRUE(cached.consumer_token(1234, {}, 2, token));
  DeepEncodedPixelLease a, b;
  ASSERT_TRUE(cached.encoded_pixel(token, 0, a));
  ASSERT_TRUE(cached.encoded_pixel(token, 1, b));
  /* The two pixels belong to distinct tiles; range borrowing keeps both alive. */
  std::array<DeepEncodedPixelLease, 2> row;
  const size_t live_before = budget.used();
  ASSERT_TRUE(cached.encoded_pixels(token, 0, row));
  EXPECT_EQ(budget.used(), live_before);
  for (size_t pixel = 0; pixel < row.size(); ++pixel) {
    const auto &reference = pixel == 0 ? a : b;
    ASSERT_EQ(row[pixel].size(), reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
      const auto actual = row[pixel].sample(i), expected = reference.sample(i);
      EXPECT_EQ(actual.r, expected.r);
      EXPECT_EQ(actual.g, expected.g);
      EXPECT_EQ(actual.b, expected.b);
      EXPECT_EQ(actual.a, expected.a);
      EXPECT_EQ(actual.z, expected.z);
      EXPECT_EQ(actual.z_back, expected.z_back);
    }
  }
  EXPECT_EQ(cached.encoded_pixels(token, 1, row).code, DeepErrorCode::InvalidLayout);
  EXPECT_EQ(cached.encoded_pixels(token, SIZE_MAX, row).code, DeepErrorCode::InvalidLayout);
  EXPECT_TRUE(cached.encoded_pixels(token, 2, {}));
  EXPECT_EQ(cached.encoded_pixels(token, 3, {}).code, DeepErrorCode::InvalidLayout);
  auto stale = token;
  stale.config_version++;
  EXPECT_EQ(cached.encoded_pixels(stale, 0, row).code, DeepErrorCode::InvalidState);
  {
    DeepEncodedRowLease borrowed(budget);
    ASSERT_TRUE(cached.encoded_row(token, 0, 2, borrowed));
    const size_t workspace_live = budget.used();
    ASSERT_EQ(borrowed.pixels().size(), 2u);
    for (size_t pixel = 0; pixel < 2; ++pixel) {
      const auto &reference = row[pixel];
      const auto &view = borrowed.pixels()[pixel];
      ASSERT_EQ(view.size(), reference.size());
      for (size_t i = 0; i < view.size(); ++i) {
        const auto actual = view.sample(i), expected = reference.sample(i);
        EXPECT_EQ(actual.r, expected.r);
        EXPECT_EQ(actual.a, expected.a);
        EXPECT_EQ(actual.z, expected.z);
        EXPECT_EQ(actual.z_back, expected.z_back);
      }
    }
    borrowed.release();
    EXPECT_TRUE(borrowed.pixels().empty());
    EXPECT_EQ(budget.used(), workspace_live);
    ASSERT_TRUE(cached.encoded_row(token, 1, 1, borrowed));
    EXPECT_EQ(budget.used(), workspace_live);
    EXPECT_EQ(cached.encoded_row(token, SIZE_MAX, 2, borrowed).code,
              DeepErrorCode::InvalidLayout);
    EXPECT_TRUE(borrowed.pixels().empty());
    EXPECT_EQ(cached.encoded_row(stale, 0, 2, borrowed).code, DeepErrorCode::InvalidState);
    EXPECT_TRUE(borrowed.pixels().empty());
    EXPECT_TRUE(cached.encoded_row(token, 2, 0, borrowed));
  }
  EXPECT_EQ(budget.used(), live_before);
  DeepEncodedPixelLease moved = std::move(row[0]);
  EXPECT_FALSE(bool(row[0]));
  EXPECT_TRUE(bool(moved));
  ASSERT_EQ(a.samples().size(), b.samples().size());
  for (size_t i = 0; i < a.samples().size(); i++) {
    EXPECT_EQ(a.samples()[i].r, b.samples()[i].r);
    EXPECT_EQ(a.samples()[i].a, b.samples()[i].a);
  }
}

TEST(DeepFrameProduction, malformed_native_halo_and_changed_core_cannot_be_ignored)
{
  const std::array<uint32_t, 2> ids{1, 2};
  for (int mutation = 0; mutation < 3; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 2, 1}, 8 << 20, ids);
    auto packet = native_packet(production.memory_budget(), 1300 + mutation);
    packet.domain.core_x = -96;
    packet.domain.core_y = -17;
    packet.domain.core_width = packet.domain.core_height = 1;
    if (mutation == 0) {
      packet.domain.core_width = 2;
      EXPECT_FALSE(begin_reference_capture(production, packet.domain));
    }
    else {
      auto binding = packet.domain;
      binding.camera_reserved = binding.camera_written = 0;
      binding.homogeneous_reserved = binding.homogeneous_written = binding.emission_calls = 0;
      ASSERT_TRUE(begin_reference_capture(production, binding));
      if (mutation == 1)
        packet.domain.core_x++;
      else
        packet.homogeneous[0].tau[0] = std::numeric_limits<float>::quiet_NaN();
      EXPECT_FALSE(complete_reference_capture(production, std::move(packet)));
    }
    EXPECT_FALSE(production.error());
  }
}

TEST(DeepFrameProduction, budget_peak_tracks_real_successful_acquisitions_and_shared_leases)
{
  DeepMemoryBudget budget(4096);
  const size_t initial = budget.used();
  auto copy = budget;
  EXPECT_EQ(copy.peak(), initial);
  budget.acquire(100);
  copy.acquire(200);
  EXPECT_EQ(copy.peak(), initial + 300);
  budget.relinquish(100);
  copy.relinquish(200);
  EXPECT_EQ(budget.used(), initial);
  EXPECT_EQ(budget.peak(), initial + 300);
  EXPECT_THROW(copy.acquire(4096), std::bad_alloc);
  EXPECT_EQ(copy.peak(), initial + 300);
}

TEST(DeepFrameProduction, expression_journal_keeps_global_heads_across_independent_pages)
{
  DeepMemoryBudget budget(1 << 20);
  auto packet = native_packet(budget, 1401);
  DeepExpressionJournal journal(budget, packet.domain, 1);
  DeepNativeExpressionSource source = {};
  source.ordinal = 1;
  source.flags = DEEP_NATIVE_EXPR_CAMERA_SPAN;
  DeepNativeExpressionNode term = {};
  term.ordinal = 1;
  term.operation = DEEP_NATIVE_EXPR_TERM;
  term.source = 1;
  DeepExpressionPage page;
  page.domain = packet.domain.id;
  page.device_generation = packet.domain.device_generation;
  page.layout_generation = packet.domain.layout_generation;
  page.sources = {&source, 1};
  page.nodes = {&term, 1};
  ASSERT_TRUE(journal.append(page));
  source.raw_rgb[0] = 1000;
  term.source = 999; /* Journal owns immutable copies. */
  DeepNativeExpressionNode scale = {};
  scale.ordinal = 2;
  scale.operation = DEEP_NATIVE_EXPR_SCALE;
  scale.a = 1;
  scale.weight[0] = scale.weight[1] = scale.weight[2] = .5f;
  DeepNativeExpressionCall call = {};
  call.ordinal = 1;
  call.head = 2;
  page.source_base = page.node_base = 1;
  page.sources = {};
  page.nodes = {&scale, 1};
  page.calls = {&call, 1};
  ASSERT_TRUE(journal.append(page));
  packet.domain.expression_source_reserved = packet.domain.expression_source_written = 1;
  packet.domain.expression_node_reserved = packet.domain.expression_node_written = 2;
  packet.domain.expression_call_reserved = packet.domain.expression_call_written = 1;
  ASSERT_TRUE(journal.release_to(packet));
  ASSERT_EQ(packet.expression_sources.size(), 1u);
  ASSERT_EQ(packet.expression_nodes.size(), 2u);
  EXPECT_EQ(packet.expression_sources[0].raw_rgb[0], 0);
  EXPECT_EQ(packet.expression_nodes[0].source, 1u);
  EXPECT_EQ(packet.expression_nodes[1].a, 1u);
  EXPECT_EQ(packet.expression_calls[0].head, 2u);
  EXPECT_EQ(packet.domain.expression_node_written, 2u);
  EXPECT_FALSE(journal.append(page));
}

TEST(DeepFrameProduction, expression_journal_rejects_replays_foreign_and_forward_handles)
{
  for (int mutation = 0; mutation < 6; mutation++) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = native_packet(budget, 1410 + mutation);
    DeepExpressionJournal journal(budget, packet.domain, 2);
    DeepNativeExpressionSource source = {};
    source.ordinal = 1;
    DeepNativeExpressionNode term = {};
    term.ordinal = 1;
    term.source = 1;
    term.operation = DEEP_NATIVE_EXPR_TERM;
    DeepExpressionPage page;
    page.domain = packet.domain.id;
    page.device_generation = packet.domain.device_generation;
    page.layout_generation = packet.domain.layout_generation;
    page.sources = {&source, 1};
    page.nodes = {&term, 1};
    if (mutation == 0)
      page.node_base = 1;
    if (mutation == 1)
      page.layout_generation++;
    if (mutation == 2)
      term.a = 1;
    if (mutation == 3)
      term.source = 2;
    if (mutation == 4)
      source.camera_index = 1;
    if (mutation == 5)
      term.weight[1] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(journal.append(page));
    EXPECT_EQ(journal.source_count(), 0u);
    EXPECT_EQ(journal.node_count(), 0u);
    EXPECT_FALSE(journal.release_to(packet));
    EXPECT_TRUE(packet.expression_sources.empty());
    EXPECT_TRUE(packet.expression_nodes.empty());
  }
}

TEST(DeepFrameProduction, expression_journal_quota_denial_never_commits_partial_counts)
{
  DeepMemoryBudget budget(16384);
  const size_t initial = budget.used();
  {
    auto packet = native_packet(budget, 1420);
    DeepExpressionJournal journal(budget, packet.domain, 1);
    DeepExpressionPage page;
    page.domain = packet.domain.id;
    page.device_generation = packet.domain.device_generation;
    page.layout_generation = packet.domain.layout_generation;
    DeepNativeExpressionSource source = {};
    source.ordinal = 1;
    page.sources = {&source, 1};
    const size_t held = budget.quota() - budget.used();
    budget.acquire(held);
    EXPECT_EQ(journal.append(page).code, DeepErrorCode::BudgetExceeded);
    EXPECT_EQ(journal.source_count(), 0u);
    budget.relinquish(held);
    EXPECT_FALSE(journal.release_to(packet));
    EXPECT_TRUE(packet.expression_sources.empty());
  }
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFrameProduction, expression_journal_cannot_rewrite_producer_counts_or_fake_join)
{
  for (const bool joined : {false, true}) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = native_packet(budget, 1430);
    DeepExpressionJournal journal(budget, packet.domain, 1);
    DeepExpressionPage page;
    page.domain = packet.domain.id;
    page.device_generation = packet.domain.device_generation;
    page.layout_generation = packet.domain.layout_generation;
    ASSERT_TRUE(journal.append(page));
    packet.domain.main_shadow_mnee_joined = joined;
    packet.domain.expression_node_written = packet.domain.expression_node_reserved = 1;
    EXPECT_FALSE(journal.release_to(packet));
    EXPECT_TRUE(packet.expression_nodes.empty());
    EXPECT_EQ(packet.domain.expression_node_written, 1u);
  }
}

TEST(DeepFrameProduction, continuation_slots_are_charged_before_native_admission)
{
  DeepNativeCaptureCapacity original, continued;
  ASSERT_TRUE(deep_native_capture_capacity(64, 100000, false, true, true, original));
  ASSERT_TRUE(deep_native_capture_capacity(64, 100000, false, true, true, continued, true));
  EXPECT_EQ(original.host_staging, continued.host_staging);
  EXPECT_EQ(continued.device - original.device, 64u * DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES);
  EXPECT_EQ(deep_native_batch_samples(
                64, 1, 100000, false, true, true, continued.host_staging, continued.device, true),
            1);
  EXPECT_EQ(
      deep_native_batch_samples(
          64, 1, 100000, false, true, true, continued.host_staging, continued.device - 1, true),
      0);
}

TEST(DeepFrameProduction, pending_volume_checkpoint_cannot_publish_a_joined_domain)
{
  const std::array<uint32_t, 2> ids{1, 2};
  DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 8 << 20, ids);
  auto packet = native_packet(production.memory_budget(), 1440);
  packet.domain.volume_yields = 3;
  packet.domain.volume_pending = 1;
  EXPECT_FALSE(admit_packet(production, std::move(packet)));
  EXPECT_EQ(production.error().code, DeepErrorCode::InvalidPayload);
  EXPECT_FALSE(production.curves()[0]);
}

namespace {
DeepNativeCaptureBatch control_packet(const DeepMemoryBudget &budget, const uint64_t id)
{
  auto packet = native_packet(budget, id);
  packet.homogeneous.clear();
  packet.domain.homogeneous_reserved = packet.domain.homogeneous_written = 0;
  packet.domain.emission_calls = 0;
  return packet;
}
DeepControlPage control_page(const DeepNativeCaptureBatch &packet)
{
  DeepControlPage page;
  page.domain = packet.domain.id;
  page.device_generation = packet.domain.device_generation;
  page.layout_generation = packet.domain.layout_generation;
  return page;
}
DeepNativeRootRecord control_root(const uint32_t ordinal, const uint32_t camera = 0)
{
  DeepNativeRootRecord root = {};
  root.camera_index = camera;
  root.ordinal = ordinal;
  root.kind = DEEP_NATIVE_ROOT_SURFACE;
  root.flags = DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY;
  root.z = root.physical_t = float(ordinal + 1);
  return root;
}
DeepNativePointRecord control_point(const uint32_t ordinal, const DeepNativeRootRecord &root)
{
  DeepNativePointRecord point = {};
  point.ordinal = ordinal;
  point.camera_index = root.camera_index;
  point.root_handle = point.root_ordinal = root.ordinal;
  point.root_kind = root.kind;
  point.z = root.z;
  point.routing = DEEP_NATIVE_POINT_COMBINED;
  point.clamp_gain = 1;
  for (int c = 0; c < 3; ++c) {
    point.pre_clamp_rgb[c] = point.clamp_rgb[c] = point.final_rgb[c] = c == 1 ? -3.0f : 2.0f;
    point.routing_scale[c] = 1;
  }
  return point;
}
}  // namespace

TEST(DeepFrameProduction, control_journal_keeps_root_history_and_native_boundary_ordinals)
{
  DeepMemoryBudget budget(1 << 20);
  auto packet = native_packet(budget, 1501);
  auto segment = packet.homogeneous[0];
  segment.ordinal = 37;
  packet.homogeneous.clear();
  DeepControlJournal journal(budget, packet.domain, 1);
  auto first_root = control_root(1);
  auto page = control_page(packet);
  page.roots = {&first_root, 1};
  page.homogeneous = {&segment, 1};
  ASSERT_TRUE(journal.append(page));
  first_root.z = 999;
  segment.tau[0] = 999;
  auto second_root = control_root(2);
  auto first_point = control_point(0, control_root(1));
  page = control_page(packet);
  page.root_base = page.homogeneous_base = 1;
  page.roots = {&second_root, 1};
  page.points = {&first_point, 1};
  ASSERT_TRUE(journal.append(page));
  auto second_point = control_point(1, second_root);
  page.root_base = 2;
  page.point_base = 1;
  page.roots = {};
  page.points = {&second_point, 1};
  ASSERT_TRUE(journal.append(page));
  EXPECT_EQ(journal.root_count(), 2u);
  EXPECT_EQ(journal.homogeneous_count(), 1u);
  EXPECT_EQ(journal.point_count(), 2u);
  packet.domain.root_reserved = packet.domain.root_written = 2;
  packet.domain.point_reserved = packet.domain.point_written = 2;
  ASSERT_TRUE(journal.release_to(packet));
  ASSERT_EQ(packet.roots.size(), 2u);
  ASSERT_EQ(packet.homogeneous.size(), 1u);
  ASSERT_EQ(packet.points.size(), 2u);
  EXPECT_EQ(packet.roots[0].z, 2);
  EXPECT_EQ(packet.homogeneous[0].ordinal, 37u);
  EXPECT_EQ(packet.homogeneous[0].tau[0], 2);
  EXPECT_EQ(packet.points[0].root_handle, 1u);
  EXPECT_EQ(packet.points[1].root_handle, 2u);
  EXPECT_EQ(packet.points[0].final_rgb[1], -3);
  EXPECT_FALSE(journal.release_to(packet));
}

TEST(DeepFrameProduction, control_journal_rejects_foreign_binding_holes_replay_and_sentinel)
{
  for (int failure = 0; failure < 6; ++failure) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = control_packet(budget, 1510 + failure);
    DeepControlJournal journal(budget, packet.domain, 1);
    auto root = control_root(1);
    auto page = control_page(packet);
    page.roots = {&root, 1};
    if (failure == 0)
      ++page.domain;
    if (failure == 1)
      ++page.device_generation;
    if (failure == 2)
      ++page.layout_generation;
    if (failure == 3)
      ++page.root_base;
    if (failure == 4)
      page.point_base = UINT32_MAX;
    if (failure == 5) {
      ASSERT_TRUE(journal.append(page));
      EXPECT_EQ(journal.root_count(), 1u);
    }
    EXPECT_FALSE(journal.append(page));
    EXPECT_FALSE(journal.append(control_page(packet)));
    EXPECT_FALSE(journal.release_to(packet));
    EXPECT_TRUE(packet.roots.empty());
  }
}

TEST(DeepFrameProduction, control_journal_rejects_unknown_and_cross_camera_point_roots)
{
  for (int failure = 0; failure < 4; ++failure) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = control_packet(budget, 1520 + failure);
    DeepControlJournal journal(budget, packet.domain, 2);
    auto root = control_root(1);
    auto point = control_point(0, root);
    auto page = control_page(packet);
    page.roots = {&root, 1};
    page.points = {&point, 1};
    if (failure == 0)
      point.root_handle = 2;
    if (failure == 1)
      point.camera_index = 1;
    if (failure == 2)
      point.root_ordinal = 9;
    if (failure == 3)
      point.ordinal = 1;
    EXPECT_EQ(journal.append(page).code, DeepErrorCode::InvalidReference);
    EXPECT_EQ(journal.root_count(), 0u);
    EXPECT_EQ(journal.point_count(), 0u);
  }
}

TEST(DeepFrameProduction, control_journal_rejects_nonfinite_uncommitted_payloads)
{
  for (int failure = 0; failure < 3; ++failure) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = control_packet(budget, 1530 + failure);
    DeepControlJournal journal(budget, packet.domain, 1);
    auto root = control_root(1);
    auto point = control_point(0, root);
    auto page = control_page(packet);
    page.roots = {&root, 1};
    page.points = {&point, 1};
    if (failure == 0)
      root.z = std::numeric_limits<float>::quiet_NaN();
    if (failure == 1)
      point.pre_clamp_rgb[0] = std::numeric_limits<float>::infinity();
    if (failure == 2)
      point.clamp_gain = 2;
    EXPECT_FALSE(journal.append(page));
    EXPECT_EQ(journal.root_count(), 0u);
    EXPECT_EQ(journal.point_count(), 0u);
  }
}

TEST(DeepFrameProduction, control_journal_quota_denial_never_commits_any_stream)
{
  DeepMemoryBudget budget(16384);
  const size_t initial = budget.used();
  {
    auto packet = control_packet(budget, 1540);
    DeepControlJournal journal(budget, packet.domain, 1);
    auto root = control_root(1);
    auto point = control_point(0, root);
    auto page = control_page(packet);
    page.roots = {&root, 1};
    page.points = {&point, 1};
    const size_t held = budget.quota() - budget.used();
    budget.acquire(held);
    EXPECT_EQ(journal.append(page).code, DeepErrorCode::BudgetExceeded);
    EXPECT_EQ(journal.root_count(), 0u);
    EXPECT_EQ(journal.homogeneous_count(), 0u);
    EXPECT_EQ(journal.point_count(), 0u);
    budget.relinquish(held);
    EXPECT_FALSE(journal.release_to(packet));
  }
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFrameProduction, control_journal_cannot_fake_join_or_authoritative_counts)
{
  for (const bool joined : {false, true}) {
    DeepMemoryBudget budget(1 << 20);
    auto packet = control_packet(budget, 1550);
    DeepControlJournal journal(budget, packet.domain, 1);
    ASSERT_TRUE(journal.append(control_page(packet)));
    packet.domain.main_shadow_mnee_joined = joined;
    packet.domain.root_reserved = packet.domain.root_written = 1;
    EXPECT_FALSE(journal.release_to(packet));
    EXPECT_EQ(packet.domain.root_written, 1u);
    EXPECT_TRUE(packet.roots.empty());
  }
}

TEST(DeepFrameProduction, control_journal_rejects_foreign_owner_and_nonempty_destination)
{
  for (const bool foreign : {false, true}) {
    DeepMemoryBudget budget(1 << 20), other(1 << 20);
    auto packet = control_packet(budget, 1560);
    DeepControlJournal journal(budget, packet.domain, 1);
    ASSERT_TRUE(journal.append(control_page(packet)));
    if (foreign) {
      auto destination = control_packet(other, 1560);
      EXPECT_FALSE(journal.release_to(destination));
    }
    else {
      packet.roots.push_back(control_root(1));
      EXPECT_FALSE(journal.release_to(packet));
      EXPECT_EQ(packet.roots.size(), 1u);
    }
  }
}

TEST(DeepFrameProduction, resident_root_pins_retire_only_descriptors_and_keep_history)
{
  DeepMemoryBudget budget(1 << 20);
  auto packet = control_packet(budget, 1601);
  DeepControlJournal journal(budget, packet.domain, 1);
  std::array<DeepNativeRootRecord, 3> history{control_root(1), control_root(2), control_root(3)};
  auto page = control_page(packet);
  page.roots = history;
  ASSERT_TRUE(journal.append(page));
  auto newest = control_root(4);
  page.root_base = 3;
  page.roots = {&newest, 1};
  ASSERT_TRUE(journal.append(page));
  const std::array<DeepNativeRootRecord, 2> resident{history[0], history[2]};
  const std::array<uint32_t, 3> pins{0, DEEP_NATIVE_ROOT_PIN_MNEE, DEEP_NATIVE_ROOT_PIN_SHADOW};
  DeepNativeCaptureVector<DeepNativeRootRecord> selected{
      DeepBudgetAllocator<DeepNativeRootRecord>(budget)};
  ASSERT_TRUE(journal.select_live_roots(resident, {&newest, 1}, pins, selected));
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[0].ordinal, 3u);
  EXPECT_EQ(selected[1].ordinal, 4u);
  /* A completed Film call may still reference historical root 1, whose GPU descriptor retired. */
  auto point = control_point(0, history[0]);
  page.root_base = 4;
  page.roots = {};
  page.points = {&point, 1};
  ASSERT_TRUE(journal.append(page));
  packet.domain.root_reserved = packet.domain.root_written = 4;
  packet.domain.point_reserved = packet.domain.point_written = 1;
  ASSERT_TRUE(journal.release_to(packet));
  EXPECT_EQ(packet.roots.size(), 4u);
  EXPECT_EQ(packet.points[0].root_handle, 1u);
}

TEST(DeepFrameProduction, resident_root_snapshot_rejects_foreign_stale_or_unordered_descriptors)
{
  for (int failure = 0; failure < 5; ++failure) {
    DeepMemoryBudget budget(1 << 20), foreign(1 << 20);
    auto packet = control_packet(budget, 1610 + failure);
    DeepControlJournal journal(budget, packet.domain, 1);
    std::array<DeepNativeRootRecord, 2> roots{control_root(1), control_root(2)};
    auto page = control_page(packet);
    page.roots = roots;
    ASSERT_TRUE(journal.append(page));
    std::array<uint32_t, 2> pins{1, 1};
    if (failure == 0)
      roots[0].z = 999;
    if (failure == 1)
      std::swap(roots[0], roots[1]);
    if (failure == 2)
      pins[0] = 16;
    DeepNativeCaptureVector<DeepNativeRootRecord> selected{
        DeepBudgetAllocator<DeepNativeRootRecord>(failure == 3 ? foreign : budget)};
    const std::span<const uint32_t> pin_view{pins.data(), failure == 4 ? 1u : 2u};
    EXPECT_FALSE(journal.select_live_roots(roots, {}, pin_view, selected));
    EXPECT_TRUE(selected.empty());
    EXPECT_FALSE(journal.release_to(packet));
  }
}

TEST(DeepFrameProduction, resident_root_snapshot_quota_failure_cannot_publish_a_partial_selection)
{
  DeepMemoryBudget budget(16384);
  const size_t initial = budget.used();
  {
    auto packet = control_packet(budget, 1620);
    DeepControlJournal journal(budget, packet.domain, 1);
    auto root = control_root(1);
    auto page = control_page(packet);
    page.roots = {&root, 1};
    ASSERT_TRUE(journal.append(page));
    DeepNativeCaptureVector<DeepNativeRootRecord> selected{
        DeepBudgetAllocator<DeepNativeRootRecord>(budget)};
    const uint32_t pin = DEEP_NATIVE_ROOT_PIN_MAIN;
    const size_t held = budget.quota() - budget.used();
    budget.acquire(held);
    EXPECT_EQ(journal.select_live_roots({&root, 1}, {}, {&pin, 1}, selected).code,
              DeepErrorCode::BudgetExceeded);
    EXPECT_TRUE(selected.empty());
    EXPECT_EQ(journal.root_count(), 1u);
    budget.relinquish(held);
    packet.domain.root_reserved = packet.domain.root_written = 1;
    EXPECT_FALSE(journal.release_to(packet));
  }
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepHomogeneousCurve, NativeControlDispatchPreflightBoundsWholeOperations)
{
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 256, 0, false, INT_MAX), 128);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 9, 256, 0, false, INT_MAX), 4);
  EXPECT_EQ(deep_native_control_dispatch_limit(7, 256, 256, 0, false, INT_MAX), 3);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 5, 0, false, INT_MAX), 5);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 256, 3, true, INT_MAX), 3);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 256, 0, true, INT_MAX), 0);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 256, 0, false, 2), 2);
  EXPECT_EQ(deep_native_control_dispatch_limit(1, 256, 256, 0, false, INT_MAX), 0);
  EXPECT_EQ(
      deep_native_control_dispatch_limit(SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, true, INT_MAX),
      INT_MAX);
  EXPECT_EQ(deep_native_control_dispatch_limit(256, 256, 256, 0, false, -1), 0);
}

TEST(DeepFrameProduction,
     radial_native_capture_uses_frozen_transform_and_original_physical_support)
{
  const std::array<uint32_t, 2> consumers{1, 2};
  DeepNativeCurvePolicy policy;
  policy.transmission_error = policy.rgb_error = .004;
  DeepFrameProduction production(
      homogeneous_frame(), {-96, -17, 1, 1}, 32 << 20, consumers, true, policy);
  auto packet = native_packet(production.memory_budget(), 980);
  packet.domain.depth_kind = DeepFrozenDepthKind::Radial;
  packet.domain.depth_transform[3] = 10;
  packet.domain.require_full_optical = true;
  auto &camera = packet.cameras[0];
  camera.flags &= ~DEEP_NATIVE_CAMERA_AFFINE_DEPTH;
  camera.flags |= DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
  camera.origin[0] = 13;
  camera.origin[1] = 4;
  camera.direction[2] = 2;
  camera.z_origin = camera.z_direction = 0;
  camera.tmin = 1.5;
  camera.tmax = 6;
  auto &span = packet.homogeneous[0];
  span.tmin = 1.5;
  span.tmax = 6;
  span.z = float(std::sqrt(34.));
  span.zback = 13;
  const double front = span.z;
  packet.optical_replay_domain = packet.domain.id;
  packet.optical_replay_joined = packet.optical_full_coverage = true;
  packet.optical_ledger.push_back({DEEP_OPTICAL_COMPLETE, 0, 1, 0});
  packet.optical_pieces.push_back(
      {0, 0, 1.5, 6, span.z, 13, {2, 2, 2}, DEEP_OWNED_OPTICAL_MEDIUM, {0, 0}});
  packet.optical_coverage.push_back(
      {1.5, 6, span.z, 13, DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_FAR, 1, 0, 0});
  ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.error().message;
  const std::array<uint32_t, 1> counts{1};
  ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
      << production.error().message;
  ASSERT_TRUE(production.finish_inputs());
  const auto &curve = production.curves()[0];
  const auto knots = read_knots(curve);
  for (int i = 0; i <= 4096; i++) {
    const double z = front + (13 - front) * i / 4096;
    const double f = i == 0 ? 0 : i == 4096 ? 1 : (std::sqrt(z * z - 25) / 2 - 1.5) / 4.5;
    const auto actual = prefix(knots, z);
    const double beer = -std::expm1(-2 * f) / -std::expm1(-2.);
    const std::array<double, 4> expected{std::exp(-2 * f), 4 * beer, -3 * beer, beer};
    for (int c = 0; c < 4; c++)
      EXPECT_LE(std::abs(actual[c] - expected[c]), curve.representation_error()[c] + 2e-12);
  }
}

TEST(DeepFrameProduction, native_capture_cannot_change_frozen_depth_transform_or_kind_after_begin)
{
  const std::array<uint32_t, 1> consumers{1};
  for (int mutation = 0; mutation < 2; mutation++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 8 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 981);
    ASSERT_TRUE(begin_reference_capture(production, packet.domain));
    if (mutation)
      packet.domain.depth_kind = DeepFrozenDepthKind::Radial;
    else
      packet.domain.depth_transform[3] = 1;
    EXPECT_FALSE(complete_reference_capture(production, std::move(packet)));
    EXPECT_EQ(production.error().code, DeepErrorCode::InvalidReference);
  }
}

TEST(DeepFrameProduction,
     finite_nonzero_camera_sensitivity_never_changes_population_or_optical_mass)
{
  const std::array<uint32_t, 1> consumers{1};
  for (const std::array<float, 3> sensitivity :
       {std::array<float, 3>{.5, .5, .5}, {2, 2, 2}, {0, 1.5, -.25}})
  {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 16 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 985);
    for (int c = 0; c < 3; c++) {
      packet.cameras[0].throughput[c] = sensitivity[c];
      packet.homogeneous[0].pre_clamp_rgb[c] *= sensitivity[c];
      packet.homogeneous[0].post_clamp_rgb[c] *= sensitivity[c];
    }
    ASSERT_TRUE(admit_packet(production, std::move(packet))) << production.error().message;
    const std::array<uint32_t, 1> counts{1};
    ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1))
        << production.error().message;
    const auto &curve = production.curves()[0];
    EXPECT_EQ(curve.native_population(), 1u);
    const auto value = prefix(read_knots(curve), 1000);
    EXPECT_NEAR(value[0], std::exp(-2.), 1e-12);
    EXPECT_NEAR(value[1], 4 * sensitivity[0], 1e-10);
    EXPECT_NEAR(value[2], -3 * sensitivity[1], 1e-10);
    EXPECT_NEAR(value[3], sensitivity[2], 1e-10);
  }
}

TEST(DeepFrameProduction, zero_or_nonfinite_sensitivity_cannot_be_mislabeled_as_a_valid_ray)
{
  const std::array<uint32_t, 1> consumers{1};
  for (int mode = 0; mode < 2; mode++) {
    DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, 8 << 20, consumers);
    auto packet = native_packet(production.memory_budget(), 986);
    for (float &value : packet.cameras[0].throughput)
      value = 0;
    if (mode)
      packet.cameras[0].throughput[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(admit_packet(production, std::move(packet)));
  }
}

TEST(DeepFrameProduction, decreasing_native_radial_branch_is_named_before_optical_dispatch)
{
  DeepMemoryBudget budget(1 << 20);
  auto packet = native_packet(budget, 987);
  packet.domain.depth_kind = DeepFrozenDepthKind::Radial;
  packet.domain.require_full_optical = true;
  auto &camera = packet.cameras[0];
  camera.flags &= ~DEEP_NATIVE_CAMERA_AFFINE_DEPTH;
  camera.flags |= DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
  camera.z_origin = camera.z_direction = 0;
  camera.origin[2] = 1;
  camera.direction[2] = -1;
  std::string error;
  EXPECT_FALSE(deep_native_capture_validate(packet, error, true));
  EXPECT_NE(error.find("NonMonotonicCameraDepth"), std::string::npos);
}

TEST(DeepFrameProduction, world_span_boundary_quantization_rejects_nonradial_or_large_overflow)
{
  const double maximum = std::numeric_limits<float>::max();
  float z;
  ASSERT_TRUE(deep_native_world_span_depth(maximum, true, z));
  EXPECT_EQ(z, float(maximum));
  ASSERT_TRUE(deep_native_world_span_depth(
      maximum * (1 + double(std::numeric_limits<float>::epsilon())), true, z));
  EXPECT_EQ(z, float(maximum));
  EXPECT_FALSE(deep_native_world_span_depth(
      maximum * (1 + double(std::numeric_limits<float>::epsilon())), false, z));
  EXPECT_FALSE(deep_native_world_span_depth(maximum * 1.01, true, z));
  EXPECT_FALSE(deep_native_world_span_depth(std::numeric_limits<double>::infinity(), true, z));
  EXPECT_FALSE(deep_native_world_span_depth(std::numeric_limits<double>::quiet_NaN(), true, z));
}

TEST(DeepFrameProduction, nonzero_camera_with_nonfinite_actual_differential_fails_by_name)
{
  DeepMemoryBudget budget(1 << 20);
  for (bool position : {false, true}) {
    auto packet = native_packet(budget, 988);
    if (position)
      packet.cameras[0].dP = std::numeric_limits<float>::infinity();
    else
      packet.cameras[0].dD = std::numeric_limits<float>::quiet_NaN();
    std::string error;
    EXPECT_FALSE(deep_native_capture_validate(packet, error));
    EXPECT_NE(error.find("NonfiniteNativeCameraDifferential"), std::string::npos);
  }
}

CCL_NAMESPACE_END

CCL_NAMESPACE_BEGIN

TEST(DeepFrameProduction, row_owner_survives_production_and_releases_tiles)
{
  DeepMemoryBudget budget(32 << 20);
  const size_t baseline = budget.used();
  {
    DeepEncodedRowLease row(budget);
    DeepEncodedPayload expected;
    {
      const std::array<uint32_t, 1> ids{1};
      DeepFrameProduction production(homogeneous_frame(), {-96, -17, 1, 1}, budget, ids, false);
      const std::array<DeepFrozenConsumer, 1> consumers{
          {{1, DEEP_DIRECT_CONSUMER_KEY, 1, 2, {}}}};
      ASSERT_TRUE(production.configure_consumers(consumers));
      ASSERT_TRUE(admit_packet(production, native_packet(budget)));
      const std::array<uint32_t, 1> counts{1};
      ASSERT_TRUE(production.reference_capture()->close_native_tile(-96, -17, 1, 1, counts, 1));
      ASSERT_TRUE(production.finish_inputs());
      DeepConsumerToken token;
      ASSERT_TRUE(production.consumer_token(DEEP_DIRECT_CONSUMER_KEY, {}, 2, token));
      ASSERT_TRUE(production.encoded_row(token, 0, 1, row));
      ASSERT_GT(row.pixels()[0].size(), 0u);
      expected = row.pixels()[0].sample(0);
    }
    EXPECT_EQ(row.pixels()[0].sample(0).a, expected.a);
    EXPECT_EQ(row.pixels()[0].sample(0).r, expected.r);
    const size_t retained = budget.used();
    row.release();
    EXPECT_TRUE(row.pixels().empty());
    EXPECT_LT(budget.used(), retained);
  }
  EXPECT_EQ(budget.used(), baseline);
}

TEST(DeepFrameProduction, receive_overwrite_storage_keeps_quota_and_cleanup)
{
  for (const bool overwrite : {false, true}) {
    DeepMemoryBudget budget(4096);
    const size_t baseline = budget.used();
    {
      DeepBoundedHostPayload payload(budget, overwrite);
      payload.offsets.resize(5);
      payload.planes.resize(512);
      if (!overwrite) {
        for (size_t i = 0; i < 512; ++i) EXPECT_EQ(payload.planes[i], 0u);
      }
      for (size_t i = 0; i < 5; ++i) payload.offsets[i] = i * 32;
      for (size_t i = 0; i < 512; ++i) payload.planes[i] = uint8_t(i);
      EXPECT_EQ(payload.offsets.back(), 128u);
      EXPECT_EQ(payload.planes[511], 255u);
      EXPECT_EQ(budget.used(), baseline + 5 * sizeof(uint64_t) + 512);
      EXPECT_THROW(payload.planes.resize(8192), std::bad_alloc);
      EXPECT_EQ(payload.planes.size(), 512u);
      EXPECT_EQ(payload.planes[511], 255u);
    }
    EXPECT_EQ(budget.used(), baseline);
  }
}

CCL_NAMESPACE_END
