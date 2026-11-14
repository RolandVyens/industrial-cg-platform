/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {
/* Execute the production device arithmetic on the host. Only the address-space
 * qualifiers and the unused error atomic need adapters; no merge is reimplemented. */
using std::isfinite;
#define __device__
#define __forceinline__ inline
inline uint32_t atomicOr(uint32_t *address, const uint32_t value)
{
  const uint32_t previous = *address;
  *address |= value;
  return previous;
}
#include "kernel/integrator/deep_bounded_records.h"
#undef __forceinline__
#undef __device__

template<typename T> uint64_t address(T *pointer)
{
  return reinterpret_cast<uintptr_t>(pointer);
}

template<bool Physical> struct MergeOutput {
  std::array<float, 128> z{}, z_back{}, alpha{};
  std::array<uint32_t, 128> object{}, shader{}, normal{};
  std::array<DeepBoundedRGBA, 128> rgba{};

  DeepBoundedPlanes planes()
  {
    return {address(z.data()),
            address(z_back.data()),
            Physical ? address(rgba.data()) : address(alpha.data()),
            address(object.data()),
            address(shader.data()),
            address(normal.data())};
  }

  double mass(const uint32_t count)
  {
    double transmission = 1.0;
    for (uint32_t i = 0; i < count; ++i) {
      transmission *= 1.0 - deep_bounded_load<Physical>(planes(), i).alpha;
    }
    return 1.0 - transmission;
  }

  double moment(const uint32_t count)
  {
    double transmission = 1.0, result = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
      const auto record = deep_bounded_load<Physical>(planes(), i);
      const double q = transmission * record.alpha;
      result += q * record.z;
      transmission *= 1.0 - record.alpha;
    }
    return result;
  }
};

DeepBoundedRecord<false> volume(const float z, const float q)
{
  return {z, z, q, DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE, 0, 0};
}

TEST(DeepBoundedMerge, AlphaToleranceComparesLocalDifference)
{
  auto a = volume(8.89f, 0.03125f), b = volume(8.891f, 0.03125f);
  a.object = b.object = 7;
  const DeepBoundedMergePolicy policy{0.01f, 0, 0, 0, 0.001f};
  EXPECT_TRUE(deep_bounded_ordinary(a, b, policy, 1.0f, true));
  b.alpha = 0.2f;
  EXPECT_FALSE(deep_bounded_ordinary(a, b, policy, 1.0f, true));
}

TEST(DeepBoundedMerge, AlphaComparisonMatchesPublicationIncludingTail)
{
  std::mt19937 random(29);
  std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
  for (int i = 0; i < 10000; ++i) {
    const float t = i % 2 ? uniform(random) : 1.0e-12f * uniform(random);
    auto a = volume(10, t * uniform(random)), b = volume(10.001f, t * uniform(random));
    const float tolerance = 0.01f;
    const DeepBoundedMergePolicy policy{tolerance, 0, 0, 0, 0.001f};
    const float local_a = deep_bounded_unit(a.alpha / fmaxf(t, 0x1p-23f));
    const float local_b = deep_bounded_unit(b.alpha / fmaxf(t - a.alpha, 0x1p-23f));
    EXPECT_EQ(deep_bounded_ordinary(a, b, policy, t, true), fabsf(local_a - local_b) < tolerance);
  }
}

template<bool Physical> void check_independent_tolerances()
{
  for (const bool surface : {false, true}) {
    for (const bool interval : {false, true}) {
      if (surface && interval) {
        continue;
      }
      SCOPED_TRACE(::testing::Message() << "physical=" << Physical << " surface=" << surface
                                       << " interval=" << interval);
      DeepBoundedRecord<Physical> a{}, b{};
      static_cast<DeepBoundedRecord<false> &>(a) = volume(1, 0.1f);
      static_cast<DeepBoundedRecord<false> &>(b) = volume(1.001f, 0.4f);
      if (surface) {
        a.object = b.object = 7; /* Ordinary compression, not opaque identity grouping. */
        a.normal = b.normal = deep_bounded_pack_normal(0, 0, 1);
      }
      if (interval) {
        a.z_back = 1.0005f;
        b.z_back = 1.002f;
      }
      EXPECT_TRUE(deep_bounded_ordinary(a, b, {0, 0.01f, 0, 0, 0.001f}, 1, true));
      EXPECT_TRUE(deep_bounded_ordinary(a, b, {0.01f, 0.01f, 0, 0, 0.001f}, 1, true));
      EXPECT_FALSE(deep_bounded_ordinary(a, b, {0.01f, 0, 0, 0, 0.001f}, 1, true));
      EXPECT_FALSE(deep_bounded_ordinary(a, b, {}, 1, true));

      b.z = 1.02f;
      b.z_back = interval ? 1.021f : b.z;
      b.alpha = 0.09f; /* Same published local alpha: 0.09 / (1 - 0.1) == 0.1. */
      EXPECT_TRUE(deep_bounded_ordinary(a, b, {0.01f, 0, 0, 0, 0.001f}, 1, true));
      EXPECT_TRUE(deep_bounded_ordinary(a, b, {0.01f, 0.01f, 0, 0, 0.001f}, 1, true));
      EXPECT_FALSE(deep_bounded_ordinary(a, b, {0, 0.01f, 0, 0, 0.001f}, 1, true));
      b.z = 3;
      b.z_back = interval ? 3.01f : b.z;
      EXPECT_FALSE(deep_bounded_ordinary(a, b, {0.01f, 0.01f, 0, 0, 0.001f}, 1, true));
    }
  }
}

