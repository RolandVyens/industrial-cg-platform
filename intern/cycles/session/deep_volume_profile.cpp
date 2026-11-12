/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_volume_profile.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "util/math.h"
#include "util/map.h"

CCL_NAMESPACE_BEGIN

namespace {

constexpr float deep_volume_alpha_epsilon = 1.0e-8f;
constexpr float deep_volume_depth_epsilon = 1.0e-6f;

struct DeepVolumeRay {
  uint32_t camera_sample;
  std::span<const DeepSampleData *> segments;
  bool complete = false;
  float front;
  float back;
  size_t segment_count = 0;
  size_t next_segment = 0;
};

bool deep_volume_accumulate_ray_transmittance(const DeepVolumeRay &ray,
                                              const vector<float> &boundaries,
                                              vector<float> &transmittance_sum,
                                              vector<float3> &slope_difference,
                                              vector<float3> &intercept_difference,
                                              vector<int> &opaque_difference)
{
  std::fill(slope_difference.begin(), slope_difference.end(), zero_float3());
  std::fill(intercept_difference.begin(), intercept_difference.end(), zero_float3());
  std::fill(opaque_difference.begin(), opaque_difference.end(), 0);

  for (const DeepSampleData *segment : ray.segments) {
    const size_t partial_begin = size_t(
        std::upper_bound(boundaries.begin(), boundaries.end(), segment->z) - boundaries.begin());
    if (partial_begin == boundaries.size()) {
      continue;
    }

    const uint32_t flags = deep_sample_info_flags(segment->flags);
    float3 log_transmittance;
    if (flags & DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH) {
      const float3 optical_depth = make_float3(__uint_as_float(segment->surface_object),
                                                __uint_as_float(segment->surface_prim),
                                                __uint_as_float(segment->surface_shader));
      if (!isfinite_safe(optical_depth) || reduce_min(optical_depth) < 0.0f) {
        continue;
      }
      log_transmittance = -optical_depth;
    }
    else {
      const float segment_transmittance = 1.0f - clamp(segment->a, 0.0f, 1.0f);
      if (segment_transmittance <= deep_volume_alpha_epsilon) {
        opaque_difference[partial_begin]++;
        opaque_difference[boundaries.size()]--;
        continue;
      }
      log_transmittance = make_float3(logf(segment_transmittance));
    }

    /* Finite optical depth remains finite inside the interval even when its endpoint is
     * effectively opaque. Never replace all partial prefixes by zero transmission. */

    const size_t full_begin = size_t(
        std::lower_bound(boundaries.begin(), boundaries.end(), segment->z_back) -
        boundaries.begin());
    if (partial_begin < full_begin) {
      const float3 slope = log_transmittance / (segment->z_back - segment->z);
      const float3 intercept = -slope * segment->z;
      if (!isfinite_safe(slope) || !isfinite_safe(intercept)) return false;
      slope_difference[partial_begin] += slope;
      slope_difference[full_begin] -= slope;
      intercept_difference[partial_begin] += intercept;
      intercept_difference[full_begin] -= intercept;
    }
    intercept_difference[full_begin] += log_transmittance;
    intercept_difference[boundaries.size()] -= log_transmittance;
  }

  float3 slope = zero_float3();
  float3 intercept = zero_float3();
  int opaque_segments = 0;
  for (size_t i = 0; i < boundaries.size(); i++) {
    slope += slope_difference[i];
    intercept += intercept_difference[i];
    opaque_segments += opaque_difference[i];
    const float3 log_ray_transmittance = slope * boundaries[i] + intercept;
    if (!isfinite_safe(slope) || !isfinite_safe(intercept) ||
        !isfinite_safe(log_ray_transmittance)) return false;
    float ray_transmittance = 0.0f;
    if (opaque_segments == 0) {
      float3 rgb_transmittance;
      if (log_ray_transmittance.x == log_ray_transmittance.y &&
          log_ray_transmittance.x == log_ray_transmittance.z)
      {
        /* Equal inputs produce the same expf result; retain the existing RGB average operation. */
        rgb_transmittance = make_float3(expf(log_ray_transmittance.x));
      }
      else {
        rgb_transmittance = make_float3(expf(log_ray_transmittance.x),
                                       expf(log_ray_transmittance.y),
                                       expf(log_ray_transmittance.z));
      }
      ray_transmittance = average(rgb_transmittance);
    }
    transmittance_sum[i] += clamp(ray_transmittance, 0.0f, 1.0f);
  }
  return true;
}

void deep_volume_accumulate_ray_radiance(const DeepVolumeRay &ray,
                                         const vector<float> &boundaries,
                                         vector<float3> &radiance_sum)
{
  for (const DeepSampleData *segment : ray.segments) {
    /* Extinction-only events contribute no radiance at any depth. Keep their optical-depth
     * accumulation in the separate transmittance pass; do not discard tiny nonzero RGB. */
    if (segment->r == 0.0f && segment->g == 0.0f && segment->b == 0.0f) {
      continue;
    }
    const float alpha = clamp(segment->a, 0.0f, 1.0f);
    const float3 radiance = make_float3(segment->r, segment->g, segment->b);
    const bool has_tau = deep_sample_info_flags(segment->flags) &
                         DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH;
    const float3 tau = make_float3(__uint_as_float(segment->surface_object),
                                  __uint_as_float(segment->surface_prim),
                                  __uint_as_float(segment->surface_shader));
    for (size_t i = 0; i < boundaries.size(); i++) {
      const float depth = boundaries[i];
      float3 fraction = zero_float3();
      if (depth >= segment->z_back) {
        fraction = one_float3();
      }
      else if (depth > segment->z) {
        const float interval_fraction = (depth - segment->z) / (segment->z_back - segment->z);
        if (has_tau) {
          for (int channel = 0; channel < 3; channel++) {
            fraction[channel] = tau[channel] == 0.0f ? interval_fraction :
                                   -expm1f(-tau[channel] * interval_fraction) /
                                       -expm1f(-tau[channel]);
          }
        }
        else if (alpha > deep_volume_alpha_epsilon) {
          const float partial_alpha = 1.0f - powf(1.0f - alpha, interval_fraction);
          fraction = make_float3(partial_alpha / alpha);
        }
        else {
          fraction = make_float3(interval_fraction);
        }
      }
      radiance_sum[i] += radiance * fraction;
    }
  }
}

void deep_volume_match_radiance_target(vector<float3> &radiance,
                                       const vector<float> &transmittance,
                                       const float3 target_rgb,
                                       const bool has_captured_rgb)
{
  /* Native contributions own their depth distribution. Beauty is only a legacy fallback
   * for extinction-only inputs; it must not rescale captured RGB, including zero-alpha light. */
  if (has_captured_rgb) {
    return;
  }
  const float3 source_rgb = radiance.back();
  const float target_alpha = 1.0f - transmittance.back();
  for (int channel = 0; channel < 3; channel++) {
    const float source = source_rgb[channel];
    const float target = target_rgb[channel];
    if (fabsf(source) > deep_volume_alpha_epsilon) {
      const float scale = target / source;
      for (float3 &value : radiance) {
        value[channel] *= scale;
      }
    }
    else if (fabsf(target) > deep_volume_alpha_epsilon &&
             target_alpha > deep_volume_alpha_epsilon)
    {
      /* Legacy volume events have no RGB. Retain a deterministic fallback which reaches Beauty,
       * while new semantic events preserve their captured depth-dependent radiance above. */
      for (size_t i = 0; i < radiance.size(); i++) {
        radiance[i][channel] = target * (1.0f - transmittance[i]) / target_alpha;
      }
    }
    else {
      for (float3 &value : radiance) {
        value[channel] = 0.0f;
      }
    }
  }
}

}  // namespace

