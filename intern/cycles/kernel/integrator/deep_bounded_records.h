/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/deep_bounded_types.h"

#include <float.h>
#include <math.h>

template<typename T> __device__ __forceinline__ T *deep_bounded_ptr(const uint64_t address)
{
  return reinterpret_cast<T *>(address);
}

template<bool Physical> struct DeepBoundedRecord;
template<> struct DeepBoundedRecord<false> {
  float z, z_back, alpha;
  uint32_t object, shader, normal;
};
template<> struct DeepBoundedRecord<true> : DeepBoundedRecord<false> {
  float rgb[3];
};
static_assert(sizeof(DeepBoundedRecord<false>) == 24);
static_assert(sizeof(DeepBoundedRecord<true>) == 36);

/* Optional lossless identity plane: null shader selects the combined word.
 * Keep the logical record and normal exactly unchanged. */
constexpr uint32_t deep_bounded_identity_objects = 0x1fffu;
constexpr uint32_t deep_bounded_identity_shaders = 0x1ffu;
constexpr uint32_t deep_bounded_identity_flags = 0xdfc00000u;

template<bool Physical>
__device__ __forceinline__ bool deep_bounded_identity_fits(const DeepBoundedRecord<Physical> &r)
{
  const uint32_t object = r.object & ~DEEP_BOUNDED_ELIGIBLE;
  return (object < deep_bounded_identity_objects || object == DEEP_BOUNDED_NONSURFACE) &&
         !(r.shader & ~(deep_bounded_identity_flags | deep_bounded_identity_shaders));
}

__device__ __forceinline__ uint32_t deep_bounded_load_object(
    const DeepBoundedPlanes &p, const uint64_t index)
{
  const uint32_t word = deep_bounded_ptr<const uint32_t>(p.object)[index];
  if (p.shader) return word;
  const uint32_t object = word & deep_bounded_identity_objects;
  return (object == deep_bounded_identity_objects ? DEEP_BOUNDED_NONSURFACE : object) |
         ((word << 2) & DEEP_BOUNDED_ELIGIBLE);
}

__device__ __forceinline__ uint32_t deep_bounded_load_shader(
    const DeepBoundedPlanes &p, const uint64_t index)
{
  if (p.shader) return deep_bounded_ptr<const uint32_t>(p.shader)[index];
  const uint32_t word = deep_bounded_ptr<const uint32_t>(p.object)[index];
  return ((word >> 13) & deep_bounded_identity_shaders) | (word & deep_bounded_identity_flags);
}

template<bool Physical>
__device__ __forceinline__ DeepBoundedRecord<Physical> deep_bounded_load(
    const DeepBoundedPlanes &planes, const uint64_t index)
{
  DeepBoundedRecord<Physical> record;
  record.z = deep_bounded_ptr<float>(planes.z)[index];
  record.z_back = deep_bounded_ptr<float>(planes.z_back)[index];
  record.object = deep_bounded_load_object(planes, index);
  record.shader = deep_bounded_load_shader(planes, index);
  record.normal = deep_bounded_ptr<uint32_t>(planes.normal)[index];
  if constexpr (Physical) {
    const DeepBoundedRGBA value = deep_bounded_ptr<DeepBoundedRGBA>(planes.rgba)[index];
    record.alpha = value.a;
    record.rgb[0] = value.r;
    record.rgb[1] = value.g;
    record.rgb[2] = value.b;
  }
  else {
    record.alpha = deep_bounded_ptr<float>(planes.rgba)[index];
  }
  return record;
}

template<bool Physical>
__device__ __forceinline__ void deep_bounded_store(const DeepBoundedPlanes &planes,
                                                    const uint64_t index,
                                                    const DeepBoundedRecord<Physical> &record)
{
  deep_bounded_ptr<float>(planes.z)[index] = record.z;
  deep_bounded_ptr<float>(planes.z_back)[index] = record.z_back;
  if (planes.shader) {
    deep_bounded_ptr<uint32_t>(planes.object)[index] = record.object;
    deep_bounded_ptr<uint32_t>(planes.shader)[index] = record.shader;
  }
  else {
    const uint32_t object = record.object & ~DEEP_BOUNDED_ELIGIBLE;
    deep_bounded_ptr<uint32_t>(planes.object)[index] =
        (object == DEEP_BOUNDED_NONSURFACE ? deep_bounded_identity_objects : object) |
        ((record.shader & deep_bounded_identity_shaders) << 13) |
        (record.shader & deep_bounded_identity_flags) |
        ((record.object & DEEP_BOUNDED_ELIGIBLE) >> 2);
  }
  deep_bounded_ptr<uint32_t>(planes.normal)[index] = record.normal;
  if constexpr (Physical) {
    deep_bounded_ptr<DeepBoundedRGBA>(planes.rgba)[index] = {
        record.rgb[0], record.rgb[1], record.rgb[2], record.alpha};
  }
  else {
    deep_bounded_ptr<float>(planes.rgba)[index] = record.alpha;
  }
}

