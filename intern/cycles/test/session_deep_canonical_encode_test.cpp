/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "session/deep_canonical_encode.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
using TestValue = std::array<double, 4>;
const std::array<double, 4> no_representation_error = {};

DeepFrameKey encode_frame()
{
  return {1, 2, 3, 4, 2, 0, 5, 6};
}

std::vector<DeepDepthKnot> polyline(const std::vector<double> &depths,
                                    const std::vector<TestValue> &values)
{
  std::vector<DeepDepthKnot> knots(depths.size());
  for (size_t i = 0; i < knots.size(); i++) {
    auto &knot = knots[i];
    knot.z = depths[i];
    knot.source_group = i + 1;
    knot.flags = DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_INTERVAL_BOUNDARY;
    for (int c = 0; c < 4; c++) {
      knot.before[c] = knot.after[c] = values[i][c];
      if (i + 1 < knots.size())
        knot.slope_right[c] = (values[i + 1][c] - values[i][c]) / (depths[i + 1] - depths[i]);
    }
  }
  return knots;
}

std::vector<DeepPageLease> publish(DeepPageStore &store, const std::vector<DeepDepthKnot> &knots)
{
  std::vector<DeepPageLease> pages;
  if (knots.empty())
    return pages;
  uint32_t page = 0;
  EXPECT_TRUE(store.allocate(DeepRecordKind::DepthKnot, page));
  for (size_t i = 0; i < knots.size(); i++) {
    uint64_t handle = 0;
    DeepStatus status = store.reserve(page, handle);
    if (status.code == DeepErrorCode::NeedPage) {
      DeepPageLease lease;
      EXPECT_TRUE(store.seal(page, lease));
      EXPECT_TRUE(store.retire(page));
      pages.push_back(lease);
      EXPECT_TRUE(store.allocate(DeepRecordKind::DepthKnot, page));
      status = store.reserve(page, handle);
    }
    EXPECT_TRUE(status) << status.message;
    DeepRecordTag tag{1, 1, 0, uint32_t(i), uint32_t(DeepRecordKind::DepthKnot), 0};
    const std::span<const uint8_t> payload{reinterpret_cast<const uint8_t *>(&knots[i]),
                                           sizeof(DeepDepthKnot)};
    EXPECT_TRUE(store.write(handle, tag, payload));
    EXPECT_TRUE(store.commit(handle));
  }
  DeepPageLease lease;
  EXPECT_TRUE(store.seal(page, lease));
  EXPECT_TRUE(store.retire(page));
  pages.push_back(lease);
  return pages;
}

/* Independent consumer oracle: ordinary scalar over plus a power-law split.
 * This intentionally does not call any encoder helper or legacy fitter. */
TestValue decode(const std::span<const DeepCanonicalEncodedSample> samples,
                 const double depth,
                 const bool after_points = true)
{
  std::array<long double, 4> accumulated = {1, 0, 0, 0};
  for (const auto &sample : samples) {
    if (sample.z > depth || (sample.z == depth && sample.z_back == sample.z && !after_points))
      break;
    long double fraction = 1;
    if (sample.z_back > sample.z) {
      const long double f = std::clamp((static_cast<long double>(depth) - sample.z) /
                                           (static_cast<long double>(sample.z_back) - sample.z),
                                       0.0L,
                                       1.0L);
      fraction = sample.a == 0 ?
                     f :
                     (1 - std::pow(1 - static_cast<long double>(sample.a), f)) / sample.a;
    }
    const long double t = accumulated[0];
    accumulated[1] += t * sample.r * fraction;
    accumulated[2] += t * sample.g * fraction;
    accumulated[3] += t * sample.b * fraction;
    accumulated[0] *= 1 - sample.a * fraction;
    if (sample.z_back > depth)
      break;
  }
  return {double(accumulated[0]),
          double(accumulated[1]),
          double(accumulated[2]),
          double(accumulated[3])};
}

TestValue reference(const std::vector<DeepDepthKnot> &knots,
                    const double z,
                    const bool after_points = true)
{
  if (knots.empty() || z < knots.front().z)
    return {1, 0, 0, 0};
  for (size_t i = 0; i < knots.size(); i++) {
    TestValue result;
    if (z == knots[i].z) {
      for (int c = 0; c < 4; c++)
        result[c] = after_points ? knots[i].after[c] : knots[i].before[c];
      return result;
    }
    if (i + 1 == knots.size() || z < knots[i + 1].z) {
      for (int c = 0; c < 4; c++)
        result[c] = knots[i].after[c] + knots[i].slope_right[c] * (z - knots[i].z);
      return result;
    }
  }
  return {};
}

