/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_homogeneous_curve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

CCL_NAMESPACE_BEGIN

template<typename T> using CurveVector = std::vector<T, DeepBudgetAllocator<T>>;

struct DeepHomogeneousCurveStorage {
  explicit DeepHomogeneousCurveStorage(const DeepMemoryBudget &budget)
      : pages(DeepBudgetAllocator<DeepPageLease>(budget)),
        sources(DeepBudgetAllocator<DeepHomogeneousSource>(budget)),
        identities(DeepBudgetAllocator<DeepHomogeneousIdentity>(budget))
  {
  }
  DeepFrameKey frame;
  int32_t x = 0, y = 0;
  uint64_t population = 0, knot_count = 0;
  double weight = 0, error[4] = {};
  CurveVector<DeepPageLease> pages;
  CurveVector<DeepHomogeneousSource> sources;
  CurveVector<DeepHomogeneousIdentity> identities;
};

namespace {
constexpr double round_unit = std::numeric_limits<double>::epsilon();
constexpr double invalid_depth = double(std::numeric_limits<float>::max());

/* Nonnegative bound operations round outward, including subnormal products. Infinite
 * bounds request further subdivision; they never authorize a chord. */
double upper(const double x)
{
  return x == 0 ? 0 : std::nextafter(x, std::numeric_limits<double>::infinity());
}
double bound_add(const double a, const double b)
{
  return upper(a + b);
}
double bound_product(const double a, const double b)
{
  if (a == 0 || b == 0)
    return 0;
  return std::nextafter(a * b, std::numeric_limits<double>::infinity());
}

struct Sum {
  double value = 0, correction = 0;
  void add(const double term)
  {
    const double next = value + term;
    correction += std::abs(value) >= std::abs(term) ? (value - next) + term :
                                                      (term - next) + value;
    value = next;
  }
  double get() const
  {
    return value + correction;
  }
  void reset(const double next = 0)
  {
    value = next;
    correction = 0;
  }
};

struct Delta {
  double z = 0;
  double slope[4] = {}, correction[4] = {}, jump[4] = {};
  int64_t active[4] = {};
  uint32_t flags = 0;
};
struct Slab {
  double a, b;
};

DeepStatus failure(const DeepErrorCode code, const char *message)
{
  return DeepStatus::fail(code, message);
}

bool sample_less(const DeepHomogeneousRay *a, const DeepHomogeneousRay *b)
{
  /* One native integer pixel/sample can be admitted once; retry/branch/disposition
   * changes cannot manufacture a second population member. */
  return a->sample.sample < b->sample.sample;
}

DeepStatus validate(const DeepPageStore &store,
                    const DeepHomogeneousPixelInput &input,
                    const DeepHomogeneousCurvePolicy &policy,
                    CurveVector<const DeepHomogeneousRay *> &rays,
                    CurveVector<const DeepHomogeneousSegment *> &segments,
                    double &weight,
                    size_t &source_count)
{
  if (!(input.frame == store.frame_key()) || input.frame.invocation == 0)
    return failure(DeepErrorCode::InvalidFrame, "Homogeneous curve frame mismatch");
  if (!input.population_complete)
    return failure(DeepErrorCode::Incomplete, "Homogeneous pixel population is not closed");
  if (!input.native_population || input.rays.empty() ||
      input.native_population != input.rays.size())
    return failure(DeepErrorCode::PopulationMismatch, "Homogeneous native population mismatch");
  if (!std::isfinite(input.exposure) || input.exposure < 0 ||
      !std::isfinite(policy.transmission_error) || policy.transmission_error <= 0 ||
      policy.transmission_error > 1e-4 || !std::isfinite(policy.rgb_error) ||
      policy.rgb_error <= 0 || policy.rgb_error > 1e-4)
    return failure(DeepErrorCode::InvalidPayload, "Invalid homogeneous exposure/error policy");
  Sum weights;
  for (const DeepHomogeneousRay &ray : input.rays) {
    if (!ray.ray || ray.sample.x != input.x || ray.sample.y != input.y)
      return failure(DeepErrorCode::InvalidReference, "Homogeneous ray/pixel identity mismatch");
    if (!ray.native_complete)
      return failure(DeepErrorCode::Incomplete, "Homogeneous native source accounting incomplete");
    if (ray.unsupported_features)
      return failure(DeepErrorCode::InvalidPayload,
                     "M1 unsupported semantics: M2 adapter required");
    if (!std::isfinite(ray.population_weight) || ray.population_weight <= 0)
      return failure(DeepErrorCode::InvalidPayload, "Invalid homogeneous population weight");
    weights.add(ray.population_weight);
    if (!std::isfinite(weights.get()))
      return failure(DeepErrorCode::InvalidCount, "Homogeneous population weight overflow");
    rays.push_back(&ray);
    switch (ray.sample.disposition) {
      case DeepCameraDisposition::ClearTransparent:
      case DeepCameraDisposition::InvalidCameraBlack:
        /* Do not inspect any possibly undefined camera/support fields here. */
        if (!ray.segments.empty())
          return failure(DeepErrorCode::InvalidPayload, "Disposition ray has unexpected supports");
        continue;
      case DeepCameraDisposition::Ray:
        break;
      default:
        return failure(DeepErrorCode::InvalidPayload, "Unknown homogeneous camera disposition");
    }
    if (!ray.coverage_complete || !ray.homogeneous_certified)
      return failure(DeepErrorCode::Incomplete, "Missing certified complete homogeneous coverage");
    if (!ray.affine_camera_depth)
      return failure(DeepErrorCode::InvalidPayload, "M1 non-affine depth: M2 adapter required");
    if (ray.segments.empty() || !std::isfinite(ray.coverage_front) ||
        !std::isfinite(ray.coverage_back) || ray.coverage_front < 0 ||
        ray.coverage_back <= ray.coverage_front || ray.coverage_back >= invalid_depth)
      return failure(DeepErrorCode::InvalidPayload, "Invalid homogeneous coverage range");
    if (ray.segments.size() > std::numeric_limits<size_t>::max() - source_count)
      return failure(DeepErrorCode::InvalidCount, "Homogeneous source count overflow");
    source_count += ray.segments.size();
    segments.clear();
    for (const DeepHomogeneousSegment &segment : ray.segments) {
      if (!segment.source || !std::isfinite(segment.z) || !std::isfinite(segment.z_back) ||
          segment.z < ray.coverage_front || segment.z_back > ray.coverage_back ||
          segment.z_back <= segment.z || !std::isfinite(segment.tau) || segment.tau < 0)
        return failure(DeepErrorCode::InvalidPayload, "Invalid homogeneous source/tau/support");
      for (const double rgb : segment.post_clamp_rgb)
        if (!std::isfinite(rgb))
          return failure(DeepErrorCode::InvalidPayload, "Nonfinite native homogeneous radiance");
      segments.push_back(&segment);
    }
    std::sort(segments.begin(), segments.end(), [](const auto *a, const auto *b) {
      return a->source < b->source;
    });
    for (size_t i = 1; i < segments.size(); i++)
      if (segments[i - 1]->source == segments[i]->source)
        return failure(DeepErrorCode::InvalidReference, "Duplicate homogeneous support identity");
    std::sort(segments.begin(), segments.end(), [](const auto *a, const auto *b) {
      return a->z < b->z;
    });
    double cursor = ray.coverage_front;
    Sum physical_tau;
    for (const auto *segment : segments) {
      if (segment->z != cursor)
        return failure(DeepErrorCode::Incomplete, "Homogeneous coverage gap or overlap");
      cursor = segment->z_back;
      physical_tau.add(segment->tau);
      if (!std::isfinite(physical_tau.get()))
        return failure(DeepErrorCode::InvalidPayload, "Homogeneous physical tau sum overflow");
    }
    if (cursor != ray.coverage_back)
      return failure(DeepErrorCode::Incomplete, "Homogeneous coverage tail is missing");
  }
  weight = weights.get();
  std::sort(rays.begin(), rays.end(), sample_less);
  for (size_t i = 1; i < rays.size(); i++)
    if (rays[i - 1]->sample.sample == rays[i]->sample.sample)
      return failure(DeepErrorCode::DuplicateSample, "Duplicate native homogeneous sample");
  std::sort(
      rays.begin(), rays.end(), [](const auto *a, const auto *b) { return a->ray < b->ray; });
  for (size_t i = 1; i < rays.size(); i++)
    if (rays[i - 1]->ray == rays[i]->ray)
      return failure(DeepErrorCode::InvalidReference, "Duplicate actual homogeneous ray ID");
  return {};
}

double beer_fraction(const double tau, const double u)
{
  if (u == 0 || u == 1)
    return u;
  return tau == 0 ? u : -std::expm1(-tau * u) / -std::expm1(-tau);
}

/* Work in local depth and normalized support coordinates. There is no slope*z+intercept
 * evaluation and no optical floor, endpoint alpha inversion, or opacity-tail shortcut. */
void evaluate(const DeepHomogeneousSegment &segment,
              const double z,
              const double prefix_tau,
              const double *prefix_rgb,
              const double exposure,
              double *value)
{
  const double u = z == segment.z      ? 0 :
                   z == segment.z_back ? 1 :
                                         (z - segment.z) / (segment.z_back - segment.z);
  value[0] = std::exp(-prefix_tau) * std::exp(-segment.tau * u);
  const double f = beer_fraction(segment.tau, u);
  for (int c = 0; c < 3; c++)
    value[c + 1] = (prefix_rgb[c] + segment.post_clamp_rgb[c] * f) * exposure;
}

void curvature_bound(const DeepHomogeneousSegment &segment,
                     const Slab &slab,
                     const double prefix_tau,
                     const double exposure,
                     double *error)
{
  const double length = segment.z_back - segment.z;
  const double u = (slab.a - segment.z) / length;
  /* tau * local_length / full_length is M2*L^2 without squaring a world-space rate.
   * Outward factors protect weak tau; infinite bounds simply keep subdividing. */
  const double q = upper(segment.tau * upper((slab.b - slab.a) / length));
  const double shape = bound_product(bound_product(q, q), 0.125);
  const double exponent = std::nextafter(std::exp(-segment.tau * u),
                                         std::numeric_limits<double>::infinity());
  const double prefix = std::nextafter(std::exp(-prefix_tau),
                                       std::numeric_limits<double>::infinity());
  error[0] = bound_product(bound_product(prefix, exponent), shape);
  for (int c = 0; c < 3; c++) {
    if (segment.tau == 0 || segment.post_clamp_rgb[c] == 0 || exposure == 0) {
      error[c + 1] = 0;
      continue;
    }
    /* tau/(1-exp(-tau)) <= 1+tau for tau<=1. This avoids dividing HDR RGB by
     * subnormal tau, and also handles the smallest positive double denominator. */
    const double ratio = segment.tau <= 1 ?
                             upper(1 + segment.tau) :
                             upper(segment.tau / std::nextafter(-std::expm1(-segment.tau), 0.0));
    const double du = upper((slab.b - slab.a) / length);
    const double rgb_shape = bound_product(bound_product(bound_product(q, du), ratio), 0.125);
    error[c + 1] = bound_product(
        bound_product(bound_product(std::abs(segment.post_clamp_rgb[c]), exposure), exponent),
        rgb_shape);
  }
}

DeepStatus append_chord(CurveVector<Delta> &events,
                        const Slab &slab,
                        const double *front,
                        const double *back,
                        const double normalized_weight,
                        const uint32_t front_flags,
                        const uint32_t back_flags)
{
  Delta start, end;
  start.z = slab.a;
  end.z = slab.b;
  start.flags = front_flags;
  end.flags = back_flags;
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(front[c]) || !std::isfinite(back[c]))
      return failure(DeepErrorCode::InvalidPayload, "Homogeneous prefix arithmetic overflow");
    const double slope = (back[c] - front[c]) / (slab.b - slab.a);
    const double weighted = slope * normalized_weight;
    if (!std::isfinite(slope) || !std::isfinite(weighted))
      return failure(DeepErrorCode::InvalidPayload, "DepthNotRepresentable: homogeneous slope");
    start.slope[c] = weighted;
    end.slope[c] = -weighted;
    start.active[c] = weighted != 0;
    end.active[c] = -start.active[c];
    /* This is an arithmetic endpoint correction to the same continuous chord, applied
     * BEFORE either side of the knot. It is not radiance, visibility, or calibration. */
    end.correction[c] = (back[c] - std::fma(slope, slab.b - slab.a, front[c])) * normalized_weight;
    if (!std::isfinite(end.correction[c]))
      return failure(DeepErrorCode::InvalidPayload, "Homogeneous endpoint arithmetic overflow");
  }
  events.push_back(start);
  events.push_back(end);
  return {};
}