__device__ __forceinline__ float deep_bounded_unit(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}

__device__ __forceinline__ bool deep_bounded_terminal(const uint32_t object,
                                                        const uint32_t shader,
                                                        const float z,
                                                        const float back)
{
  return (object & ~DEEP_BOUNDED_ELIGIBLE) == DEEP_BOUNDED_NONSURFACE &&
         shader == 1 && z == DEEP_BOUNDED_WORLD_TERMINAL && back == z;
}

__device__ __forceinline__ uint64_t deep_bounded_terminal_index(
    const DeepBoundedPlanes &raw, const uint64_t root, const uint64_t stride, const uint32_t count)
{
  for (uint32_t i = count; i > 0; --i) {
    const uint64_t index = root + uint64_t(i - 1) * stride;
    const float z = deep_bounded_ptr<const float>(raw.z)[index];
    if (z < DEEP_BOUNDED_WORLD_TERMINAL) {
      break;
    }
    if (deep_bounded_terminal(deep_bounded_load_object(raw, index),
                              deep_bounded_load_shader(raw, index),
                              z,
                              deep_bounded_ptr<const float>(raw.z_back)[index]))
    {
      return index;
    }
  }
  return UINT64_MAX;
}

/* Physical whole contributions use the current raw chain, whose source flags
 * retain their native meaning. Keep the general nearest/first exact match;
 * a unique eligible record with an exact endpoint needs no distance search. */
__device__ __forceinline__ uint64_t deep_bounded_rgb_source_index(
    const DeepBoundedPlanes &raw, const uint64_t root, const uint64_t stride,
    const uint32_t count, const DeepBoundedSourceKind kind, const float anchor)
{
  if (kind == DeepBoundedSourceKind::UNBOUND || kind == DeepBoundedSourceKind::EXCLUDED) {
    return UINT64_MAX;
  }
  if (count == 1) {
    if (!(deep_bounded_load_object(raw, root) & DEEP_BOUNDED_ELIGIBLE)) {
      return UINT64_MAX;
    }
    if (anchor == deep_bounded_ptr<const float>(raw.z)[root] ||
        anchor == deep_bounded_ptr<const float>(raw.z_back)[root])
    {
      return root;
    }
  }
  float nearest = FLT_MAX;
  uint64_t best = UINT64_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t index = root + uint64_t(i) * stride;
    const uint32_t object = deep_bounded_load_object(raw, index);
    if (!(object & DEEP_BOUNDED_ELIGIBLE)) {
      continue;
    }
    const float z = deep_bounded_ptr<const float>(raw.z)[index];
    const float back = deep_bounded_ptr<const float>(raw.z_back)[index];
    const bool volume = (object & ~DEEP_BOUNDED_ELIGIBLE) == DEEP_BOUNDED_NONSURFACE &&
                        deep_bounded_load_shader(raw, index) == 0;
    const float distance = fmaxf(fmaxf(z - anchor, anchor - back), 0.0f);
    if (distance < nearest) {
      best = index;
      nearest = distance;
    }
    if (distance == 0.0f && ((kind == DeepBoundedSourceKind::VOLUME && volume) ||
                           (kind == DeepBoundedSourceKind::POINT && z == back && !volume)))
    {
      return index;
    }
  }
  return best;
}

/* Called after the stable front-depth insertion shifts, before storing the new
 * record. An unproven chain stays unproven. Raw contiguous pressure groups
 * retain an end between their first and last input ends, preserving this order. */
__device__ __forceinline__ bool deep_bounded_ordered_end_insert(const DeepBoundedPlanes &raw,
                                                                const uint64_t root,
                                                                const uint64_t stride,
                                                                const uint32_t count,
                                                                const uint32_t slot,
                                                                const float back,
                                                                const bool previous)
{
  if (!count)
    return true;
  if (!previous)
    return false;
  const float *ends = deep_bounded_ptr<const float>(raw.z_back);
  if (slot && ends[root + uint64_t(slot - 1) * stride] > back)
    return false;
  return slot == count || back <= ends[root + uint64_t(slot + 1) * stride];
}