void verify_prefixes(const DeepCanonicalEncoded &encoded,
                     const std::vector<DeepDepthKnot> &knots,
                     const DeepCanonicalEncodeConfig &config)
{
  ASSERT_TRUE(encoded);
  ASSERT_NE(encoded.report(), nullptr);
  EXPECT_TRUE(encoded.report()->normalized_checked);
  EXPECT_TRUE(encoded.report()->quantized_checked);
  EXPECT_EQ(encoded.samples().size(), encoded.report()->output_samples);
  TestValue observed = {};
  const auto check = [&](const double z, const bool after_points) {
    const TestValue expected = reference(knots, z, after_points);
    const TestValue actual = decode(encoded.samples(), z, after_points);
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
      observed[c] = std::max(observed[c], std::abs(actual[c] - expected[c]));
      const double allowed = c == 0 ? config.alpha_tolerance :
                                      config.rgb_absolute_tolerance +
                                          config.rgb_relative_tolerance *
                                              std::max(1.0, std::abs(expected[c]));
      EXPECT_LE(std::abs(actual[c] - expected[c]), allowed + 1e-10);
    }
  };
  for (const auto &knot : knots) {
    check(knot.z, false);
    check(knot.z, true);
  }
  for (size_t i = 1; i < knots.size(); i++)
    for (int j = 1; j < 1000; j++)
      check(knots[i - 1].z + (knots[i].z - knots[i - 1].z) * (j / 1000.0), true);
  for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
    EXPECT_LE(observed[c], encoded.report()->encoding_error[c] + 1e-10);
    if (c == 0 || config.rgb_relative_tolerance == 0)
      EXPECT_LE(encoded.report()->total_error[c],
                c == 0 ? config.alpha_tolerance : config.rgb_absolute_tolerance);
    EXPECT_LE(encoded.report()->maximum_budget_ratio[c], 1);
  }
  for (const auto &sample : encoded.samples()) {
    EXPECT_LE(sample.z, sample.z_back);
    if (sample.z_back > sample.z)
      EXPECT_LT(sample.a, 1);
  }
}
}  // namespace