DeepStatus build_events(const DeepHomogeneousPixelInput &input,
                        const DeepHomogeneousCurvePolicy &policy,
                        const CurveVector<const DeepHomogeneousRay *> &rays,
                        DeepHomogeneousCurveStorage &result,
                        CurveVector<const DeepHomogeneousSegment *> &segments,
                        CurveVector<Slab> &pending,
                        CurveVector<Delta> &events,
                        double *terminal,
                        double &terminal_before_transmission)
{
  Delta origin;
  origin.flags = DEEP_KNOT_PROTECTED_CUT;
  events.push_back(origin); /* Complete all-clear pixels still publish owned constant knots. */
  Sum terminal_sum[4], invalid_sum;
  for (const auto *ray : rays) {
    const double normalized_weight = ray->population_weight / result.weight;
    result.identities.push_back({ray->ray, ray->sample, ray->population_weight});
    if (ray->sample.disposition == DeepCameraDisposition::ClearTransparent) {
      terminal_sum[0].add(ray->population_weight);
      continue;
    }
    if (ray->sample.disposition == DeepCameraDisposition::InvalidCameraBlack) {
      Delta black;
      black.z = invalid_depth;
      black.flags = DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_WORLD_TERMINAL |
                    DEEP_KNOT_INVALID_CAMERA_BLACK;
      black.jump[0] = -normalized_weight;
      events.push_back(black);
      invalid_sum.add(ray->population_weight);
      continue;
    }
    segments.clear();
    for (const auto &segment : ray->segments)
      segments.push_back(&segment);
    std::sort(segments.begin(), segments.end(), [](const auto *a, const auto *b) {
      return a->z < b->z;
    });
    Sum tau_sum, rgb_sum[3], rgb_absolute[3];
    double ray_error[4] = {};
    for (const auto *segment : segments) {
      if (policy.float_depth_grid && (double(float(segment->z)) != segment->z ||
                                      double(float(segment->z_back)) != segment->z_back))
        return failure(DeepErrorCode::InvalidPayload,
                       "DepthNotRepresentable: native support is not exact FLOAT");
      result.sources.push_back({ray->ray, *segment});
      const double prefix_rgb[3] = {rgb_sum[0].get(), rgb_sum[1].get(), rgb_sum[2].get()};
      pending.clear();
      pending.push_back({segment->z, segment->z_back});
      while (!pending.empty()) {
        const Slab slab = pending.back();
        pending.pop_back();
        double error[4];
        curvature_bound(*segment, slab, tau_sum.get(), input.exposure, error);
        bool accepted = true;
        for (int c = 0; c < 4; c++)
          accepted &= error[c] <= 0.5 * (c == 0 ? policy.transmission_error : policy.rgb_error);
        if (!accepted) {
          const double candidate = slab.a + (slab.b - slab.a) * 0.5;
          const double midpoint = policy.float_depth_grid ? double(float(candidate)) : candidate;
          if (midpoint <= slab.a || midpoint >= slab.b)
            return failure(DeepErrorCode::InvalidPayload,
                           "DepthNotRepresentable: homogeneous curvature bound");
          pending.push_back({midpoint, slab.b});
          pending.push_back({slab.a, midpoint});
          continue;
        }
        double front[4], back[4];
        evaluate(*segment, slab.a, tau_sum.get(), prefix_rgb, input.exposure, front);
        evaluate(*segment, slab.b, tau_sum.get(), prefix_rgb, input.exposure, back);
        const uint32_t boundary = DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_INTERVAL_BOUNDARY;
        if (const auto status = append_chord(events,
                                             slab,
                                             front,
                                             back,
                                             normalized_weight,
                                             slab.a == segment->z ? boundary : 0,
                                             slab.b == segment->z_back ? boundary : 0);
            !status)
          return status;
        for (int c = 0; c < 4; c++)
          ray_error[c] = std::max(ray_error[c], error[c]);
      }
      tau_sum.add(segment->tau);
      if (!std::isfinite(tau_sum.get()))
        return failure(DeepErrorCode::InvalidPayload, "Homogeneous physical tau sum overflow");
      for (int c = 0; c < 3; c++) {
        rgb_sum[c].add(segment->post_clamp_rgb[c]);
        rgb_absolute[c].add(std::abs(segment->post_clamp_rgb[c]) * input.exposure);
        if (!std::isfinite(rgb_sum[c].get()) || !std::isfinite(rgb_absolute[c].get()))
          return failure(DeepErrorCode::InvalidPayload, "Homogeneous native RGB sum overflow");
      }
    }
    terminal_sum[0].add(std::exp(-tau_sum.get()) * ray->population_weight);
    for (int c = 0; c < 3; c++)
      terminal_sum[c + 1].add(rgb_sum[c].get() * input.exposure * normalized_weight);
    /* Conservative FP64 evaluation/normalization allowance, separate from the analytical
     * interpolation bound. Unrepresentable HDR/cancellation fails instead of using relative
     * tolerance. The sweep below additionally accounts for its own operations. */
    for (int c = 0; c < 4; c++) {
      const double magnitude = c == 0 ? 1 : bound_add(1, rgb_absolute[c - 1].get());
      const double nu = bound_product(64 * round_unit, double(segments.size()) + 1);
      const double gamma = nu < 1 ? upper(nu / (1 - nu)) : std::numeric_limits<double>::infinity();
      const double arithmetic = bound_product(gamma, magnitude);
      result.error[c] = bound_add(
          result.error[c], bound_product(bound_add(ray_error[c], arithmetic), normalized_weight));
    }
  }
  for (int c = 0; c < 4; c++) {
    terminal[c] = c == 0 ? terminal_sum[c].get() / result.weight : terminal_sum[c].get();
    if (!std::isfinite(terminal[c]))
      return failure(DeepErrorCode::InvalidPayload, "Homogeneous terminal prefix overflow");
  }
  Sum terminal_before;
  terminal_before.add(terminal_sum[0].get());
  terminal_before.add(invalid_sum.get());
  terminal_before_transmission = terminal_before.get() / result.weight;
  return {};
}

