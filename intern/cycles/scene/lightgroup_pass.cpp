/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/lightgroup_pass.h"

CCL_NAMESPACE_BEGIN

#define LIGHTGROUP_SPLIT_PASS_POLICY(M) \
  M("use_lightgroup_light_pass_aov_diffuse_combined", \
    "diffuse_", \
    PASS_DIFFUSE, \
    pass_lightgroup_diffuse) \
  M("use_lightgroup_light_pass_aov_diffuse_direct", \
    "diffuse_direct_", \
    PASS_DIFFUSE_DIRECT, \
    pass_lightgroup_diffuse_direct) \
  M("use_lightgroup_light_pass_aov_diffuse_indirect", \
    "diffuse_indirect_", \
    PASS_DIFFUSE_INDIRECT, \
    pass_lightgroup_diffuse_indirect) \
  M("use_lightgroup_light_pass_aov_glossy_combined", \
    "glossy_", \
    PASS_GLOSSY, \
    pass_lightgroup_glossy) \
  M("use_lightgroup_light_pass_aov_glossy_direct", \
    "glossy_direct_", \
    PASS_GLOSSY_DIRECT, \
    pass_lightgroup_glossy_direct) \
  M("use_lightgroup_light_pass_aov_glossy_indirect", \
    "glossy_indirect_", \
    PASS_GLOSSY_INDIRECT, \
    pass_lightgroup_glossy_indirect) \
  M("use_lightgroup_light_pass_aov_transmission_combined", \
    "transmission_", \
    PASS_TRANSMISSION, \
    pass_lightgroup_transmission) \
  M("use_lightgroup_light_pass_aov_transmission_direct", \
    "transmission_direct_", \
    PASS_TRANSMISSION_DIRECT, \
    pass_lightgroup_transmission_direct) \
  M("use_lightgroup_light_pass_aov_transmission_indirect", \
    "transmission_indirect_", \
    PASS_TRANSMISSION_INDIRECT, \
    pass_lightgroup_transmission_indirect) \
  M("use_lightgroup_light_pass_aov_volume_combined", \
    "volume_", \
    PASS_VOLUME, \
    pass_lightgroup_volume) \
  M("use_lightgroup_light_pass_aov_volume_direct", \
    "volume_direct_", \
    PASS_VOLUME_DIRECT, \
    pass_lightgroup_volume_direct) \
  M("use_lightgroup_light_pass_aov_volume_indirect", \
    "volume_indirect_", \
    PASS_VOLUME_INDIRECT, \
    pass_lightgroup_volume_indirect)

const LightgroupSplitPassDescriptors &lightgroup_split_pass_descriptors()
{
#define LIGHTGROUP_DESCRIPTOR(property_name, pass_name_prefix, pass_type, kernel_offset) \
  LightgroupSplitPassDescriptor{property_name, pass_name_prefix, pass_type},
  static const LightgroupSplitPassDescriptors descriptors = {
      LIGHTGROUP_SPLIT_PASS_POLICY(LIGHTGROUP_DESCRIPTOR)};
#undef LIGHTGROUP_DESCRIPTOR
  return descriptors;
}

int *lightgroup_pass_offset(KernelFilm &kernel_film, const PassType pass_type)
{
  if (pass_type == PASS_COMBINED) {
    return &kernel_film.pass_lightgroup;
  }

  switch (pass_type) {
#define LIGHTGROUP_OFFSET_CASE(property_name, pass_name_prefix, type, kernel_offset) \
  case type: \
    return &kernel_film.kernel_offset;
    LIGHTGROUP_SPLIT_PASS_POLICY(LIGHTGROUP_OFFSET_CASE)
#undef LIGHTGROUP_OFFSET_CASE
    default:
      return nullptr;
  }
}

vector<int> compact_lightgroup_split_indices(const vector<bool> &has_split_lightgroup)
{
  vector<int> split_index_map(has_split_lightgroup.size(), -1);
  int split_index = 0;
  for (size_t lightgroup_index = 0; lightgroup_index < has_split_lightgroup.size();
       lightgroup_index++)
  {
    if (has_split_lightgroup[lightgroup_index]) {
      split_index_map[lightgroup_index] = split_index++;
    }
  }

  return split_index == 0 ? vector<int>() : split_index_map;
}

#undef LIGHTGROUP_SPLIT_PASS_POLICY

CCL_NAMESPACE_END
