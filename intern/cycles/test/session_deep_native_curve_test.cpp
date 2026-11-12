/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "session/deep_canonical_encode.h"
#include "session/deep_native_curve.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
DeepFrameKey frame()
{
  return {811, 2, 3, 4, 2, 0, 1, 1};
}
DeepNativeCurveRay ray(uint64_t id,
                       uint32_t sample,
                       std::span<const DeepCurveOptical> optical,
                       std::span<const DeepCurveVisibility> visibility = {},
                       std::span<const DeepCurveRadiance> radiance = {})
{
  DeepNativeCurveRay r;
  r.ray = id;
  r.sample = {-96, -17, sample, 17, 99, 47, DeepCameraDisposition::Ray, sample + 17};
  r.native_complete = r.optical_complete = r.affine_camera_depth = true;
  r.optical = optical;
  r.visibility = visibility;
  r.radiance = radiance;
  if (!optical.empty()) {
    r.coverage_front = optical.front().z;
    r.coverage_back = optical.back().z_back;
  }
  return r;
}
DeepNativeCurvePixel pixel(std::span<const DeepNativeCurveRay> rays)
{
  DeepNativeCurvePixel p;
  p.frame = frame();
  p.x = -96;
  p.y = -17;
  p.native_population = rays.size();
  p.population_complete = true;
  p.rays = rays;
  return p;
}
std::vector<DeepDepthKnot> read(const DeepNativeCurve &curve)
{
  std::vector<DeepDepthKnot> knots(curve.knot_count());
  for (size_t i = 0; i < knots.size(); i++)
    EXPECT_TRUE(curve.read_knot(i, knots[i]));
  return knots;
}
std::array<double, 4> at(std::span<const DeepDepthKnot> knots, double z, bool before = false)
{
  std::array<double, 4> value{1, 0, 0, 0};
  for (const auto &k : knots) {
    if (k.z > z)
      break;
    for (int c = 0; c < 4; c++)
      value[c] = k.z == z ? (before ? k.before[c] : k.after[c]) :
                            k.after[c] + k.slope_right[c] * (z - k.z);
  }
  return value;
}

/* Independent reference walks visibility using optical integrals, then evaluates
 * each native source from its original support. It uses neither sparse events,
 * adaptive chord refinement nor the canonical encoder's acceptance code. */
std::array<double, 4> reference(const DeepNativeCurvePixel &p, double z, bool before = false)
{
  std::array<double, 4> result = {};
  double W = 0;
  for (const auto &r : p.rays) {
    W += r.population_weight;
    if (r.sample.disposition == DeepCameraDisposition::ClearTransparent) {
      result[0] += r.population_weight;
      continue;
    }
    if (r.sample.disposition == DeepCameraDisposition::InvalidCameraBlack) {
      result[0] += r.population_weight *
                   (z < double(std::numeric_limits<float>::max()) || before ? 1 : 0);
      continue;
    }
    std::vector<const DeepCurveVisibility *> points;
    for (const auto &s : r.visibility)
      points.push_back(&s);
    std::sort(points.begin(), points.end(), [](auto a, auto b) {
      return a->z == b->z ? a->ordinal < b->ordinal : a->z < b->z;
    });
    long double v[3] = {1, 1, 1}, h[3] = {}, cursor = 0;
    const auto attenuate = [&](long double end) {
      for (int c = 0; c < 3; c++) {
        long double tau = 0;
        for (const auto &s : r.optical) {
          const long double start = std::max(cursor, static_cast<long double>(s.z));
          const long double finish = std::min(end, static_cast<long double>(s.z_back));
          if (finish > start)
            tau += static_cast<long double>(s.tau[c]) * (finish - start) / (s.z_back - s.z);
        }
        v[c] *= std::exp(-tau);
      }
      cursor = end;
    };
    for (const auto *s : points) {
      if (s->z > z || (before && s->z == z))
        break;
      attenuate(s->z);
      for (int c = 0; c < 3; c++) {
        h[c] += v[c] * s->holdout[c];
        v[c] *= s->transparency[c];
      }
    }
    attenuate(z);
    for (int c = 0; c < 3; c++)
      result[0] += r.population_weight * double(v[c] + h[c]) / 3;
    for (const auto &s : r.radiance) {
      if (s.basis == DeepCurveRadianceBasis::Point) {
        if (z > s.z || (!before && z == s.z))
          for (int c = 0; c < 3; c++)
            result[c + 1] += r.population_weight * p.exposure * s.post_clamp_rgb[c];
      }
      else {
        const long double fraction = std::clamp(
            (static_cast<long double>(z) - s.z) / (s.z_back - s.z), 0.0L, 1.0L);
        for (int c = 0; c < 3; c++) {
          const long double f = s.tau[c] == 0 ?
                                    fraction :
                                    -std::expm1(-s.tau[c] * fraction) /
                                        -std::expm1(-static_cast<long double>(s.tau[c]));
          result[c + 1] += r.population_weight * p.exposure * s.post_clamp_rgb[c] * double(f);
        }
      }
    }
  }
  for (auto &x : result)
    x /= W;
  return result;
}
void verify(const DeepNativeCurve &curve, const DeepNativeCurvePixel &p, double front, double back)
{
  auto knots = read(curve);
  for (int i = 0; i <= 800; i++) {
    const double z = front + (back - front) * i / 800;
    auto actual = at(knots, z), expected = reference(p, z);
    for (int c = 0; c < 4; c++)
      EXPECT_LE(std::abs(actual[c] - expected[c]), curve.representation_error()[c] + 3e-12)
          << "depth=" << z << " component=" << c;
  }
  for (double error : curve.representation_error()) {
    EXPECT_GE(error, 0);
    EXPECT_LE(error, 1e-4);
  }
}
std::array<double, 4> decode(const DeepCanonicalEncoded &output, double z)
{
  std::array<double, 4> result{1, 0, 0, 0};
  for (const auto &s : output.samples()) {
    if (z < s.z)
      break;
    const double f = s.z_back == s.z ? 1 : std::clamp((z - s.z) / (s.z_back - s.z), 0.0, 1.0);
    const double a = f == 1 ? s.a : -std::expm1(f * std::log1p(-double(s.a)));
    const double fraction = s.a == 0 ? f : a / s.a;
    result[1] += result[0] * s.r * fraction;
    result[2] += result[0] * s.g * fraction;
    result[3] += result[0] * s.b * fraction;
    result[0] *= 1 - a;
    if (f < 1)
      break;
  }
  return result;
}
void verify_encoded(const DeepNativeCurve &curve,
                    const DeepNativeCurvePixel &p,
                    const DeepMemoryBudget &budget)
{
  DeepCanonicalEncoded output;
  DeepCanonicalEncodeConfig config;
  const auto status = deep_encode_canonical_knots(
      budget, curve.pages(), curve.knot_count(), curve.representation_error(), config, output);
  ASSERT_TRUE(status) << status.message;
  ASSERT_TRUE(output.report()->quantized_checked);
  for (int i = 0; i <= 500; i++) {
    const double z = 1 + 3.0 * i / 500;
    auto actual = decode(output, z), expected = reference(p, z);
    for (int c = 0; c < 4; c++)
      EXPECT_LE(std::abs(actual[c] - expected[c]), output.report()->total_error[c] + 4e-12);
  }
}
}  // namespace

