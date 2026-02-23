/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Utility functions for merging deep samples that are already sorted by depth.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace blender::imbuf::deep_merge {

template<typename Sample> struct DeepSampleTraits;

template<typename Sample>
inline size_t merge_sorted_deep_samples(Sample *samples,
                                        size_t count,
                                        const float depth_merge_threshold,
                                        const float alpha_merge_threshold,
                                        const float volume_depth_epsilon)
{
  if (depth_merge_threshold <= 0.0f || count <= 1) {
    return count;
  }

  size_t write_idx = 0;
  for (size_t read_idx = 0; read_idx < count; read_idx++) {
    const Sample current = samples[read_idx];

    if (write_idx > 0) {
      Sample &prev = samples[write_idx - 1];
      const float prev_z = DeepSampleTraits<Sample>::z(prev);
      const float prev_z_back = DeepSampleTraits<Sample>::z_back(prev);
      const float prev_a = DeepSampleTraits<Sample>::a(prev);
      const bool prev_volume = (prev_z_back > prev_z + volume_depth_epsilon);

      const float curr_z = DeepSampleTraits<Sample>::z(current);
      const float curr_z_back = DeepSampleTraits<Sample>::z_back(current);
      const float curr_a = DeepSampleTraits<Sample>::a(current);
      const bool curr_volume = (curr_z_back > curr_z + volume_depth_epsilon);

      if (prev_volume == curr_volume) {
        bool depth_similar = std::abs(curr_z - prev_z) < depth_merge_threshold;
        if (prev_volume) {
          /* Volumes: allow merging of contiguous segments along the ray. */
          depth_similar = (curr_z <= prev_z_back + depth_merge_threshold);
        }
        const bool alpha_similar = std::abs(curr_a - prev_a) < alpha_merge_threshold;
        if (depth_similar && alpha_similar) {
          /* Merge: composite current over previous. */
          const float one_minus_a = 1.0f - curr_a;
          const float prev_r = DeepSampleTraits<Sample>::r(prev);
          const float prev_g = DeepSampleTraits<Sample>::g(prev);
          const float prev_b = DeepSampleTraits<Sample>::b(prev);

          DeepSampleTraits<Sample>::set_r(prev, DeepSampleTraits<Sample>::r(current) +
                                                     prev_r * one_minus_a);
          DeepSampleTraits<Sample>::set_g(prev, DeepSampleTraits<Sample>::g(current) +
                                                     prev_g * one_minus_a);
          DeepSampleTraits<Sample>::set_b(prev, DeepSampleTraits<Sample>::b(current) +
                                                     prev_b * one_minus_a);
          DeepSampleTraits<Sample>::set_a(prev, curr_a + prev_a * one_minus_a);

          if (prev_volume) {
            DeepSampleTraits<Sample>::set_z_back(prev, std::max(prev_z_back, curr_z_back));
          }
          else {
            /* Keep merged surfaces as surfaces (no thickness). */
            DeepSampleTraits<Sample>::set_z_back(prev, prev_z);
          }
          continue;
        }
      }
    }

    if (write_idx != read_idx) {
      samples[write_idx] = current;
    }
    write_idx++;
  }

  return write_idx;
}

}  // namespace blender::imbuf::deep_merge