TEST(DeepBoundedMerge, IndependentTolerancesSimpleSurfaceAndVolume)
{
  check_independent_tolerances<false>();
}

TEST(DeepBoundedMerge, IndependentTolerancesPhysicalSurfaceAndVolume)
{
  check_independent_tolerances<true>();
}

TEST(DeepBoundedMerge, DepthOnlyVolumeReductionPreservesMassAndMoment)
{
  DeepBoundedRecord<false> input[] = {volume(10, 0.1f), volume(10.001f, 0.4f)};
  MergeOutput<false> output;
  const auto n = deep_bounded_reduce_mature(
      input, 2, output.planes(), 0, 1, 32, {0, 0.01f, 0, 0, 0.001f});
  ASSERT_EQ(n, 1u);
  EXPECT_NEAR(output.mass(n), 0.5, 1e-6);
  EXPECT_NEAR(output.moment(n), 0.1 * 10 + 0.4 * double(input[1].z), 1e-6);
}

TEST(DeepBoundedMerge, IndependentTolerancesKeepPhysicalColourGate)
{
  DeepBoundedRecord<true> a{}, b{};
  static_cast<DeepBoundedRecord<false> &>(a) = volume(1, 0.1f);
  static_cast<DeepBoundedRecord<false> &>(b) = volume(1.001f, 0.4f);
  a.rgb[0] = 1;
  EXPECT_FALSE(deep_bounded_ordinary(a, b, {0, 0.01f, 0, 0, 0.001f}, 1, true));
}

TEST(DeepBoundedMerge, SurfaceIdentityAndNormalRemainRequired)
{
  auto a = volume(10, 0.03125f), b = volume(10.001f, 0.03125f);
  const DeepBoundedMergePolicy policy{0.01f, 0.01f, 0, 0, 0};
  a.object = b.object = 7;
  a.normal = b.normal = deep_bounded_pack_normal(0, 0, 1);
  EXPECT_TRUE(deep_bounded_ordinary(a, b, policy, 1, true));
  b.object = 8;
  EXPECT_FALSE(deep_bounded_ordinary(a, b, policy, 1, true));
  b.object = 7;
  b.normal = deep_bounded_pack_normal(1, 0, 0);
  EXPECT_FALSE(deep_bounded_ordinary(a, b, policy, 1, true));
}

TEST(DeepBoundedMerge, MatureVolumePreservesMassAndFirstMoment)
{
  DeepBoundedRecord<false> input[] = {volume(10, 0.2f), volume(12, 0.2f)};
  MergeOutput<false> output;
  const DeepBoundedMergePolicy policy{0.1f, 3, 0, 0, 0};
  const auto n = deep_bounded_reduce_mature(input, 2, output.planes(), 0, 1, 32, policy);
  ASSERT_EQ(n, 1u);
  EXPECT_NEAR(output.mass(n), 0.4, 1e-6);
  EXPECT_NEAR(output.z[0], 11, 1e-6);
  EXPECT_EQ(output.z[0], output.z_back[0]);
  EXPECT_NEAR(output.moment(n), 4.4, 1e-6);
}

TEST(DeepBoundedMerge, RawVolumeKeepsPendingAnchorFront)
{
  DeepBoundedRecord<false> input[] = {volume(10, 0.2f), volume(12, 0.2f)};
  MergeOutput<false> output;
  const DeepBoundedMergePolicy policy{0.1f, 3, 0, 0, 0};
  const auto n = deep_bounded_reduce(input, 2, output.planes(), 0, 1, 32, policy, false, 0);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(output.z[0], 10);
  EXPECT_NEAR(output.mass(n), 0.4, 1e-6);
}

TEST(DeepBoundedMerge, BackgroundAndSurfaceKeepFrontRepresentation)
{
  for (const bool background : {false, true}) {
    DeepBoundedRecord<false> input[] = {volume(10, 0.2f), volume(12, 0.2f)};
    for (auto &r : input) {
      if (background)
        r.shader = 1;
      else
        r.object = 7;
    }
    MergeOutput<false> output;
    const DeepBoundedMergePolicy policy{0.1f, 3, 0, 0, 0};
    const auto n = deep_bounded_reduce_mature(input, 2, output.planes(), 0, 1, 32, policy);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(output.z[0], 10);
  }
}