TEST(DeepNativeCurve, colored_optical_prefix_is_mean_of_channel_transmissions)
{
  std::array<DeepCurveOptical, 2> optical{{{1, 2, {0.2, 1, 2}, 1}, {2, 4, {2, 0.4, 1}, 2}}};
  auto r = ray(1, 0, optical);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 24, 4096);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  verify(curve, p, 0, 5);
  verify_encoded(curve, p, store.memory_budget());
  EXPECT_EQ(curve.optical_sources().size(), 2u);
  auto value = at(read(curve), 4);
  EXPECT_NEAR(value[0], (std::exp(-2.2) + std::exp(-1.4) + std::exp(-3.0)) / 3, 1e-12);
}

TEST(DeepNativeCurve, surface_volume_aa_correlation_is_preserved_before_population_average)
{
  std::array<DeepCurveOptical, 1> absorbing{{{1, 3, {1, 1, 1}, 1}}},
      vacuum{{{1, 3, {0, 0, 0}, 2}}};
  std::array<DeepCurveVisibility, 1> opaque{{{2, {0, 0, 0}, {0, 0, 0}, 3, 1}}};
  std::array<DeepCurveRadiance, 1> point{
      {{DeepCurveRadianceBasis::Point, 2, 2, {0.8, -0.2, 0.3}, {0, 0, 0}, 4, 1}}};
  std::array<DeepNativeCurveRay, 2> rays{ray(1, 0, absorbing, opaque, point), ray(2, 1, vacuum)};
  auto p = pixel(rays);
  DeepPageStore store(p.frame, 1 << 24);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  verify(curve, p, 0, 4);
  verify_encoded(curve, p, store.memory_budget());
  auto value = at(read(curve), 3);
  EXPECT_NEAR(value[0], 0.5, 1e-12);
  EXPECT_NEAR(value[1], 0.4, 1e-12);
  EXPECT_NEAR(value[2], -0.1, 1e-12);  // Actual native source is not attenuated twice.
}

TEST(DeepNativeCurve, partial_and_full_holdout_preserve_hole_mass_after_front_volume)
{
  std::array<DeepCurveOptical, 2> optical{{{1, 2, {0.1, 0.3, 0.5}, 1}, {2, 3, {0.7, 1.2, 2}, 2}}};
  std::array<DeepCurveVisibility, 1> partial{{{2, {0.5, 0.5, 0.5}, {0.25, 0.25, 0.25}, 3, 1}}};
  auto r = ray(1, 0, optical, partial);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 24);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  verify(curve, p, 0, 4);
  verify_encoded(curve, p, store.memory_budget());
  partial[0].transparency[0] = partial[0].transparency[1] = partial[0].transparency[2] = 0;
  partial[0].holdout[0] = partial[0].holdout[1] = partial[0].holdout[2] = 1;
  DeepPageStore second(p.frame, 1 << 24);
  ASSERT_TRUE(deep_build_native_curve(second, p, curve));
  verify(curve, p, 0, 4);
  auto knots = read(curve);
  auto left = at(knots, 2, true), right = at(knots, 2);
  EXPECT_NEAR(left[0], right[0], 1e-12);
  EXPECT_NEAR(at(knots, 3)[0], right[0], 1e-12);
}

TEST(DeepNativeCurve, independent_signed_beer_and_zero_tau_sources_keep_native_depths)
{
  std::array<DeepCurveOptical, 1> optical{{{1, 3, {2, 2, 2}, 1}}};
  std::array<DeepCurveRadiance, 3> terms{
      {{DeepCurveRadianceBasis::BeerSpan, 1, 3, {0.3, -0.4, 0.5}, {0.5, 0.7, 0}, 2, 7},
       {DeepCurveRadianceBasis::Point, 2, 2, {-0.1, 0.2, 0.3}, {0, 0, 0}, 3, 8},
       {DeepCurveRadianceBasis::BeerSpan, 1.5, 2.5, {0.2, 0.4, -0.2}, {1, 1, 1}, 4, 9}}};
  auto r = ray(1, 0, optical, {}, terms);
  auto p = pixel({&r, 1});
  p.exposure = 2;
  DeepPageStore store(p.frame, 1 << 25);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  verify(curve, p, 0, 4);
  verify_encoded(curve, p, store.memory_budget());
  auto end = at(read(curve), 3);
  EXPECT_NEAR(end[1], 0.8, 1e-12);
  EXPECT_NEAR(end[2], 0.4, 1e-12);
  EXPECT_NEAR(end[3], 1.2, 1e-12);
  EXPECT_EQ(curve.radiance_sources().size(), 3u);
  EXPECT_EQ(curve.radiance_sources()[0].support.tau[2], 0);
}

TEST(DeepNativeCurve, large_finite_tau_retains_early_prefix_and_full_owned_tau)
{
  std::array<DeepCurveOptical, 1> optical{{{1, 2, {1000, 1000, 1000}, 1}}};
  auto r = ray(1, 0, optical);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 24);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(curve.optical_sources()[0].support.tau[0], 1000);
  EXPECT_NEAR(at(read(curve), 1.001)[0], std::exp(-1), 1e-4);
  EXPECT_NEAR(at(read(curve), 1.002)[0], std::exp(-2), 1e-4);
  EXPECT_EQ(at(read(curve), 2)[0], 0);
}

TEST(DeepNativeCurve, world_terminal_preserves_holdout_and_native_world_rgb)
{
  const double world = std::numeric_limits<float>::max();
  std::array<DeepCurveOptical, 1> optical{{{1, 2, {1, 1, 1}, 1}}};
  std::array<DeepCurveVisibility, 2> visibility{
      {{2, {0.5, 0.5, 0.5}, {0.25, 0.25, 0.25}, 2, 1},
       {world, {0, 0, 0}, {0, 0, 0}, 3, 2, DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_WORLD_TERMINAL}}};
  std::array<DeepCurveRadiance, 1> background{
      {{DeepCurveRadianceBasis::Point,
        world,
        world,
        {0.123, 0.1, -0.02},
        {0, 0, 0},
        4,
        1,
        1,
        DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_WORLD_TERMINAL}}};
  auto r = ray(1, 0, optical, visibility, background);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 24);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  auto knots = read(curve);
  auto v = at(knots, world);
  EXPECT_NEAR(v[0], 0.25 * std::exp(-1), 1e-12);
  EXPECT_NEAR(v[1], 0.123, 1e-12);
  EXPECT_TRUE(knots.back().flags & DEEP_KNOT_WORLD_TERMINAL);
  DeepCanonicalEncoded encoded;
  auto encode = deep_encode_canonical_knots(store.memory_budget(),
                                            curve.pages(),
                                            curve.knot_count(),
                                            curve.representation_error(),
                                            {},
                                            encoded);
  ASSERT_TRUE(encode) << encode.message;
  EXPECT_EQ(encoded.samples().back().z, float(world));
}

