/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>

#include "session/deep_page_store.h"

CCL_NAMESPACE_BEGIN

/* Narrow M1 input. These cases require the complete M2 NativeCurve/Optical adapter. */
enum DeepHomogeneousUnsupported : uint32_t {
  DEEP_HOMOGENEOUS_SCATTERING = 1u << 0,
  DEEP_HOMOGENEOUS_SURFACE_RGB = 1u << 1,
  DEEP_HOMOGENEOUS_VISIBILITY = 1u << 2,
  DEEP_HOMOGENEOUS_HOLDOUT = 1u << 3,
  DEEP_HOMOGENEOUS_HETEROGENEOUS = 1u << 4,
  DEEP_HOMOGENEOUS_MIXED = 1u << 5,
  DEEP_HOMOGENEOUS_BACKGROUND = 1u << 6,
};

/* A complete, non-overlapping elementary support in affine camera Z. Vacuum gaps must be
 * explicit zero-tau, zero-RGB supports. RGB is the actual native post-clamp contribution,
 * already throughput weighted; it is never multiplied by Optical transmission again.
 * source is a nonzero, unique support identity within this ray, not a recycled state index. */
struct DeepHomogeneousSegment {
  double z = 0, z_back = 0, tau = 0;
  double post_clamp_rgb[3] = {};
  uint64_t source = 0;
};

struct DeepHomogeneousRay {
  uint64_t ray = 0;
  DeepSampleKey sample;
  double population_weight = 1;
  bool native_complete = false;
  uint32_t unsupported_features = 0;

  /* Only disposition Ray reads these fields. ClearTransparent and InvalidCameraBlack
   * need no initialized camera Ray, depth map, or optical support. */
  bool coverage_complete = false, homogeneous_certified = false, affine_camera_depth = false;
  double coverage_front = 0, coverage_back = 0;
  std::span<const DeepHomogeneousSegment> segments;
};

struct DeepHomogeneousPixelInput {
  DeepFrameKey frame;
  int32_t x = 0, y = 0;
  uint64_t native_population = 0;
  bool population_complete = false;
  double exposure = 1;
  std::span<const DeepHomogeneousRay> rays;
};

/* Absolute budgets in the final exposure domain, with no per-ray relative allowance.
 * Tighter registered consumer policies may lower these; M1 cannot raise the 1e-4 ceiling. */
struct DeepHomogeneousCurvePolicy {
  double transmission_error = 1e-4, rgb_error = 1e-4;
  /* Native EXR supports already have FLOAT endpoints. Use representable FLOAT
   * subdivision cuts so AA unions cannot create artificial sub-ULP intervals. */
  bool float_depth_grid = false;
};

struct DeepHomogeneousSource {
  uint64_t ray;
  DeepHomogeneousSegment support;
};
struct DeepHomogeneousIdentity {
  uint64_t ray;
  DeepSampleKey sample;
  double population_weight;
};

struct DeepHomogeneousCurveStorage;

/* Immutable owned result. All source tau/supports survive endpoint exp underflow and are
 * available to a stricter consumer/refit. Knots span as many owned pages as required;
 * page count is never a sample cap. Copies retain the same charged storage and leases.
 * source_group in a knot is its one-based canonical depth group ordinal; exact source
 * identities and protected support boundaries are retained in sources(). */
class DeepHomogeneousCurve {
 public:
  explicit operator bool() const;
  const DeepFrameKey *frame_key() const;
  int32_t x() const;
  int32_t y() const;
  uint64_t native_population() const;
  double population_weight() const;
  uint64_t knot_count() const;
  std::span<const DeepPageLease> pages() const;
  std::span<const DeepHomogeneousSource> sources() const;
  std::span<const DeepHomogeneousIdentity> identities() const;
  /* T/R/G/B absolute representation bounds, including checked arithmetic allowance. */
  std::span<const double, 4> representation_error() const;
  DeepStatus read_knot(uint64_t index, DeepDepthKnot &knot) const;

 private:
  std::shared_ptr<const DeepHomogeneousCurveStorage> storage_;
  friend DeepStatus deep_build_homogeneous_curve(DeepPageStore &,
                                                 const DeepHomogeneousPixelInput &,
                                                 DeepHomogeneousCurve &,
                                                 const DeepHomogeneousCurvePolicy &);
};

/* All scratch, result/control blocks, and pages use store.memory_budget(). No input is
 * published until complete population, native accounting, support and quota checks pass.
 * Output is cleared on failure. This builder alone does not prove M1 OptiX integration. */
DeepStatus deep_build_homogeneous_curve(DeepPageStore &store,
                                        const DeepHomogeneousPixelInput &input,
                                        DeepHomogeneousCurve &output,
                                        const DeepHomogeneousCurvePolicy &policy = {});

CCL_NAMESPACE_END
