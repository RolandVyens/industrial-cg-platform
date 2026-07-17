/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "IMB_deep_sample_merge.hh"

namespace blender::imbuf::deep_merge {

struct TestDeepSample {
  float r, g, b, a;
  float z, z_back;
};

template<> struct DeepSampleTraits<TestDeepSample> {
  static float r(const TestDeepSample &sample)
  {
    return sample.r;
  }
  static float g(const TestDeepSample &sample)
  {
    return sample.g;
  }
  static float b(const TestDeepSample &sample)
  {
    return sample.b;
  }
  static float a(const TestDeepSample &sample)
  {
    return sample.a;
  }
  static float z(const TestDeepSample &sample)
  {
    return sample.z;
  }
  static float z_back(const TestDeepSample &sample)
  {
    return sample.z_back;
  }
  static void set_r(TestDeepSample &sample, const float value)
  {
    sample.r = value;
  }
  static void set_g(TestDeepSample &sample, const float value)
  {
    sample.g = value;
  }
  static void set_b(TestDeepSample &sample, const float value)
  {
    sample.b = value;
  }
  static void set_a(TestDeepSample &sample, const float value)
  {
    sample.a = value;
  }
  static void set_z_back(TestDeepSample &sample, const float value)
  {
    sample.z_back = value;
  }
};

}  // namespace blender::imbuf::deep_merge

namespace blender::imbuf::tests {

using deep_merge::TestDeepSample;

TEST(IMB_deep_sample_merge, composite_near_sample_over_far_sample)
{
  TestDeepSample samples[] = {
      {0.25f, 0.0f, 0.0f, 0.25f, 1.00f, 1.00f},
      {0.0f, 0.0f, 0.50f, 0.50f, 1.01f, 1.01f},
  };

  const size_t merged_count = deep_merge::merge_sorted_deep_samples(
      samples, 2, 0.1f, 1.0f, 1.0e-5f);

  EXPECT_EQ(merged_count, 1);
  EXPECT_NEAR(samples[0].r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(samples[0].g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(samples[0].b, 0.375f, 1.0e-6f);
  EXPECT_NEAR(samples[0].a, 0.625f, 1.0e-6f);
  EXPECT_NEAR(samples[0].z, 1.0f, 1.0e-6f);
  EXPECT_NEAR(samples[0].z_back, 1.0f, 1.0e-6f);
}

}  // namespace blender::imbuf::tests