TEST(DeepNativeCurve, clear_and_invalid_camera_keep_complete_population_without_geometry)
{
  std::array<DeepNativeCurveRay, 2> rays{ray(1, 0, {}), ray(2, 1, {})};
  rays[0].sample.disposition = DeepCameraDisposition::ClearTransparent;
  rays[1].sample.disposition = DeepCameraDisposition::InvalidCameraBlack;
  for (auto &r : rays) {
    r.coverage_front = r.coverage_back = std::numeric_limits<double>::quiet_NaN();
    r.affine_camera_depth = false;
    r.optical_complete = false;
  }
  auto p = pixel(rays);
  DeepPageStore store(p.frame, 1 << 20);
  DeepNativeCurve curve;
  ASSERT_TRUE(deep_build_native_curve(store, p, curve));
  EXPECT_EQ(curve.native_population(), 2u);
  EXPECT_NEAR(at(read(curve), std::numeric_limits<float>::max())[0], 0.5, 1e-12);
  EXPECT_TRUE(read(curve).back().flags & DEEP_KNOT_INVALID_CAMERA_BLACK);
}

TEST(DeepNativeCurve, weak_layers_cross_pages_without_cap_or_endpoint_alpha_reconstruction)
{
  std::vector<DeepCurveOptical> optical;
  for (int i = 0; i < 180; i++)
    optical.push_back({double(i + 1), double(i + 2), {1e-7, 2e-7, 3e-7}, uint64_t(i + 1)});
  auto r = ray(1, 0, optical);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 24, 512);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(curve.optical_sources().size(), 180u);
  EXPECT_GT(curve.pages().size(), 1u);
  EXPECT_GT(curve.knot_count(), 64u);
  verify(curve, p, 0, 182);
}

TEST(DeepNativeCurve, native_identity_coverage_and_unresolved_source_fail_closed)
{
  std::array<DeepCurveOptical, 2> optical{{{1, 2, {1, 1, 1}, 1}, {2, 3, {1, 1, 1}, 2}}};
  auto r = ray(1, 0, optical);
  auto p = pixel({&r, 1});
  DeepNativeCurve curve;
  {
    DeepPageStore s(p.frame, 1 << 20);
    p.population_complete = false;
    EXPECT_EQ(deep_build_native_curve(s, p, curve).code, DeepErrorCode::Incomplete);
    p.population_complete = true;
  }
  {
    DeepPageStore s(p.frame, 1 << 20);
    r.native_complete = false;
    EXPECT_EQ(deep_build_native_curve(s, p, curve).code, DeepErrorCode::Incomplete);
    r.native_complete = true;
  }
  {
    DeepPageStore s(p.frame, 1 << 20);
    r.optical_complete = false;
    EXPECT_EQ(deep_build_native_curve(s, p, curve).code, DeepErrorCode::Incomplete);
    r.optical_complete = true;
  }
  {
    DeepPageStore s(p.frame, 1 << 20);
    optical[1].z = 2.25;
    EXPECT_EQ(deep_build_native_curve(s, p, curve).code, DeepErrorCode::Incomplete);
    optical[1].z = 2;
  }
  {
    DeepPageStore s(p.frame, 1 << 20);
    optical[1].source = 1;
    EXPECT_EQ(deep_build_native_curve(s, p, curve).code, DeepErrorCode::InvalidReference);
    optical[1].source = 2;
  }
  std::array<DeepNativeCurveRay, 2> duplicate{r, r};
  duplicate[1].ray = 2;
  duplicate[1].sample.rng_offset = 100;
  auto dup = pixel(duplicate);
  {
    DeepPageStore s(p.frame, 1 << 20);
    EXPECT_EQ(deep_build_native_curve(s, dup, curve).code, DeepErrorCode::DuplicateSample);
  }
  std::array<DeepCurveRadiance, 1> missing{
      {{DeepCurveRadianceBasis::Point, 2, 2, {1, 0, 0}, {0, 0, 0}, 9, 0}}};
  r.radiance = missing;
  {
    DeepPageStore s(p.frame, 1 << 20);
    auto status = deep_build_native_curve(s, p, curve);
    EXPECT_FALSE(status);
    EXPECT_NE(status.message.find("MissingRadianceSupport"), std::string::npos);
  }
  EXPECT_FALSE(curve);
}

TEST(DeepNativeCurve, same_depth_visibility_uses_native_ordinal_and_rejects_ambiguous_order)
{
  std::array<DeepCurveOptical, 1> vacuum{{{1, 3, {0, 0, 0}, 1}}};
  std::array<DeepCurveVisibility, 2> points{
      {{2, {0.5, 0.5, 0.5}, {0, 0, 0}, 2, 1}, {2, {0.5, 0.5, 0.5}, {0.25, 0.25, 0.25}, 3, 2}}};
  std::reverse(points.begin(), points.end());
  auto r = ray(1, 0, vacuum, points);
  auto p = pixel({&r, 1});
  DeepPageStore store(p.frame, 1 << 20);
  DeepNativeCurve curve;
  auto status = deep_build_native_curve(store, p, curve);
  ASSERT_TRUE(status) << status.message;
  EXPECT_NEAR(at(read(curve), 2)[0], 0.375, 1e-12);
  points[0].ordinal = points[1].ordinal;
  DeepPageStore second(p.frame, 1 << 20);
  EXPECT_FALSE(deep_build_native_curve(second, p, curve));
}

TEST(DeepNativeCurve, quota_and_owned_lease_preserve_source_identity_after_borrowers_die)
{
  std::array<DeepCurveOptical, 1> optical{{{1, 3, {1, 2, 3}, 8}}};
  std::array<DeepCurveRadiance, 1> term{
      {{DeepCurveRadianceBasis::BeerSpan, 1, 3, {0.1, -0.2, 0.3}, {0, 1, 2}, 9, 7}}};
  auto r = ray(1, 0, optical, {}, term);
  auto p = pixel({&r, 1});
  DeepMemoryBudget budget(1 << 24);
  size_t empty = budget.used();
  DeepNativeCurve retained;
  {
    DeepPageStore store(p.frame, budget, 512);
    auto status = deep_build_native_curve(store, p, retained);
    ASSERT_TRUE(status) << status.message;
  }
  EXPECT_GT(budget.used(), empty);
  optical[0].tau[0] = 999;
  term[0].post_clamp_rgb[0] = 999;
  EXPECT_EQ(retained.optical_sources()[0].support.tau[0], 1);
  EXPECT_EQ(retained.radiance_sources()[0].support.post_clamp_rgb[0], 0.1);
  DeepDepthKnot knot;
  EXPECT_TRUE(retained.read_knot(2, knot));
  retained = {};
  EXPECT_EQ(budget.used(), empty);
  DeepPageStore low(p.frame, 700, 512);
  auto status = deep_build_native_curve(low, p, retained);
  EXPECT_EQ(status.code, DeepErrorCode::BudgetExceeded);
  EXPECT_FALSE(retained);
}

