/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "device/cuda/deep_launch.h"

CCL_NAMESPACE_BEGIN

TEST(DeepNativeLaunch, MissingResourcesRetainOriginalLaunch)
{
  DeepNativeLaunchScale scale;
  for (const int sms : {0, -1, std::numeric_limits<int>::min()}) {
    scale.initialize(sms);
    EXPECT_EQ(scale.select(1, 64), 64);
    EXPECT_EQ(scale.select(1, 128), 128);
  }
}

TEST(DeepNativeLaunch, WaveBoundariesTrackDeviceScale)
{
  for (const int sms : {1, 8, 20, 40, 80, 128, 192, 256, 512}) {
    DeepNativeLaunchScale scale;
    scale.initialize(sms);
    EXPECT_EQ(scale.select(32 * sms, 64), 32);
    EXPECT_EQ(scale.select(32 * sms + 1, 64), 64);
    EXPECT_EQ(scale.select(32 * sms, 128), 32);
    EXPECT_EQ(scale.select(32 * sms + 1, 128), 64);
    EXPECT_EQ(scale.select(64 * sms, 128), 64);
    EXPECT_EQ(scale.select(64 * sms + 1, 128), 128);
    EXPECT_EQ(scale.select(std::numeric_limits<int>::max(), 128), 128);
  }
}

TEST(DeepNativeLaunch, UnknownAndCollectiveBlocksAreNotReinterpreted)
{
  DeepNativeLaunchScale scale;
  scale.initialize(80);
  for (const int block : {0, 1, 16, 32, 96, 256, 384, 1024}) {
    EXPECT_EQ(scale.select(1, block), block);
  }
  EXPECT_EQ(scale.select(0, 128), 128);
  EXPECT_EQ(scale.select(-1, 64), 64);
}

TEST(DeepNativeLaunch, LargeResourceCountDoesNotOverflow)
{
  DeepNativeLaunchScale scale;
  scale.initialize(std::numeric_limits<int>::max());
  EXPECT_EQ(scale.one_warp_wave, std::numeric_limits<int>::max());
  EXPECT_EQ(scale.two_warp_wave, std::numeric_limits<int>::max());
  EXPECT_EQ(scale.select(std::numeric_limits<int>::max(), 128), 32);
}

TEST(DeepNativeLaunch, CoverageAndLaunchBoundsAcrossWorkSizes)
{
  for (const int sms : {1, 8, 20, 40, 80, 128, 192, 256, 512}) {
    DeepNativeLaunchScale scale;
    scale.initialize(sms);
    for (const int original : {64, 128}) {
      for (int work = 1; work <= 131073; work += 17) {
        const int block = scale.select(work, original);
        ASSERT_EQ(block % 32, 0);
        ASSERT_GE(block, 32);
        ASSERT_LE(block, original);
        const int64_t grid = (int64_t(work) + block - 1) / block;
        const int64_t original_grid = (int64_t(work) + original - 1) / original;
        ASSERT_GE(grid * block, work);
        ASSERT_LT(grid * block - work, block);
        ASSERT_LE(grid, original_grid * (original / block));
        if (work > 64 * sms) {
          ASSERT_EQ(block, original);
        }
      }
    }
  }
}

CCL_NAMESPACE_END
