/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <set>
#include <string>

#include "testing/testing.h"

#include "scene/lightgroup_pass.h"

CCL_NAMESPACE_BEGIN

TEST(LightgroupPassPolicy, descriptors_are_complete_and_unique)
{
  const auto &descriptors = lightgroup_split_pass_descriptors();
  EXPECT_EQ(descriptors.size(), 12u);

  std::set<PassType> pass_types;
  std::set<std::string> property_names;
  std::set<std::string> pass_name_prefixes;
  KernelFilm kernel_film = {};
  for (const LightgroupSplitPassDescriptor &descriptor : descriptors) {
    EXPECT_TRUE(pass_types.insert(descriptor.pass_type).second);
    EXPECT_TRUE(property_names.insert(descriptor.property_name).second);
    EXPECT_TRUE(pass_name_prefixes.insert(descriptor.pass_name_prefix).second);
    EXPECT_NE(lightgroup_pass_offset(kernel_film, descriptor.pass_type), nullptr);
  }
}

TEST(LightgroupPassPolicy, combined_only_has_no_device_map)
{
  const vector<bool> has_split_lightgroup(32, false);
  EXPECT_TRUE(compact_lightgroup_split_indices(has_split_lightgroup).empty());
}

TEST(LightgroupPassPolicy, sparse_groups_keep_scene_index_shape)
{
  vector<bool> has_split_lightgroup(5, false);
  has_split_lightgroup[1] = true;
  has_split_lightgroup[4] = true;

  const vector<int> index_map = compact_lightgroup_split_indices(has_split_lightgroup);
  ASSERT_EQ(index_map.size(), 5u);
  EXPECT_EQ(index_map[0], -1);
  EXPECT_EQ(index_map[1], 0);
  EXPECT_EQ(index_map[2], -1);
  EXPECT_EQ(index_map[3], -1);
  EXPECT_EQ(index_map[4], 1);
}

CCL_NAMESPACE_END
