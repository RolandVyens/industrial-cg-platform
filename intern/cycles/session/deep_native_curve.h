/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "session/deep_depth_support.h"
#include <memory>
#include <span>

CCL_NAMESPACE_BEGIN

/* A complete elementary partition from the independent Optical/Visibility adapter.
 * Overlapping media are evaluated together before these non-overlapping slabs. */
struct DeepCurveOptical {
  double z = 0, z_back = 0, tau[3] = {};
  uint64_t source = 0;
  uint32_t ordinal = 0;
  /* Absolute all-depth transmission error for a derived Optical group.
   * Zero for unmodified raw supports. Never applies to native radiance. */
  double projection_error = 0;
};
struct DeepCurveVisibility {
  double z = 0, transparency[3] = {1, 1, 1}, holdout[3] = {};
  uint64_t source = 0;
  uint32_t ordinal = 0, flags = DEEP_KNOT_PROTECTED_CUT;
};
enum class DeepCurveRadianceBasis { Point, BeerSpan };
struct DeepCurveRadiance {
  DeepCurveRadianceBasis basis = DeepCurveRadianceBasis::Point;
  double z = 0, z_back = 0, post_clamp_rgb[3] = {}, tau[3] = {};
  uint64_t source = 0, emission_call = 0;
  uint32_t ordinal = 0, flags = DEEP_KNOT_PROTECTED_CUT;
};
struct DeepNativeCurveRay {
  uint64_t ray = 0;
  DeepSampleKey sample;
  double population_weight = 1;
  bool native_complete = false, optical_complete = false, affine_camera_depth = false;
  double coverage_front = 0, coverage_back = 0;
  std::span<const DeepCurveOptical> optical;
  std::span<const DeepCurveVisibility> visibility;
  /* Resolved native expression terms. Each has its actual source/root support and
   * actual film-call clamp, throughput and pass weight; no second attenuation. */
  std::span<const DeepCurveRadiance> radiance;
  bool native_world_support = false;
  DeepDepthSupportView depth_support;
};
struct DeepNativeCurvePixel {
  DeepFrameKey frame;
  int32_t x = 0, y = 0;
  uint64_t native_population = 0;
  bool population_complete = false;
  double exposure = 1;
  std::span<const DeepNativeCurveRay> rays;
};
struct DeepNativeCurvePolicy {
  double transmission_error = 1e-4, rgb_error = 1e-4;
  bool float_depth_grid = true;
  /* Reference callers keep inspectable inputs. Production may release these
   * duplicate sources after complete validation and canonical normalization. */
  bool retain_sources = true;
};
struct DeepOwnedOpticalSource {
  uint64_t ray;
  DeepCurveOptical support;
  DeepCameraDepthSpan physical_span;
};

/* Freeze a conservative share of actual positive consumer budgets before capture.
 * Exact zero is never silently raised to a representation default. */
DeepStatus deep_native_curve_policy_for_budget(double alpha_tolerance,
                                               double rgb_absolute_tolerance,
                                               DeepNativeCurvePolicy &policy);
struct DeepOwnedVisibilitySource {
  uint64_t ray;
  DeepCurveVisibility support;
};
struct DeepOwnedRadianceSource {
  uint64_t ray;
  DeepCurveRadiance support;
  DeepCameraDepthSpan physical_span;
};
struct DeepNativeCurveStorage;

/* Immutable, quota-owned normalized canonical pages and optional input provenance.
 * This mathematical adapter does not claim a CPU scene mirror or native producer exists. */
class DeepNativeCurve {
 public:
  explicit operator bool() const;
  const DeepFrameKey *frame_key() const;
  uint64_t native_population() const;
  uint64_t knot_count() const;
  std::span<const DeepPageLease> pages() const;
  std::span<const double, 4> representation_error() const;
  std::span<const DeepOwnedOpticalSource> optical_sources() const;
  std::span<const DeepOwnedVisibilitySource> visibility_sources() const;
  std::span<const DeepOwnedRadianceSource> radiance_sources() const;
  DeepStatus read_knot(uint64_t index, DeepDepthKnot &knot) const;

 private:
  std::shared_ptr<const DeepNativeCurveStorage> storage_;
  friend DeepStatus deep_build_native_curve(DeepPageStore &,
                                            const DeepNativeCurvePixel &,
                                            DeepNativeCurve &,
                                            const DeepNativeCurvePolicy &);
};

DeepStatus deep_build_native_curve(DeepPageStore &store,
                                   const DeepNativeCurvePixel &input,
                                   DeepNativeCurve &output,
                                   const DeepNativeCurvePolicy &policy = {});

CCL_NAMESPACE_END