TEST(DeepCanonicalEncode, continuous_exponential_decoder_bound_is_not_endpoint_only)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  auto knots = polyline({2, 6}, {{1, 0, 0, 0}, {0.05, 0.75, -0.3, 1.2}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  config.precision = DeepCanonicalPrecision::Float;
  config.alpha_tolerance = 0.002;
  config.rgb_absolute_tolerance = 0.002;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_GT(encoded.report()->subdivisions, 0u);
  EXPECT_GT(encoded.samples().size(), 1u);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, protected_narrow_hdr_peak_survives_same_endpoints)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  const double h = std::ldexp(1.0, -16);
  auto knots = polyline({1, 1 + h, 1 + 2 * h}, {{1, 0, 0, 0}, {1, 1000, -500, 0}, {1, 0, 0, 0}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded));
  ASSERT_EQ(encoded.samples().size(), 2u);
  EXPECT_EQ(encoded.samples()[0].z_back, float(1 + h));
  EXPECT_DOUBLE_EQ(decode(encoded.samples(), 1 + h)[1], 1000);
  EXPECT_DOUBLE_EQ(decode(encoded.samples(), 1 + 2 * h)[1], 0);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, hdr_signed_point_is_split_before_its_opacity_jump)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  auto knots = polyline({2, 4}, {{1, 0, 0, 0}, {0.5, 200000, -150000, 17.3}});
  for (int c = 0; c < 4; c++) {
    knots[0].after[c] = knots[1].before[c] = knots[1].after[c];
    knots[0].slope_right[c] = 0;
  }
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_GT(encoded.samples().size(), 3u);
  for (const auto &sample : encoded.samples()) {
    EXPECT_LE(std::abs(sample.r), 65504);
    EXPECT_LE(std::abs(sample.g), 65504);
  }
  EXPECT_NEAR(decode(encoded.samples(), 2)[1], 200000, 0.006);
  EXPECT_NEAR(decode(encoded.samples(), 2)[2], -150000, 0.006);
  EXPECT_DOUBLE_EQ(decode(encoded.samples(), 2, false)[1], 0);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, half_point_rounding_does_not_create_radiance_in_following_vacuum)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  auto knots = polyline({2, 1000}, {{1, 0, 0, 0}, {1, 0.813429892, -0.37329135, 0.12345678}});
  for (int c = 0; c < 4; c++) {
    knots[0].after[c] = knots[1].before[c] = knots[1].after[c];
    knots[0].slope_right[c] = 0;
  }
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  ASSERT_EQ(encoded.samples().size(), 1u);
  EXPECT_EQ(encoded.samples()[0].z, 2);
  EXPECT_EQ(encoded.samples()[0].z_back, 2);
  EXPECT_EQ(encoded.samples()[0].a, 0);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, half_point_rounding_does_not_create_rgb_in_opacity_only_support)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  auto knots = polyline({2, 3}, {{1, 0, 0, 0}, {0.5, 0.813429892, -0.37329135, 0.12345678}});
  for (int c = 1; c < 4; c++) {
    knots[0].after[c] = knots[1].before[c] = knots[1].after[c];
    knots[0].slope_right[c] = 0;
  }
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  ASSERT_GT(encoded.samples().size(), 1u);
  for (const auto &sample : encoded.samples()) {
    if (sample.z_back > sample.z) {
      EXPECT_EQ(sample.r, 0);
      EXPECT_EQ(sample.g, 0);
      EXPECT_EQ(sample.b, 0);
    }
  }
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, zero_alpha_interval_preserves_throughput_weighted_rgb)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  auto knots = polyline({1, 3}, {{1, 0, 0, 0}, {1, 1000, -300, 0.5}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded));
  ASSERT_EQ(encoded.samples().size(), 1u);
  EXPECT_EQ(encoded.samples()[0].a, 0);
  EXPECT_DOUBLE_EQ(decode(encoded.samples(), 2)[1], 500);
  EXPECT_DOUBLE_EQ(decode(encoded.samples(), 3)[2], -300);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, half_near_opaque_interval_keeps_later_emission_visible)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  auto knots = polyline({1, 2, 3}, {{1, 0, 0, 0}, {1e-6, 0, 0, 0}, {1e-6, 1, -0.5, 0.25}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  config.alpha_tolerance = 0.1;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  for (const auto &sample : encoded.samples())
    EXPECT_LT(sample.a, 1);
  EXPECT_NEAR(decode(encoded.samples(), 3)[1], 1, 0.006);
  EXPECT_NEAR(decode(encoded.samples(), 3)[2], -0.5, 0.006);
  verify_prefixes(encoded, knots, config);
}

TEST(DeepCanonicalEncode, low_and_high_tau_have_checked_interior_prefixes)
{
  for (const double terminal : {1 - 1e-9, 1e-30}) {
    DeepPageStore store(encode_frame(), 1 << 23, 4096);
    auto knots = polyline({1, 2}, {{1, 0, 0, 0}, {terminal, 0, 0, 0}});
    const auto pages = publish(store, knots);
    DeepCanonicalEncodeConfig config;
    config.alpha_only = true;
    config.alpha_tolerance = 0.001;
    DeepCanonicalEncoded encoded;
    const auto status = deep_encode_canonical_knots(
        store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
    ASSERT_TRUE(status) << status.message;
    verify_prefixes(encoded, knots, config);
  }
}

TEST(DeepCanonicalEncode, positive_thin_interval_cannot_be_fabricated_as_a_point)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  auto knots = polyline({1, 1 + 1e-10}, {{1, 0, 0, 0}, {0.8, 1, 0, 0}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message.find("DepthNotRepresentable"), std::string::npos);
  EXPECT_FALSE(encoded);
}

TEST(DeepCanonicalEncode, adjacent_float_depths_fail_if_continuous_fit_is_impossible)
{
  DeepPageStore store(encode_frame(), 1 << 23, 4096);
  const double back = std::nextafter(1.0f, 2.0f);
  auto knots = polyline({1, back}, {{1, 0, 0, 0}, {0.01, 0, 0, 0}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  config.alpha_tolerance = 0.001;
  config.alpha_only = true;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message.find("DepthNotRepresentable"), std::string::npos);
  EXPECT_FALSE(encoded);
}

TEST(DeepCanonicalEncode, long_curve_crosses_pages_and_has_no_max64_cap)
{
  DeepPageStore store(encode_frame(), 1 << 24, 1024);
  std::vector<double> depths;
  std::vector<TestValue> values;
  for (int i = 0; i < 140; i++) {
    depths.push_back(i);
    values.push_back({1, i == 0 ? 0.0 : (i % 2 ? 1.0 : -1.0), 0, 0});
  }
  const auto knots = polyline(depths, values);
  const auto pages = publish(store, knots);
  ASSERT_GT(pages.size(), 1u);
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded));
  EXPECT_EQ(encoded.samples().size(), 139u);
  EXPECT_GT(encoded.samples().size(), 64u);
  verify_prefixes(encoded, knots, {});
}

TEST(DeepCanonicalEncode, alpha_consumers_independently_use_actual_tolerance_and_ignore_rgb)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  const auto knots = polyline({1, 2}, {{1, 0, 0, 0}, {0.01, 1e100, -1e100, 1e90}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig loose, tight;
  loose.alpha_only = tight.alpha_only = true;
  loose.alpha_tolerance = 0.1;
  tight.alpha_tolerance = 0.01;
  const std::array<double, 4> upstream = {1e-4, 1, 1, 1};
  DeepCanonicalEncoded first, second;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), upstream, loose, first));
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), upstream, tight, second));
  EXPECT_GT(second.samples().size(), first.samples().size());
  for (const auto &sample : second.samples())
    EXPECT_EQ(sample.r, 0);
  verify_prefixes(first, knots, loose);
  verify_prefixes(second, knots, tight);
  DeepCanonicalEncodeConfig explicit_zero = tight;
  explicit_zero.alpha_tolerance = 0;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), upstream, explicit_zero, second);
  EXPECT_FALSE(status);
  EXPECT_NE(status.message.find("InsufficientErrorBudget"), std::string::npos);
  EXPECT_FALSE(second);
  EXPECT_TRUE(first);
}