TEST(DeepNativeCurve, zero_exposure_preserves_sources_and_optical_without_division)
{
  std::array<DeepCurveOptical, 1> optical{{{1, 3, {1, 2, 3}, 1}}};
  std::array<DeepCurveRadiance, 2> terms{
      {{DeepCurveRadianceBasis::BeerSpan, 1, 3, {2, -3, 4}, {0, 1, 2}, 2, 1},
       {DeepCurveRadianceBasis::Point, 2, 2, {10, -10, 20}, {0, 0, 0}, 3, 2}}};
  auto r = ray(1, 0, optical, {}, terms);
  auto p = pixel({&r, 1});
  p.exposure = 0;
  DeepPageStore store(p.frame, 1 << 24);
  DeepNativeCurve curve;
  ASSERT_TRUE(deep_build_native_curve(store, p, curve));
  verify(curve, p, 0, 4);
  EXPECT_EQ(curve.radiance_sources().size(), 2u);
  EXPECT_EQ(curve.radiance_sources()[0].support.post_clamp_rgb[0], 2);
}

TEST(DeepNativeCurve, shared_source_distinct_calls_accumulate_but_duplicate_call_is_rejected)
{
  DeepPageStore store(frame(), 4 << 20);
  const std::array<DeepCurveOptical, 1> optical{{{1, 4, {0, 0, 0}, 8}}};
  std::array<DeepCurveRadiance, 2> terms{
      DeepCurveRadiance{DeepCurveRadianceBasis::Point, 2, 2, {2, -1, 3}, {}, 11, 31},
      DeepCurveRadiance{DeepCurveRadianceBasis::Point, 2, 2, {1, -0.5, 1.5}, {}, 11, 32}};
  const std::array rays{ray(1, 0, optical, {}, terms)};
  auto input = pixel(rays);
  DeepNativeCurve curve;
  ASSERT_TRUE(deep_build_native_curve(store, input, curve));
  EXPECT_EQ(curve.radiance_sources().size(), 2u);
  EXPECT_EQ(at(read(curve), 3)[1], 3);
  terms[1].emission_call = 31;
  EXPECT_FALSE(deep_build_native_curve(store, input, curve));
}

TEST(DeepNativeCurve, mixed_native_four_ray_plateau_reaches_opaque_cut_without_increasing_T)
{
  /* Captured failing OptiX pixel (index 34), reduced to the complete mathematical
   * seam. Independent source support/call identity and all four AA rays remain. */
  DeepPageStore store(frame(), 16 << 20);
  const std::array<std::array<DeepCurveOptical, 2>, 4> optical{
      {{{{1.5, 2.5, {0.28496786952018738, 0.1424839198589325, 0.035620991140604019}, 431},
         {2.5, 3, {0, 0, 0}, 612}}},
       {{{1.5000002384185791,
          2.5,
          {0.28088897466659546, 0.14044447243213654, 0.035111129283905029},
          430},
         {2.5, 3, {0, 0, 0}, 613}}},
       {{{1.5,
          2.4999997615814209,
          {0.2821018397808075, 0.14105090498924255, 0.035262737423181534},
          194},
         {2.4999997615814209, 3, {0, 0, 0}, 614}}},
       {{{1.5000002384185791,
          2.5,
          {0.28281959891319275, 0.14140978455543518, 0.035352457314729691},
          59},
         {2.5, 3, {0, 0, 0}, 615}}}}};
  const std::array<DeepCurveVisibility, 4> visibility{{{3, {0, 0, 0}, {0, 0, 0}, 274, 1},
                                                       {3, {0, 0, 0}, {0, 0, 0}, 273, 1},
                                                       {3, {0, 0, 0}, {0, 0, 0}, 156, 1},
                                                       {3, {0, 0, 0}, {0, 0, 0}, 19, 1}}};
  const std::array<DeepCurveRadiance, 4> radiance{
      {{DeepCurveRadianceBasis::Point,
        3,
        3,
        {0.010367549024522305, 0.071337983012199402, 0.043271105736494064},
        {},
        482,
        482,
        41},
       {DeepCurveRadianceBasis::Point,
        3,
        3,
        {0.010425507090985775, 0.070966675877571106, 0.044467221945524216},
        {},
        481,
        481,
        40},
       {DeepCurveRadianceBasis::Point,
        3,
        3,
        {0.010396252386271954, 0.071415595710277557, 0.043313268572092056},
        {},
        239,
        239,
        17},
       {DeepCurveRadianceBasis::Point,
        3,
        3,
        {0.007851053960621357, 0.059272900223731995, 0.048529934138059616},
        {},
        128,
        128,
        39}}};
  const std::array rays{ray(134, 3, optical[0], {&visibility[0], 1}, {&radiance[0], 1}),
                        ray(133, 2, optical[1], {&visibility[1], 1}, {&radiance[1], 1}),
                        ray(99, 1, optical[2], {&visibility[2], 1}, {&radiance[2], 1}),
                        ray(35, 0, optical[3], {&visibility[3], 1}, {&radiance[3], 1})};
  const auto input = pixel(rays);
  DeepNativeCurve curve;
  ASSERT_TRUE(deep_build_native_curve(store, input, curve));
  DeepCanonicalEncoded encoded;
  const auto status = deep_encode_canonical_knots(store.memory_budget(),
                                                  curve.pages(),
                                                  curve.knot_count(),
                                                  curve.representation_error(),
                                                  {},
                                                  encoded);
  ASSERT_TRUE(status) << status.message;
  EXPECT_TRUE(encoded.report()->quantized_checked);
  const auto knots = read(curve);
  for (size_t i = 1; i < knots.size(); i++)
    EXPECT_LE(knots[i].before[0], knots[i - 1].after[0]);
  EXPECT_EQ(at(knots, 3)[0], 0);
  verify(curve, input, 1.5, 3);
}