__device__ __forceinline__ bool deep_bounded_rgb_source_exact_kind(
    const DeepBoundedPlanes &raw,
    const uint64_t index,
    const uint32_t object,
    const DeepBoundedSourceKind kind,
    const float z,
    const float back)
{
  const bool volume = (object & ~DEEP_BOUNDED_ELIGIBLE) == DEEP_BOUNDED_NONSURFACE &&
                      deep_bounded_load_shader(raw, index) == 0;
  return (kind == DeepBoundedSourceKind::VOLUME && volume) ||
         (kind == DeepBoundedSourceKind::POINT && z == back && !volume);
}

/* Fronts are stably sorted by capture; the proof bit also guarantees sorted
 * ends. Find the end bracket, then retain the original earliest-distance tie
 * and first exact-kind rules. Overlaps, eligibility holes and float-distance
 * ties can require a longer scan; unproven/short chains use the original path. */
__device__ __forceinline__ uint64_t
deep_bounded_rgb_source_index_ordered(const DeepBoundedPlanes &raw,
                                      const uint64_t root,
                                      const uint64_t stride,
                                      const uint32_t count,
                                      const DeepBoundedSourceKind kind,
                                      const float anchor,
                                      const bool ordered)
{
  if (kind == DeepBoundedSourceKind::POINT && anchor == DEEP_BOUNDED_WORLD_TERMINAL) {
    const uint64_t terminal = deep_bounded_terminal_index(raw, root, stride, count);
    if (terminal != UINT64_MAX) {
      return terminal;
    }
  }
  if (!ordered || count < 8 || !isfinite(anchor) || kind == DeepBoundedSourceKind::UNBOUND ||
      kind == DeepBoundedSourceKind::EXCLUDED)
  {
    return deep_bounded_rgb_source_index(raw, root, stride, count, kind, anchor);
  }
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const float back = deep_bounded_ptr<const float>(raw.z_back)[root + uint64_t(mid) * stride];
    if (back < anchor)
      lo = mid + 1;
    else
      hi = mid;
  }
  const uint32_t split = lo;
  float nearest = FLT_MAX;
  uint64_t best = UINT64_MAX, exact = UINT64_MAX;
  for (uint32_t j = split; j > 0; --j) {
    const uint64_t index = root + uint64_t(j - 1) * stride;
    const float z = deep_bounded_ptr<const float>(raw.z)[index];
    const float back = deep_bounded_ptr<const float>(raw.z_back)[index];
    const float distance = fmaxf(fmaxf(z - anchor, anchor - back), 0.0f);
    if (distance > nearest)
      break;
    const uint32_t object = deep_bounded_load_object(raw, index);
    if (!(object & DEEP_BOUNDED_ELIGIBLE))
      continue;
    if (distance < nearest || (distance == nearest && best != UINT64_MAX && index < best)) {
      nearest = distance;
      best = index;
    }
    if (distance == 0.0f && deep_bounded_rgb_source_exact_kind(raw, index, object, kind, z, back))
    {
      exact = index;
    }
  }
  if (exact != UINT64_MAX)
    return exact;
  for (uint32_t j = split; j < count; ++j) {
    const uint64_t index = root + uint64_t(j) * stride;
    const float z = deep_bounded_ptr<const float>(raw.z)[index];
    const float back = deep_bounded_ptr<const float>(raw.z_back)[index];
    const float distance = fmaxf(fmaxf(z - anchor, anchor - back), 0.0f);
    if (distance > nearest)
      break;
    const uint32_t object = deep_bounded_load_object(raw, index);
    if (!(object & DEEP_BOUNDED_ELIGIBLE))
      continue;
    if (distance < nearest) {
      nearest = distance;
      best = index;
    }
    if (distance == 0.0f && deep_bounded_rgb_source_exact_kind(raw, index, object, kind, z, back))
    {
      return index;
    }
  }
  return best;
}

__device__ __forceinline__ float deep_bounded_local_alpha(const float q, const float prefix)
{
  return deep_bounded_unit(q / fmaxf(1.0f - prefix, 0x1p-23f));
}