/* No allocation occurs between reserve and commit. Full pages are sealed/retired before
 * allocating the next one; leases keep their bytes charged. Any later failure publishes
 * no result. A hard PageStore error remains sticky for the frame owner's cancellation. */
class PageWriter {
 public:
  PageWriter(DeepPageStore &store, DeepHomogeneousCurveStorage &result)
      : store_(store), result_(result)
  {
  }
  DeepStatus append(const DeepDepthKnot &knot)
  {
    if (result_.knot_count > std::numeric_limits<uint32_t>::max())
      return failure(DeepErrorCode::InvalidCount, "Homogeneous knot ordinal overflow");
    if (!page_)
      if (const auto status = store_.allocate(DeepRecordKind::DepthKnot, page_); !status)
        return status;
    uint64_t handle;
    auto status = store_.reserve(page_, handle);
    if (status.code == DeepErrorCode::NeedPage) {
      if (status = finish(); !status)
        return status;
      if (status = store_.allocate(DeepRecordKind::DepthKnot, page_); !status)
        return status;
      status = store_.reserve(page_, handle);
    }
    if (!status)
      return status;
    const DeepRecordTag tag = {0,
                               knot.source_group,
                               0,
                               uint32_t(result_.knot_count),
                               uint32_t(DeepRecordKind::DepthKnot),
                               knot.flags};
    status = store_.write(handle, tag, {reinterpret_cast<const uint8_t *>(&knot), sizeof(knot)});
    if (!status)
      return status;
    if (status = store_.commit(handle); !status)
      return status;
    result_.knot_count++;
    return {};
  }
  DeepStatus finish()
  {
    if (!page_)
      return {};
    DeepPageLease lease;
    if (const auto status = store_.seal(page_, lease); !status)
      return status;
    if (const auto status = store_.retire(page_); !status)
      return status;
    page_ = 0;
    result_.pages.push_back(lease);
    return {};
  }
  ~PageWriter()
  {
    /* Every successful write is committed, so a failure during the sweep can release
     * its open page without aborting any valid slot or publishing the partial curve. */
    if (page_) {
      DeepPageLease lease;
      if (store_.seal(page_, lease))
        store_.retire(page_);
    }
  }