TEST(DeepNativeCurve, complete_aa_visibility_cuts_keep_constant_transmission_monotone)
{
  /* The original 32-spp pilot leaves one vacuum ray alive after thirty-one
   * differently attenuated rays meet their real opaque surfaces. No RGB target
   * or synthetic opacity correction is involved in this mathematical fixture. */
  for (uint32_t seed = 1; seed <= 64; ++seed) {
    DeepPageStore store(frame(), 16 << 20);
    std::array<std::array<DeepCurveOptical, 2>, 32> optical;
    std::array<DeepCurveVisibility, 32> visibility;
    std::array<DeepNativeCurveRay, 32> rays;
    uint32_t random = seed;
    for (uint32_t i = 0; i < 32; ++i) {
      random = random * 1664525u + 1013904223u;
      const double tau = i == 31 ? 0 : double(float(double(random >> 8) / 16777216.0));
      const double end = i == 31 ? 8 : 2 + double(i) / 8;
      optical[i] = {
          {{0, 1, {tau, tau * .5, tau * .25}, 1000 + i * 2}, {1, end, {0, 0, 0}, 1001 + i * 2}}};
      visibility[i] = {end, {0, 0, 0}, {0, 0, 0}, 2000 + i, 1};
      rays[i] = ray(i + 1, i, optical[i], {&visibility[i], 1});
    }
    const auto input = pixel(rays);
    DeepNativeCurve curve;
    ASSERT_TRUE(deep_build_native_curve(store, input, curve)) << seed;
    const auto knots = read(curve);
    for (size_t i = 1; i < knots.size(); ++i)
      ASSERT_LE(knots[i].before[0], knots[i - 1].after[0]) << "seed=" << seed << " knot=" << i;
    DeepCanonicalEncoded encoded;
    const auto status = deep_encode_canonical_knots(store.memory_budget(),
                                                    curve.pages(),
                                                    curve.knot_count(),
                                                    curve.representation_error(),
                                                    {},
                                                    encoded);
    ASSERT_TRUE(status) << "seed=" << seed << " " << status.message;
    verify(curve, input, 0, 8);
  }
}

TEST(DeepNativeCurve, strongly_attenuated_aa_tail_remains_monotone_before_encoding)
{
  /* A small surviving ray follows real opaque cuts in the other AA samples.
   * This reproduces the original larger-scene endpoint increase without Film/I/O. */
  for (const double gain : {1.0, 2.0}) {
  for (const size_t count : {size_t(5), size_t(32)}) {
    for (uint32_t seed = 1; seed <= 64; ++seed) {
      DeepPageStore store(frame(), 64 << 20);
      std::array<std::array<DeepCurveOptical, 3>, 32> optical;
      std::array<DeepCurveVisibility, 32> visibility;
      std::array<DeepNativeCurveRay, 32> all_rays, selected;
      uint32_t random = seed;
      for (uint32_t i = 0; i < 32; ++i) {
        random = random * 1664525u + 1013904223u;
        const double x = double(random >> 8) / 16777216.0;
        const double end = i == 31 ? 8 : 2 + double(i) / 8;
        const double tau0 = i == 31 ? (16 + 3 * x) * gain : x;
        const double tau1 = i == 31 ? 1e-6 : 16 * (1 + x);
        optical[i] = {{{0, 1, {tau0, tau0, tau0}, 1000 + i * 3},
                       {1, end, {tau1, tau1, tau1}, 1001 + i * 3},
                       {end, 9, {0, 0, 0}, 1002 + i * 3}}};
        visibility[i] = {i == 31 ? 9 : end, {0, 0, 0}, {0, 0, 0}, 2000 + i, 1};
        all_rays[i] = ray(i + 1, i, optical[i], {&visibility[i], 1});
      }
      for (size_t i = 0; i < count; ++i)
        selected[i] = all_rays[i + 1 == count ? 31 : i];
      const auto input = pixel({selected.data(), count});
      DeepNativeCurve curve;
      ASSERT_TRUE(deep_build_native_curve(store, input, curve)) << count << " " << seed;
      const auto knots = read(curve);
      for (size_t i = 1; i < knots.size(); ++i)
        ASSERT_LE(knots[i].before[0], knots[i - 1].after[0])
            << "population=" << count << " seed=" << seed << " knot=" << i;
      DeepCanonicalEncoded encoded;
      const auto status = deep_encode_canonical_knots(store.memory_budget(),
                                                      curve.pages(),
                                                      curve.knot_count(),
                                                      curve.representation_error(),
                                                      {},
                                                      encoded);
      ASSERT_TRUE(status) << count << " " << seed << " " << status.message;
      verify(curve, input, 0, 9);
    }
  }
  }
}

TEST(DeepNativeCurve, strong_tail_keeps_signed_native_rgb_and_retires_quota_owned_pages)
{
  std::array<std::array<DeepCurveOptical, 3>, 32> optical;
  std::array<DeepCurveVisibility, 32> visibility;
  std::array<DeepCurveRadiance, 32> radiance;
  std::array<DeepNativeCurveRay, 32> rays;
  uint32_t random = 1;
  for (uint32_t i = 0; i < 32; ++i) {
    random = random * 1664525u + 1013904223u;
    const double x = double(random >> 8) / 16777216.0;
    const double end = i == 31 ? 8 : 2 + double(i) / 8;
    const double tau0 = i == 31 ? 2 * (16 + 3 * x) : x;
    const double tau1 = i == 31 ? 1e-6 : 16 * (1 + x);
    optical[i] = {{{0, 1, {tau0, tau0, tau0}, 1000 + i * 3},
                   {1, end, {tau1, tau1, tau1}, 1001 + i * 3},
                   {end, 9, {0, 0, 0}, 1002 + i * 3}}};
    visibility[i] = {i == 31 ? 9 : end, {0, 0, 0}, {}, 2000 + i, 1};
    radiance[i] = {DeepCurveRadianceBasis::Point, 0, 0, {4, -2, 1}, {}, 3000 + i, 3000 + i, 0};
    rays[i] = ray(i + 1, i, optical[i], {&visibility[i], 1}, {&radiance[i], 1});
  }
  const auto input = pixel(rays);
  DeepMemoryBudget budget(64 << 20);
  const size_t initial = budget.used();
  size_t builder_peak = 0;
  {
    DeepPageStore store(frame(), budget);
    DeepNativeCurve curve;
    ASSERT_TRUE(deep_build_native_curve(store, input, curve));
    builder_peak = budget.peak();
    const auto knots = read(curve);
    for (size_t i = 1; i < knots.size(); ++i)
      ASSERT_LE(knots[i].before[0], knots[i - 1].after[0]);
    for (double z : {0.0, 0.5, 1.0, 3.0, 7.0, 8.0, 9.0}) {
      const auto value = at(knots, z);
      EXPECT_DOUBLE_EQ(value[1], 4);
      EXPECT_DOUBLE_EQ(value[2], -2);
      EXPECT_DOUBLE_EQ(value[3], 1);
    }
    verify(curve, input, 0, 9);
    DeepCanonicalEncoded encoded;
    const auto status = deep_encode_canonical_knots(store.memory_budget(),
                                                    curve.pages(),
                                                    curve.knot_count(),
                                                    curve.representation_error(),
                                                    {},
                                                    encoded);
    ASSERT_TRUE(status) << status.message;
    EXPECT_TRUE(encoded.report()->normalized_checked && encoded.report()->quantized_checked);
  }
  EXPECT_EQ(budget.used(), initial);
  ASSERT_GT(builder_peak, 1u);
  for (const size_t limit : {builder_peak - 1, builder_peak / 2}) {
    DeepMemoryBudget denied(limit);
    const size_t before = denied.used();
    {
      DeepPageStore store(frame(), denied);
      DeepNativeCurve curve;
      const auto status = deep_build_native_curve(store, input, curve);
      EXPECT_EQ(status.code, DeepErrorCode::BudgetExceeded) << status.message;
      EXPECT_FALSE(curve);
    }
    EXPECT_EQ(denied.used(), before);
  }
}

