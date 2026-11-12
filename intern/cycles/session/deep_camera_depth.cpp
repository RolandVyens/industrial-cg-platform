/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_camera_depth.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef _MSC_VER
#  pragma float_control(precise, on, push)
#endif

CCL_NAMESPACE_BEGIN
namespace {
constexpr double infinity = std::numeric_limits<double>::infinity();
struct DepthArithmeticInterval {
  double lo = 0, hi = 0;
};
double down(double x)
{
  return std::nextafter(x, -infinity);
}
double up(double x)
{
  return std::nextafter(x, infinity);
}
bool zero(DepthArithmeticInterval a)
{
  return a.lo == 0 && a.hi == 0;
}
DepthArithmeticInterval exact(double x)
{
  return {x, x};
}
DepthArithmeticInterval add(DepthArithmeticInterval a, DepthArithmeticInterval b)
{
  if (zero(a))
    return b;
  if (zero(b))
    return a;
  if (a.lo == a.hi && b.lo == b.hi && a.lo == -b.lo)
    return {};
  return {down(a.lo + b.lo), up(a.hi + b.hi)};
}
DepthArithmeticInterval subtract(DepthArithmeticInterval a, DepthArithmeticInterval b)
{
  return add(a, {-b.hi, -b.lo});
}
DepthArithmeticInterval multiply(DepthArithmeticInterval a, DepthArithmeticInterval b)
{
  if (zero(a) || zero(b))
    return {};
  if (a.lo == 1 && a.hi == 1)
    return b;
  if (b.lo == 1 && b.hi == 1)
    return a;
  if (a.lo == -1 && a.hi == -1)
    return {-b.hi, -b.lo};
  if (b.lo == -1 && b.hi == -1)
    return {-a.hi, -a.lo};
  const double values[] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
  return {down(*std::min_element(values, values + 4)), up(*std::max_element(values, values + 4))};
}
DepthArithmeticInterval square(DepthArithmeticInterval a)
{
  if (zero(a))
    return {};
  if (a.lo == a.hi && std::abs(a.lo) == 1)
    return exact(1);
  const double low = a.lo <= 0 && a.hi >= 0 ? 0 : std::min(a.lo * a.lo, a.hi * a.hi);
  return {low == 0 ? 0 : std::max(0., down(low)), up(std::max(a.lo * a.lo, a.hi * a.hi))};
}
DepthArithmeticInterval root(DepthArithmeticInterval a)
{
  return {a.lo <= 0 ? 0 : std::max(0., down(std::sqrt(a.lo))),
          a.hi == 0 ? 0 : up(std::sqrt(a.hi))};
}
DepthArithmeticInterval divide(DepthArithmeticInterval a, DepthArithmeticInterval positive)
{
  return multiply(a, {down(1 / positive.hi), up(1 / positive.lo)});
}
std::array<double, 2> save(DepthArithmeticInterval a)
{
  return {a.lo, a.hi};
}
DepthArithmeticInterval load(const std::array<double, 2> &a)
{
  return {a[0], a[1]};
}
DeepStatus invalid(const char *message)
{
  return DeepStatus::fail(DeepErrorCode::InvalidPayload, message);
}
bool captured_number(double x)
{
  return std::isfinite(x) && std::abs(x) <= double(std::numeric_limits<float>::max());
}
}  // namespace