TEST(DeepBoundedMerge, EqualMassCapacityDoesNotCollapseToOneFront)
{
  for (const uint32_t count : {33u, 64u, 128u}) {
    std::array<DeepBoundedRecord<false>, 128> input;
    double expected_moment = 0;
    for (uint32_t i = 0; i < count; ++i) {
      input[i] = volume(10 + float(i) / 128, 1.0f / count);
      expected_moment += double(input[i].z) * input[i].alpha;
    }
    MergeOutput<false> output;
    const DeepBoundedMergePolicy policy{};
    const auto n = deep_bounded_reduce_mature(
        input.data(), count, output.planes(), 0, 1, 32, policy);
    EXPECT_EQ(n, 32u);
    EXPECT_NEAR(output.mass(n), 1, 2e-6);
    EXPECT_NEAR(output.moment(n), expected_moment, 2e-5);
    for (uint32_t i = 1; i < n; ++i)
      EXPECT_LE(output.z[i - 1], output.z[i]);
  }
}

TEST(DeepBoundedMerge, PhysicalSignedColourIsAddedOnce)
{
  DeepBoundedRecord<true> input[2]{};
  static_cast<DeepBoundedRecord<false> &>(input[0]) = volume(10, 0.2f);
  static_cast<DeepBoundedRecord<false> &>(input[1]) = volume(12, 0.2f);
  input[0].rgb[0] = -4;
  input[1].rgb[0] = 3;
  input[0].rgb[1] = 9;
  input[1].rgb[1] = 2;
  input[0].rgb[2] = 1;
  input[1].rgb[2] = -5;
  MergeOutput<true> output;
  const DeepBoundedMergePolicy policy{0.1f, 3, 100, 100, 0};
  const auto n = deep_bounded_reduce_mature(input, 2, output.planes(), 0, 1, 32, policy);
  ASSERT_EQ(n, 1u);
  EXPECT_FLOAT_EQ(output.rgba[0].r, -1);
  EXPECT_FLOAT_EQ(output.rgba[0].g, 11);
  EXPECT_FLOAT_EQ(output.rgba[0].b, -4);
  EXPECT_NEAR(output.mass(n), 0.4, 1e-6);
}

TEST(DeepBoundedMerge, ZeroAlphaPhysicalColourSurvivesPressure)
{
  std::array<DeepBoundedRecord<true>, 64> input{};
  for (uint32_t i = 0; i < input.size(); ++i) {
    static_cast<DeepBoundedRecord<false> &>(input[i]) = volume(10 + float(i), 0);
    input[i].rgb[0] = 1;
  }
  MergeOutput<true> output;
  const DeepBoundedMergePolicy policy{};
  const auto n = deep_bounded_reduce_mature(
      input.data(), input.size(), output.planes(), 0, 1, 32, policy);
  ASSERT_EQ(n, 32u);
  float colour = 0;
  for (uint32_t i = 0; i < n; ++i)
    colour += output.rgba[i].r;
  EXPECT_EQ(colour, 64);
  EXPECT_EQ(output.mass(n), 0);
}

TEST(DeepBoundedMerge, PressurePreservesIntervalAndDominantSurfaceMetadata)
{
  DeepBoundedRecord<false> input[] = {volume(10, 0.2f), volume(12, 0.4f), volume(14, 0.1f)};
  input[0].z_back = 11;
  input[1].object = 9;
  input[1].shader = 42;
  input[1].normal = 19;
  MergeOutput<false> output;
  const DeepBoundedMergePolicy policy{};
  const auto n = deep_bounded_reduce_mature(input, 3, output.planes(), 0, 1, 1, policy);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(output.z[0], 10);
  EXPECT_EQ(output.z_back[0], 14);
  EXPECT_EQ(output.object[0] & ~DEEP_BOUNDED_ELIGIBLE, 9u);
  EXPECT_EQ(output.shader[0], 42u);
  EXPECT_EQ(output.normal[0], 19u);
  EXPECT_NEAR(output.mass(n), 0.7, 1e-6);
}