/* Raw uses the high object bit as source eligibility. Every stored real Surface
 * is a valid source: full holdout has zero alpha and is never appended. Mature
 * Surface records can therefore use this bit for opaque grouping provenance;
 * non-Surface records retain the original eligibility meaning. */
__device__ __forceinline__ bool deep_bounded_mature_eligible(const uint32_t object)
{
  return (object & ~DEEP_BOUNDED_ELIGIBLE) != DEEP_BOUNDED_NONSURFACE ||
         (object & DEEP_BOUNDED_ELIGIBLE);
}

template<bool Physical>
__device__ __forceinline__ void deep_bounded_classify_raw_surface(
    DeepBoundedRecord<Physical> &record, const uint32_t raw_count)
{
  const uint32_t object = record.object & ~DEEP_BOUNDED_ELIGIBLE;
  if (object != DEEP_BOUNDED_NONSURFACE) {
    /* A pressure-reduced raw chain must append again and has at least two
     * records. Only a singleton can prove this is an original opaque hit. */
    const bool opaque = raw_count == 1 && (record.object & DEEP_BOUNDED_ELIGIBLE) &&
                        record.z == record.z_back && record.alpha >= 1.0f - 1e-6f;
    record.object = object | (opaque ? DEEP_BOUNDED_ELIGIBLE : 0u);
  }
}

__device__ __forceinline__ void deep_bounded_error(const uint64_t status, const uint32_t error)
{
  if (status) {
    atomicOr(deep_bounded_ptr<uint32_t>(status), error);
  }
}

__device__ __forceinline__ uint32_t deep_bounded_pack_normal(float x, float y, const float z)
{
  const float sum = fabsf(x) + fabsf(y) + fabsf(z);
  if (!(sum > 0.0f) || !isfinite(sum)) {
    return 0x80008000u;
  }
  x /= sum;
  y /= sum;
  if (z < 0.0f) {
    const float old_x = x;
    x = copysignf(1.0f - fabsf(y), x);
    y = copysignf(1.0f - fabsf(old_x), y);
  }
  const uint32_t u = uint32_t(deep_bounded_unit(x * 0.5f + 0.5f) * 65535.0f + 0.5f);
  const uint32_t v = uint32_t(deep_bounded_unit(y * 0.5f + 0.5f) * 65535.0f + 0.5f);
  return u | (v << 16);
}

__device__ __forceinline__ void deep_bounded_normal(const uint32_t packed, float (&n)[3])
{
  n[0] = float(packed & 65535u) * (2.0f / 65535.0f) - 1.0f;
  n[1] = float(packed >> 16) * (2.0f / 65535.0f) - 1.0f;
  n[2] = 1.0f - fabsf(n[0]) - fabsf(n[1]);
  const float fold = fmaxf(-n[2], 0.0f);
  n[0] -= copysignf(fold, n[0]);
  n[1] -= copysignf(fold, n[1]);
}

__device__ __forceinline__ bool deep_bounded_same_normal(const uint32_t a, const uint32_t b)
{
  if (a == b) {
    return true;
  }
  float na[3], nb[3];
  deep_bounded_normal(a, na);
  deep_bounded_normal(b, nb);
  const float dot = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
  const float aa = na[0] * na[0] + na[1] * na[1] + na[2] * na[2];
  const float bb = nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2];
  return dot > 0.0f && dot * dot >= (0.94f * 0.94f) * aa * bb;
}