 private:
  DeepPageStore &store_;
  DeepHomogeneousCurveStorage &result_;
  uint32_t page_ = 0;
};

/* Exceptional arithmetic repair, not a sampling/fitting test: if the delta sweep's
 * rounding residue escapes [0,1], evaluate the original positive per-ray optical sum.
 * Only an actual out-of-range residue takes this O(actual sources) path. Its entire
 * correction is charged to the absolute ledger; there is no clipping or tail floor. */
double optical_reference(const DeepHomogeneousCurveStorage &result,
                         const double z,
                         const bool before)
{
  Sum weighted;
  size_t source = 0;
  for (const DeepHomogeneousIdentity &identity : result.identities) {
    double transmission = 1;
    if (identity.sample.disposition == DeepCameraDisposition::InvalidCameraBlack)
      transmission = z < invalid_depth || (before && z == invalid_depth) ? 1 : 0;
    else if (identity.sample.disposition == DeepCameraDisposition::Ray) {
      Sum tau;
      while (source < result.sources.size() && result.sources[source].ray == identity.ray) {
        const auto &support = result.sources[source++].support;
        const double fraction = z <= support.z ? 0 :
                                z >= support.z_back ?
                                                 1 :
                                                 (z - support.z) / (support.z_back - support.z);
        tau.add(support.tau * fraction);
      }
      transmission = std::exp(-tau.get());
    }
    weighted.add(identity.population_weight * transmission);
  }
  return weighted.get() / result.weight;
}

