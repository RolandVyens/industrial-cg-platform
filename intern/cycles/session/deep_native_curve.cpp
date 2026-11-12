/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_native_curve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

#ifdef _MSC_VER
#  pragma float_control(precise, on, push)
#endif

CCL_NAMESPACE_BEGIN
template<class T> using NativeVector = std::vector<T, DeepBudgetAllocator<T>>;
namespace {
constexpr double infinity_depth = double(std::numeric_limits<float>::max());
constexpr size_t absent = std::numeric_limits<size_t>::max();
using Value = std::array<double, 4>;
struct Sum {
  double s = 0, c = 0;
  void add(double x)
  {
    double next = s + x;
    c += std::abs(s) >= std::abs(x) ? (s - next) + x : (x - next) + s;
    s = next;
  }
  double value() const
  {
    return s + c;
  }
  void reset(double x = 0)
  {
    s = x;
    c = 0;
  }
};
double upward(double x)
{
  return x == 0 ? 0 : std::nextafter(x, std::numeric_limits<double>::infinity());
}
double add_bound(double a, double b)
{
  return upward(a + b);
}
double product_bound(double a, double b)
{
  return a == 0 || b == 0 ? 0 : std::nextafter(a * b, std::numeric_limits<double>::infinity());
}
DeepStatus fail(DeepErrorCode code, const char *message)
{
  return DeepStatus::fail(code, message);
}
struct RayRange {
  uint64_t id;
  DeepSampleKey sample;
  double weight;
  size_t optical, optical_count, visibility, visibility_count, radiance, radiance_count;
  DeepDepthSupportView depth_support;
  DeepCameraDepthMap depth_map;
  bool radial = false;
};
struct DepthEvaluationError {
  DeepStatus status;
};
void require_depth(const DeepStatus &status)
{
  if (!status)
    throw DepthEvaluationError{status};
}
struct Delta {
  double z = 0;
  Value slope = {}, correction = {}, jump = {};
  int64_t active[4] = {};
  size_t ray = absent, chord = absent;
  int edge = 0;
  uint32_t flags = 0;
};
/* Each ray owns at most one elementary chord. Replacing its leaf removes the
 * retired chord's uncertainty without losing bounds for any live chord. */
class ActiveSlopes {
 public:
  explicit ActiveSlopes(const DeepMemoryBudget &budget)
      : nodes_(DeepBudgetAllocator<Node>(budget)), owners_(DeepBudgetAllocator<size_t>(budget))
  {
  }
  DeepStatus initialize(size_t rays)
  {
    while (leaves_ < rays) {
      if (leaves_ > std::numeric_limits<size_t>::max() / 2)
        return fail(DeepErrorCode::InvalidCount, "NativeCurve slope tree size overflow");
      leaves_ *= 2;
    }
    if (leaves_ > nodes_.max_size() / 2 || rays > owners_.max_size())
      return fail(DeepErrorCode::InvalidCount, "NativeCurve slope tree allocation overflow");
    nodes_.resize(2 * leaves_);
    owners_.assign(rays, absent);
    return {};
  }
  DeepStatus update(const Delta &event)
  {
    if (!event.edge)
      return {};
    if (event.ray >= owners_.size() || event.chord == absent)
      return fail(DeepErrorCode::InvalidReference, "NativeCurve chord leaf outside population");
    auto &owner = owners_[event.ray];
    if ((event.edge < 0 && owner != event.chord) || (event.edge > 0 && owner != absent))
      return fail(DeepErrorCode::InvalidReference, "NativeCurve active chord ownership mismatch");
    owner = event.edge > 0 ? event.chord : absent;
    size_t index = leaves_ + event.ray;
    nodes_[index] = {};
    if (event.edge > 0)
      for (int c = 0; c < 4; c++) {
        nodes_[index].slope[c] = event.slope[c];
        nodes_[index].error[c] = product_bound(8 * std::numeric_limits<double>::epsilon(),
                                               std::abs(event.slope[c]));
      }
    while (index > 1) {
      index /= 2;
      auto &node = nodes_[index];
      const auto &left = nodes_[2 * index], &right = nodes_[2 * index + 1];
      for (int c = 0; c < 4; c++) {
        Sum sum;
        sum.add(left.slope[c]);
        sum.add(right.slope[c]);
        node.slope[c] = sum.value();
        node.error[c] = add_bound(
            add_bound(left.error[c], right.error[c]),
            product_bound(8 * std::numeric_limits<double>::epsilon(),
                          add_bound(std::abs(left.slope[c]), std::abs(right.slope[c]))));
      }
    }
    return {};
  }
  double slope(int channel) const
  {
    return nodes_[1].slope[channel];
  }
  double error(int channel) const
  {
    return nodes_[1].error[channel];
  }
  bool empty() const
  {
    return std::all_of(
        owners_.begin(), owners_.end(), [](size_t owner) { return owner == absent; });
  }

 private:
  struct Node {
    Value slope = {}, error = {};
  };
  size_t leaves_ = 1;
  NativeVector<Node> nodes_;
  NativeVector<size_t> owners_;
};
enum BoundaryKind {
  OpticalEnd,
  RadianceEnd,
  OpticalBegin,
  RadianceBegin,
  Visibility,
  RadiancePoint
};
struct Boundary {
  double z;
  BoundaryKind kind;
  size_t index;
  uint32_t ordinal = 0;
};
struct Slab {
  double a, b;
};
}  // namespace

struct DeepNativeCurveStorage {
  explicit DeepNativeCurveStorage(const DeepMemoryBudget &budget)
      : rays(DeepBudgetAllocator<RayRange>(budget)),
        optical(DeepBudgetAllocator<DeepOwnedOpticalSource>(budget)),
        visibility(DeepBudgetAllocator<DeepOwnedVisibilitySource>(budget)),
        radiance(DeepBudgetAllocator<DeepOwnedRadianceSource>(budget)),
        pages(DeepBudgetAllocator<DeepPageLease>(budget))
  {
  }
  DeepFrameKey frame;
  uint64_t population = 0, knots = 0;
  double weight = 0, exposure = 1, error[4] = {};
  NativeVector<RayRange> rays;
  NativeVector<DeepOwnedOpticalSource> optical;
  NativeVector<DeepOwnedVisibilitySource> visibility;
  NativeVector<DeepOwnedRadianceSource> radiance;
  NativeVector<DeepPageLease> pages;
};

