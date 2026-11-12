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
#include <array>
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
inline double interval_contribution_fraction(const Sample &sample, const double depth)
{
  using Traits = DeepSampleTraits<Sample>;
  if (depth <= Traits::z(sample)) return 0.0;
  if (depth >= Traits::z_back(sample)) return 1.0;
  const double fraction = (depth - Traits::z(sample)) /
                          (double(Traits::z_back(sample)) - Traits::z(sample));
  const double alpha = Traits::a(sample);
  if (alpha == 0.0) return fraction;
  return -std::expm1(std::log1p(-alpha) * fraction) / alpha;
}

/* A bounded run retains original prefix witnesses instead of repeatedly comparing a
 * candidate against an already approximated pair. Post-integration fitting checks
 * opacity and premultiplied RGB with separate residuals. The probes are a local production
 * policy, not a formal continuous/global error certificate. Original knots and four inner
 * subintervals protect depth cuts; a failed fit keeps the original output run. */
template<typename Sample> class VolumeMergeReference {
  struct Witness {
    double depth, transmission;
    std::array<double, 3> rgb;
  };
  static constexpr size_t maximum_segments = 32;
  std::array<Witness, maximum_segments * 5> witnesses_;
  size_t count_ = 0, segments_ = 0;
  double transmission_ = 1.0;
  std::array<double, 3> rgb_ = {};

 public:
  void reset(const Sample &sample)
  {
    count_ = segments_ = 0;
    transmission_ = 1.0;
    rgb_ = {};
    append(sample);
  }

  bool append(const Sample &sample)
  {
    using Traits = DeepSampleTraits<Sample>;
    if (segments_ == maximum_segments) return false;
    const std::array<double, 3> color = {Traits::r(sample), Traits::g(sample), Traits::b(sample)};
    for (int probe = 0; probe <= 4; probe++) {
      const double depth = double(Traits::z(sample)) +
                           (double(Traits::z_back(sample)) - Traits::z(sample)) * (probe / 4.0);
      const double fraction = probe == 0 ? 0.0 :
                               probe == 4 ? 1.0 : interval_contribution_fraction(sample, depth);
      Witness &witness = witnesses_[count_++];
      witness.depth = depth;
      witness.transmission = transmission_ * (1.0 - Traits::a(sample) * fraction);
      for (int channel = 0; channel < 3; channel++)
        witness.rgb[channel] = rgb_[channel] + transmission_ * color[channel] * fraction;
    }
    for (int channel = 0; channel < 3; channel++)
      rgb_[channel] += transmission_ * color[channel];
    transmission_ *= 1.0 - Traits::a(sample);
    segments_++;
    return true;
  }

  bool accepts(const Sample &candidate, const double tolerance) const
  {
    if (!std::isfinite(tolerance) || tolerance < 0.0) return false;
    using Traits = DeepSampleTraits<Sample>;
    const std::array<double, 3> color = {
        Traits::r(candidate), Traits::g(candidate), Traits::b(candidate)};
    for (size_t i = 0; i < count_; i++) {
      const Witness &reference = witnesses_[i];
      const double fraction = interval_contribution_fraction(candidate, reference.depth);
      const double transmission = 1.0 - Traits::a(candidate) * fraction;
      if (!std::isfinite(transmission) ||
          std::abs(transmission - reference.transmission) > tolerance) return false;
      for (int channel = 0; channel < 3; channel++) {
        const double actual = color[channel] * fraction;
        if (!std::isfinite(actual) || std::abs(actual - reference.rgb[channel]) >
                                         tolerance * (1.0 + std::abs(reference.rgb[channel])))
          return false;
      }
    }
    return true;
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
  VolumeMergeReference<Sample> volume_reference;
  float previous_input_back = 0.0f;
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
          bool alpha_similar = std::abs(curr_a - prev_a) < alpha_merge_threshold;
          if (prev_volume) {
            Sample candidate = prev;
            const float remaining = 1.0f - prev_a;
            DeepSampleTraits<Sample>::set_r(candidate, DeepSampleTraits<Sample>::r(prev) +
                                                      remaining * DeepSampleTraits<Sample>::r(current));
            DeepSampleTraits<Sample>::set_g(candidate, DeepSampleTraits<Sample>::g(prev) +
                                                      remaining * DeepSampleTraits<Sample>::g(current));
            DeepSampleTraits<Sample>::set_b(candidate, DeepSampleTraits<Sample>::b(prev) +
                                                      remaining * DeepSampleTraits<Sample>::b(current));
            DeepSampleTraits<Sample>::set_a(candidate, prev_a + remaining * curr_a);
            DeepSampleTraits<Sample>::set_z_back(candidate, std::max(prev_z_back, curr_z_back));
            /* Overlapping intervals cannot be composed as two disjoint front-to-back pieces. */
            alpha_similar = curr_z >= previous_input_back &&
                            volume_reference.append(current) &&
                            volume_reference.accepts(candidate, alpha_merge_threshold);
          }
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
            previous_input_back = curr_z_back;
            continue;
          }
        }
      }
    }

    if (write_idx != read_idx) {
      samples[write_idx] = current;
    }
    write_idx++;
    const float front = DeepSampleTraits<Sample>::z(current);
    previous_input_back = DeepSampleTraits<Sample>::z_back(current);
    if (previous_input_back > front + volume_depth_epsilon) volume_reference.reset(current);
  }

  return write_idx;
}

}  // namespace blender::imbuf::deep_merge
