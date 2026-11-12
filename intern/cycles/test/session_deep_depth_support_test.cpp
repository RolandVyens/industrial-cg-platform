/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <limits>

#include "session/deep_depth_support.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
DeepFrameKey frame()
{
  return {71, 2, 3, 4, 2, 0, 5, 6};
}
DeepFrozenDepthMap map_record()
{
  DeepFrozenDepthMap map;
  map.transform[0] = map.transform[5] = map.transform[10] = 1;
  map.direction[2] = 2;
  map.affine_slope = 2;
  map.tmax = 8;
  return map;
}
DeepPhysicalSourceSupport support(uint64_t map, uint64_t source = 9)
{
  DeepPhysicalSourceSupport result;
  result.depth_map = map;
  result.source = source;
  result.front = 1;
  result.back = 3;
  result.z = 2;
  result.z_back = 6;
  result.ordinal = 7;
  return result;
}
}  // namespace

TEST(DeepDepthSupport, held_views_keep_frame_and_budget_owned_pages_after_producer_destruction)
{
  DeepMemoryBudget budget(1 << 20), other(1 << 20);
  const auto baseline = budget.used();
  DeepDepthSupportView held;
  uint64_t id = 0;
  {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    ASSERT_TRUE(builder.add_map(8, map_record(), id));
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(builder.add_support(8, support(id)));
    DeepDepthSupportTable table;
    ASSERT_TRUE(builder.finish(table));
    ASSERT_TRUE(table.view(8, held));
    EXPECT_EQ(builder.add_support(8, support(id, 10)).code, DeepErrorCode::InvalidState);
  }
  EXPECT_GT(budget.used(), baseline);
  ASSERT_TRUE(held.frame_key());
  EXPECT_TRUE(*held.frame_key() == frame());
  EXPECT_EQ(held.ray(), 8u);
  EXPECT_EQ(held.support_count(), 1u);
  EXPECT_TRUE(held.is_charged_to(budget));
  EXPECT_FALSE(held.is_charged_to(other));
  DeepFrozenDepthMap record;
  ASSERT_TRUE(held.read_map(record));
  EXPECT_EQ(record.id, id);
  DeepPhysicalSourceSupport paired;
  ASSERT_TRUE(held.read_support(DeepPhysicalSourceKind::Optical, 9, paired));
  EXPECT_EQ(paired.ordinal, 7u);
  EXPECT_EQ(paired.front, 1);
  EXPECT_EQ(paired.back, 3);
  EXPECT_EQ(held.read_support(DeepPhysicalSourceKind::Radiance, 9, paired).code,
            DeepErrorCode::InvalidReference);
  DeepCameraDepthMap model;
  ASSERT_TRUE(held.make_model(model));
  double z;
  ASSERT_TRUE(model.depth(3, z));
  EXPECT_EQ(z, 6);
  held = {};
  EXPECT_EQ(budget.used(), baseline);
}

TEST(DeepDepthSupport, sorted_lookup_survives_page_rollover_and_sparse_ray_order)
{
  DeepMemoryBudget budget(1 << 20);
  DeepDepthSupportTable table;
  {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    for (uint64_t ray : {81, 3, 54, 1, 105}) {
      uint64_t map;
      ASSERT_TRUE(builder.add_map(ray, map_record(), map));
      for (uint64_t source : {21, 2, 19, 7, 13})
        ASSERT_TRUE(builder.add_support(ray, support(map, source)));
    }
    ASSERT_TRUE(builder.finish(table));
  }
  for (uint64_t ray : {1, 3, 54, 81, 105}) {
    DeepDepthSupportView view;
    ASSERT_TRUE(table.view(ray, view));
    EXPECT_EQ(view.support_count(), 5u);
    for (uint64_t source : {2, 7, 13, 19, 21}) {
      DeepPhysicalSourceSupport paired;
      ASSERT_TRUE(view.read_support(DeepPhysicalSourceKind::Optical, source, paired));
      EXPECT_EQ(paired.source, source);
    }
  }
  DeepDepthSupportView missing;
  EXPECT_FALSE(table.view(2, missing));
  EXPECT_FALSE(missing);
}