namespace {
bool native_depth(double z, const DeepNativeCurvePolicy &policy)
{
  return std::isfinite(z) && z >= 0 && z <= infinity_depth &&
         (!policy.float_depth_grid || double(float(z)) == z);
}

DeepStatus paired_support(const DeepDepthSupportView &view,
                          DeepPhysicalSourceKind kind,
                          uint64_t source,
                          uint32_t ordinal,
                          double z,
                          double z_back,
                          uint32_t flags,
                          DeepPhysicalSourceSupport &physical)
{
  if (auto status = view.read_support(kind, source, physical); !status)
    return status;
  if (physical.ordinal != ordinal || physical.z != z || physical.z_back != z_back ||
      physical.flags != flags)
    return fail(DeepErrorCode::InvalidReference, "NativeCurve physical source pairing mismatch");
  return {};
}

DeepStatus retain_input(DeepPageStore &store,
                        const DeepNativeCurvePixel &input,
                        const DeepNativeCurvePolicy &policy,
                        DeepNativeCurveStorage &out)
{
  if (!(store.frame_key() == input.frame) || !input.frame.invocation)
    return fail(DeepErrorCode::InvalidFrame, "NativeCurve frame mismatch");
  if (!input.population_complete)
    return fail(DeepErrorCode::Incomplete, "NativeCurve population is open");
  if (!input.native_population || input.native_population != input.rays.size())
    return fail(DeepErrorCode::PopulationMismatch, "NativeCurve native population mismatch");
  if (!std::isfinite(input.exposure) || input.exposure < 0 ||
      !std::isfinite(policy.transmission_error) || policy.transmission_error <= 0 ||
      policy.transmission_error > 1 || !std::isfinite(policy.rgb_error) || policy.rgb_error <= 0)
    return fail(DeepErrorCode::InvalidPayload, "Invalid NativeCurve exposure/error policy");
  const auto budget = store.memory_budget();
  NativeVector<uint64_t> identities{DeepBudgetAllocator<uint64_t>(budget)};
  NativeVector<uint32_t> samples{DeepBudgetAllocator<uint32_t>(budget)};
  using SourceKey = std::tuple<int, uint64_t, uint64_t>;
  NativeVector<SourceKey> sources{DeepBudgetAllocator<SourceKey>(budget)};
  Sum weight;
  for (const auto &ray : input.rays) {
    if (!ray.ray || ray.sample.x != input.x || ray.sample.y != input.y)
      return fail(DeepErrorCode::InvalidReference, "NativeCurve ray/pixel mismatch");
    if (!ray.native_complete)
      return fail(DeepErrorCode::Incomplete, "NativeCurve native source accounting is incomplete");
    if (!std::isfinite(ray.population_weight) || ray.population_weight <= 0)
      return fail(DeepErrorCode::InvalidPayload, "Invalid NativeCurve population weight");
    identities.push_back(ray.ray);
    samples.push_back(ray.sample.sample);
    weight.add(ray.population_weight);
    RayRange range{ray.ray,
                   ray.sample,
                   ray.population_weight,
                   out.optical.size(),
                   0,
                   out.visibility.size(),
                   0,
                   out.radiance.size(),
                   0};
    if (ray.sample.disposition == DeepCameraDisposition::ClearTransparent ||
        ray.sample.disposition == DeepCameraDisposition::InvalidCameraBlack)
    {
      if (!ray.optical.empty() || !ray.visibility.empty() || !ray.radiance.empty())
        return fail(DeepErrorCode::InvalidPayload,
                    "Disposition sample owns geometric/radiance sources");
      out.rays.push_back(range);
      continue; /* Geometric fields need not have been initialized by the camera. */
    }
    if (ray.sample.disposition != DeepCameraDisposition::Ray ||
        (!ray.affine_camera_depth && !ray.depth_support))
      return fail(DeepErrorCode::InvalidPayload,
                  "NativeCurve requires a complete camera depth adapter");
    if (ray.depth_support) {
      if (!ray.depth_support.frame_key() || !(*ray.depth_support.frame_key() == input.frame) ||
          ray.depth_support.ray() != ray.ray || !ray.depth_support.is_charged_to(budget))
        return fail(DeepErrorCode::InvalidReference, "NativeCurve depth map ownership mismatch");
      DeepFrozenDepthMap map;
      if (auto status = ray.depth_support.read_map(map); !status)
        return status;
      range.radial = map.kind == DeepFrozenDepthKind::Radial;
      if (range.radial == ray.affine_camera_depth)
        return fail(DeepErrorCode::InvalidReference, "NativeCurve camera map kind mismatch");
      range.depth_support = ray.depth_support;
      if (auto status = ray.depth_support.make_model(range.depth_map); !status)
        return status;
    }
    if (!ray.optical_complete)
      return fail(DeepErrorCode::Incomplete, "NativeCurve optical coverage is incomplete");
    if (!native_depth(ray.coverage_front, policy) || !native_depth(ray.coverage_back, policy) ||
        ray.coverage_back < ray.coverage_front || ray.coverage_back >= infinity_depth ||
        (ray.coverage_back != ray.coverage_front && ray.optical.empty()))
      return fail(DeepErrorCode::InvalidPayload, "NativeCurve finite coverage range is invalid");
    sources.clear();
    for (const auto &piece : ray.optical) {
      if (!piece.source || !native_depth(piece.z, policy) || !native_depth(piece.z_back, policy) ||
          piece.z_back <= piece.z || piece.z < ray.coverage_front ||
          piece.z_back > ray.coverage_back)
        return fail(DeepErrorCode::InvalidPayload, "Invalid NativeCurve optical support");
      for (double tau : piece.tau)
        if (!std::isfinite(tau) || tau < 0)
          return fail(DeepErrorCode::InvalidPayload, "Invalid NativeCurve finite physical tau");
      if (!std::isfinite(piece.projection_error) || piece.projection_error < 0)
        return fail(DeepErrorCode::InvalidPayload, "Invalid Optical projection certificate");
      out.optical.push_back({ray.ray, piece});
      if (ray.depth_support) {
        DeepPhysicalSourceSupport physical;
        if (auto status = paired_support(ray.depth_support,
                                         DeepPhysicalSourceKind::Optical,
                                         piece.source,
                                         piece.ordinal,
                                         piece.z,
                                         piece.z_back,
                                         0,
                                         physical);
            !status)
          return status;
        if (range.radial)
          if (auto status = out.optical.back().physical_span.initialize(
                  range.depth_map, physical.front, physical.back, physical.z, physical.z_back);
              !status)
            return status;
      }
      sources.emplace_back(0, piece.source, 0);
    }
    range.optical_count = out.optical.size() - range.optical;
    auto optical_begin = out.optical.begin() + range.optical;
    std::sort(optical_begin, out.optical.end(), [](auto &a, auto &b) {
      return a.support.z < b.support.z;
    });
    double cursor = ray.coverage_front;
    Sum physical_tau[3];
    for (auto it = optical_begin; it != out.optical.end(); ++it) {
      if (it->support.z != cursor)
        return fail(DeepErrorCode::Incomplete,
                    "NativeCurve optical partition has a gap or overlap");
      cursor = it->support.z_back;
      for (int c = 0; c < 3; c++) {
        physical_tau[c].add(it->support.tau[c]);
        if (!std::isfinite(physical_tau[c].value()))
          return fail(DeepErrorCode::InvalidPayload, "NativeCurve physical tau sum overflow");
      }
    }
    if (cursor != ray.coverage_back)
      return fail(DeepErrorCode::Incomplete, "NativeCurve optical coverage tail missing");
    for (const auto &point : ray.visibility) {
      const bool world = point.z == infinity_depth && (point.flags & DEEP_KNOT_WORLD_TERMINAL);
      if (!point.source || !native_depth(point.z, policy) ||
          (!world && (point.z < ray.coverage_front || point.z > ray.coverage_back)))
        return fail(DeepErrorCode::InvalidPayload, "Invalid NativeCurve visibility support");
      for (int c = 0; c < 3; c++)
        if (!std::isfinite(point.transparency[c]) || !std::isfinite(point.holdout[c]) ||
            point.transparency[c] < 0 || point.transparency[c] > 1 || point.holdout[c] < 0 ||
            point.holdout[c] > 1 - point.transparency[c])
          return fail(DeepErrorCode::InvalidPayload,
                      "Visibility must contain already bounded transparency/holdout weights");
      if (world) {
        for (int c = 0; c < 3; c++)
          if (point.transparency[c] != 0 || point.holdout[c] != 0)
            return fail(
                DeepErrorCode::InvalidPayload,
                "World terminal must end live transmission without creating a holdout hole");
      }
      out.visibility.push_back({ray.ray, point});
      if (ray.depth_support) {
        DeepPhysicalSourceSupport physical;
        if (auto status = paired_support(ray.depth_support,
                                         DeepPhysicalSourceKind::Visibility,
                                         point.source,
                                         point.ordinal,
                                         point.z,
                                         point.z,
                                         DEEP_PHYSICAL_POINT |
                                             (world ? DEEP_PHYSICAL_INFINITY : 0),
                                         physical);
            !status)
          return status;
      }
      sources.emplace_back(1, point.source, 0);
    }
    range.visibility_count = out.visibility.size() - range.visibility;
    std::sort(
        out.visibility.begin() + range.visibility, out.visibility.end(), [](auto &a, auto &b) {
          return std::tie(a.support.z, a.support.ordinal) <
                 std::tie(b.support.z, b.support.ordinal);
        });
    for (size_t i = range.visibility + 1; i < out.visibility.size(); i++)
      if (out.visibility[i - 1].support.z == out.visibility[i].support.z &&
          out.visibility[i - 1].support.ordinal == out.visibility[i].support.ordinal)
        return fail(DeepErrorCode::InvalidReference,
                    "Ambiguous native visibility order at shared depth");
    for (const auto &term : ray.radiance) {
      const bool point = term.basis == DeepCurveRadianceBasis::Point;
      const bool span = term.basis == DeepCurveRadianceBasis::BeerSpan;
      const bool world_support = term.flags & DEEP_KNOT_NATIVE_WORLD_RADIANCE;
      const bool infinity = point && term.z == infinity_depth &&
                            (term.flags &
                             (DEEP_KNOT_WORLD_TERMINAL | DEEP_KNOT_INFINITY_RADIANCE));
      if (!term.source || !term.emission_call || (!point && !span) ||
          !native_depth(term.z, policy) || !native_depth(term.z_back, policy) ||
          (point ? term.z_back != term.z : term.z_back <= term.z) ||
          ((term.flags & DEEP_KNOT_INFINITY_RADIANCE) && !infinity) ||
          (world_support && !ray.native_world_support) ||
          (!infinity && !world_support &&
           (term.z < ray.coverage_front || term.z_back > ray.coverage_back)))
        return fail(DeepErrorCode::InvalidPayload,
                    "MissingRadianceSupport: invalid native source or film-call identity");
      for (int c = 0; c < 3; c++)
        if (!std::isfinite(term.post_clamp_rgb[c]) || !std::isfinite(term.tau[c]) ||
            term.tau[c] < 0 || (point && term.tau[c] != 0))
          return fail(DeepErrorCode::InvalidPayload,
                      "Invalid native post-clamp radiance/tau payload");
      out.radiance.push_back({ray.ray, term});
      if (ray.depth_support) {
        DeepPhysicalSourceSupport physical;
        const uint32_t flags = (point ? DEEP_PHYSICAL_POINT : 0) |
                               (infinity ? DEEP_PHYSICAL_INFINITY : 0) |
                               (world_support ? DEEP_PHYSICAL_WORLD_RADIANCE : 0);
        if (auto status = paired_support(ray.depth_support,
                                         DeepPhysicalSourceKind::Radiance,
                                         term.source,
                                         term.ordinal,
                                         term.z,
                                         term.z_back,
                                         flags,
                                         physical);
            !status)
          return status;
        if (range.radial && span)
          if (auto status = out.radiance.back().physical_span.initialize(
                  range.depth_map, physical.front, physical.back, physical.z, physical.z_back);
              !status)
            return status;
        if (range.radial && span && world_support && term.z_back == infinity_depth) {
          double endpoint;
          if (auto status = range.depth_map.depth(physical.back, endpoint); !status)
            return status;
          if (endpoint > infinity_depth) {
            DeepDepthParameterBounds fraction;
            if (auto status = out.radiance.back().physical_span.fraction(
                    range.depth_map, infinity_depth, fraction);
                !status)
              return status;
            for (int c = 0; c < 3; c++) {
              const double tau = term.tau[c];
              const double missing = tau == 0 ? 1 - fraction.lower :
                                                std::exp(-tau * fraction.lower) *
                                                    (-std::expm1(-tau * (1 - fraction.lower)) /
                                                     -std::expm1(-tau));
              if (product_bound(std::abs(term.post_clamp_rgb[c]),
                                product_bound(input.exposure, upward(missing))) >
                  .25 * policy.rgb_error)
                return fail(
                    DeepErrorCode::InvalidPayload,
                    "DepthNotRepresentable: World endpoint quantization exceeds RGB budget");
            }
          }
        }
      }
      /* Immutable source prefixes can contribute to distinct native film calls.
       * Duplicating the same (source, call) remains a hard error. */
      sources.emplace_back(2, term.source, term.emission_call);
    }
    range.radiance_count = out.radiance.size() - range.radiance;
    std::sort(sources.begin(), sources.end());
    if (std::adjacent_find(sources.begin(), sources.end()) != sources.end())
      return fail(DeepErrorCode::InvalidReference,
                  "Duplicate native source within its record kind");
    if (ray.depth_support) {
      size_t unique = 0;
      for (size_t i = 0; i < sources.size(); i++)
        if (!i || std::get<0>(sources[i]) != std::get<0>(sources[i - 1]) ||
            std::get<1>(sources[i]) != std::get<1>(sources[i - 1]))
          ++unique;
      if (unique != ray.depth_support.support_count())
        return fail(DeepErrorCode::InvalidReference, "NativeCurve has unpaired physical sources");
    }
    out.rays.push_back(range);
  }
  std::sort(identities.begin(), identities.end());
  std::sort(samples.begin(), samples.end());
  if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
    return fail(DeepErrorCode::InvalidReference, "Duplicate actual ray identity");
  if (std::adjacent_find(samples.begin(), samples.end()) != samples.end())
    return fail(DeepErrorCode::DuplicateSample,
                "Duplicate logical sample regardless of retry metadata");
  if (!std::isfinite(weight.value()) || weight.value() <= 0)
    return fail(DeepErrorCode::InvalidCount, "NativeCurve population weight overflow");
  out.frame = input.frame;
  out.population = input.native_population;
  out.weight = weight.value();
  out.exposure = input.exposure;
  return {};
}

double beer_delta(const DeepCurveRadiance &term, int c, double a, double b)
{
  const double width = term.z_back - term.z;
  if (a == term.z && b == term.z_back)
    return term.post_clamp_rgb[c];
  if (term.tau[c] == 0)
    return term.post_clamp_rgb[c] * ((b - a) / width);
  return term.post_clamp_rgb[c] * std::exp(-term.tau[c] * ((a - term.z) / width)) *
         (-std::expm1(-term.tau[c] * ((b - a) / width)) / -std::expm1(-term.tau[c]));
}

struct MappedIntegral {
  double value, lower, upper;
  double error() const
  {
    return upward(std::max(value - lower, upper - value));
  }
};
MappedIntegral mapped_integral(const DeepCameraDepthSpan &span,
                               const DeepCameraDepthMap &map,
                               double tau,
                               double a,
                               double b,
                               bool transmission = false)
{
  DeepDepthParameterBounds delta, start;
  require_depth(span.delta(map, a, b, delta));
  double value, lower, upper;
  if (transmission) {
    value = std::exp(-tau * delta.value);
    lower = std::exp(-tau * delta.upper);
    upper = std::exp(-tau * delta.lower);
  }
  else if (tau == 0) {
    value = delta.value;
    lower = delta.lower;
    upper = delta.upper;
  }
  else {
    require_depth(span.fraction(map, a, start));
    const double denominator = -std::expm1(-tau);
    const auto integral = [&](double front, double length) {
      return std::exp(-tau * front) * (-std::expm1(-tau * length) / denominator);
    };
    value = integral(start.value, delta.value);
    lower = integral(start.upper, delta.lower);
    upper = integral(start.lower, delta.upper);
  }
  const double round = 32 * std::numeric_limits<double>::epsilon();
  lower = std::max(0.,
                   std::nextafter(lower * (1 - round), -std::numeric_limits<double>::infinity()));
  upper = std::min(1., upward(upper * (1 + round)));
  return {value, std::min(value, lower), std::max(value, upper)};
}

double beer_delta(const DeepNativeCurveStorage &data,
                  const RayRange &ray,
                  size_t index,
                  int c,
                  double a,
                  double b)
{
  const auto &term = data.radiance[index];
  if (!ray.radial)
    return beer_delta(term.support, c, a, b);
  return term.support.post_clamp_rgb[c] *
         mapped_integral(term.physical_span, ray.depth_map, term.support.tau[c], a, b).value;
}
double optical_step(const DeepNativeCurveStorage &data,
                    const RayRange &ray,
                    size_t index,
                    int c,
                    double a,
                    double b)
{
  const auto &term = data.optical[index];
  if (!ray.radial)
    return std::exp(-term.support.tau[c] * ((b - a) / (term.support.z_back - term.support.z)));
  return mapped_integral(term.physical_span, ray.depth_map, term.support.tau[c], a, b, true).value;
}

Value state_value(const DeepNativeCurveStorage &data,
                  const RayRange &ray,
                  size_t optical,
                  std::span<const size_t> active,
                  const double *live,
                  const double *holes,
                  const double *rgb,
                  double origin,
                  double depth)
{
  Value result = {};
  Sum t;
  for (int c = 0; c < 3; c++) {
    t.add(live[c] * (optical == absent ? 1 : optical_step(data, ray, optical, c, origin, depth)) +
          holes[c]);
    Sum color;
    color.add(rgb[c]);
    for (size_t index : active)
      color.add(beer_delta(data, ray, index, c, origin, depth));
    result[c + 1] = color.value() * data.exposure;
  }
  result[0] = t.value() / 3;
  return result;
}

Value radial_chord_error(const DeepNativeCurveStorage &data,
                         const RayRange &ray,
                         size_t optical,
                         std::span<const size_t> active,
                         const double *live,
                         double origin,
                         Slab slab)
{
  Value result = {};
  const double width = upward(slab.b - slab.a), square_width = product_bound(width, width);
  for (int c = 0; c < 3; c++) {
    if (optical != absent) {
      const auto &term = data.optical[optical];
      const double tau = term.support.tau[c];
      DeepDepthDerivativeBounds derivative;
      require_depth(term.physical_span.derivatives(ray.depth_map, slab.a, slab.b, derivative));
      const auto left = mapped_integral(
          term.physical_span, ray.depth_map, tau, origin, slab.a, true);
      const auto right = mapped_integral(
          term.physical_span, ray.depth_map, tau, origin, slab.b, true);
      const double first = product_bound(tau, derivative.first_abs);
      const double second = add_bound(product_bound(first, first),
                                      product_bound(tau, derivative.second_abs));
      const double curvature = product_bound(product_bound(left.upper, second), square_width) / 8;
      const double variation = upward(std::max(0., left.upper - right.lower));
      const double endpoint = std::max(left.error(), right.error());
      result[0] = add_bound(
          result[0],
          product_bound(live[c], add_bound(std::min(curvature, variation), endpoint)) / 3);
    }
    for (size_t index : active) {
      const auto &term = data.radiance[index];
      const double tau = term.support.tau[c];
      DeepDepthDerivativeBounds derivative;
      require_depth(term.physical_span.derivatives(ray.depth_map, slab.a, slab.b, derivative));
      const auto left = mapped_integral(term.physical_span, ray.depth_map, tau, origin, slab.a);
      const auto right = mapped_integral(term.physical_span, ray.depth_map, tau, origin, slab.b);
      const auto variation = mapped_integral(
          term.physical_span, ray.depth_map, tau, slab.a, slab.b);
      double curvature;
      if (tau == 0)
        curvature = product_bound(derivative.second_abs, square_width) / 8;
      else {
        DeepDepthParameterBounds start;
        require_depth(term.physical_span.fraction(ray.depth_map, slab.a, start));
        const double second = add_bound(
            product_bound(tau, product_bound(derivative.first_abs, derivative.first_abs)),
            derivative.second_abs);
        curvature = product_bound(product_bound(product_bound(std::exp(-tau * start.lower),
                                                              upward(tau / -std::expm1(-tau))),
                                                second),
                                  square_width) /
                    8;
      }
      const double endpoint = std::max(left.error(), right.error());
      const double scale = product_bound(std::abs(term.support.post_clamp_rgb[c]), data.exposure);
      result[c + 1] = add_bound(
          result[c + 1],
          product_bound(scale, add_bound(std::min(curvature, variation.upper), endpoint)));
    }
  }
  return result;
}

/* Bounded chord error over the complete known exponential mixture. Total variation
 * independently caps the curvature enclosure at extreme tau; no opacity shortcut. */
Value chord_error(const DeepNativeCurveStorage &data,
                  const RayRange &ray,
                  size_t optical,
                  std::span<const size_t> active,
                  const double *live,
                  double origin,
                  Slab slab)
{
  if (ray.radial)
    return radial_chord_error(data, ray, optical, active, live, origin, slab);
  Value bound = {};
  for (int c = 0; c < 3; c++) {
    if (optical != absent) {
      const auto &p = data.optical[optical].support;
      const double at_left = live[c] *
                             std::exp(-p.tau[c] * ((slab.a - origin) / (p.z_back - p.z)));
      const double delta = p.tau[c] * ((slab.b - slab.a) / (p.z_back - p.z));
      const double curvature = product_bound(at_left, product_bound(delta, delta)) / 8;
      const double variation = product_bound(at_left, -std::expm1(-delta));
      bound[0] = add_bound(bound[0], std::min(curvature, variation) / 3);
    }
    for (size_t index : active) {
      const auto &p = data.radiance[index].support;
      if (p.tau[c] == 0)
        continue;
      const double delta = p.tau[c] * ((slab.b - slab.a) / (p.z_back - p.z));
      const double attenuation = std::exp(-p.tau[c] * ((slab.a - p.z) / (p.z_back - p.z)));
      const double scale = product_bound(std::abs(p.post_clamp_rgb[c]),
                                         product_bound(data.exposure, attenuation));
      const double denominator = -std::expm1(-p.tau[c]);
      const double curvature = product_bound(scale, product_bound(delta / denominator, delta)) / 8;
      const double variation = product_bound(scale, -std::expm1(-delta) / denominator);
      bound[c + 1] = add_bound(bound[c + 1], std::min(curvature, variation));
    }
  }
  return bound;
}

void add_chord(NativeVector<Delta> &events,
               size_t ray,
               Slab slab,
               Value front,
               Value back,
               double weight,
               uint32_t front_flags,
               uint32_t back_flags)
{
  Delta begin, end;
  begin.ray = end.ray = ray;
  begin.chord = end.chord = events.size();
  begin.edge = 1;
  end.edge = -1;
  begin.z = slab.a;
  end.z = slab.b;
  begin.flags = front_flags;
  end.flags = back_flags;
  for (int c = 0; c < 4; c++) {
    const double slope = (back[c] - front[c]) / (slab.b - slab.a);
    begin.slope[c] = slope * weight;
    end.slope[c] = -begin.slope[c];
    end.correction[c] = (back[c] - std::fma(slope, slab.b - slab.a, front[c])) * weight;
    if (slope != 0) {
      begin.active[c] = 1;
      end.active[c] = -1;
    }
  }
  events.push_back(begin);
  events.push_back(end);
}

DeepStatus ray_events(DeepNativeCurveStorage &data,
                      const DeepNativeCurvePolicy &policy,
                      NativeVector<Delta> &events,
                      const DeepMemoryBudget &budget)
{
  NativeVector<Boundary> boundaries{DeepBudgetAllocator<Boundary>(budget)};
  NativeVector<size_t> active{DeepBudgetAllocator<size_t>(budget)};
  NativeVector<size_t> positions{DeepBudgetAllocator<size_t>(budget)};
  NativeVector<Slab> pending{DeepBudgetAllocator<Slab>(budget)};
  positions.resize(data.radiance.size(), absent);
  Delta zero;
  zero.flags = DEEP_KNOT_PROTECTED_CUT;
  events.push_back(zero);
  for (size_t ray_index = 0; ray_index < data.rays.size(); ray_index++) {
    const auto &ray = data.rays[ray_index];
    const size_t ray_event_begin = events.size();
    const double weight = ray.weight / data.weight;
    if (ray.sample.disposition == DeepCameraDisposition::ClearTransparent)
      continue;
    if (ray.sample.disposition == DeepCameraDisposition::InvalidCameraBlack) {
      Delta black;
      black.z = infinity_depth;
      black.jump[0] = -weight;
      black.flags = DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_WORLD_TERMINAL |
                    DEEP_KNOT_INVALID_CAMERA_BLACK;
      events.push_back(black);
      continue;
    }
    boundaries.clear();
    active.clear();
    for (size_t i = ray.optical; i < ray.optical + ray.optical_count; i++) {
      boundaries.push_back({data.optical[i].support.z, OpticalBegin, i});
      boundaries.push_back({data.optical[i].support.z_back, OpticalEnd, i});
    }
    for (size_t i = ray.visibility; i < ray.visibility + ray.visibility_count; i++)
      boundaries.push_back(
          {data.visibility[i].support.z, Visibility, i, data.visibility[i].support.ordinal});
    for (size_t i = ray.radiance; i < ray.radiance + ray.radiance_count; i++) {
      const auto &term = data.radiance[i].support;
      if (term.basis == DeepCurveRadianceBasis::Point)
        boundaries.push_back({term.z, RadiancePoint, i, term.ordinal});
      else {
        boundaries.push_back({term.z, RadianceBegin, i});
        boundaries.push_back({term.z_back, RadianceEnd, i});
      }
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const auto &a, const auto &b) {
      return std::tie(a.z, a.kind, a.ordinal) < std::tie(b.z, b.kind, b.ordinal);
    });
    double live[3] = {1, 1, 1}, holes[3] = {}, rgb[3] = {}, last = 0;
    size_t optical = absent;
    Value ray_bound = {}, arithmetic = {}, absolute = {1, 0, 0, 0};
    double projection_error = 0;
    for (size_t i = ray.optical; i < ray.optical + ray.optical_count; i++)
      projection_error = add_bound(projection_error, data.optical[i].support.projection_error);
    for (size_t i = ray.radiance; i < ray.radiance + ray.radiance_count; i++)
      for (int c = 0; c < 3; c++)
        absolute[c + 1] = add_bound(
            absolute[c + 1],
            product_bound(std::abs(data.radiance[i].support.post_clamp_rgb[c]), data.exposure));
    for (size_t first = 0; first < boundaries.size();) {
      size_t end = first + 1;
      while (end < boundaries.size() && boundaries[end].z == boundaries[first].z)
        end++;
      const double z = boundaries[first].z;
      if (z > last) {
        pending.clear();
        pending.push_back({last, z});
        while (!pending.empty()) {
          Slab slab = pending.back();
          pending.pop_back();
          const Value error = chord_error(data, ray, optical, active, live, last, slab);
          bool accepted = true;
          for (int c = 0; c < 4; c++)
            accepted &= std::isfinite(error[c]) &&
                        error[c] <= 0.25 * (c == 0 ? policy.transmission_error : policy.rgb_error);
          if (!accepted) {
            const double candidate = slab.a + (slab.b - slab.a) * 0.5;
            const double mid = policy.float_depth_grid ? double(float(candidate)) : candidate;
            if (!(mid > slab.a && mid < slab.b))
              return fail(DeepErrorCode::InvalidPayload,
                          "DepthNotRepresentable: NativeCurve curvature bound");
            pending.push_back({mid, slab.b});
            pending.push_back({slab.a, mid});
            continue;
          }
          const Value front = state_value(
              data, ray, optical, active, live, holes, rgb, last, slab.a);
          const Value back = state_value(
              data, ray, optical, active, live, holes, rgb, last, slab.b);
          add_chord(events,
                    ray_index,
                    slab,
                    front,
                    back,
                    weight,
                    slab.a == last ? DEEP_KNOT_INTERVAL_BOUNDARY : 0,
                    slab.b == z ? DEEP_KNOT_INTERVAL_BOUNDARY : 0);
          for (int c = 0; c < 4; c++)
            ray_bound[c] = std::max(ray_bound[c], error[c]);
        }
      }
      const Value before = state_value(data, ray, optical, active, live, holes, rgb, last, z);
      for (int c = 0; c < 3; c++) {
        if (optical != absent) {
          const auto &p = data.optical[optical];
          if (ray.radial)
            arithmetic[0] = add_bound(
                arithmetic[0],
                product_bound(live[c],
                              mapped_integral(
                                  p.physical_span, ray.depth_map, p.support.tau[c], last, z, true)
                                  .error()) /
                    3);
          live[c] *= optical_step(data, ray, optical, c, last, z);
        }
        Sum next;
        next.add(rgb[c]);
        for (size_t index : active) {
          if (ray.radial) {
            const auto &p = data.radiance[index];
            arithmetic[c + 1] = add_bound(
                arithmetic[c + 1],
                product_bound(
                    data.exposure,
                    product_bound(
                        std::abs(p.support.post_clamp_rgb[c]),
                        mapped_integral(p.physical_span, ray.depth_map, p.support.tau[c], last, z)
                            .error())));
          }
          next.add(beer_delta(data, ray, index, c, last, z));
        }
        rgb[c] = next.value();
      }
      uint32_t flags = DEEP_KNOT_INTERVAL_BOUNDARY;
      for (size_t i = first; i < end; i++) {
        const auto &event = boundaries[i];
        switch (event.kind) {
          case OpticalEnd:
            optical = absent;
            break;
          case OpticalBegin:
            optical = event.index;
            break;
          case RadianceBegin:
            flags |= data.radiance[event.index].support.flags;
            positions[event.index] = active.size();
            active.push_back(event.index);
            break;
          case RadianceEnd: {
            flags |= data.radiance[event.index].support.flags;
            const size_t position = positions[event.index];
            if (position == absent || position >= active.size())
              return fail(DeepErrorCode::InvalidReference,
                          "NativeCurve active radiance ownership mismatch");
            active[position] = active.back();
            positions[active[position]] = position;
            active.pop_back();
            positions[event.index] = absent;
            break;
          }
          case Visibility: {
            const auto &p = data.visibility[event.index].support;
            flags |= p.flags | DEEP_KNOT_PROTECTED_CUT;
            for (int c = 0; c < 3; c++) {
              holes[c] = std::fma(live[c], p.holdout[c], holes[c]);
              live[c] *= p.transparency[c];
            }
            break;
          }
          case RadiancePoint: {
            const auto &p = data.radiance[event.index].support;
            flags |= p.flags | DEEP_KNOT_POINT_RADIANCE | DEEP_KNOT_PROTECTED_CUT;
            for (int c = 0; c < 3; c++)
              rgb[c] += p.post_clamp_rgb[c];
            break;
          }
        }
      }
      const Value after = state_value(data, ray, optical, active, live, holes, rgb, z, z);
      Delta jump;
      jump.z = z;
      jump.flags = flags;
      for (int c = 0; c < 4; c++)
        jump.jump[c] = (after[c] - before[c]) * weight;
      events.push_back(jump);
      last = z;
      first = end;
    }
    if (optical != absent || !active.empty())
      return fail(DeepErrorCode::Incomplete, "NativeCurve unfinished optical/radiance interval");
    const double nu = 256 * std::numeric_limits<double>::epsilon() *
                      (double(boundaries.size()) + double(events.size() - ray_event_begin) + 1);
    const double gamma = nu < 1 ? upward(nu / (1 - nu)) : std::numeric_limits<double>::infinity();
    for (int c = 0; c < 4; c++) {
      double source_error = add_bound(ray_bound[c], arithmetic[c]);
      if (c == 0 && projection_error != 0)
        source_error = add_bound(source_error, projection_error);
      data.error[c] = add_bound(
          data.error[c],
          product_bound(weight, add_bound(source_error, product_bound(gamma, absolute[c]))));
    }
  }
  return {};
}