TEST(DeepNativeCurve, frozen_consumer_budget_has_no_positive_precision_floor)
{
  DeepNativeCurvePolicy policy;
  ASSERT_TRUE(deep_native_curve_policy_for_budget(0.1, 0.006, policy));
  EXPECT_DOUBLE_EQ(policy.transmission_error, 0.004);
  EXPECT_DOUBLE_EQ(policy.rgb_error, 0.0012);
  ASSERT_TRUE(deep_native_curve_policy_for_budget(1e-6, 1e-6, policy));
  EXPECT_DOUBLE_EQ(policy.transmission_error, 0.2 * 1e-6);
  EXPECT_DOUBLE_EQ(policy.rgb_error, 0.2 * 1e-6);
  EXPECT_FALSE(deep_native_curve_policy_for_budget(0, 0.006, policy));
  EXPECT_FALSE(deep_native_curve_policy_for_budget(0.1, -1, policy));
  EXPECT_FALSE(deep_native_curve_policy_for_budget(
      std::numeric_limits<double>::quiet_NaN(), 0.006, policy));
}

TEST(DeepNativeCurve, actual_consumer_policy_refines_tight_inputs_and_preserves_semantic_cuts)
{
  const std::array<DeepCurveOptical, 2> optical{
      {{1, 2, {0.5, 0.5, 0.5}, 1}, {2, 3, {0.5, 0.5, 0.5}, 2}}};
  const std::array<DeepCurveVisibility, 1> visibility{{{3, {0, 0, 0}, {}, 3, 1}}};
  const auto r = ray(1, 0, optical, visibility);
  const auto input = pixel({&r, 1});
  DeepNativeCurvePolicy loose, tight;
  ASSERT_TRUE(deep_native_curve_policy_for_budget(0.1, 0.006, loose));
  ASSERT_TRUE(deep_native_curve_policy_for_budget(1e-6, 1e-6, tight));
  DeepPageStore loose_store(input.frame, 32 << 20), tight_store(input.frame, 32 << 20);
  DeepNativeCurve coarse, refined;
  ASSERT_TRUE(deep_build_native_curve(loose_store, input, coarse, loose));
  ASSERT_TRUE(deep_build_native_curve(tight_store, input, refined, tight));
  EXPECT_GT(refined.knot_count(), coarse.knot_count());
  const auto coarse_knots = read(coarse), tight_knots = read(refined);
  bool found_micro = false, found_surface = false;
  for (const auto &knot : coarse_knots) {
    if (knot.z == 2) {
      found_micro = true;
      EXPECT_EQ(knot.flags & DEEP_KNOT_PROTECTED_CUT, 0u);
    }
    if (knot.z == 3) {
      found_surface = true;
      EXPECT_NE(knot.flags & DEEP_KNOT_PROTECTED_CUT, 0u);
    }
  }
  EXPECT_TRUE(found_micro && found_surface);
  for (int i = 0; i <= 2000; i++) {
    const double z = 4.0 * i / 2000;
    const auto expected = reference(input, z);
    EXPECT_LE(std::abs(at(coarse_knots, z)[0] - expected[0]),
              coarse.representation_error()[0] + 3e-12);
    EXPECT_LE(std::abs(at(tight_knots, z)[0] - expected[0]),
              refined.representation_error()[0] + 3e-12);
  }
  DeepCanonicalEncodeConfig config;
  config.precision = DeepCanonicalPrecision::Float;
  config.alpha_tolerance = config.rgb_absolute_tolerance = 1e-6;
  DeepCanonicalEncoded encoded;
  ASSERT_TRUE(deep_encode_canonical_knots(tight_store.memory_budget(),
                                          refined.pages(),
                                          refined.knot_count(),
                                          refined.representation_error(),
                                          config,
                                          encoded));
  EXPECT_LE(encoded.report()->total_error[0], 1e-6);
}

TEST(DeepNativeCurve, live_slope_tree_handles_adjacent_zero_and_cancelling_world_chords)
{
  const double far = std::numeric_limits<float>::max();
  const std::array<DeepCurveOptical, 2> optical{
      {{.125, 1, {.25, .5, 1}, 1}, {1, 4, {1, .5, .25}, 2}}};
  std::array<DeepCurveRadiance, 5> terms;
  std::array<DeepNativeCurveRay, 5> rays;
  for (size_t i = 0; i < rays.size(); i++) {
    const double sign = i == 0 || i == 2 ? 1 : i == 1 || i == 3 ? -1 : 0;
    terms[i] = {DeepCurveRadianceBasis::BeerSpan,
                .125,
                far,
                {sign * 8, sign * .04, 0},
                {2 * far, 2 * far, 2 * far},
                uint32_t(i + 3),
                uint64_t(i + 7)};
    terms[i].flags = DEEP_KNOT_NATIVE_WORLD_RADIANCE;
    rays[i] = ray(i + 1, uint32_t(i), optical, {}, {&terms[i], 1});
    rays[i].native_world_support = true;
  }
  const auto input = pixel(rays);
  DeepMemoryBudget budget(32 << 20);
  const size_t baseline = budget.used();
  {
    DeepPageStore store(input.frame, budget);
    DeepNativeCurve curve;
    const auto status = deep_build_native_curve(store, input, curve);
    ASSERT_TRUE(status) << status.message;
    verify(curve, input, 0, 8);
    for (const double z : {16., 100., 1e12, far}) {
      const auto value = at(read(curve), z);
      for (int c = 1; c < 4; c++)
        EXPECT_LE(std::abs(value[c]), curve.representation_error()[c]);
    }
  }
  EXPECT_EQ(budget.used(), baseline);
}

