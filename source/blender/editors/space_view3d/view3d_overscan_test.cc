/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "RE_pipeline.h"

#include "view3d_intern.hh"

namespace blender::tests {

TEST(View3DOverscan, RenderResolutionPreservesValidHighDimensions)
{
  EXPECT_EQ(view3d_cycles_render_resolution_get(32767, 100), 32767);
  EXPECT_EQ(view3d_cycles_render_resolution_get(32768, 100), 32768);
  EXPECT_EQ(view3d_cycles_render_resolution_get(65536, 100), 65536);
  EXPECT_EQ(view3d_cycles_render_resolution_get(65536, 50), 32768);
}

TEST(View3DOverscan, PercentagePaddingUsesLargestReferenceDimension)
{
  const RenderOverscanPadding padding = RE_overscan_padding_resolve(
      false, 10.0f, 0, 0, 0, 0, 1920, 1080);

  EXPECT_EQ(padding.left, 192);
  EXPECT_EQ(padding.right, 192);
  EXPECT_EQ(padding.bottom, 192);
  EXPECT_EQ(padding.top, 192);
  EXPECT_TRUE(padding.any());
}

TEST(View3DOverscan, PercentagePaddingRoundsOutward)
{
  const RenderOverscanPadding padding = RE_overscan_padding_resolve(
      false, 0.01f, 0, 0, 0, 0, 1920, 1080);

  EXPECT_EQ(padding.left, 1);
  EXPECT_EQ(padding.right, 1);
  EXPECT_EQ(padding.bottom, 1);
  EXPECT_EQ(padding.top, 1);
}

TEST(View3DOverscan, PixelPaddingClampsEachEdge)
{
  const RenderOverscanPadding padding = RE_overscan_padding_resolve(
      true, 50.0f, -1, 2, -3, 4, 1920, 1080);

  EXPECT_EQ(padding.left, 0);
  EXPECT_EQ(padding.right, 2);
  EXPECT_EQ(padding.bottom, 0);
  EXPECT_EQ(padding.top, 4);
  EXPECT_TRUE(padding.any());
}

TEST(View3DOverscan, NonPositivePercentageDisablesPadding)
{
  const RenderOverscanPadding padding = RE_overscan_padding_resolve(
      false, -10.0f, 1, 2, 3, 4, 1920, 1080);

  EXPECT_FALSE(padding.any());
}

}  // namespace blender::tests