template<bool Physical>
__device__ __forceinline__ bool deep_bounded_ordinary(const DeepBoundedRecord<Physical> &group,
                                                      const DeepBoundedRecord<Physical> &candidate,
                                                      const DeepBoundedMergePolicy &policy,
                                                      const float transmission,
                                                      const bool mature_colour)
{
  const bool ordinary_enabled = policy.alpha_tolerance > 0.0f || policy.depth_tolerance > 0.0f;
  if (!mature_colour && !ordinary_enabled) {
    return false;
  }
  const bool group_point = group.z == group.z_back;
  const bool candidate_point = candidate.z == candidate.z_back;
  const uint32_t object = group.object & ~DEEP_BOUNDED_ELIGIBLE;
  if (group_point != candidate_point || object != (candidate.object & ~DEEP_BOUNDED_ELIGIBLE) ||
      group.shader != candidate.shader)
  {
    return false;
  }
  if (mature_colour && group_point && object != DEEP_BOUNDED_NONSURFACE &&
      (group.object & candidate.object & DEEP_BOUNDED_ELIGIBLE))
  {
    /* Group compatible opaque camera hits before comparing their AA-encoded
     * alpha or radiance. This is Surface identity grouping, not compression. */
    return deep_bounded_same_normal(group.normal, candidate.normal);
  }
  if (!ordinary_enabled) {
    return false;
  }
  const float dz = fmaxf(0.0f, candidate.z - group.z_back);
  const bool same_point = group_point && candidate_point && group.z == candidate.z;
  const bool depth_match = policy.depth_tolerance > 0.0f && dz <= policy.depth_tolerance;
  if (!depth_match) {
    if (!(policy.alpha_tolerance > 0.0f)) {
      return false;
    }
    /* Alpha is an independent compression channel, bounded by the existing
     * projection neighbourhood so equal opacity cannot join arbitrarily far layers. */
    const float scale = group.z * policy.projection_scale;
    if (dz > 0.0f &&
        (!(scale > 0.0f) || fmaxf(1.0f, 1.0f / scale) * dz > 32.0f))
    {
      return false;
    }
    if (!same_point) {
      /* Internal alpha is absolute coverage q, not the local alpha displayed by
       * a Deep reader. Compare local alphas without two divisions per boundary.
       * Use the same denominator floor and clamp as publication. */
      const float before = fmaxf(transmission, 0x1p-23f);
      const float after = fmaxf(transmission - group.alpha, 0x1p-23f);
      const float a = fminf(group.alpha, before);
      const float b = fminf(candidate.alpha, after);
      if (!(fabsf(a * after - b * before) < policy.alpha_tolerance * before * after)) {
        return false;
      }
    }
  }
  if (object != DEEP_BOUNDED_NONSURFACE &&
      !deep_bounded_same_normal(group.normal, candidate.normal))
  {
    return false;
  }
  if constexpr (Physical) {
    if (mature_colour && !same_point) {
      const float q = group.alpha + candidate.alpha;
      for (int c = 0; c < 3; ++c) {
        const float absolute = policy.colour_absolute * transmission;
        if (q == 0.0f) {
          if (fmaxf(fabsf(group.rgb[c]), fabsf(candidate.rgb[c])) > absolute) {
            return false;
          }
          continue;
        }
        const float delta = fabsf(group.rgb[c] * candidate.alpha - candidate.rgb[c] * group.alpha);
        const float bound = fmaxf(absolute,
                                  policy.colour_relative *
                                      fmaxf(fabsf(group.rgb[c]), fabsf(candidate.rgb[c]))) *
                            q;
        if (!isfinite(delta) || !isfinite(bound) || delta > bound) {
          return false;
        }
      }
    }
  }
  return true;
}

template<bool Physical>
__device__ __forceinline__ bool deep_bounded_real_surface(const DeepBoundedRecord<Physical> &r)
{
  return r.z == r.z_back && (r.object & ~DEEP_BOUNDED_ELIGIBLE) != DEEP_BOUNDED_NONSURFACE;
}

template<bool Physical>
__device__ __forceinline__ bool deep_bounded_volume_point(const DeepBoundedRecord<Physical> &r)
{
  /* Background uses shader 1. It is not a volume scattering point. */
  return r.z == r.z_back && (r.object & ~DEEP_BOUNDED_ELIGIBLE) == DEEP_BOUNDED_NONSURFACE &&
         r.shader == 0;
}