double transmission_reference(const DeepNativeCurveStorage &data, double z, bool before)
{
  Sum population;
  for (const auto &ray : data.rays) {
    if (ray.sample.disposition == DeepCameraDisposition::ClearTransparent) {
      population.add(ray.weight);
      continue;
    }
    if (ray.sample.disposition == DeepCameraDisposition::InvalidCameraBlack) {
      population.add(ray.weight * (z < infinity_depth || (before && z == infinity_depth) ? 1 : 0));
      continue;
    }
    double v[3] = {1, 1, 1}, h[3] = {}, last = 0;
    auto attenuate = [&](double end) {
      Sum tau[3];
      for (size_t i = ray.optical; i < ray.optical + ray.optical_count; i++) {
        const auto &p = data.optical[i].support;
        const double a = std::max(last, p.z), b = std::min(end, p.z_back);
        if (b > a)
          for (int c = 0; c < 3; c++)
            if (ray.radial) {
              DeepDepthParameterBounds fraction;
              require_depth(data.optical[i].physical_span.delta(ray.depth_map, a, b, fraction));
              tau[c].add(p.tau[c] * fraction.value);
            }
            else
              tau[c].add(p.tau[c] * ((b - a) / (p.z_back - p.z)));
      }
      for (int c = 0; c < 3; c++)
        v[c] *= std::exp(-tau[c].value());
      last = end;
    };
    for (size_t i = ray.visibility; i < ray.visibility + ray.visibility_count; i++) {
      const auto &p = data.visibility[i].support;
      if (p.z > z || (before && p.z == z))
        break;
      attenuate(p.z);
      for (int c = 0; c < 3; c++) {
        h[c] = std::fma(v[c], p.holdout[c], h[c]);
        v[c] *= p.transparency[c];
      }
    }
    attenuate(z);
    Sum ray_t;
    for (int c = 0; c < 3; c++)
      ray_t.add(v[c] + h[c]);
    population.add(ray.weight * (ray_t.value() / 3));
  }
  return population.value() / data.weight;
}