namespace {
DeepFrozenDepthMap radial_capture(double front = 0)
{
  DeepFrozenDepthMap map;
  map.kind = DeepFrozenDepthKind::Radial;
  map.transform[0] = map.transform[5] = map.transform[10] = 1;
  map.origin[0] = 3;
  map.origin[1] = 4;
  map.direction[2] = 2;
  map.tmin = float(front);
  map.tmax = 6;
  return map;
}
DeepDepthSupportView radial_view(const DeepMemoryBudget &budget,
                                 const DeepNativeCurveRay &input,
                                 double front = 0,
                                 int corrupt = 0)
{
  DeepDepthSupportBuilder builder(frame(), budget, 1024);
  uint64_t map;
  EXPECT_TRUE(builder.add_map(input.ray, radial_capture(front), map));
  auto physical =
      [&](DeepPhysicalSourceKind kind, uint64_t source, uint32_t ordinal, double z, double back) {
        DeepPhysicalSourceSupport p;
        p.depth_map = map;
        p.source = source;
        p.ordinal = ordinal;
        p.kind = kind;
        p.z = z;
        p.z_back = back;
        p.front = front;
        p.back = 6;
        if (corrupt == 1)
          ++p.ordinal;
        if (corrupt == 2)
          p.z = std::nextafter(z, 0.);
        return p;
      };
  for (const auto &s : input.optical) {
    auto p = physical(DeepPhysicalSourceKind::Optical, s.source, s.ordinal, s.z, s.z_back);
    EXPECT_TRUE(builder.add_support(input.ray, p));
  }
  for (const auto &s : input.radiance) {
    auto p = physical(DeepPhysicalSourceKind::Radiance, s.source, s.ordinal, s.z, s.z_back);
    EXPECT_TRUE(builder.add_support(input.ray, p));
  }
  if (corrupt == 3) {
    auto p = physical(
        DeepPhysicalSourceKind::Optical, 999, 0, input.coverage_front, input.coverage_back);
    EXPECT_TRUE(builder.add_support(input.ray, p));
  }
  DeepDepthSupportTable table;
  EXPECT_TRUE(builder.finish(table));
  DeepDepthSupportView view;
  EXPECT_TRUE(table.view(input.ray, view));
  return view;
}
}  // namespace

TEST(DeepNativeCurve, radial_nonunit_physical_beer_and_zero_tau_emission_have_full_prefix_proof)
{
  for (double front : {0., 1.5}) {
    const double z = double(float(std::sqrt(25 + 4 * front * front)));
    const DeepCurveOptical optical{z, 13, {2, 3, 4}, 1};
    const DeepCurveRadiance emission{
        DeepCurveRadianceBasis::BeerSpan, z, 13, {.6, -.2, .9}, {.8, 0, 5}, 2, 3};
    auto r = ray(1, 0, {&optical, 1}, {}, {&emission, 1});
    r.affine_camera_depth = false;
    DeepMemoryBudget budget(32 << 20);
    r.depth_support = radial_view(budget, r, front);
    DeepPageStore store(frame(), budget);
    DeepNativeCurvePolicy policy;
    policy.transmission_error = policy.rgb_error = .004;
    DeepNativeCurve curve;
    const auto status = deep_build_native_curve(store, pixel({&r, 1}), curve, policy);
    ASSERT_TRUE(status) << status.message;
    const auto knots = read(curve);
    for (int i = 0; i <= 8192; i++) {
      const double cut = z + (13 - z) * i / 8192;
      const long double t = std::sqrt(static_cast<long double>(cut) * cut - 25) / 2;
      const double f = i == 0    ? 0 :
                       i == 8192 ? 1 :
                                   double(std::clamp((t - front) / (6 - front), 0.L, 1.L));
      const auto value = at(knots, cut);
      double transmission = 0;
      for (int c = 0; c < 3; c++) {
        transmission += std::exp(-optical.tau[c] * f) / 3;
        const double expected = emission.post_clamp_rgb[c] *
                                (emission.tau[c] == 0 ? f :
                                                        -std::expm1(-emission.tau[c] * f) /
                                                            -std::expm1(-emission.tau[c]));
        EXPECT_LE(std::abs(value[c + 1] - expected), curve.representation_error()[c + 1] + 2e-12);
      }
      EXPECT_LE(std::abs(value[0] - transmission), curve.representation_error()[0] + 2e-12);
    }
    for (double error : curve.representation_error())
      EXPECT_LE(error, .004);
    EXPECT_NEAR(at(knots, 13)[2], -.2, 1e-12);
  }
}

TEST(DeepNativeCurve, radial_pairing_rejects_ordinal_depth_or_orphan_source)
{
  const DeepCurveOptical optical{5, 13, {2, 3, 4}, 1};
  for (int corrupt : {1, 2, 3}) {
    auto r = ray(1, 0, {&optical, 1});
    r.affine_camera_depth = false;
    DeepMemoryBudget budget(1 << 20);
    r.depth_support = radial_view(budget, r, 0, corrupt);
    DeepPageStore store(frame(), budget);
    DeepNativeCurve output;
    EXPECT_EQ(deep_build_native_curve(store, pixel({&r, 1}), output).code,
              DeepErrorCode::InvalidReference);
    EXPECT_FALSE(output);
  }
}

TEST(DeepNativeCurve, radial_view_from_foreign_budget_or_frame_cannot_be_adopted)
{
  const DeepCurveOptical optical{5, 13, {2, 3, 4}, 1};
  auto r = ray(1, 0, {&optical, 1});
  r.affine_camera_depth = false;
  DeepMemoryBudget budget(1 << 20), other(1 << 20);
  r.depth_support = radial_view(budget, r);
  DeepNativeCurve curve;
  DeepPageStore foreign(frame(), other);
  EXPECT_EQ(deep_build_native_curve(foreign, pixel({&r, 1}), curve).code,
            DeepErrorCode::InvalidReference);
  auto input = pixel({&r, 1});
  ++input.frame.invocation;
  DeepPageStore next(input.frame, budget);
  EXPECT_EQ(deep_build_native_curve(next, input, curve).code, DeepErrorCode::InvalidReference);
  EXPECT_FALSE(curve);
}

TEST(DeepNativeCurve, tangent_float_depth_resolution_is_an_explicit_representability_limit)
{
  const DeepCurveOptical optical{5, 13, {20, 30, 40}, 1};
  auto r = ray(1, 0, {&optical, 1});
  r.affine_camera_depth = false;
  DeepMemoryBudget budget(8 << 20);
  r.depth_support = radial_view(budget, r);
  DeepPageStore store(frame(), budget);
  DeepNativeCurve output;
  DeepNativeCurvePolicy policy;
  policy.transmission_error = policy.rgb_error = 1e-7;
  const auto status = deep_build_native_curve(store, pixel({&r, 1}), output, policy);
  EXPECT_FALSE(status);
  EXPECT_FALSE(output);
  EXPECT_NE(status.message.find("DepthNotRepresentable"), std::string::npos);
}