TEST(DeepBoundedMerge, RepeatedDistributionDoesNotMoveCoverageMomentForward)
{
  for (const uint32_t spp : {128u, 512u}) {
    std::array<std::vector<DeepBoundedRecord<false>>, 16> buckets;
    std::array<uint32_t, 16> populations{};
    double expected_moment = 0;
    const DeepBoundedMergePolicy policy{0.01f, 1, 0, 0, 0};
    for (uint32_t sample = 0; sample < spp; ++sample) {
      const uint32_t source = sample < 128 ? sample : (sample - 128) / 3;
      const uint32_t bucket = source % 16;
      const float z = 1 + float(source) / 128;
      expected_moment += double(z) / spp;
      auto &chain = buckets[bucket];
      const float wb = 1.0f / ++populations[bucket];
      const float wa = 1.0f - wb;
      float transmission = 1;
      for (auto &r : chain) {
        const float local = r.alpha;
        r.alpha *= transmission * wa;
        transmission *= 1 - local;
      }
      chain.push_back(volume(z, wb));
      std::stable_sort(
          chain.begin(), chain.end(), [](const auto &a, const auto &b) { return a.z < b.z; });
      MergeOutput<false> output;
      const auto count = deep_bounded_reduce_mature(
          chain.data(), chain.size(), output.planes(), 0, 1, 32, policy);
      chain.resize(count);
      for (uint32_t i = 0; i < count; ++i)
        chain[i] = deep_bounded_load<false>(output.planes(), i);
    }
    std::vector<DeepBoundedRecord<false>> model;
    uint32_t population = 0;
    MergeOutput<false> output;
    for (uint32_t b = 0; b < 16; ++b) {
      const float wb = float(populations[b]) / (population + populations[b]);
      const float wa = 1 - wb;
      float transmission = 1;
      for (auto &r : model) {
        const float local = r.alpha;
        r.alpha *= transmission * wa;
        transmission *= 1 - local;
      }
      transmission = 1;
      for (auto r : buckets[b]) {
        const float local = r.alpha;
        r.alpha *= transmission * wb;
        transmission *= 1 - local;
        model.push_back(r);
      }
      std::stable_sort(
          model.begin(), model.end(), [](const auto &a, const auto &b) { return a.z < b.z; });
      const auto count = deep_bounded_reduce_mature(
          model.data(), model.size(), output.planes(), 0, 1, 32, policy);
      model.resize(count);
      for (uint32_t i = 0; i < count; ++i)
        model[i] = deep_bounded_load<false>(output.planes(), i);
      population += populations[b];
    }
    EXPECT_NEAR(output.mass(model.size()), 1, 2e-6);
    EXPECT_NEAR(output.moment(model.size()), expected_moment, 2e-5);
  }
}
TEST(DeepBoundedMerge, OpaqueSurfaceIdentityPrecedesAATolerance)
{
  for (const float tolerance : {0.0f, 0.01f, 0.1f}) {
    std::array<DeepBoundedRecord<false>, 32> input{};
    for (uint32_t i = 0; i < input.size(); ++i) {
      const float z = 8.89f + float(i) * 0.00017f;
      input[i] = {z, z, 1.0f / 32, 5u | DEEP_BOUNDED_ELIGIBLE, 2,
                  deep_bounded_pack_normal(0, 0, 1)};
    }
    MergeOutput<false> output;
    const DeepBoundedMergePolicy policy{tolerance, tolerance, 0.006f, 0.002f, 0};
    const auto n = deep_bounded_reduce_mature(
        input.data(), input.size(), output.planes(), 0, 1, 32, policy);
    ASSERT_EQ(n, 1u) << tolerance;
    EXPECT_EQ(output.z[0], input[0].z);
    EXPECT_EQ(output.z[0], output.z_back[0]);
    EXPECT_NEAR(output.mass(n), 1, 1e-6);
  }
}

TEST(DeepBoundedMerge, OpaqueSurfacePhysicalColourAccumulatesBeforeCompression)
{
  std::array<DeepBoundedRecord<true>, 32> input{};
  float sum = 0;
  for (uint32_t i = 0; i < input.size(); ++i) {
    const float z = 8.89f + float(i) * 0.00017f;
    static_cast<DeepBoundedRecord<false> &>(input[i]) = {
        z, z, 1.0f / 32, 5u | DEEP_BOUNDED_ELIGIBLE, 2,
        deep_bounded_pack_normal(0, 0, 1)};
    input[i].rgb[0] = (i % 2 ? 16.0f : -0.5f) / 32;
    sum += input[i].rgb[0];
  }
  MergeOutput<true> output;
  const DeepBoundedMergePolicy policy{0.01f, 0.01f, 0.006f, 0.002f, 0};
  const auto n = deep_bounded_reduce_mature(
      input.data(), input.size(), output.planes(), 0, 1, 32, policy);
  ASSERT_EQ(n, 1u);
  EXPECT_NEAR(deep_bounded_load<true>(output.planes(), 0).rgb[0], sum, 1e-6);
  EXPECT_NEAR(output.mass(n), 1, 1e-6);
}