class PageWriter {
 public:
  PageWriter(DeepPageStore &store, DeepNativeCurveStorage &data) : store_(store), data_(data) {}
  DeepStatus append(const DeepDepthKnot &knot)
  {
    if (!page_)
      if (auto status = store_.allocate(DeepRecordKind::DepthKnot, page_); !status)
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
    const DeepRecordTag tag{
        0, knot.source_group, 0, 0, uint32_t(DeepRecordKind::DepthKnot), knot.flags};
    if (status = store_.write(
            handle, tag, {reinterpret_cast<const uint8_t *>(&knot), sizeof(knot)});
        !status)
      return status;
    if (status = store_.commit(handle); !status)
      return status;
    data_.knots++;
    return {};
  }
  DeepStatus finish()
  {
    if (!page_)
      return {};
    DeepPageLease lease;
    if (auto status = store_.seal(page_, lease); !status)
      return status;
    if (auto status = store_.retire(page_); !status)
      return status;
    page_ = 0;
    data_.pages.push_back(lease);
    return {};
  }

 private:
  DeepPageStore &store_;
  DeepNativeCurveStorage &data_;
  uint32_t page_ = 0;
};

DeepStatus reduce(DeepPageStore &store,
                  NativeVector<Delta> &events,
                  DeepNativeCurveStorage &data,
                  const DeepNativeCurvePolicy &policy,
                  bool &needs_transmission_rebase)
{
  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return std::tie(a.z, a.edge, a.ray, a.chord) < std::tie(b.z, b.edge, b.ray, b.chord);
  });
  ActiveSlopes slopes(store.memory_budget());
  if (auto status = slopes.initialize(data.rays.size()); !status)
    return status;
  Sum values[4];
  values[0].reset(1);
  uint64_t active[4] = {};
  double arithmetic[4] = {}, last = 0, previous_transmission = 1;
  needs_transmission_rebase = false;
  PageWriter writer(store, data);
  for (size_t first = 0; first < events.size();) {
    size_t end = first + 1;
    while (end < events.size() && events[end].z == events[first].z)
      end++;
    const double z = events[first].z, distance = z - last;
    Sum correction[4], jumps[4];
    int64_t active_delta[4] = {};
    const bool transmission_was_active = active[0] != 0;
    DeepDepthKnot knot = {};
    knot.z = z;
    knot.source_group = data.knots + 1;
    for (size_t i = first; i < end; i++) {
      knot.flags |= events[i].flags;
      for (int c = 0; c < 4; c++) {
        correction[c].add(events[i].correction[c]);
        jumps[c].add(events[i].jump[c]);
        if ((events[i].active[c] > 0 && active_delta[c] == INT64_MAX) ||
            (events[i].active[c] < 0 && active_delta[c] == INT64_MIN))
          return fail(DeepErrorCode::InvalidCount, "NativeCurve active event count overflow");
        active_delta[c] += events[i].active[c];
      }
    }
    for (int c = 0; c < 4; c++) {
      if (active[c] && distance) {
        const double advance = slopes.slope(c) * distance;
        arithmetic[c] = add_bound(arithmetic[c], product_bound(slopes.error(c), distance));
        arithmetic[c] = add_bound(arithmetic[c],
                                  product_bound(8 * std::numeric_limits<double>::epsilon(),
                                                std::abs(values[c].value()) + std::abs(advance)));
        /* Preserve the low part across advances. Rounding value() back into a
         * new accumulator discards earlier compensation before later AA cuts. */
        values[c].add(advance);
        values[c].add(std::fma(slopes.slope(c), distance, -advance));
      }
      values[c].add(correction[c].value());
      knot.before[c] = values[c].value();
      values[c].add(jumps[c].value());
      knot.after[c] = values[c].value();
      arithmetic[c] = add_bound(arithmetic[c],
                                product_bound(8 * std::numeric_limits<double>::epsilon(),
                                              std::abs(knot.before[c]) +
                                                  std::abs(correction[c].value()) +
                                                  std::abs(jumps[c].value())));
      if (active_delta[c] < 0) {
        const uint64_t count = uint64_t(-(active_delta[c] + 1)) + 1;
        if (count > active[c])
          return fail(DeepErrorCode::InvalidCount, "NativeCurve chord ownership underflow");
        active[c] -= count;
      }
      else {
        if (uint64_t(active_delta[c]) > UINT64_MAX - active[c])
          return fail(DeepErrorCode::InvalidCount, "NativeCurve chord ownership overflow");
        active[c] += uint64_t(active_delta[c]);
      }
    }
    for (size_t i = first; i < end; i++)
      if (auto status = slopes.update(events[i]); !status)
        return status;
    for (int c = 0; c < 4; c++) {
      knot.slope_right[c] = slopes.slope(c);
      if (!std::isfinite(knot.before[c]) || !std::isfinite(knot.after[c]) ||
          !std::isfinite(knot.slope_right[c]))
        return fail(DeepErrorCode::InvalidPayload, "NativeCurve AA arithmetic overflow");
    }
    /* Anchor each new constant-transmission interval to the owned sources,
     * including visibility cuts while no optical chord is active.
     * Otherwise chord summation roundoff can leave its plateau just below a later
     * exact opaque-cut left limit, making the normalized curve increase by one ULP.
     * Charge the arithmetic difference instead of clipping or altering the sources. */
    if ((!active[0] && (transmission_was_active || jumps[0].value() != 0)) || knot.before[0] < 0 ||
        knot.before[0] > 1 || knot.after[0] < 0 || knot.after[0] > knot.before[0])
    {
      const double before = transmission_reference(data, z, true),
                   after = transmission_reference(data, z, false);
      arithmetic[0] = add_bound(
          arithmetic[0], std::abs(knot.before[0] - before) + std::abs(knot.after[0] - after));
      knot.before[0] = before;
      knot.after[0] = after;
      values[0].reset(after);
    }
    if (knot.before[0] < 0 || knot.before[0] > 1 || knot.after[0] < 0 ||
        knot.after[0] > knot.before[0] || knot.slope_right[0] > 0)
      return fail(DeepErrorCode::InvalidPayload,
                  "NativeCurve normalized optical arithmetic outside range");
    for (int c = 0; c < 4; c++)
      if (add_bound(data.error[c], arithmetic[c]) >
          (c == 0 ? policy.transmission_error : policy.rgb_error))
      {
        char message[256];
        std::snprintf(message,
                      sizeof(message),
                      "NativeCurve absolute representation budget exceeded: channel=%d z=%.17g "
                      "representation=%.17g arithmetic=%.17g budget=%.17g",
                      c,
                      z,
                      data.error[c],
                      arithmetic[c],
                      c == 0 ? policy.transmission_error : policy.rgb_error);
        return fail(DeepErrorCode::InvalidPayload, message);
      }
    needs_transmission_rebase |= knot.before[0] > previous_transmission;
    previous_transmission = knot.after[0];
    if (auto status = writer.append(knot); !status)
      return status;
    last = z;
    first = end;
  }
  if (!slopes.empty())
    return fail(DeepErrorCode::Incomplete, "NativeCurve unfinished chord ownership");
  for (int c = 0; c < 4; c++)
    data.error[c] = add_bound(data.error[c], arithmetic[c]);
  return writer.finish();
}

