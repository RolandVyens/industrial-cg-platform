/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>

#include "session/deep_page_store.h"

CCL_NAMESPACE_BEGIN

using DeepCameraDepthKind = DeepFrozenDepthKind;
struct DeepCameraDepthSpec {
  DeepCameraDepthKind kind = DeepCameraDepthKind::Affine;
  double origin[3] = {}, direction[3] = {0, 0, 1}, camera_origin[3] = {};
  double affine_origin = 0, affine_slope = 1;
  double tmin = 0, tmax = 1;
};
struct DeepDepthParameterBounds {
  double value = 0, lower = 0, upper = 0;
};
struct DeepDepthDerivativeBounds {
  double first_abs = 0, second_abs = 0;
};

/* Numerical map only: frame/ray/source identity and storage belong to its owner.
 * All input values are frozen; the actual direction is never normalized. */
class DeepCameraDepthMap {
 public:
  DeepStatus initialize(const DeepCameraDepthSpec &spec);
  bool valid() const
  {
    return valid_;
  }
  DeepStatus depth(double physical_t, double &z) const;
  DeepStatus depth_bounds(double physical_t, DeepDepthParameterBounds &output) const;
  DeepStatus parameter(double z, DeepDepthParameterBounds &output) const;
  DeepStatus derivatives(double front, double back, DeepDepthDerivativeBounds &output) const;

 private:
  DeepCameraDepthSpec spec_;
  double offset_[3] = {}, a_value_ = 0, b_value_ = 0, c_value_ = 0;
  std::array<double, 2> a_ = {}, b_ = {}, c_ = {};
  double cross_squared_upper_ = 0;
  bool valid_ = false;
};

/* Physical source coordinates over captured depth endpoints. Endpoint pinning
 * preserves native totals; returned enclosures also contain the unpinned map,
 * so a chord certificate must include endpoint arithmetic discrepancies. */
class DeepCameraDepthSpan {
 public:
  DeepStatus initialize(
      const DeepCameraDepthMap &map, double front, double back, double z, double z_back);
  DeepStatus fraction(const DeepCameraDepthMap &map,
                      double z,
                      DeepDepthParameterBounds &output) const;
  DeepStatus delta(const DeepCameraDepthMap &map,
                   double a,
                   double b,
                   DeepDepthParameterBounds &output) const;
  DeepStatus derivatives(const DeepCameraDepthMap &map,
                         double a,
                         double b,
                         DeepDepthDerivativeBounds &output) const;

 private:
  double front_ = 0, back_ = 0, z_ = 0, z_back_ = 0, geometric_front_ = 0, geometric_back_ = 0;
  DeepDepthParameterBounds front_bounds_, back_bounds_;
  bool valid_ = false;
};

CCL_NAMESPACE_END