TEST(DeepBoundedMerge, RawSurfaceClassificationRequiresOriginalOpaqueSingleton)
{
  DeepBoundedRecord<false> hit{10, 10, 1, 5u | DEEP_BOUNDED_ELIGIBLE, 2,
                              deep_bounded_pack_normal(0, 0, 1)};
  auto opaque = hit;
  deep_bounded_classify_raw_surface(opaque, 1);
  EXPECT_TRUE(opaque.object & DEEP_BOUNDED_ELIGIBLE);
  auto multi = hit;
  deep_bounded_classify_raw_surface(multi, 2);
  EXPECT_FALSE(multi.object & DEEP_BOUNDED_ELIGIBLE);
  auto transparent = hit;
  transparent.alpha = 0.5f;
  deep_bounded_classify_raw_surface(transparent, 1);
  EXPECT_FALSE(transparent.object & DEEP_BOUNDED_ELIGIBLE);
  auto emission = hit;
  emission.alpha = 0;
  deep_bounded_classify_raw_surface(emission, 1);
  EXPECT_FALSE(emission.object & DEEP_BOUNDED_ELIGIBLE);
  EXPECT_TRUE(deep_bounded_mature_eligible(emission.object));
  auto medium = volume(10, 1);
  deep_bounded_classify_raw_surface(medium, 1);
  EXPECT_EQ(medium.object, DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE);
  EXPECT_FALSE(deep_bounded_mature_eligible(DEEP_BOUNDED_NONSURFACE));
}

TEST(DeepBoundedMerge, UnqualifiedSurfaceLayersKeepAlphaCompressionGate)
{
  DeepBoundedRecord<false> input[] = {{10, 10, 0.2f, 5, 2, deep_bounded_pack_normal(0, 0, 1)},
                                     {10.001f, 10.001f, 0.64f, 5, 2,
                                      deep_bounded_pack_normal(0, 0, 1)}};
  MergeOutput<false> output;
  const DeepBoundedMergePolicy policy{0.01f, 0, 0, 0, 0.001f};
  const auto n = deep_bounded_reduce_mature(input, 2, output.planes(), 0, 1, 32, policy);
  EXPECT_EQ(n, 2u);
  EXPECT_NEAR(output.mass(n), 0.84, 1e-6);
  EXPECT_TRUE(deep_bounded_mature_eligible(output.object[0]));
  EXPECT_FALSE(output.object[0] & DEEP_BOUNDED_ELIGIBLE);
}

TEST(DeepBoundedMerge, OpaqueNormalAnchorDoesNotDriftWithLargerContributions)
{
  DeepBoundedRecord<false> input[3];
  const float angles[] = {0, 0.174532925f, 0.436332313f};
  const float q[] = {0.1f, 0.4f, 0.5f};
  for (int i = 0; i < 3; ++i) {
    const float z = 10 + float(i) * 0.001f;
    input[i] = {z, z, q[i], 5u | DEEP_BOUNDED_ELIGIBLE, 2,
                deep_bounded_pack_normal(sinf(angles[i]), 0, cosf(angles[i]))};
  }
  MergeOutput<false> output;
  const auto n = deep_bounded_reduce_mature(
      input, 3, output.planes(), 0, 1, 32, DeepBoundedMergePolicy{});
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(output.normal[0], input[0].normal);
  EXPECT_EQ(output.z[0], input[0].z);
}

TEST(DeepBoundedMerge, MixedPressureDoesNotCreateOpaqueSurfaceQualification)
{
  DeepBoundedRecord<false> input[] = {
      {10, 10, 0.75f, 5u | DEEP_BOUNDED_ELIGIBLE, 2,
       deep_bounded_pack_normal(0, 0, 1)}, volume(12, 0.25f)};
  MergeOutput<false> output;
  const auto n = deep_bounded_reduce_mature(
      input, 2, output.planes(), 0, 1, 1, DeepBoundedMergePolicy{});
  ASSERT_EQ(n, 1u);
  EXPECT_FALSE(output.object[0] & DEEP_BOUNDED_ELIGIBLE);
  EXPECT_TRUE(deep_bounded_mature_eligible(output.object[0]));
  EXPECT_NEAR(output.mass(n), 1, 1e-6);
}

TEST(DeepBoundedMerge, RawSurfaceEligibilityDoesNotEnableMatureGrouping)
{
  std::array<DeepBoundedRecord<false>, 32> input{};
  for (uint32_t i = 0; i < input.size(); ++i) {
    const float z = 10 + float(i) * 0.0001f;
    input[i] = {z, z, 1.0f / 32, 5u | DEEP_BOUNDED_ELIGIBLE, 2,
                deep_bounded_pack_normal(0, 0, 1)};
  }
  MergeOutput<false> output;
  const DeepBoundedMergePolicy policy{0.01f, 0, 0, 0, 0.001f};
  const auto n = deep_bounded_reduce(
      input.data(), input.size(), output.planes(), 0, 1, 32, policy, false, 0);
  EXPECT_GT(n, 1u);
}

