/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "session/deep_frame_production.h"
#include "session/deep_bounded_format.h"

CCL_NAMESPACE_BEGIN

/* Shared cold host policy. CPU/GPU traversal and physical layouts remain separate. */
inline bool deep_bounded_same_family(const DeepCanonicalEncodeConfig &a, const DeepCanonicalEncodeConfig &b)
{
  return a.depth_tolerance == b.depth_tolerance && a.alpha_tolerance == b.alpha_tolerance;
}

inline size_t deep_bounded_encoding_owner(const std::span<const DeepFrozenConsumer> consumers, const size_t index)
{
  const auto &target = consumers[index].config;
  size_t result = index;
  for (size_t i = 0; i < consumers.size(); ++i) {
    const auto &candidate = consumers[i].config;
    if (!deep_bounded_same_family(target, candidate) || target.precision != candidate.precision ||
        (!target.alpha_only && candidate.alpha_only))
    {
      continue;
    }
    const bool result_alpha = consumers[result].config.alpha_only;
    if ((result_alpha && !candidate.alpha_only) ||
        (result_alpha == candidate.alpha_only && i < result))
    {
      result = i;
    }
  }
  return result;
}

inline uint32_t deep_bounded_family_count(const std::span<const DeepFrozenConsumer> consumers)
{
  uint32_t count = 0;
  for (size_t i = 0; i < consumers.size(); ++i) {
    bool first = true;
    for (size_t j = 0; j < i; ++j) {
      first &= !deep_bounded_same_family(consumers[i].config, consumers[j].config);
    }
    count += first;
  }
  return count;
}

CCL_NAMESPACE_END