DeepStatus sweep(DeepPageStore &store,
                 CurveVector<Delta> &events,
                 DeepHomogeneousCurveStorage &result,
                 const DeepHomogeneousCurvePolicy &policy,
                 const double *terminal,
                 const double terminal_before_transmission)
{
  std::sort(
      events.begin(), events.end(), [](const Delta &a, const Delta &b) { return a.z < b.z; });
  Sum value[4], slope[4];
  value[0].reset(1); /* The complete admitted population, including clear/invalid samples. */
  uint64_t active[4] = {};
  double slope_error[4] = {}, arithmetic[4] = {};
  double last_z = 0;
  PageWriter writer(store, result);
  for (size_t first = 0; first < events.size();) {
    size_t end = first + 1;
    while (end < events.size() && events[end].z == events[first].z)
      end++;
    const double z = events[first].z, distance = z - last_z;
    DeepDepthKnot knot = {};
    knot.z = z;
    knot.source_group = result.knot_count + 1;
    Sum corrections[4], jumps[4], changes[4];
    int64_t active_delta[4] = {};
    for (size_t i = first; i < end; i++) {
      knot.flags |= events[i].flags;
      for (int c = 0; c < 4; c++) {
        corrections[c].add(events[i].correction[c]);
        jumps[c].add(events[i].jump[c]);
        changes[c].add(events[i].slope[c]);
        if ((events[i].active[c] > 0 && active_delta[c] == INT64_MAX) ||
            (events[i].active[c] < 0 && active_delta[c] == INT64_MIN))
          return failure(DeepErrorCode::InvalidCount, "Homogeneous active chord count overflow");
        active_delta[c] += events[i].active[c];
        slope_error[c] = bound_add(slope_error[c],
                                   bound_product(8 * round_unit, std::abs(events[i].slope[c])));
      }
    }
    for (int c = 0; c < 4; c++) {
      if (active[c] && distance != 0) {
        const double advance = slope[c].get() * distance;
        arithmetic[c] = bound_add(arithmetic[c], bound_product(slope_error[c], distance));
        arithmetic[c] = bound_add(
            arithmetic[c],
            bound_product(8 * round_unit, std::abs(value[c].get()) + std::abs(advance)));
        value[c].reset(std::fma(slope[c].get(), distance, value[c].get()));
      }
      value[c].add(corrections[c].get());
      knot.before[c] = value[c].get();
      value[c].add(jumps[c].get());
      knot.after[c] = value[c].get();
      arithmetic[c] = bound_add(arithmetic[c],
                                bound_product(8 * round_unit,
                                              std::abs(knot.before[c]) + std::abs(jumps[c].get()) +
                                                  std::abs(corrections[c].get())));
      if (active_delta[c] < 0) {
        const uint64_t removed = uint64_t(-(active_delta[c] + 1)) + 1;
        if (removed > active[c])
          return failure(DeepErrorCode::InvalidCount, "Homogeneous active chord underflow");
        active[c] -= removed;
      }
      else {
        if (uint64_t(active_delta[c]) > UINT64_MAX - active[c])
          return failure(DeepErrorCode::InvalidCount, "Homogeneous active chord overflow");
        active[c] += uint64_t(active_delta[c]);
      }
      slope[c].add(changes[c].get());
      if (active[c] == 0) {
        slope[c].reset();
        slope_error[c] = 0;
      }
      knot.slope_right[c] = slope[c].get();
      if (end == events.size()) {
        /* Preserve the exact analytic/source endpoint rather than a slope accumulation
         * residue. The independent native support totals define this anchor; there is no
         * Beauty target and no alteration of any finite physical tau. */
        const double before = c == 0 ? terminal_before_transmission : terminal[c];
        arithmetic[c] = bound_add(arithmetic[c], std::abs(knot.before[c] - before));
        arithmetic[c] = bound_add(arithmetic[c], std::abs(knot.after[c] - terminal[c]));
        knot.before[c] = before;
        knot.after[c] = terminal[c];
        value[c].reset(terminal[c]);
      }
      if (!std::isfinite(knot.before[c]) || !std::isfinite(knot.after[c]) ||
          !std::isfinite(knot.slope_right[c]))
        return failure(DeepErrorCode::InvalidPayload, "Homogeneous AA arithmetic overflow");
      if (c == 0 && (knot.before[0] < 0 || knot.before[0] > 1 || knot.after[0] < 0 ||
                     knot.after[0] > knot.before[0]))
      {
        const double exact_before = optical_reference(result, z, true);
        const double exact_after = optical_reference(result, z, false);
        arithmetic[0] = bound_add(arithmetic[0], std::abs(knot.before[0] - exact_before));
        arithmetic[0] = bound_add(arithmetic[0], std::abs(knot.after[0] - exact_after));
        knot.before[0] = exact_before;
        knot.after[0] = exact_after;
        value[0].reset(exact_after);
      }
      const double budget = c == 0 ? policy.transmission_error : policy.rgb_error;
      if (bound_add(result.error[c], arithmetic[c]) > budget)
        return failure(DeepErrorCode::InvalidPayload,
                       "Homogeneous absolute representation arithmetic budget exceeded");
    }
    if (knot.before[0] < 0 || knot.before[0] > 1 || knot.after[0] < 0 ||
        knot.after[0] > knot.before[0] || knot.slope_right[0] > 0)
      return failure(DeepErrorCode::InvalidPayload,
                     "Homogeneous transmission arithmetic outside range");
    if (const auto status = writer.append(knot); !status)
      return status;
    first = end;
    last_z = z;
  }
  for (int c = 0; c < 4; c++)
    result.error[c] = bound_add(result.error[c], arithmetic[c]);
  return writer.finish();
}
}  // namespace