TEST(DeepBoundedMerge, MatureSurfaceTagRoundTripsCompactIdentityWithoutLoss)
{
  MergeOutput<false> output;
  auto planes = output.planes();
  planes.shader = 0;
  for (const uint32_t flag : {0u, DEEP_BOUNDED_ELIGIBLE}) {
    const DeepBoundedRecord<false> r{10, 10, 0.2f, 5u | flag, 0x40000002u,
                                    deep_bounded_pack_normal(0.3f, 0.2f, 0.9f)};
    ASSERT_TRUE(deep_bounded_identity_fits(r));
    deep_bounded_store(planes, 0, r);
    const auto decoded = deep_bounded_load<false>(planes, 0);
    EXPECT_EQ(decoded.object, r.object);
    EXPECT_EQ(decoded.shader, r.shader);
    EXPECT_EQ(decoded.normal, r.normal);
    EXPECT_TRUE(deep_bounded_mature_eligible(decoded.object));
  }
}
uint64_t reference_rgb_source_index(const DeepBoundedPlanes &raw,
                                     const uint64_t root,
                                     const uint64_t stride,
                                     const uint32_t count,
                                     const DeepBoundedSourceKind kind,
                                     const float anchor)
{
  if (kind == DeepBoundedSourceKind::UNBOUND || kind == DeepBoundedSourceKind::EXCLUDED) {
    return UINT64_MAX;
  }
  float nearest = FLT_MAX;
  uint64_t best = UINT64_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t index = root + uint64_t(i) * stride;
    if (!(deep_bounded_load_object(raw, index) & DEEP_BOUNDED_ELIGIBLE)) continue;
    const float z = deep_bounded_ptr<const float>(raw.z)[index];
    const float back = deep_bounded_ptr<const float>(raw.z_back)[index];
    const bool volume = (deep_bounded_load_object(raw, index) & ~DEEP_BOUNDED_ELIGIBLE) ==
                            DEEP_BOUNDED_NONSURFACE &&
                        deep_bounded_load_shader(raw, index) == 0;
    const float distance = fmaxf(fmaxf(z - anchor, anchor - back), 0.0f);
    if (distance < nearest) {
      best = index;
      nearest = distance;
    }
    if (distance == 0.0f && ((kind == DeepBoundedSourceKind::VOLUME && volume) ||
                           (kind == DeepBoundedSourceKind::POINT && z == back && !volume)))
    {
      best = index;
      break;
    }
  }
  return best;
}

TEST(DeepBoundedMerge, RGBSourceLookupPreservesOriginalSelection)
{
  std::mt19937 random(260109);
  std::uniform_real_distribution<float> value(0, 100);
  for (uint32_t trial = 0; trial < 20000; ++trial) {
    MergeOutput<true> storage;
    auto raw = storage.planes();
    if (trial & 1u) raw.shader = 0;
    const uint32_t count = trial % 33;
    constexpr uint64_t root = 2, stride = 3;
    for (uint32_t i = 0; i < count; ++i) {
      const float z = value(random);
      const bool point = random() & 1;
      const bool surface = random() & 1;
      const bool eligible = random() % 4 != 0;
      DeepBoundedRecord<true> record = {};
      record.z = z;
      record.z_back = point ? z : z + value(random);
      record.object = (surface ? 7u : DEEP_BOUNDED_NONSURFACE) |
                      (eligible ? DEEP_BOUNDED_ELIGIBLE : 0u);
      record.shader = surface ? 2u : uint32_t(random() & 1u);
      deep_bounded_store(raw, root + i * stride, record);
    }
    float anchor = value(random);
    if (count && trial % 3 == 0) {
      const auto sample = deep_bounded_load<true>(raw, root + (trial % count) * stride);
      anchor = trial & 2u ? sample.z : sample.z_back;
    }
    if (trial % 127 == 0) anchor = FLT_MAX;
    const auto kind = DeepBoundedSourceKind(trial % 4);
    EXPECT_EQ(deep_bounded_rgb_source_index(raw, root, stride, count, kind, anchor),
              reference_rgb_source_index(raw, root, stride, count, kind, anchor)) << trial;
  }
}