int deep_volume_profile_camera_population(const int rendered_camera_samples,
                                          const int retained_deep_samples,
                                          const int deep_sample_capacity)
{
  /* Capacity describes retained events, not retained camera rays. Even when the event buffer is
   * full, reducing the population to the distinct retained ray IDs biases the profile toward rays
   * which happened to generate volume events. */
  (void)retained_deep_samples;
  (void)deep_sample_capacity;
  return max(1, rendered_camera_samples);
}

bool deep_build_volume_profile(const DeepSampleData *sample_data,
                               const size_t offset,
                               const int count,
                               const int total_camera_samples,
                               const int max_profile_samples,
                               const float3 target_rgb,
                               const float target_alpha,
                               vector<DeepVolumeProfileSample> &profile)
{
  profile.clear();
  if (sample_data == nullptr || count <= 0 || total_camera_samples <= 0 ||
      max_profile_samples <= 0 || !std::isfinite(target_alpha) || !isfinite_safe(target_rgb))
  {
    return false;
  }
  /* Beauty is deliberately not an alpha authority. It remains an argument while the existing
   * RGB distribution path is kept stable during this alpha-only development slice. */
  (void)target_alpha;

  vector<float> boundaries;
  vector<DeepVolumeRay> rays;
  vector<int> event_ray_indices(size_t(count), 0);
  /* Dense IDs are bounded by the observed camera population, never the maximum ID.
   * Sparse/high IDs use a count-bounded lookup without changing ray encounter order. */
  vector<int> ray_indices(size_t(min(count, total_camera_samples)), -1);
  unordered_map<uint32_t, size_t> sparse_ray_indices;
  float profile_front = sample_data[offset].z;
  float profile_back = sample_data[offset].z_back;
  float second_back = profile_front;
  bool has_captured_rgb = false;

  for (int i = 0; i < count; i++) {
    const DeepSampleData &sample = sample_data[offset + i];
    const uint32_t flags = deep_sample_info_flags(sample.flags);
    if ((flags & DEEP_SAMPLE_FLAG_VOLUME_METADATA) == 0 ||
        (flags & DEEP_SAMPLE_FLAG_HARD_SURFACE_METADATA) != 0 || !std::isfinite(sample.a) ||
        sample.a < 0.0f || sample.a > 1.0f || !std::isfinite(sample.r) ||
        !std::isfinite(sample.g) || !std::isfinite(sample.b) ||
        !std::isfinite(sample.z) || !std::isfinite(sample.z_back) ||
        sample.z_back <= sample.z + deep_volume_depth_epsilon)
    {
      return false;
    }

    if (flags & DEEP_SAMPLE_FLAG_VOLUME_RGB_OPTICAL_DEPTH) {
      const float3 tau = make_float3(__uint_as_float(sample.surface_object),
                                    __uint_as_float(sample.surface_prim),
                                    __uint_as_float(sample.surface_shader));
      if (!isfinite_safe(tau) || reduce_min(tau) < 0.0f) {
        return false;
      }
    }

    const uint32_t camera_sample = deep_sample_info_camera_sample(sample.flags);
    has_captured_rgb |= sample.r != 0.0f || sample.g != 0.0f || sample.b != 0.0f;
    size_t ray_index;
    bool inserted;
    if (camera_sample < ray_indices.size()) {
      int &index = ray_indices[camera_sample];
      inserted = index < 0;
      if (inserted) {
        index = int(rays.size());
      }
      ray_index = size_t(index);
    }
    else {
      const auto [index, added] = sparse_ray_indices.try_emplace(camera_sample, rays.size());
      ray_index = index->second;
      inserted = added;
    }
    if (inserted) {
      if (rays.size() >= size_t(total_camera_samples)) {
        return false;
      }
      rays.push_back({camera_sample, {}, false, sample.z, sample.z_back});
    }
    /* Keep encounter order: it is also the floating-point transmittance summation order. */
    DeepVolumeRay &ray = rays[ray_index];
    event_ray_indices[i] = int(ray_index);
    ray.segment_count++;
    ray.complete |= (flags & DEEP_SAMPLE_FLAG_VOLUME_PRIMARY_RAY_COMPLETE) != 0;
    ray.front = min(ray.front, sample.z);
    ray.back = max(ray.back, sample.z_back);
    profile_front = min(profile_front, sample.z);
    for (const float depth : {sample.z, sample.z_back}) {
      if (depth > profile_back) {
        second_back = profile_back;
        profile_back = depth;
      }
      else if (depth < profile_back) {
        second_back = max(second_back, depth);
      }
    }
  }

  /* One stable scatter replaces growing a separate event vector for every camera ray. */
  vector<const DeepSampleData *> ray_segments(size_t(count), nullptr);
  size_t segment_offset = 0;
  for (DeepVolumeRay &ray : rays) {
    ray.next_segment = segment_offset;
    ray.segments = std::span<const DeepSampleData *>(ray_segments.data() + segment_offset,
                                                    ray.segment_count);
    segment_offset += ray.segment_count;
  }
  for (int i = 0; i < count; i++) {
    DeepVolumeRay &ray = rays[event_ray_indices[i]];
    ray_segments[ray.next_segment++] = &sample_data[offset + i];
  }

  /* Multiple rays use an aligned grid, so only the union's first/last retained endpoints matter.
   * If the last distinct gap exceeds epsilon, unique() must retain the maximum endpoint.
   * Otherwise keep the original sort/unique: its epsilon chain can choose a different endpoint.
   * Signed zero also retains the original ordering behavior. */
  if (rays.size() > 1 && profile_front != 0.0f && profile_back != 0.0f &&
      profile_back - second_back > deep_volume_depth_epsilon)
  {
    boundaries.clear();
    boundaries.push_back(profile_front);
    boundaries.push_back(profile_back);
  }
  else {
    boundaries.reserve(static_cast<size_t>(count) * 2);
    for (int i = 0; i < count; i++) {
      const DeepSampleData &sample = sample_data[offset + i];
      boundaries.push_back(sample.z);
      boundaries.push_back(sample.z_back);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(),
                                 boundaries.end(),
                                 [](const float a, const float b) {
                                   return fabsf(a - b) <= deep_volume_depth_epsilon;
                                 }),
                     boundaries.end());
  }
  if (boundaries.size() < 2) {
    return false;
  }

  /* A retained segment without an end-of-traversal marker is incomplete optical data. Treating
   * it as either vacuum or a hard-surface cut biases every deeper prefix. MoonRay avoids this by
   * handing DeepBuffer the complete VolumeProperties list after ray marching. */
  if (std::any_of(rays.begin(), rays.end(), [](const DeepVolumeRay &ray) { return !ray.complete; }))
  {
    return false;
  }

  const size_t max_boundaries = static_cast<size_t>(max_profile_samples) + 1;
  if (rays.size() > 1 || boundaries.size() > max_boundaries) {
    /* Follow MoonRay DeepBuffer's alignment rule: derive one common, evenly spaced output grid
     * from the average input segment length. This preserves cumulative optical depth more
     * predictably than selecting arbitrary entries from the union of jittered ray boundaries. */
    float total_ray_span = 0.0f;
    size_t total_segments = 0;
    for (const DeepVolumeRay &ray : rays) {
      total_ray_span += ray.back - ray.front;
      total_segments += ray.segments.size();
    }

    const float profile_front = boundaries.front();
    const float profile_back = boundaries.back();
    const float average_segment_length = total_segments > 0 ?
                                             total_ray_span / float(total_segments) :
                                             profile_back - profile_front;
    const size_t aligned_segment_count = min(
        static_cast<size_t>(max_profile_samples),
        max(size_t(1),
            static_cast<size_t>(ceilf((profile_back - profile_front) /
                                      max(average_segment_length, deep_volume_depth_epsilon)))));
    vector<float> reduced_boundaries;
    reduced_boundaries.reserve(aligned_segment_count + 1);
    for (size_t i = 0; i <= aligned_segment_count; i++) {
      reduced_boundaries.push_back(profile_front +
                                   (profile_back - profile_front) *
                                       (float(i) / float(aligned_segment_count)));
    }
    boundaries.swap(reduced_boundaries);
  }

  const int resolved_camera_samples = total_camera_samples;
  const int clear_camera_samples = resolved_camera_samples - int(rays.size());
  vector<float> transmittance_sum(boundaries.size(), float(clear_camera_samples));
  vector<float3> radiance_sum(boundaries.size(), zero_float3());
  vector<float3> slope_difference(boundaries.size() + 1, zero_float3());
  vector<float3> intercept_difference(boundaries.size() + 1, zero_float3());
  vector<int> opaque_difference(boundaries.size() + 1, 0);
  for (const DeepVolumeRay &ray : rays) {
    if (!deep_volume_accumulate_ray_transmittance(ray,
                                             boundaries,
                                             transmittance_sum,
                                             slope_difference,
                                             intercept_difference,
                                             opaque_difference)) return false;
    deep_volume_accumulate_ray_radiance(ray, boundaries, radiance_sum);
  }

  vector<float> transmittance(boundaries.size(), 1.0f);
  for (size_t boundary_index = 0; boundary_index < boundaries.size(); boundary_index++) {
    const float active_population = float(resolved_camera_samples);
    transmittance[boundary_index] = clamp(
        transmittance_sum[boundary_index] / active_population, 0.0f, 1.0f);
    if (boundary_index > 0) {
      transmittance[boundary_index] = min(transmittance[boundary_index],
                                          transmittance[boundary_index - 1]);
    }
    radiance_sum[boundary_index] /= active_population;
  }

  deep_volume_match_radiance_target(radiance_sum, transmittance, target_rgb, has_captured_rgb);

  profile.reserve(boundaries.size() - 1);
  for (size_t i = 0; i + 1 < boundaries.size(); i++) {
    const float front_transmittance = transmittance[i];
    const float back_transmittance = min(transmittance[i + 1], front_transmittance);
    if (front_transmittance <= 0.0f) {
      if (radiance_sum[i + 1] != radiance_sum[i]) return false;
      continue;
    }

    const float alpha = clamp(1.0f - back_transmittance / front_transmittance, 0.0f, 1.0f);
    const float3 interval_rgb = (radiance_sum[i + 1] - radiance_sum[i]) /
                                front_transmittance;
    if (!isfinite_safe(interval_rgb)) return false;
    if (alpha > deep_volume_alpha_epsilon || interval_rgb.x != 0.0f || interval_rgb.y != 0.0f ||
        interval_rgb.z != 0.0f) {
      profile.push_back({interval_rgb, alpha, boundaries[i], boundaries[i + 1]});
    }
  }
  return true;
}

CCL_NAMESPACE_END