template<bool Physical, bool TrackDepth> struct DeepBoundedGroup {
  DeepBoundedRecord<Physical> record;
  float representative_q;
  float moment = 0.0f;
  float last_z;
  bool volume;
  bool mature;

  __device__ __forceinline__ DeepBoundedGroup(const DeepBoundedRecord<Physical> &first,
                                              const bool mature)
      : record(first),
        representative_q(deep_bounded_real_surface(first) ? first.alpha : -1.0f),
        last_z(first.z),
        volume(TrackDepth && mature && deep_bounded_volume_point(first)),
        mature(mature)
  {
  }

  __device__ __forceinline__ void append(const DeepBoundedRecord<Physical> &candidate,
                                         const bool ordinary_group = true)
  {
    const bool opaque_group = mature && ordinary_group && deep_bounded_real_surface(record) &&
                              deep_bounded_real_surface(candidate) &&
                              (record.object & candidate.object & DEEP_BOUNDED_ELIGIBLE) &&
                              (record.object & ~DEEP_BOUNDED_ELIGIBLE) ==
                                  (candidate.object & ~DEEP_BOUNDED_ELIGIBLE) &&
                              record.shader == candidate.shader;
    const uint32_t source = mature ?
        ((deep_bounded_mature_eligible(record.object) ||
          deep_bounded_mature_eligible(candidate.object)) ? DEEP_BOUNDED_ELIGIBLE : 0u) :
        ((record.object | candidate.object) & DEEP_BOUNDED_ELIGIBLE);
    if constexpr (TrackDepth) {
      if (volume) {
        volume = deep_bounded_volume_point(candidate);
        if (volume) {
          /* A relative moment avoids cancellation at large camera depths.
           * Keep the original front during gating; move only at publication. */
          moment = fmaf(candidate.alpha, candidate.z - record.z, moment);
          last_z = candidate.z;
        }
      }
    }
    if (record.z_back > record.z) {
      record.z_back = fmaxf(record.z_back, candidate.z_back);
    }
    if (deep_bounded_real_surface(candidate) && candidate.alpha > representative_q) {
      representative_q = candidate.alpha;
      if (!opaque_group) {
        record.object = (candidate.object & ~DEEP_BOUNDED_ELIGIBLE) |
                        (record.object & DEEP_BOUNDED_ELIGIBLE);
        record.shader = candidate.shader;
        record.normal = candidate.normal;
      }
    }
    const bool surface = (record.object & ~DEEP_BOUNDED_ELIGIBLE) != DEEP_BOUNDED_NONSURFACE;
    const uint32_t flag = mature && surface ? (opaque_group ? DEEP_BOUNDED_ELIGIBLE : 0u) : source;
    record.object = (record.object & ~DEEP_BOUNDED_ELIGIBLE) | flag;
    record.alpha += candidate.alpha;
    if constexpr (Physical) {
      for (int c = 0; c < 3; ++c) {
        record.rgb[c] += candidate.rgb[c];
      }
    }
  }

  __device__ __forceinline__ DeepBoundedRecord<Physical> finish() const
  {
    auto result = record;
    if constexpr (TrackDepth) {
      if (volume && moment > 0.0f && record.alpha > 0.0f) {
        result.z = fminf(last_z, fmaxf(record.z, record.z + moment / record.alpha));
        result.z_back = result.z;
      }
    }
    return result;
  }
};

/* One partition at a fixed pressure. Compile-time counting avoids output
 * writes; independent warp trials need no next-pressure discovery scan. */
template<bool Physical, bool Publish, bool TrackPressure, bool Absolute = false,
         bool SumTotal = false>
__device__ __forceinline__ uint32_t
deep_bounded_partition(const DeepBoundedRecord<Physical> *input,
                       const uint32_t count,
                       const DeepBoundedPlanes &output,
                       const uint64_t first,
                       const uint64_t stride,
                       const uint32_t capacity,
                       const DeepBoundedMergePolicy &policy,
                       const bool mature_colour,
                       const float pressure,
                       float &next_pressure,
                       DeepBoundedRecord<Physical> *absolute_output = nullptr,
                       float *total = nullptr)
{
  next_pressure = FLT_MAX;
  float prefix = 0.0f;
  uint32_t written = 0;
  DeepBoundedGroup<Physical, Publish> group(input[0], mature_colour);
  for (uint32_t i = 1; i <= count; ++i) {
    if (i < count) {
      const auto &record = input[i];
      const bool forced = record.alpha <= pressure;
      if (forced ||
          deep_bounded_ordinary(group.record, record, policy, 1.0f - prefix, mature_colour))
      {
        group.append(record, !forced);
        continue;
      }
      /* Raising pressure across an already ordinary-merged record cannot
       * change this partition. Only rejected boundaries can change it. */
      if constexpr (TrackPressure) {
        next_pressure = fminf(next_pressure, record.alpha);
      }
    }
    const float q = group.record.alpha;
    if constexpr (SumTotal) {
      *total += q;
    }
    if (Publish && written < capacity) {
      auto result = group.finish();
      if constexpr (Absolute) {
        absolute_output[written] = result;
      }
      else {
        result.alpha = deep_bounded_local_alpha(q, prefix);
        deep_bounded_store(output, first + written * stride, result);
      }
    }
    ++written;
    if (!Publish && written > capacity) {
      /* This trial cannot fit. Remaining grouping/metadata/colour work has
       * no consumer; conservatively include every remaining mass as a
       * possible next pressure, without evaluating those groups. */
      if constexpr (TrackPressure) {
        for (uint32_t j = i; j < count; ++j) {
          if (input[j].alpha > pressure) {
            next_pressure = fminf(next_pressure, input[j].alpha);
          }
        }
      }
      break;
    }
    prefix = deep_bounded_unit(prefix + q);
    if (i < count) {
      group = DeepBoundedGroup<Physical, Publish>(input[i], mature_colour);
    }
  }
  return written;
}