TEST(DeepBoundedMerge, RGBSourceLookupKeepsExactKindPriorityAndTieOrder)
{
  MergeOutput<true> storage;
  const auto raw = storage.planes();
  DeepBoundedRecord<true> record = {};
  record.z = 1;
  record.z_back = 10;
  record.object = DEEP_BOUNDED_ELIGIBLE | DEEP_BOUNDED_NONSURFACE;
  record.shader = 0;
  deep_bounded_store(raw, 0, record);
  record.z = record.z_back = 5;
  record.object = 7u | DEEP_BOUNDED_ELIGIBLE;
  record.shader = 2;
  deep_bounded_store(raw, 1, record);
  EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 2, DeepBoundedSourceKind::POINT, 5), 1u);
  EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 2, DeepBoundedSourceKind::VOLUME, 5), 0u);
  EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 2, DeepBoundedSourceKind::POINT, 20), 0u);
  EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 0, DeepBoundedSourceKind::POINT, 5), UINT64_MAX);
  EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 2, DeepBoundedSourceKind::UNBOUND, 5), UINT64_MAX);
  for (const float anchor : {1.0f, 10.0f, FLT_MAX, -FLT_MAX}) {
    EXPECT_EQ(deep_bounded_rgb_source_index(raw, 0, 1, 1, DeepBoundedSourceKind::POINT, anchor),
              reference_rgb_source_index(raw, 0, 1, 1, DeepBoundedSourceKind::POINT, anchor));
  }
}

TEST(DeepBoundedMerge, RGBOrderedQueryExactlyMatchesLinearReference)
{
  std::mt19937 rng(0x28cc65u);
  for (uint32_t trial = 0; trial < 20000; ++trial) {
    const uint32_t count = rng() % 33;
    std::array<DeepBoundedRecord<true>, 32> records{};
    float front = 1.f, back = 1.f;
    for (uint32_t i = 0; i < count; ++i) {
      front += float(rng() % 5) * .03125f;
      back = std::max(back, front + float(rng() % 8) * .0625f);
      auto &r = records[i];
      r.z = front;
      r.z_back = back;
      r.alpha = .1f;
      r.object = ((rng() % 3) ? DEEP_BOUNDED_NONSURFACE : 7u) |
                 ((rng() % 5) ? DEEP_BOUNDED_ELIGIBLE : 0u);
      r.shader = rng() % 2;
    }
    for (int compact = 0; compact < 2; ++compact) {
      MergeOutput<true> m;
      auto p = m.planes();
      if (compact)
        p.shader = 0;
      for (uint32_t i = 0; i < count; ++i)
        deep_bounded_store(p, 3 + uint64_t(i) * 2, records[i]);
      for (uint32_t q = 0; q < 12; ++q) {
        float anchor;
        if (q == 0)
          anchor = FLT_MAX;
        else if (q == 1)
          anchor = -FLT_MAX;
        else if (q == 2)
          anchor = std::numeric_limits<float>::quiet_NaN();
        else if (q == 3)
          anchor = std::numeric_limits<float>::infinity();
        else if (q == 4)
          anchor = 0;
        else if (count && q < 9) {
          auto &r = records[rng() % count];
          anchor = (q & 1) ? r.z : r.z_back;
        }
        else
          anchor = float(rng() % 4096) / 128.f;
        auto kind = DeepBoundedSourceKind(rng() % 6);
        ASSERT_EQ(deep_bounded_rgb_source_index(p, 3, 2, count, kind, anchor),
                  deep_bounded_rgb_source_index_ordered(p, 3, 2, count, kind, anchor, true))
            << trial << ":" << compact << ":" << q;
      }
    }
  }
}

TEST(DeepBoundedMerge, RawEndOrderHintPreservesInsertAndPressure)
{
  std::mt19937 rng(0x159ca2u);
  DeepBoundedMergePolicy policy = {.01f, .01f, .006f, .002f, 0.f};
  for (uint32_t trial = 0; trial < 5000; ++trial) {
    MergeOutput<true> m;
    auto p = m.planes();
    uint32_t count = 0;
    bool ordered = true;
    float forward = 1, last_back = 1;
    for (uint32_t event = 0; event < 64; ++event) {
      if (count == 32) {
        std::array<DeepBoundedRecord<true>, 32> a;
        float prefix = 0;
        for (uint32_t i = 0; i < count; ++i) {
          a[i] = deep_bounded_load<true>(p, i);
          float q = (1 - prefix) * a[i].alpha;
          a[i].alpha = q;
          prefix = deep_bounded_unit(prefix + q);
        }
        uint32_t status = 0;
        count = deep_bounded_reduce(a.data(), count, p, 0, 1, 31, policy, false, address(&status));
        ASSERT_EQ(status, 0u);
        ASSERT_GT(count, 0u);
        ASSERT_LE(count, 31u);
        if (ordered)
          for (uint32_t i = 1; i < count; ++i)
            ASSERT_LE(m.z_back[i - 1], m.z_back[i]);
      }
      DeepBoundedRecord<true> r = {};
      forward += float(rng() % 5) * .03125f;
      r.z = trial % 2 ? 1.f + float(rng() % 256) / 128.f : forward;
      last_back = std::max(last_back, r.z + float(rng() % 8) * .0625f);
      r.z_back = trial % 2 ? r.z + float(rng() % 64) / 128.f : last_back;
      r.alpha = .02f;
      r.object = DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE;
      uint32_t slot = count;
      while (slot && m.z[slot - 1] > r.z) {
        auto prev = deep_bounded_load<true>(p, slot - 1);
        deep_bounded_store(p, slot, prev);
        --slot;
      }
      ordered = deep_bounded_ordered_end_insert(p, 0, 1, count, slot, r.z_back, ordered);
      deep_bounded_store(p, slot, r);
      ++count;
      bool oracle = true;
      for (uint32_t i = 1; i < count; ++i)
        oracle &= m.z_back[i - 1] <= m.z_back[i];
      ASSERT_TRUE(!ordered || oracle) << trial << ":" << event;
      float anchor = r.z + (r.z_back - r.z) * .5f;
      auto kind = DeepBoundedSourceKind::VOLUME;
      ASSERT_EQ(deep_bounded_rgb_source_index(p, 0, 1, count, kind, anchor),
                deep_bounded_rgb_source_index_ordered(p, 0, 1, count, kind, anchor, ordered));
    }
  }
}