TEST(DeepNativeCurve, radial_population_visibility_and_shared_source_calls_keep_independent_proof)
{
  const std::array<DeepCurveOptical, 2> optical{{{5, 13, {2, 3, 4}, 1}, {5, 13, {.5, 1, 2}, 11}}};
  std::array<DeepCurveVisibility, 2> visibility;
  std::array<std::array<DeepCurveRadiance, 2>, 2> emission;
  std::array<DeepNativeCurveRay, 2> rays;
  DeepMemoryBudget budget(32 << 20);
  DeepDepthSupportBuilder builder(frame(), budget, 1024);
  for (size_t i = 0; i < 2; i++) {
    visibility[i] = {9, {.5, .5, .5}, {.2, .2, .2}, uint64_t(20 + i), 3};
    emission[i][0] = {
        DeepCurveRadianceBasis::BeerSpan, 5, 13, {.4, -.1, .3}, {1, 0, 4}, uint64_t(30 + i), 41};
    emission[i][1] = emission[i][0];
    emission[i][1].emission_call = 42;
    for (double &rgb : emission[i][1].post_clamp_rgb)
      rgb *= -.25;
    rays[i] = ray(i + 1, uint32_t(i), {&optical[i], 1}, {&visibility[i], 1}, emission[i]);
    rays[i].affine_camera_depth = false;
    rays[i].population_weight = i ? 3 : 1;
    auto map = radial_capture();
    if (i) {
      map.origin[0] = map.origin[1] = 0;
      map.tmin = 2.5;
      map.tmax = 6.5;
    }
    uint64_t id;
    ASSERT_TRUE(builder.add_map(i + 1, map, id));
    DeepPhysicalSourceSupport p;
    p.depth_map = id;
    p.source = optical[i].source;
    p.front = i ? 2.5 : 0;
    p.back = i ? 6.5 : 6;
    p.z = 5;
    p.z_back = 13;
    ASSERT_TRUE(builder.add_support(i + 1, p));
    p.kind = DeepPhysicalSourceKind::Radiance;
    p.source = emission[i][0].source;
    ASSERT_TRUE(builder.add_support(i + 1, p));
    p.kind = DeepPhysicalSourceKind::Visibility;
    p.source = visibility[i].source;
    p.ordinal = 3;
    p.front = p.back = i ? 4.5 : std::sqrt(56.) / 2;
    p.z = p.z_back = 9;
    p.flags = DEEP_PHYSICAL_POINT;
    ASSERT_TRUE(builder.add_support(i + 1, p));
  }
  DeepDepthSupportTable table;
  ASSERT_TRUE(builder.finish(table));
  for (size_t i = 0; i < 2; i++)
    ASSERT_TRUE(table.view(i + 1, rays[i].depth_support));
  DeepPageStore store(frame(), budget);
  DeepNativeCurve curve;
  DeepNativeCurvePolicy policy;
  policy.transmission_error = policy.rgb_error = .004;
  policy.retain_sources = false;
  const auto status = deep_build_native_curve(store, pixel(rays), curve, policy);
  ASSERT_TRUE(status) << status.message;
  EXPECT_TRUE(curve.optical_sources().empty());
  EXPECT_TRUE(curve.radiance_sources().empty());
  const auto knots = read(curve);
  for (int sample = 0; sample <= 4096; sample++) {
    const double z = 5 + 8 * double(sample) / 4096;
    for (bool before : {false, true}) {
      const auto actual = at(knots, z, before);
      std::array<double, 4> expected = {};
      for (size_t i = 0; i < 2; i++) {
        const double f = i ? (z - 5) / 8 : std::sqrt(std::max(0., z * z - 25)) / 12;
        const double vf = i ? .5 : std::sqrt(56.) / 12;
        const bool visible = z > 9 || (z == 9 && !before);
        const double weight = i ? .75 : .25;
        for (int c = 0; c < 3; c++) {
          const double t = std::exp(-optical[i].tau[c] * f);
          expected[0] += weight * (visible ? .5 * t + .2 * std::exp(-optical[i].tau[c] * vf) : t) /
                         3;
          const auto &p = emission[i][0];
          expected[c + 1] += weight * .75 * p.post_clamp_rgb[c] *
                             (p.tau[c] == 0 ? f :
                                              -std::expm1(-p.tau[c] * f) / -std::expm1(-p.tau[c]));
        }
      }
      for (int c = 0; c < 4; c++)
        EXPECT_LE(std::abs(actual[c] - expected[c]), curve.representation_error()[c] + 2e-12);
    }
  }
}

TEST(DeepNativeCurve,
     radial_world_finite_boundary_keeps_full_physical_span_and_charges_endpoint_error)
{
  const double maximum = std::numeric_limits<float>::max();
  const float direction = std::nextafter(1.f, 2.f);
  for (int mode = 0; mode < 3; mode++) {
    const double sigma = mode == 0 ? 4 : 1e-38;
    const double total = mode == 2 ? 1e14 : 1.6;
    const DeepCurveOptical optical{
        .125, double(float(.125 + double(direction) * 4)), {sigma * 4, sigma * 4, sigma * 4}, 1};
    DeepCurveRadiance emission{DeepCurveRadianceBasis::BeerSpan,
                               .125,
                               maximum,
                               {total, total / 2, total / 4},
                               {sigma * maximum, sigma * maximum, sigma * maximum},
                               2,
                               3};
    emission.flags = DEEP_KNOT_NATIVE_WORLD_RADIANCE;
    auto r = ray(1, 0, {&optical, 1}, {}, {&emission, 1});
    r.affine_camera_depth = false;
    r.native_world_support = true;
    DeepMemoryBudget budget(32 << 20);
    DeepDepthSupportBuilder builder(frame(), budget, 1024);
    auto map = radial_capture();
    map.origin[0] = map.origin[1] = 0;
    map.origin[2] = .125;
    map.direction[2] = direction;
    map.tmax = 4;
    uint64_t id;
    ASSERT_TRUE(builder.add_map(1, map, id));
    DeepPhysicalSourceSupport p;
    p.depth_map = id;
    p.source = 1;
    p.front = 0;
    p.back = 4;
    p.z = optical.z;
    p.z_back = optical.z_back;
    ASSERT_TRUE(builder.add_support(1, p));
    p.kind = DeepPhysicalSourceKind::Radiance;
    p.source = 2;
    p.back = maximum;
    p.z_back = maximum;
    p.flags = DEEP_PHYSICAL_WORLD_RADIANCE;
    ASSERT_TRUE(builder.add_support(1, p));
    DeepDepthSupportTable table;
    ASSERT_TRUE(builder.finish(table));
    ASSERT_TRUE(table.view(1, r.depth_support));
    DeepPageStore store(frame(), budget);
    DeepNativeCurve curve;
    const auto status = deep_build_native_curve(store, pixel({&r, 1}), curve);
    if (mode == 2) {
      EXPECT_FALSE(status);
      EXPECT_FALSE(curve);
      EXPECT_NE(status.message.find("endpoint quantization exceeds RGB budget"),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(status) << status.message;
    const auto knots = read(curve);
    for (double z : {.125, .25, 1., 4., 16., 100., 1e12, 1e30, maximum / 2, maximum}) {
      const double t = std::clamp((z - .125) / direction, 0., maximum);
      const double f = z == maximum ? 1 : t / maximum;
      const auto actual = at(knots, z);
      EXPECT_LE(std::abs(actual[0] - std::exp(-sigma * std::min(4., t))),
                curve.representation_error()[0] + 2e-12);
      for (int c = 0; c < 3; c++) {
        const double expected = emission.post_clamp_rgb[c] * (-std::expm1(-emission.tau[c] * f) /
                                                              -std::expm1(-emission.tau[c]));
        EXPECT_LE(std::abs(actual[c + 1] - expected), curve.representation_error()[c + 1] + 2e-12);
      }
    }
    EXPECT_EQ(curve.radiance_sources()[0].support.z_back, maximum);
  }
}

CCL_NAMESPACE_END