TEST(DeepCanonicalEncode, count_and_shared_quota_failure_clear_output_without_leaking)
{
  DeepPageStore store(encode_frame(), 1 << 20, 4096);
  const auto knots = polyline({1, 2}, {{1, 0, 0, 0}, {1, 1, 0, 0}});
  const auto pages = publish(store, knots);
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded));
  const auto wrong_count = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size() + 1, no_representation_error, {}, encoded);
  EXPECT_EQ(wrong_count.code, DeepErrorCode::InvalidCount);
  EXPECT_FALSE(encoded);
  const size_t baseline = store.memory_budget().used();
  const size_t reservation = store.memory_budget().quota() - baseline - 64;
  store.memory_budget().acquire(reservation);
  const auto exhausted = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded);
  EXPECT_EQ(exhausted.code, DeepErrorCode::BudgetExceeded);
  EXPECT_FALSE(encoded);
  EXPECT_EQ(store.memory_budget().used(), baseline + reservation);
  store.memory_budget().relinquish(reservation);
  EXPECT_EQ(store.memory_budget().used(), baseline);
}

TEST(DeepCanonicalEncode, homogeneous_owned_curve_has_native_rgb_without_second_attenuation)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  DeepHomogeneousSegment segment{1, 3, 2, {0.7, -0.2, 0.1}, 7};
  DeepHomogeneousRay ray;
  ray.ray = 1;
  ray.native_complete = ray.coverage_complete = ray.homogeneous_certified = true;
  ray.affine_camera_depth = true;
  ray.coverage_front = 1;
  ray.coverage_back = 3;
  ray.segments = {&segment, 1};
  DeepHomogeneousPixelInput input;
  input.frame = encode_frame();
  input.native_population = 1;
  input.population_complete = true;
  input.exposure = 2;
  input.rays = {&ray, 1};
  DeepHomogeneousCurve curve;
  ASSERT_TRUE(deep_build_homogeneous_curve(store, input, curve));
  DeepCanonicalEncodeConfig config;
  config.precision = DeepCanonicalPrecision::Float;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_curve(store.memory_budget(), curve, config, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_TRUE(encoded.is_charged_to(store.memory_budget()));
  EXPECT_NEAR(decode(encoded.samples(), 3)[0], std::exp(-2.0), 0.1);
  EXPECT_NEAR(decode(encoded.samples(), 3)[1], 1.4, 0.006);
  EXPECT_NEAR(decode(encoded.samples(), 3)[2], -0.4, 0.006);
  EXPECT_LE(encoded.report()->representation_error[0], 1e-4);
  EXPECT_LE(encoded.report()->representation_error[1], 1e-4);
}

TEST(DeepCanonicalEncode, joins_numeric_microsteps_against_the_complete_quantized_reference)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  std::vector<double> depths;
  std::vector<TestValue> values;
  for (int i = 0; i <= 256; i++) {
    const double z = i / 256.0, t = std::exp(-0.75 * z);
    depths.push_back(z);
    values.push_back({t, .8 * (1 - t), -.2 * (1 - t), .4 * (1 - t)});
  }
  auto knots = polyline(depths, values);
  for (size_t i = 1; i + 1 < knots.size(); i++)
    knots[i].flags = DEEP_KNOT_INTERVAL_BOUNDARY;
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  config.alpha_tolerance = .01;
  DeepCanonicalEncoded joined, unjoined;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, joined));
  EXPECT_LE(joined.samples().size(), 2u);
  EXPECT_GT(joined.report()->merged_intervals, 0u);
  verify_prefixes(joined, knots, config);
  config.depth_tolerance = 0;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, unjoined));
  EXPECT_GT(unjoined.samples().size(), joined.samples().size());
  EXPECT_EQ(unjoined.report()->merged_intervals, 0u);
}