/* The delta sweep may lose a very small surviving prefix while other rays end.
 * Rebuild only an actually nonmonotone pixel from its still-owned positive source
 * reference, before the curve is published or any source is retired. The maximum
 * whole-domain PWL change is charged in addition to the original certificate. */
DeepStatus rebase_transmission(DeepPageStore &store,
                               DeepNativeCurveStorage &data,
                               const DeepNativeCurvePolicy &policy)
{
  NativeVector<DeepPageLease> old_pages(data.pages.get_allocator());
  old_pages.swap(data.pages);
  const uint64_t old_count = data.knots;
  data.knots = 0;
  PageWriter writer(store, data);
  DeepDepthKnot old_previous = {}, pending = {};
  bool have_pending = false;
  uint64_t count = 0;
  double change = 0;
  const auto difference = [](double a, double b) { return upward(std::abs(a - b)); };
  const auto budget_status = [&]() {
    if (add_bound(data.error[0], change) > policy.transmission_error)
      return fail(DeepErrorCode::InvalidPayload,
                  "NativeCurve source transmission rebase exceeds the remaining error budget");
    return DeepStatus{};
  };
  for (const auto &page : old_pages) {
    const auto *descriptor = page.descriptor();
    if (!descriptor || descriptor->kind != DeepRecordKind::DepthKnot)
      return fail(DeepErrorCode::InvalidLayout, "NativeCurve rebase page kind mismatch");
    uint64_t valid = 0;
    for (uint32_t slot = 0; slot < descriptor->reserved; slot++) {
      const uint64_t handle = page.valid_handle_at(slot);
      if (!handle)
        continue;
      if (count == old_count)
        return fail(DeepErrorCode::InvalidCount, "NativeCurve rebase exceeds owned knot count");
      DeepRecordTag tag;
      std::span<const uint8_t> bytes;
      if (auto status = page.read(handle, tag, bytes); !status)
        return status;
      if (bytes.size() != sizeof(DeepDepthKnot) || tag.kind != uint32_t(DeepRecordKind::DepthKnot))
        return fail(DeepErrorCode::InvalidLayout, "NativeCurve rebase knot ABI mismatch");
      DeepDepthKnot original;
      std::memcpy(&original, bytes.data(), sizeof(original));
      DeepDepthKnot next = original;
      next.before[0] = transmission_reference(data, next.z, true);
      next.after[0] = transmission_reference(data, next.z, false);
      if (!std::isfinite(next.before[0]) || !std::isfinite(next.after[0]) || next.before[0] < 0 ||
          next.before[0] > 1 || next.after[0] < 0 || next.after[0] > next.before[0] ||
          (!have_pending && next.before[0] != 1) ||
          (have_pending && next.before[0] > pending.after[0]))
        return fail(DeepErrorCode::InvalidPayload,
                    "NativeCurve source transmission reference is not normalized monotone");
      change = std::max({change,
                         difference(next.before[0], original.before[0]),
                         difference(next.after[0], original.after[0])});
      if (have_pending) {
        const double distance = next.z - pending.z;
        if (!(distance > 0) || !std::isfinite(distance))
          return fail(DeepErrorCode::InvalidPayload, "NativeCurve rebase knot depth order");
        pending.slope_right[0] = (next.before[0] - pending.after[0]) / distance;
        if (!std::isfinite(pending.slope_right[0]) || pending.slope_right[0] > 0)
          return fail(DeepErrorCode::InvalidPayload, "NativeCurve rebase transmission slope");
        /* Both PWL functions are linear on this unchanged interval. Their maximum
         * difference is at an endpoint. Enclose rounded endpoint FMA evaluation. */
        const double old_end = std::fma(
            old_previous.slope_right[0], distance, old_previous.after[0]);
        const double new_end = std::fma(pending.slope_right[0], distance, pending.after[0]);
        const double magnitude = add_bound(
            add_bound(std::abs(old_previous.after[0]), std::abs(pending.after[0])),
            add_bound(product_bound(std::abs(old_previous.slope_right[0]), distance),
                      product_bound(std::abs(pending.slope_right[0]), distance)));
        const double rounding = product_bound(8 * std::numeric_limits<double>::epsilon(),
                                              magnitude);
        change = std::max(change, add_bound(difference(old_end, new_end), rounding));
        if (auto status = budget_status(); !status)
          return status;
        if (auto status = writer.append(pending); !status)
          return status;
      }
      old_previous = original;
      pending = next;
      have_pending = true;
      count++;
      valid++;
    }
    if (valid != descriptor->valid)
      return fail(DeepErrorCode::InvalidCount, "NativeCurve rebase page valid count mismatch");
  }
  if (!have_pending || count != old_count)
    return fail(DeepErrorCode::InvalidCount, "NativeCurve rebase incomplete owned knots");
  pending.slope_right[0] = 0;
  if (auto status = budget_status(); !status)
    return status;
  if (auto status = writer.append(pending); !status)
    return status;
  if (auto status = writer.finish(); !status)
    return status;
  if (data.knots != old_count)
    return fail(DeepErrorCode::InvalidCount, "NativeCurve rebase output knot count mismatch");
  data.error[0] = add_bound(data.error[0], change);
  return {};
}
}  // namespace