DeepHomogeneousCurve::operator bool() const
{
  return bool(storage_);
}
const DeepFrameKey *DeepHomogeneousCurve::frame_key() const
{
  return storage_ ? &storage_->frame : nullptr;
}
int32_t DeepHomogeneousCurve::x() const
{
  return storage_ ? storage_->x : 0;
}
int32_t DeepHomogeneousCurve::y() const
{
  return storage_ ? storage_->y : 0;
}
uint64_t DeepHomogeneousCurve::native_population() const
{
  return storage_ ? storage_->population : 0;
}
double DeepHomogeneousCurve::population_weight() const
{
  return storage_ ? storage_->weight : 0;
}
uint64_t DeepHomogeneousCurve::knot_count() const
{
  return storage_ ? storage_->knot_count : 0;
}
std::span<const DeepPageLease> DeepHomogeneousCurve::pages() const
{
  return storage_ ? std::span<const DeepPageLease>(storage_->pages) :
                    std::span<const DeepPageLease>();
}
std::span<const DeepHomogeneousSource> DeepHomogeneousCurve::sources() const
{
  return storage_ ? std::span<const DeepHomogeneousSource>(storage_->sources) :
                    std::span<const DeepHomogeneousSource>();
}
std::span<const DeepHomogeneousIdentity> DeepHomogeneousCurve::identities() const
{
  return storage_ ? std::span<const DeepHomogeneousIdentity>(storage_->identities) :
                    std::span<const DeepHomogeneousIdentity>();
}
std::span<const double, 4> DeepHomogeneousCurve::representation_error() const
{
  static const double empty[4] = {};
  return storage_ ? std::span<const double, 4>(storage_->error) :
                    std::span<const double, 4>(empty);
}
DeepStatus DeepHomogeneousCurve::read_knot(uint64_t index, DeepDepthKnot &knot) const
{
  if (!storage_ || index >= storage_->knot_count)
    return failure(DeepErrorCode::InvalidReference, "Homogeneous knot index outside owned curve");
  for (const DeepPageLease &page : storage_->pages) {
    const DeepPageDescriptor &descriptor = *page.descriptor();
    if (index >= descriptor.valid) {
      index -= descriptor.valid;
      continue;
    }
    for (uint32_t slot = 0; slot < descriptor.reserved; slot++) {
      const uint64_t handle = page.valid_handle_at(slot);
      if (!handle)
        continue;
      if (index != 0) {
        index--;
        continue;
      }
      DeepRecordTag tag;
      std::span<const uint8_t> bytes;
      if (const auto status = page.read(handle, tag, bytes); !status)
        return status;
      if (bytes.size() != sizeof(knot))
        return failure(DeepErrorCode::InvalidLayout, "Homogeneous knot payload stride mismatch");
      std::memcpy(&knot, bytes.data(), sizeof(knot));
      return {};
    }
  }
  return failure(DeepErrorCode::InvalidReference, "Homogeneous knot count/page mismatch");
}