TEST(DeepDepthSupport, foreign_map_cross_ray_and_previous_frame_handles_fail_closed)
{
  DeepMemoryBudget budget(1 << 20);
  uint64_t old_map;
  {
    DeepDepthSupportBuilder previous(frame(), budget, 512);
    ASSERT_TRUE(previous.add_map(1, map_record(), old_map));
  }
  for (int mode = 0; mode < 3; mode++) {
    auto key = frame();
    ++key.invocation;
    DeepDepthSupportBuilder builder(key, budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    const auto input = support(mode == 0 ? old_map : mode == 1 ? map + 123456 : map);
    EXPECT_EQ(builder.add_support(mode == 2 ? 2 : 1, input).code, DeepErrorCode::InvalidReference);
    DeepDepthSupportTable table;
    EXPECT_FALSE(builder.finish(table));
    EXPECT_FALSE(table);
  }
}

TEST(DeepDepthSupport, duplicate_ray_or_source_identity_cannot_publish_partial_table)
{
  DeepMemoryBudget budget(1 << 20);
  for (int duplicate_ray = 0; duplicate_ray < 2; duplicate_ray++) {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    ASSERT_TRUE(builder.add_support(1, support(map)));
    if (duplicate_ray) {
      uint64_t second;
      ASSERT_TRUE(builder.add_map(1, map_record(), second));
    }
    else
      ASSERT_TRUE(builder.add_support(1, support(map)));
    DeepDepthSupportTable table;
    EXPECT_EQ(builder.finish(table).code, DeepErrorCode::InvalidReference);
    EXPECT_FALSE(table);
    EXPECT_FALSE(builder.finish(table));
    EXPECT_FALSE(table);
  }
}

TEST(DeepDepthSupport, only_world_radiance_can_extend_beyond_the_camera_optical_range)
{
  DeepMemoryBudget budget(1 << 20);
  for (int mode = 0; mode < 3; mode++) {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    auto input = support(map);
    input.back = 10;
    input.z_back = 20;
    if (mode == 1)
      input.flags = DEEP_PHYSICAL_WORLD_RADIANCE;
    if (mode == 2) {
      input.flags = DEEP_PHYSICAL_WORLD_RADIANCE;
      input.kind = DeepPhysicalSourceKind::Radiance;
    }
    const auto result = builder.add_support(1, input);
    EXPECT_EQ(bool(result), mode == 2);
    DeepDepthSupportTable table;
    EXPECT_EQ(bool(builder.finish(table)), mode == 2);
  }
}

TEST(DeepDepthSupport, infinity_is_an_explicit_nongeometric_point_not_an_optical_span)
{
  DeepMemoryBudget budget(1 << 20);
  for (int mode = 0; mode < 4; mode++) {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    auto input = support(map);
    input.front = input.back = -1;
    input.z = input.z_back = std::numeric_limits<float>::max();
    input.flags = DEEP_PHYSICAL_INFINITY | DEEP_PHYSICAL_POINT;
    input.kind = mode == 0 ? DeepPhysicalSourceKind::Optical : DeepPhysicalSourceKind::Radiance;
    if (mode == 2)
      input.flags |= DEEP_PHYSICAL_WORLD_RADIANCE;
    if (mode == 3)
      input.front = input.back = 0;
    EXPECT_EQ(bool(builder.add_support(1, input)), mode == 1);
    DeepDepthSupportTable table;
    EXPECT_EQ(bool(builder.finish(table)), mode == 1);
  }
}

TEST(DeepDepthSupport, quota_failure_and_unfinished_builder_return_all_allocations)
{
  DeepMemoryBudget budget(8192);
  const auto baseline = budget.used();
  {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    DeepStatus status;
    for (uint64_t i = 1; i < 1000 && status; i++)
      status = builder.add_support(1, support(map, i));
    EXPECT_EQ(status.code, DeepErrorCode::BudgetExceeded);
    DeepDepthSupportTable table;
    EXPECT_FALSE(builder.finish(table));
    EXPECT_FALSE(table);
    EXPECT_LE(budget.peak(), budget.quota());
  }
  EXPECT_EQ(budget.used(), baseline);
  {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t map;
    ASSERT_TRUE(builder.add_map(1, map_record(), map));
    ASSERT_TRUE(builder.add_support(1, support(map)));
  }
  EXPECT_EQ(budget.used(), baseline);
}

TEST(DeepDepthSupport, radial_model_preserves_nonunit_ray_and_frozen_camera_center)
{
  DeepMemoryBudget budget(1 << 20);
  DeepDepthSupportBuilder builder(frame(), budget, 512);
  auto input = map_record();
  input.kind = DeepFrozenDepthKind::Radial;
  input.affine_slope = 0;
  input.origin[0] = 13;
  input.origin[1] = 4;
  input.transform[3] = 10;
  uint64_t id;
  ASSERT_TRUE(builder.add_map(1, input, id));
  DeepDepthSupportTable table;
  ASSERT_TRUE(builder.finish(table));
  DeepDepthSupportView view;
  ASSERT_TRUE(table.view(1, view));
  DeepCameraDepthMap model;
  ASSERT_TRUE(view.make_model(model));
  double z;
  ASSERT_TRUE(model.depth(6, z));
  EXPECT_EQ(z, 13);
}

TEST(DeepDepthSupport, paired_depth_cannot_disagree_with_its_actual_physical_geometry)
{
  DeepMemoryBudget budget(1 << 20);
  for (int point = 0; point < 2; point++) {
    DeepDepthSupportBuilder builder(frame(), budget, 512);
    uint64_t id;
    ASSERT_TRUE(builder.add_map(1, map_record(), id));
    auto input = support(id);
    input.z = 3;
    if (point) {
      input.kind = DeepPhysicalSourceKind::Visibility;
      input.flags = DEEP_PHYSICAL_POINT;
      input.back = input.front;
      input.z_back = input.z;
    }
    EXPECT_FALSE(builder.add_support(1, input));
    DeepDepthSupportTable table;
    EXPECT_FALSE(builder.finish(table));
    EXPECT_FALSE(table);
  }
}
CCL_NAMESPACE_END