DeepNativeCurve::operator bool() const
{
  return bool(storage_);
}
const DeepFrameKey *DeepNativeCurve::frame_key() const
{
  return storage_ ? &storage_->frame : nullptr;
}
uint64_t DeepNativeCurve::native_population() const
{
  return storage_ ? storage_->population : 0;
}
uint64_t DeepNativeCurve::knot_count() const
{
  return storage_ ? storage_->knots : 0;
}
std::span<const DeepPageLease> DeepNativeCurve::pages() const
{
  return storage_ ? std::span<const DeepPageLease>(storage_->pages) :
                    std::span<const DeepPageLease>();
}
std::span<const double, 4> DeepNativeCurve::representation_error() const
{
  static const double empty[4] = {};
  return storage_ ? std::span<const double, 4>(storage_->error) :
                    std::span<const double, 4>(empty);
}
std::span<const DeepOwnedOpticalSource> DeepNativeCurve::optical_sources() const
{
  return storage_ ? std::span<const DeepOwnedOpticalSource>(storage_->optical) :
                    std::span<const DeepOwnedOpticalSource>();
}
std::span<const DeepOwnedVisibilitySource> DeepNativeCurve::visibility_sources() const
{
  return storage_ ? std::span<const DeepOwnedVisibilitySource>(storage_->visibility) :
                    std::span<const DeepOwnedVisibilitySource>();
}
std::span<const DeepOwnedRadianceSource> DeepNativeCurve::radiance_sources() const
{
  return storage_ ? std::span<const DeepOwnedRadianceSource>(storage_->radiance) :
                    std::span<const DeepOwnedRadianceSource>();
}
DeepStatus DeepNativeCurve::read_knot(uint64_t index, DeepDepthKnot &knot) const
{
  if (!storage_ || index >= storage_->knots)
    return fail(DeepErrorCode::InvalidReference, "NativeCurve knot outside owned range");
  for (const auto &page : storage_->pages) {
    if (index >= page.descriptor()->valid) {
      index -= page.descriptor()->valid;
      continue;
    }
    for (uint32_t slot = 0; slot < page.descriptor()->reserved; slot++) {
      const uint64_t handle = page.valid_handle_at(slot);
      if (!handle)
        continue;
      if (index) {
        index--;
        continue;
      }
      DeepRecordTag tag;
      std::span<const uint8_t> payload;
      if (auto status = page.read(handle, tag, payload); !status)
        return status;
      if (payload.size() != sizeof(knot))
        return fail(DeepErrorCode::InvalidLayout, "NativeCurve knot ABI mismatch");
      std::memcpy(&knot, payload.data(), sizeof(knot));
      return {};
    }
  }
  return fail(DeepErrorCode::InvalidReference, "NativeCurve immutable knot count mismatch");
}