TEST(DeepBoundedMerge, RGBOrderedQueryKeepsCoincidentKindsAndFarDistanceTies)
{
  for (const bool compact : {false, true}) {
    MergeOutput<true> m;
    auto raw = m.planes();
    if (compact)
      raw.shader = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      DeepBoundedRecord<true> r = {};
      r.z = r.z_back = 3.0f;
      r.alpha = 0.1f;
      r.object = (i == 3 ? 7u : DEEP_BOUNDED_NONSURFACE) | DEEP_BOUNDED_ELIGIBLE;
      deep_bounded_store(raw, 2 + uint64_t(i) * 3, r);
    }
    for (const auto kind : {DeepBoundedSourceKind::POINT, DeepBoundedSourceKind::VOLUME}) {
      for (const float anchor : {3.0f, 1e30f, -1e30f, FLT_MAX, -FLT_MAX}) {
        EXPECT_EQ(deep_bounded_rgb_source_index(raw, 2, 3, 16, kind, anchor),
                  deep_bounded_rgb_source_index_ordered(raw, 2, 3, 16, kind, anchor, true));
      }
    }
    EXPECT_EQ(deep_bounded_rgb_source_index_ordered(
                  raw, 2, 3, 16, DeepBoundedSourceKind::POINT, 3.0f, true),
              11u);
  }
}

}  // namespace

namespace {
template<bool Physical> void compare_fused_mature_total()
{
  std::mt19937 random(13693);
  for (uint32_t trial = 0; trial < 2000; ++trial) {
    const uint32_t count = random() % 129;
    const uint32_t capacity = 1 + random() % 32;
    std::array<DeepBoundedRecord<Physical>, 128> reference{}, fused{};
    const float tolerance = trial % 3 == 0 ? 0 : trial % 3 == 1 ? .01f : .1f;
    const DeepBoundedMergePolicy policy{tolerance, tolerance, .006f, .002f, .0001f};
    for (uint32_t i = 0; i < count; ++i) {
      auto &r = reference[i];
      r.z = 1 + i * .003f;
      r.z_back = r.z + (trial % 4 == 0 ? .001f : 0);
      r.alpha = (random() % 17) / float(17 * std::max(1u, count));
      r.object = trial % 4 == 1 ? 7u | DEEP_BOUNDED_ELIGIBLE :
                                   DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE;
      r.shader = 0;
      r.normal = deep_bounded_pack_normal(0, 0, 1);
      if constexpr (Physical) {
        for (int c = 0; c < 3; ++c) r.rgb[c] = r.alpha * (1 + c * .02f);
      }
    }
    fused = reference;
    MergeOutput<Physical> a, b;
    const auto na = deep_bounded_reduce_mature<Physical, false>(
        reference.data(), count, a.planes(), 0, 1, capacity, policy);
    const auto nb = deep_bounded_reduce_mature<Physical, true>(
        fused.data(), count, b.planes(), 0, 1, capacity, policy);
    ASSERT_EQ(na, nb) << trial;
    for (uint32_t i = 0; i < na; ++i) {
      const auto ra = deep_bounded_load<Physical>(a.planes(), i);
      const auto rb = deep_bounded_load<Physical>(b.planes(), i);
      ASSERT_EQ(std::memcmp(&ra, &rb, sizeof(ra)), 0) << trial << ":" << i;
    }
  }
}
TEST(DeepBoundedMerge, FusedTotalPreservesSimplePartitions)
{
  compare_fused_mature_total<false>();
}
TEST(DeepBoundedMerge, FusedTotalPreservesPhysicalPartitions)
{
  compare_fused_mature_total<true>();
}
}  // namespace