DeepStatus DeepCameraDepthMap::initialize(const DeepCameraDepthSpec &spec)
{
  valid_ = false;
  if (!captured_number(spec.tmin) || !captured_number(spec.tmax) || spec.tmin < 0 ||
      spec.tmax < spec.tmin)
    return invalid("CameraDepth: invalid frozen physical range");
  spec_ = spec;
  if (spec.kind == DeepCameraDepthKind::Affine) {
    if (!captured_number(spec.affine_origin) || !captured_number(spec.affine_slope))
      return invalid("CameraDepth: nonfinite affine definition");
    if (spec.affine_slope <= 0)
      return invalid("NonMonotonicCameraDepth: affine slope is not positive");
  }
  else if (spec.kind == DeepCameraDepthKind::Radial) {
    DepthArithmeticInterval a, b, c, u[3], d[3], cross;
    a_value_ = b_value_ = c_value_ = 0;
    for (int i = 0; i < 3; i++) {
      if (!captured_number(spec.origin[i]) || !captured_number(spec.direction[i]) ||
          !captured_number(spec.camera_origin[i]))
        return invalid("CameraDepth: nonfinite frozen radial geometry");
      u[i] = subtract(exact(spec.origin[i]), exact(spec.camera_origin[i]));
      d[i] = exact(spec.direction[i]);
      offset_[i] = spec.origin[i] - spec.camera_origin[i];
      a = add(a, square(d[i]));
      b = add(b, multiply(u[i], d[i]));
      c = add(c, square(u[i]));
      a_value_ = std::fma(spec.direction[i], spec.direction[i], a_value_);
      b_value_ = std::fma(offset_[i], spec.direction[i], b_value_);
      c_value_ = std::fma(offset_[i], offset_[i], c_value_);
    }
    if (!(a.lo > 0) || !std::isfinite(a.hi))
      return invalid("CameraDepth: radial direction is not invertible");
    if (add(b, multiply(a, exact(spec.tmin))).lo < 0)
      return invalid("NonMonotonicCameraDepth: outward radial branch is not certified");
    for (int i = 0; i < 3; i++) {
      const int j = (i + 1) % 3, k = (i + 2) % 3;
      cross = add(cross, square(subtract(multiply(u[j], d[k]), multiply(u[k], d[j]))));
    }
    a_ = save(a);
    b_ = save(b);
    c_ = save(c);
    cross_squared_upper_ = cross.hi;
  }
  else
    return invalid("CameraDepth: unknown mapping kind");
  valid_ = true;
  double front, back;
  if (auto status = depth(spec.tmin, front); !status) {
    valid_ = false;
    return status;
  }
  if (auto status = depth(spec.tmax, back); !status) {
    valid_ = false;
    return status;
  }
  if (front < 0 || back < front) {
    valid_ = false;
    return invalid("CameraDepth: invalid nonnegative depth range");
  }
  return {};
}

DeepStatus DeepCameraDepthMap::depth(const double physical_t, double &z) const
{
  z = 0;
  if (!valid_ || !captured_number(physical_t) || physical_t < spec_.tmin)
    return invalid("CameraDepth: invalid map or physical parameter");
  if (spec_.kind == DeepCameraDepthKind::Affine)
    z = std::fma(spec_.affine_slope, physical_t, spec_.affine_origin);
  else {
    double v[3];
    for (int i = 0; i < 3; i++)
      v[i] = std::fma(spec_.direction[i], physical_t, offset_[i]);
    z = std::hypot(v[0], v[1], v[2]);
  }
  if (!std::isfinite(z))
    return invalid("CameraDepth: forward arithmetic overflow");
  return {};
}