DeepStatus deep_build_homogeneous_curve(DeepPageStore &store,
                                        const DeepHomogeneousPixelInput &input,
                                        DeepHomogeneousCurve &output,
                                        const DeepHomogeneousCurvePolicy &policy)
{
  output = {};
  try {
    const DeepMemoryBudget &budget = store.memory_budget();
    CurveVector<const DeepHomogeneousRay *> rays{
        DeepBudgetAllocator<const DeepHomogeneousRay *>(budget)};
    CurveVector<const DeepHomogeneousSegment *> segments{
        DeepBudgetAllocator<const DeepHomogeneousSegment *>(budget)};
    CurveVector<Slab> pending{DeepBudgetAllocator<Slab>(budget)};
    CurveVector<Delta> events{DeepBudgetAllocator<Delta>(budget)};
    double weight = 0;
    size_t source_count = 0;
    if (const auto status = validate(store, input, policy, rays, segments, weight, source_count);
        !status)
      return status;
    auto result = std::allocate_shared<DeepHomogeneousCurveStorage>(
        DeepBudgetAllocator<DeepHomogeneousCurveStorage>(budget), budget);
    result->frame = input.frame;
    result->x = input.x;
    result->y = input.y;
    result->population = input.native_population;
    result->weight = weight;
    result->sources.reserve(source_count);
    result->identities.reserve(rays.size());
    double terminal[4] = {}, terminal_before_transmission = 0;
    if (const auto status = build_events(input,
                                         policy,
                                         rays,
                                         *result,
                                         segments,
                                         pending,
                                         events,
                                         terminal,
                                         terminal_before_transmission);
        !status)
      return status;
    if (const auto status = sweep(
            store, events, *result, policy, terminal, terminal_before_transmission);
        !status)
      return status;
    if (!result->knot_count || result->pages.empty())
      return failure(DeepErrorCode::Incomplete, "Homogeneous builder published no owned knots");
    output.storage_ = std::move(result);
    return {};
  }
  catch (const std::bad_alloc &) {
    return failure(DeepErrorCode::BudgetExceeded, "Homogeneous shared memory quota exceeded");
  }
  catch (const std::length_error &) {
    return failure(DeepErrorCode::InvalidCount, "Homogeneous container count overflow");
  }
}

CCL_NAMESPACE_END