/* The sorted reducers consume absolute coverage. Pull the terminal out before
 * applying any finite grouping, including the forced capacity paths. */
template<bool Physical>
__device__ __forceinline__ bool deep_bounded_take_terminal(
    DeepBoundedRecord<Physical> *input, uint32_t &count, DeepBoundedRecord<Physical> &terminal)
{
  uint32_t first = count;
  while (first && input[first - 1].z >= DEEP_BOUNDED_WORLD_TERMINAL) {
    --first;
  }
  uint32_t out = first;
  bool found = false;
  for (uint32_t i = first; i < count; ++i) {
    const auto &r = input[i];
    if (deep_bounded_terminal(r.object, r.shader, r.z, r.z_back)) {
      if (!found) {
        terminal = r;
        found = true;
      }
      else {
        terminal.alpha += r.alpha;
        terminal.object |= r.object & DEEP_BOUNDED_ELIGIBLE;
        if constexpr (Physical) {
          for (int c = 0; c < 3; ++c) {
            terminal.rgb[c] += r.rgb[c];
          }
        }
      }
    }
    else if (out++ != i) {
      input[out - 1] = r;
    }
  }
  count = out;
  return found;
}

template<bool Physical>
__device__ __forceinline__ uint32_t deep_bounded_publish_terminal(
    const DeepBoundedPlanes &output,
    const uint64_t first,
    const uint64_t stride,
    const uint32_t count,
    DeepBoundedRecord<Physical> terminal)
{
  float finite_prefix = 0.0f;
  uint32_t insert = 0;
  const auto *depths = deep_bounded_ptr<const float>(output.z);
  for (; insert < count; ++insert) {
    const uint64_t index = first + uint64_t(insert) * stride;
    if (depths[index] > terminal.z) {
      break;
    }
    const float alpha = Physical ? deep_bounded_ptr<const DeepBoundedRGBA>(output.rgba)[index].a :
                                   deep_bounded_ptr<const float>(output.rgba)[index];
    finite_prefix = deep_bounded_unit(fmaf(1.0f - finite_prefix, alpha, finite_prefix));
  }
  const float q = terminal.alpha;
  terminal.alpha = deep_bounded_local_alpha(q, finite_prefix);
  if (insert == count) {
    deep_bounded_store(output, first + uint64_t(count) * stride, terminal);
    return count + 1;
  }

  /* Very large scenes can have real depths past the format's fixed terminal.
   * Preserve sorted depth and absolute coverage when inserting in that case. */
  auto next = deep_bounded_load<Physical>(output, first + uint64_t(insert) * stride);
  deep_bounded_store(output, first + uint64_t(insert) * stride, terminal);
  float prefix = deep_bounded_unit(finite_prefix + q);
  for (uint32_t i = insert + 1; i <= count; ++i) {
    auto r = next;
    if (i < count) {
      next = deep_bounded_load<Physical>(output, first + uint64_t(i) * stride);
    }
    const float finite_q = (1.0f - finite_prefix) * r.alpha;
    r.alpha = deep_bounded_local_alpha(finite_q, prefix);
    deep_bounded_store(output, first + uint64_t(i) * stride, r);
    finite_prefix = deep_bounded_unit(finite_prefix + finite_q);
    prefix = deep_bounded_unit(prefix + finite_q);
  }
  return count + 1;
}

/* Mature numeric chains have no pending RGB anchor. Compact ordinary groups
 * in the existing candidate array, then bound pressure work by one forward
 * coverage-quantile pass. Equal masses do not trigger a whole-chain collapse.
 * Raw capture retains its anchor-preserving pressure path below. */