DeepStatus DeepCameraDepthMap::parameter(const double z, DeepDepthParameterBounds &output) const
{
  output = {};
  if (!valid_ || !captured_number(z) || z < 0)
    return invalid("CameraDepth: invalid inverse depth");
  DepthArithmeticInterval result;
  double value;
  if (spec_.kind == DeepCameraDepthKind::Affine) {
    result = divide(subtract(exact(z), exact(spec_.affine_origin)), exact(spec_.affine_slope));
    value = (z - spec_.affine_origin) / spec_.affine_slope;
  }
  else {
    const DepthArithmeticInterval a = load(a_), b = load(b_), c = load(c_);
    const DepthArithmeticInterval numerator = subtract(square(exact(z)), c);
    DepthArithmeticInterval discriminant = add(square(b), multiply(a, numerator));
    if (discriminant.hi < 0)
      return invalid("CameraDepth: depth has no radial preimage");
    discriminant.lo = std::max(0., discriminant.lo);
    const DepthArithmeticInterval r = root(discriminant), denominator = add(r, b);
    result = b.lo >= 0 && denominator.lo > 0 ? divide(numerator, denominator) :
                                               divide(subtract(r, b), a);
    const double n = std::fma(z, z, -c_value_);
    const double q = std::max(0., std::fma(a_value_, n, b_value_ * b_value_));
    const double sqrt_q = std::sqrt(q);
    value = b_value_ >= 0 && sqrt_q + b_value_ > 0 ? n / (sqrt_q + b_value_) :
                                                     (sqrt_q - b_value_) / a_value_;
  }
  if (!std::isfinite(result.lo) || !std::isfinite(result.hi) || result.hi < result.lo)
    return invalid("CameraDepth: inverse arithmetic is not representable");
  output = {std::clamp(value, result.lo, result.hi), result.lo, result.hi};
  return {};
}

DeepStatus DeepCameraDepthMap::depth_bounds(double physical_t,
                                            DeepDepthParameterBounds &output) const
{
  output = {};
  double value;
  if (auto status = depth(physical_t, value); !status)
    return status;
  DepthArithmeticInterval result;
  if (spec_.kind == DeepCameraDepthKind::Affine)
    result = add(exact(spec_.affine_origin),
                 multiply(exact(spec_.affine_slope), exact(physical_t)));
  else {
    for (int c = 0; c < 3; c++)
      result = add(result,
                   square(add(subtract(exact(spec_.origin[c]), exact(spec_.camera_origin[c])),
                              multiply(exact(spec_.direction[c]), exact(physical_t)))));
    result = root(result);
  }
  if (!std::isfinite(result.lo) || !std::isfinite(result.hi))
    return invalid("CameraDepth: forward enclosure is not representable");
  output = {std::clamp(value, result.lo, result.hi), result.lo, result.hi};
  return {};
}

DeepStatus DeepCameraDepthMap::derivatives(const double front,
                                           const double back,
                                           DeepDepthDerivativeBounds &output) const
{
  output = {};
  if (!valid_ || !captured_number(front) || !captured_number(back) || front < 0 || back < front)
    return invalid("CameraDepth: invalid derivative interval");
  if (spec_.kind == DeepCameraDepthKind::Affine) {
    output.first_abs = up(1 / spec_.affine_slope);
    return {};
  }
  const DepthArithmeticInterval a = load(a_), b = load(b_), c = load(c_);
  if (cross_squared_upper_ == 0) {
    output.first_abs = up(1 / root(a).lo);
    return {};
  }
  const DepthArithmeticInterval discriminant = add(square(b),
                                                   multiply(a, subtract(square(exact(front)), c)));
  if (discriminant.hi < 0)
    return invalid("CameraDepth: derivative interval has no radial preimage");
  if (!(discriminant.lo > 0)) {
    output.first_abs = output.second_abs = infinity;
    return {};  // Caller must use absolute total variation, not sample curvature.
  }
  const double denominator = std::max(0., down(std::sqrt(discriminant.lo)));
  output.first_abs = denominator > 0 ? up(back / denominator) : infinity;
  const double cube = std::max(0., down(discriminant.lo * denominator));
  output.second_abs = cube > 0 ? up(cross_squared_upper_ / cube) : infinity;
  return {};
}

