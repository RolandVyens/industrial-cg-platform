/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

struct CompactScalarFit {
  float front, back;
  double q, log_before, log_after;
  uint32_t kind, source;
};

template<bool Physical> struct CompactFitValue : CompactScalarFit {};
template<> struct CompactFitValue<true> : CompactScalarFit {
  double rgb[3], rgb_epsilon[3];
};
static_assert(sizeof(CompactFitValue<false>) == 40);
static_assert(sizeof(CompactFitValue<true>) == 88);

template<bool Physical>
__device__ double compact_fit_weight(const CompactFitValue<Physical> &value)
{
  if constexpr (Physical)
    return value.q + (fabs(value.rgb[0]) + fabs(value.rgb[1]) + fabs(value.rgb[2])) / 3.0;
  return value.q;
}

template<bool Physical>
__device__ double compact_fit_fraction(const double z, const CompactFitValue<Physical> &value)
{
  return fmin(1.0, fmax(0.0, (z - value.front) / (double(value.back) - value.front)));
}

/* Returns false to emit the already legal L and seed R. There is no upstream
 * work request, original-ray query or retry of the rejected candidate. */
template<bool Physical>
__device__ bool compact_fit_extend(const CompactFitValue<Physical> &left,
                                  const CompactFitValue<Physical> &reference,
                                  const CompactFitValue<Physical> &right,
                                  const DeepGPUFrameConsumer &policy,
                                  const double footprint,
                                  CompactFitValue<Physical> &trial)
{
  if (left.kind != DEEP_COMPACT_VOLUME || right.kind != DEEP_COMPACT_VOLUME) return false;
  const double gap = fmax(0.0, double(right.front) - left.back);
  if ((policy.depth_tolerance == 0 && gap > 0) ||
      double(right.back) - right.front < gap) return false;
  const double wl = compact_fit_weight(left), wr = compact_fit_weight(right);
  if (!(wl > 0 && wr > 0)) return false;
  const double center = (wl * (double(left.front) + left.back) +
                         wr * (double(right.front) + right.back)) / (2 * (wl + wr));
  const double radius = fmin(center - left.front, double(right.back) - center);
  trial = left;
  trial.front = float(center - radius); trial.back = float(center + radius);
  trial.q = left.q + right.q;
  trial.log_after = right.log_after; /* never reconstruct the finite tail from q */
  if (!(radius > 0) || !(trial.back > trial.front)) return false;
  const double alpha_epsilon = fmax(double(policy.alpha_tolerance) - .004, .0001);
  const double fh = compact_fit_fraction(reference.back, trial);
  const double fl = compact_fit_fraction(left.back, trial);
  const bool ordinary = (gap == 0 || gap < policy.depth_tolerance) &&
                        fabs(fh * trial.q - reference.q) <= alpha_epsilon &&
                        fabs(fl * trial.q - left.q) <= alpha_epsilon;
  const bool projection = footprint > 0 && double(right.back) - left.front <= footprint;
  if (!ordinary && !projection) return false;
  if constexpr (Physical) {
    const double transmission = exp(trial.log_before);
    for (int c = 0; c < 3; c++) {
      trial.rgb[c] = left.rgb[c] + right.rgb[c];
      const double absolute = double(policy.rgb_absolute) * transmission;
      const double relative = double(policy.rgb_relative) * fmax(fabs(reference.rgb[c]), fabs(left.rgb[c]));
      const double epsilon = fmax(fmax(absolute, relative) - .004 * transmission,
                                   .0001 * transmission);
      trial.rgb_epsilon[c] = fmin(epsilon, fmin(left.rgb_epsilon[c], right.rgb_epsilon[c]));
      if (fabs(fh * trial.rgb[c] - reference.rgb[c]) > trial.rgb_epsilon[c] ||
          fabs(fl * trial.rgb[c] - left.rgb[c]) > trial.rgb_epsilon[c]) return false;
    }
  }
  return true;
}

template<bool Physical>
__device__ void compact_fit_update_reference(CompactFitValue<Physical> &reference,
                                           const CompactFitValue<Physical> &left,
                                           const float right_back)
{
  const double old_length = double(reference.back) - reference.front;
  const double new_length = double(right_back) - reference.front;
  if (old_length > 0 && new_length > 0 &&
      compact_fit_weight(reference) / old_length <= compact_fit_weight(left) / new_length) {
    const double scale = new_length / old_length;
    reference.q *= scale;
    if constexpr (Physical) {
      for (int c = 0; c < 3; c++) reference.rgb[c] *= scale;
    }
    reference.back = right_back;
  }
  else {
    reference = left;
    reference.back = right_back;
  }
}

__device__ V compact_surface_normal(const uint32_t packed)
{
  const float x = float(packed & 0xffffu) * (2.f / 65535.f) - 1.f;
  const float y = float(packed >> 16) * (2.f / 65535.f) - 1.f;
  V n{x, y, 1.f - fabsf(x) - fabsf(y)};
  if (n.z < 0) {
    const float a = copysignf(1.f - fabsf(y), x), b = copysignf(1.f - fabsf(x), y);
    n = {a, b, 1.f - fabsf(a) - fabsf(b)};
  }
  const float inv = rsqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  return {n.x * inv, n.y * inv, n.z * inv};
}

__device__ bool compact_surface_group_compatible(const DeepCompactPrimitive &a,
                                               const DeepCompactPrimitive &b)
{
  if (a.kind != b.kind || a.object != b.object || a.shader != b.shader) return false;
  const V na = compact_surface_normal(a.normal), nb = compact_surface_normal(b.normal);
  return na.x * nb.x + na.y * nb.y + na.z * nb.z >= .94f;
}

__device__ bool compact_native_opaque_surface(const DeepCompactPrimitive &value)
{
  return value.kind == DEEP_COMPACT_SURFACE && value.front == value.back &&
         value.log_before == 0.0 && value.log_after == -INFINITY;
}

__device__ double compact_projection_footprint(const float2 projection, const double depth)
{
  return projection.x > 0 ? double(projection.x) * fmax(0.0, depth - projection.y) :
         projection.x < 0 ? -double(projection.x) : 0.0;
}

template<bool Physical>
__device__ bool compact_points_can_merge(const CompactFitValue<Physical> &left,
                                        const CompactFitValue<Physical> &right,
                                        const DeepGPUFrameConsumer &policy,
                                        const DeepCompactPrimitive *primitives)
{
  if (!(policy.depth_tolerance > 0 && policy.alpha_tolerance > 0) || left.kind != right.kind ||
      left.kind == DEEP_COMPACT_BLACK || left.kind == DEEP_COMPACT_BACKGROUND ||
      left.kind == DEEP_COMPACT_COLLAPSED_VOLUME ||
      !compact_surface_group_compatible(primitives[left.source], primitives[right.source]) ||
      !(fabs(double(right.front) - left.front) < policy.depth_tolerance)) return false;
  const double a = -expm1(left.log_after - left.log_before);
  const double b = -expm1(right.log_after - right.log_before);
  if (!(fabs(a - b) < policy.alpha_tolerance)) return false;
  if constexpr (Physical) {
    const double prefix = exp(left.log_before);
    for (int c = 0; c < 3; c++) {
      const double epsilon = fmax(double(policy.rgb_absolute) * prefix,
                                  double(policy.rgb_relative) * fmax(fabs(left.rgb[c]), fabs(right.rgb[c])));
      if (fabs(left.rgb[c] - right.rgb[c]) > epsilon) return false;
    }
  }
  return true;
}
