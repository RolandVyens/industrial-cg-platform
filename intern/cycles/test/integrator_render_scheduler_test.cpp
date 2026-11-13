/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "integrator/render_scheduler.h"
#include "integrator/deep_bounded_layout.h"

CCL_NAMESPACE_BEGIN

TEST(IntegratorRenderScheduler, calculate_resolution_divider_for_resolution)
{
  EXPECT_EQ(calculate_resolution_divider_for_resolution(1920, 1080, 1920), 1);
  EXPECT_EQ(calculate_resolution_divider_for_resolution(1920, 1080, 960), 2);
  EXPECT_EQ(calculate_resolution_divider_for_resolution(1920, 1080, 480), 4);
}

TEST(IntegratorRenderScheduler, calculate_resolution_for_divider)
{
  EXPECT_EQ(calculate_resolution_for_divider(1920, 1080, 1), 1440);
  EXPECT_EQ(calculate_resolution_for_divider(1920, 1080, 2), 720);
  EXPECT_EQ(calculate_resolution_for_divider(1920, 1080, 4), 360);
}

TEST(IntegratorRenderScheduler, adaptive_work_bound_preserves_initial_filter_interval)
{
  AdaptiveSampling adaptive;
  adaptive.use = true;
  adaptive.adaptive_step = 16;
  adaptive.min_samples = 4;
  EXPECT_EQ(RenderScheduler::get_max_num_samples_per_work(adaptive, 256), 16);
  EXPECT_EQ(RenderScheduler::get_max_num_samples_per_work(adaptive, 10), 10);
  adaptive.min_samples = 64;
  EXPECT_EQ(RenderScheduler::get_max_num_samples_per_work(adaptive, 256), 80);
  adaptive.use = false;
  EXPECT_EQ(RenderScheduler::get_max_num_samples_per_work(adaptive, 256), 256);
}

TEST(IntegratorRenderScheduler, adaptive_work_bound_covers_every_native_filter_interval)
{
  AdaptiveSampling adaptive;
  adaptive.use = true;
  for (const int step : {1, 4, 16, 32}) {
    adaptive.adaptive_step = step;
    for (const int minimum : {0, 4, 16, 64, 128}) {
      adaptive.min_samples = minimum;
      for (const int total : {1, 10, 64, 256}) {
        const int bound = RenderScheduler::get_max_num_samples_per_work(adaptive, total);
        for (int start = 0; start < total; ++start) {
          EXPECT_LE(adaptive.align_samples(start, total - start), bound);
        }
      }
    }
  }
}

TEST(IntegratorRenderScheduler, deep_tile_admission_scores_usable_native_capacity)
{
  AdaptiveSampling adaptive;
  adaptive.use = true;
  adaptive.adaptive_step = 16;
  adaptive.min_samples = 4;
  const uint32_t samples = RenderScheduler::get_max_num_samples_per_work(adaptive, 256);
  DeepBoundedDeviceAdmission device;
  device.native_states = 1966080;
  const size_t quota = (size_t(512) << 20) - 40;
  const auto admit = [&](const uint32_t side) {
    return deep_bounded_choose_devices(side * side, samples, DeepBoundedPolicy::ALPHA,
                                      true, 1, quota, std::span(&device, 1));
  };
  const auto small = admit(64), larger = admit(128);
  ASSERT_EQ(larger.packet_samples, 16);
  EXPECT_GT(larger.roots, small.roots);
  EXPECT_LE(larger.known_total, quota);
  EXPECT_GT(admit(384).roots, 0u);

  /* Single-device cold selection uses power-of-two packet candidates; it
   * must agree with aggregate admission about usable native Work capacity. */
  const auto cold = [&](const uint32_t side, const uint32_t limit) {
    return deep_bounded_choose_packet(side * side, limit, device.native_states,
                                     DeepBoundedPolicy::ALPHA, true, 1, quota);
  };
  EXPECT_GE(cold(64, 256).roots, small.roots);
  EXPECT_GE(cold(128, 256).roots, larger.roots);
  EXPECT_GT(cold(128, samples).roots, cold(64, samples).roots);
  EXPECT_EQ(cold(128, samples).roots, larger.roots);

  /* A previously retained large phase arena cannot disappear from the quota
   * just because the new packet bound is lower. */
  ASSERT_TRUE(DeepBoundedAdmission::calculate(64 * 64, 148, device.native_states,
      DeepBoundedPolicy::ALPHA, true, 1, device.retained));
  const auto retained = deep_bounded_choose_devices(128 * 128, samples,
      DeepBoundedPolicy::ALPHA, true, 1, 0,
      std::span(&device, 1));
  EXPECT_EQ(retained.roots, 0);
}

CCL_NAMESPACE_END