DeepStatus deep_native_curve_policy_for_budget(const double alpha_tolerance,
                                               const double rgb_absolute_tolerance,
                                               DeepNativeCurvePolicy &policy)
{
  if (!std::isfinite(alpha_tolerance) || alpha_tolerance < 0 || alpha_tolerance > 1 ||
      !std::isfinite(rgb_absolute_tolerance) || rgb_absolute_tolerance < 0)
    return DeepStatus::fail(DeepErrorCode::InvalidPayload, "Invalid frozen consumer error budget");
  if (alpha_tolerance == 0 || rgb_absolute_tolerance == 0)
    return DeepStatus::fail(DeepErrorCode::InvalidPayload,
                            "InsufficientErrorBudget: exact continuous zero cannot be raised to a "
                            "finite NativeCurve representation tolerance");
  const double transmission = std::min(0.004, 0.2 * alpha_tolerance);
  const double rgb = std::min(0.004, 0.2 * rgb_absolute_tolerance);
  if (!(transmission > 0) || !(rgb > 0))
    return DeepStatus::fail(DeepErrorCode::InvalidPayload,
                            "InsufficientErrorBudget: representation share is not representable");
  policy.transmission_error = transmission;
  policy.rgb_error = rgb;
  return {};
}

DeepStatus deep_build_native_curve(DeepPageStore &store,
                                   const DeepNativeCurvePixel &input,
                                   DeepNativeCurve &output,
                                   const DeepNativeCurvePolicy &policy)
{
  output = {};
  try {
    const auto budget = store.memory_budget();
    auto data = std::allocate_shared<DeepNativeCurveStorage>(
        DeepBudgetAllocator<DeepNativeCurveStorage>(budget), budget);
    if (auto status = retain_input(store, input, policy, *data); !status)
      return status;
    NativeVector<Delta> events{DeepBudgetAllocator<Delta>(budget)};
    if (auto status = ray_events(*data, policy, events, budget); !status)
      return status;
    bool needs_transmission_rebase = false;
    if (auto status = reduce(store, events, *data, policy, needs_transmission_rebase); !status)
      return status;
    if (needs_transmission_rebase)
      if (auto status = rebase_transmission(store, *data, policy); !status)
        return status;
    if (!policy.retain_sources) {
      /* All source/coverage/order/population checks and normalization succeeded.
       * Drop duplicate scratch before publication; knots and error proof remain
       * immutable and sufficient for every independent output consumer. */
      NativeVector<RayRange>(data->rays.get_allocator()).swap(data->rays);
      NativeVector<DeepOwnedOpticalSource>(data->optical.get_allocator()).swap(data->optical);
      NativeVector<DeepOwnedVisibilitySource>(data->visibility.get_allocator())
          .swap(data->visibility);
      NativeVector<DeepOwnedRadianceSource>(data->radiance.get_allocator()).swap(data->radiance);
    }
    output.storage_ = std::move(data);
    return {};
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "NativeCurve scratch/source/page quota exhausted");
  }
  catch (const DepthEvaluationError &error) {
    return error.status;
  }
  catch (const std::length_error &) {
    return fail(DeepErrorCode::InvalidCount, "NativeCurve checked vector count overflow");
  }
}

CCL_NAMESPACE_END
#ifdef _MSC_VER
#  pragma float_control(pop)
#endif