TEST(DeepCanonicalEncode, cross_knot_join_cannot_hide_signed_peaks_with_equal_endpoints)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  auto knots = polyline({0, 1, 2, 3, 4},
                        {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 0, 0}, {1, -1, 0, 0}, {1, 0, 0, 0}});
  for (auto &k : knots)
    k.flags = DEEP_KNOT_INTERVAL_BOUNDARY;
  const auto pages = publish(store, knots);
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded));
  EXPECT_GE(encoded.samples().size(), 3u);
  verify_prefixes(encoded, knots, {});
  EXPECT_NEAR(decode(encoded.samples(), 2)[1], 1, .006);
  EXPECT_NEAR(decode(encoded.samples(), 3)[1], -1, .006);
}

TEST(DeepCanonicalEncode, protected_semantic_boundary_survives_a_smooth_numeric_fit)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  auto knots = polyline(
      {0, .25, .5, .75, 1},
      {{1, 0, 0, 0}, {.9, 0, 0, 0}, {.8, 0, 0, 0}, {.7, 0, 0, 0}, {.6, 0, 0, 0}});
  for (auto &k : knots)
    k.flags = DEEP_KNOT_INTERVAL_BOUNDARY;
  knots[2].flags |= DEEP_KNOT_PROTECTED_CUT;
  const auto pages = publish(store, knots);
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, {}, encoded));
  for (const auto &sample : encoded.samples())
    EXPECT_FALSE(sample.z < .5f && sample.z_back > .5f);
  verify_prefixes(encoded, knots, {});
}

TEST(DeepCanonicalEncode, relative_hdr_budget_is_local_and_preserves_absolute_error_reporting)
{
  DeepPageStore store(encode_frame(), 1 << 24, 4096);
  auto knots = polyline({1, 2}, {{1, 0, 0, 0}, {1, 20000.125, -30000.25, 0}});
  knots[0].after[1] = 20000.125;
  knots[0].after[2] = -30000.25;
  knots[0].slope_right[1] = knots[0].slope_right[2] = 0;
  knots[0].flags |= DEEP_KNOT_POINT_RADIANCE;
  const auto pages = publish(store, knots);
  DeepCanonicalEncoded encoded;
  DeepCanonicalEncodeConfig config;
  EXPECT_FALSE(deep_encode_canonical_knots(store.memory_budget(), pages, knots.size(),
                                           no_representation_error, config, encoded));
  config.rgb_relative_tolerance = .002;
  ASSERT_TRUE(deep_encode_canonical_knots(store.memory_budget(), pages, knots.size(),
                                          no_representation_error, config, encoded));
  EXPECT_GT(encoded.report()->total_error[1], config.rgb_absolute_tolerance);
  EXPECT_LE(encoded.report()->maximum_budget_ratio[1], 1);
  verify_prefixes(encoded, knots, config);
  const std::array<double, 4> upstream{0, .007, .007, .007};
  ASSERT_TRUE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), upstream, config, encoded));
  EXPECT_LE(encoded.report()->maximum_budget_ratio[1], 1);
  config.rgb_relative_tolerance = -.002;
  EXPECT_FALSE(deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded));
}

TEST(DeepCanonicalEncode, hdr_endpoints_cannot_relax_a_signed_zero_crossing)
{
  DeepPageStore store(encode_frame(), 1 << 25, 4096);
  auto knots = polyline({0, 1, 2}, {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, -20000.125, 0, 0}});
  knots[0].after[1] = 20000.125;
  knots[0].slope_right[1] = -20000.125;
  knots[0].flags |= DEEP_KNOT_POINT_RADIANCE;
  knots[1].flags = DEEP_KNOT_INTERVAL_BOUNDARY;
  const auto pages = publish(store, knots);
  DeepCanonicalEncodeConfig config;
  config.rgb_relative_tolerance = .002;
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(
      store.memory_budget(), pages, knots.size(), no_representation_error, config, encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_LE(std::abs(decode(encoded.samples(), 1)[1]), .008);
  EXPECT_GT(encoded.samples().size(), 1u);
  verify_prefixes(encoded, knots, config);
}

CCL_NAMESPACE_END