DeepStatus DeepCameraDepthSpan::initialize(
    const DeepCameraDepthMap &map, double front, double back, double z, double z_back)
{
  valid_ = false;
  if (!map.valid() || !captured_number(front) || !captured_number(back) || front < 0 ||
      back <= front || !captured_number(z) || !captured_number(z_back) || z < 0 || z_back <= z)
    return invalid("CameraDepth: invalid physical source span");
  if (auto status = map.depth(front, geometric_front_); !status)
    return status;
  if (auto status = map.depth(back, geometric_back_); !status)
    return status;
  if (auto status = map.depth_bounds(front, front_bounds_); !status)
    return status;
  if (auto status = map.depth_bounds(back, back_bounds_); !status)
    return status;
  if (!(geometric_back_ > geometric_front_))
    return invalid("DepthNotRepresentable: physical support collapses in camera depth");
  const double tolerance = 64 * std::numeric_limits<float>::epsilon();
  if (std::abs(z - geometric_front_) > tolerance * std::max(1., geometric_front_) ||
      std::abs(z_back - geometric_back_) > tolerance * std::max(1., geometric_back_))
    return invalid("CameraDepth: captured source depth disagrees with frozen geometry");
  front_ = front;
  back_ = back;
  z_ = z;
  z_back_ = z_back;
  valid_ = true;
  return {};
}

DeepStatus DeepCameraDepthSpan::fraction(const DeepCameraDepthMap &map,
                                         double z,
                                         DeepDepthParameterBounds &output) const
{
  output = {};
  if (!valid_ || !captured_number(z) || z < z_ || z > z_back_)
    return invalid("CameraDepth: fraction outside captured physical support");
  DeepDepthParameterBounds t;
  if (z <= front_bounds_.lower)
    t = {front_, front_, front_};
  else if (z >= back_bounds_.upper)
    t = {back_, back_, back_};
  else if (front_bounds_.upper >= back_bounds_.lower)
    t = {front_, front_, back_};
  else {
    if (auto status = map.parameter(std::clamp(z, front_bounds_.upper, back_bounds_.lower), t);
        !status)
      return status;
    if (z <= front_bounds_.upper)
      t.lower = std::min(t.lower, front_);
    if (z >= back_bounds_.lower)
      t.upper = std::max(t.upper, back_);
  }
  const auto bounds = divide(subtract({t.lower, t.upper}, exact(front_)),
                             subtract(exact(back_), exact(front_)));
  double value = std::clamp((t.value - front_) / (back_ - front_), 0., 1.);
  if (z == z_)
    value = 0;
  else if (z == z_back_)
    value = 1;
  output = {value,
            std::min(value, std::clamp(bounds.lo, 0., 1.)),
            std::max(value, std::clamp(bounds.hi, 0., 1.))};
  return {};
}

DeepStatus DeepCameraDepthSpan::delta(const DeepCameraDepthMap &map,
                                      double a,
                                      double b,
                                      DeepDepthParameterBounds &output) const
{
  output = {};
  if (!valid_ || b < a)
    return invalid("CameraDepth: reversed physical source interval");
  DeepDepthParameterBounds left, right;
  if (auto status = fraction(map, a, left); !status)
    return status;
  if (auto status = fraction(map, b, right); !status)
    return status;
  if (a == b)
    return {};
  const auto difference = subtract({right.lower, right.upper}, {left.lower, left.upper});
  output = {std::clamp(right.value - left.value, 0., 1.),
            std::clamp(difference.lo, 0., 1.),
            std::clamp(difference.hi, 0., 1.)};
  return {};
}

DeepStatus DeepCameraDepthSpan::derivatives(const DeepCameraDepthMap &map,
                                            double a,
                                            double b,
                                            DeepDepthDerivativeBounds &output) const
{
  output = {};
  if (!valid_ || !captured_number(a) || !captured_number(b) || a < z_ || b > z_back_ || b < a)
    return invalid("CameraDepth: derivative outside physical source support");
  if (a < front_bounds_.upper || b > back_bounds_.lower) {
    output.first_abs = output.second_abs = infinity;
    return {};  // Clamp transitions and endpoint pins require total variation.
  }
  if (auto status = map.derivatives(a, b, output); !status)
    return status;
  const double width = down(back_ - front_);
  output.first_abs = up(output.first_abs / width);
  output.second_abs = output.second_abs == 0 ? 0 : up(output.second_abs / width);
  return {};
}

CCL_NAMESPACE_END
#ifdef _MSC_VER
#  pragma float_control(pop)
#endif