template<bool Physical, bool FuseTotal = false>
__device__ __forceinline__ uint32_t
deep_bounded_reduce_mature_finite(DeepBoundedRecord<Physical> *input,
                           const uint32_t count,
                           const DeepBoundedPlanes &output,
                           const uint64_t first,
                           const uint64_t stride,
                           const uint32_t capacity,
                           const DeepBoundedMergePolicy &policy)
{
  if (!count || !capacity)
    return 0;
  float unused;
  if (count <= capacity) {
    return deep_bounded_partition<Physical, true, false>(
        input, count, output, first, stride, capacity, policy, true, -1.0f, unused);
  }
  float total = 0.0f;
  const uint32_t ordinary = deep_bounded_partition<Physical, true, false, true, FuseTotal>(
      input, count, output, first, stride, count, policy, true, -1.0f, unused, input, &total);
  float prefix = 0.0f;
  if (ordinary <= capacity) {
    for (uint32_t i = 0; i < ordinary; ++i) {
      auto record = input[i];
      const float q = record.alpha;
      record.alpha = deep_bounded_local_alpha(q, prefix);
      deep_bounded_store(output, first + uint64_t(i) * stride, record);
      prefix = deep_bounded_unit(prefix + q);
    }
    return ordinary;
  }
  if constexpr (!FuseTotal) {
    for (uint32_t i = 0; i < ordinary; ++i)
      total += input[i].alpha;
  }
  const float scale = total > 0.0f ? float(capacity) / total : float(capacity) / float(ordinary);
  float consumed = 0.0f;
  uint32_t written = 0, previous_bin = UINT32_MAX;
  DeepBoundedGroup<Physical, true> group(input[0], true);
  for (uint32_t i = 0; i <= ordinary; ++i) {
    uint32_t bin = UINT32_MAX;
    if (i < ordinary) {
      const auto &record = input[i];
      const float center = total > 0.0f ? consumed + 0.5f * record.alpha : float(i);
      const float position = fminf(float(capacity - 1), fmaxf(0.0f, center * scale));
      bin = uint32_t(position);
      consumed += record.alpha;
      if (i && bin == previous_bin) {
        group.append(record, false);
        continue;
      }
    }
    if (i) {
      auto result = group.finish();
      const float q = result.alpha;
      result.alpha = deep_bounded_local_alpha(q, prefix);
      deep_bounded_store(output, first + uint64_t(written++) * stride, result);
      prefix = deep_bounded_unit(prefix + q);
    }
    if (i < ordinary) {
      group = DeepBoundedGroup<Physical, true>(input[i], true);
      previous_bin = bin;
    }
  }
  return written;
}

template<bool Physical, bool FuseTotal = false>
__device__ __forceinline__ uint32_t deep_bounded_reduce_mature(
    DeepBoundedRecord<Physical> *input, uint32_t count, const DeepBoundedPlanes &output,
    const uint64_t first, const uint64_t stride, const uint32_t capacity,
    const DeepBoundedMergePolicy &policy)
{
  if (!count || !capacity) {
    return 0;
  }
  DeepBoundedRecord<Physical> terminal = {};
  if (!deep_bounded_take_terminal(input, count, terminal)) {
    return deep_bounded_reduce_mature_finite<Physical, FuseTotal>(
        input, count, output, first, stride, capacity, policy);
  }
  const uint32_t finite = deep_bounded_reduce_mature_finite<Physical, FuseTotal>(
      input, count, output, first, stride, capacity - 1, policy);
  return deep_bounded_publish_terminal(output, first, stride, finite, terminal);
}

/* Candidate alpha holds absolute mass q. Once a group is published, its slot
 * alpha is local again. Each pressure attempt sees the same bounded inputs. */
template<bool Physical>
__device__ __forceinline__ uint32_t deep_bounded_reduce(
    const DeepBoundedRecord<Physical> *input, const uint32_t count,
    const DeepBoundedPlanes &output, const uint64_t first, const uint64_t stride,
    const uint32_t capacity, const DeepBoundedMergePolicy &policy,
    const bool mature_colour, const uint64_t status)
{
  if (!count) return 0;
  float pressure = -1.0f;
  bool publish = count <= capacity;
  for (;;) {
    float next_pressure;
    const uint32_t written = publish ?
        deep_bounded_partition<Physical, true, true>(input, count, output, first, stride,
            capacity, policy, mature_colour, pressure, next_pressure) :
        deep_bounded_partition<Physical, false, true>(input, count, output, first, stride,
            capacity, policy, mature_colour, pressure, next_pressure);
    if (written <= capacity) {
      if (publish) return written;
      publish = true;
      continue;
    }
    if (!(next_pressure < FLT_MAX && next_pressure > pressure)) {
      deep_bounded_error(status, DEEP_BOUNDED_CAPACITY);
      return 0;
    }
    pressure = next_pressure;
  }
}
