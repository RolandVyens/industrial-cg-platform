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

#include "IMB_deep_sample.hh"

namespace blender::imbuf::deep_merge {

template<typename Sample> struct DeepSampleTraits;

template<> struct DeepSampleTraits<blender::DeepSample> {
  static float r(const blender::DeepSample &sample)
  {
    return sample.r;
  }

  static float g(const blender::DeepSample &sample)
  {
    return sample.g;
  }

  static float b(const blender::DeepSample &sample)
  {
    return sample.b;
  }

  static float a(const blender::DeepSample &sample)
  {
    return sample.a;
  }

  static float z(const blender::DeepSample &sample)
  {
    return sample.z;
  }

  static float z_back(const blender::DeepSample &sample)
  {
    return sample.z_back;
  }

  static void set_r(blender::DeepSample &sample, const float value)
  {
    sample.r = value;
  }

  static void set_g(blender::DeepSample &sample, const float value)
  {
    sample.g = value;
  }

  static void set_b(blender::DeepSample &sample, const float value)
  {
    sample.b = value;
  }

  static void set_a(blender::DeepSample &sample, const float value)
  {
    sample.a = value;
  }

  static void set_z_back(blender::DeepSample &sample, const float value)
  {
    sample.z_back = value;
  }
};

template<typename Sample>
inline size_t merge_sorted_deep_samples(Sample *samples,
                                        size_t count,
                                        const float depth_merge_threshold,
                                        const float alpha_merge_threshold,
                                        const float volume_depth_epsilon,
                                        const bool preserve_opaque_surface_duplicates = false)
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
      const bool prev_opaque_surface = !prev_volume && prev_a >= (1.0f - volume_depth_epsilon);
      const bool curr_opaque_surface = !curr_volume && curr_a >= (1.0f - volume_depth_epsilon);

      if (prev_volume == curr_volume) {
        if (!(preserve_opaque_surface_duplicates && prev_opaque_surface && curr_opaque_surface)) {
          bool depth_similar = std::abs(curr_z - prev_z) < depth_merge_threshold;
          if (prev_volume) {
            /* Volumes: allow merging of contiguous segments along the ray. */
            depth_similar = (curr_z <= prev_z_back + depth_merge_threshold);
          }
          const bool alpha_similar = std::abs(curr_a - prev_a) < alpha_merge_threshold;
          if (depth_similar && alpha_similar) {
            /* Samples are sorted front to back: composite the nearer previous sample first. */
            const float one_minus_a = 1.0f - prev_a;
            const float prev_r = DeepSampleTraits<Sample>::r(prev);
            const float prev_g = DeepSampleTraits<Sample>::g(prev);
            const float prev_b = DeepSampleTraits<Sample>::b(prev);

            DeepSampleTraits<Sample>::set_r(prev, prev_r + DeepSampleTraits<Sample>::r(current) *
                                                         one_minus_a);
            DeepSampleTraits<Sample>::set_g(prev, prev_g + DeepSampleTraits<Sample>::g(current) *
                                                         one_minus_a);
            DeepSampleTraits<Sample>::set_b(prev, prev_b + DeepSampleTraits<Sample>::b(current) *
                                                         one_minus_a);
            DeepSampleTraits<Sample>::set_a(prev, prev_a + curr_a * one_minus_a);

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
    }

    if (write_idx != read_idx) {
      samples[write_idx] = current;
    }
    write_idx++;
  }

  return write_idx;
}

}  // namespace blender::imbuf::deep_merge
