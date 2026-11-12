/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>

#include "session/deep_homogeneous_curve.h"

CCL_NAMESPACE_BEGIN

enum class DeepCanonicalPrecision { Half, Float };

struct DeepCanonicalEncodeConfig {
  /* Reference: zero disables optional cross-knot fitting; adjacency does not
   * replace continuous residual checks. Bounded: Alpha/Depth control mature
   * compression; qualified Surface semantic grouping has its own priority. */
  double depth_tolerance = 0.1;
  double alpha_tolerance = 0.1;
  /* Reference accepts configurable RGB error. Bounded production accepts only
   * its factory values (.006 absolute/.002 relative), checked at route entry. */
  double rgb_absolute_tolerance = 0.006;
  double rgb_relative_tolerance = 0;
  DeepCanonicalPrecision precision = DeepCanonicalPrecision::Half;
  bool alpha_only = false;
  bool operator==(const DeepCanonicalEncodeConfig &) const = default;
};

DeepCanonicalEncodeConfig deep_production_encode_config(double depth,
                                                        double alpha,
                                                        DeepCanonicalPrecision precision,
                                                        bool alpha_only);
DeepStatus deep_validate_canonical_encode_config(const DeepCanonicalEncodeConfig &config);

/* RGBA values are already rounded to the actual selected EXR channel format.
 * Z/ZBack are FLOAT in both modes. The first six members map directly to DeepSample;
 * provenance/flags are not additional EXR channels. No imbuf dependency is required. */
struct DeepCanonicalEncodedSample {
  float r = 0, g = 0, b = 0, a = 0, z = 0, z_back = 0;
  uint64_t source_group = 0;
  uint32_t flags = 0;
};

struct DeepCanonicalEncodeReport {
  /* T/R/G/B absolute bounds. encoding_error includes FLOAT depths, payload rounding
   * and sequential prefix arithmetic; it is against the full original polyline.
   * representation_error is an independently supplied upstream ledger. */
  double representation_error[4] = {};
  double encoding_error[4] = {};
  double total_error[4] = {};
  /* Maximum continuous (upstream + encoding bound) / local allowed error.
   * RGB minima include the upstream reference uncertainty and signed zero crossings. */
  double maximum_budget_ratio[4] = {};
  /* Sum of local ideal-payload versus actual-payload bounds at the quantized prefix.
   * Diagnostic decomposition only: already included in encoding_error. */
  double payload_quantization_bound[4] = {};
  uint64_t input_knots = 0, output_samples = 0, subdivisions = 0;
  uint64_t merge_candidates = 0, merged_intervals = 0;
  bool normalized_checked = false, quantized_checked = false;
  /* GPU production checks its adaptive grid and actual quantized decoder.
   * This is not the CPU continuous-reference certificate above. */
  bool gpu_grid_checked = false;
  bool bounded_payload_checked = false;
};

struct DeepCanonicalEncodedStorage;

/* Immutable output. Copies share charged storage, which can outlive the frame owner. */
class DeepCanonicalEncoded {
 public:
  explicit operator bool() const;
  std::span<const DeepCanonicalEncodedSample> samples() const;
  const DeepCanonicalEncodeReport *report() const;
  bool is_charged_to(const DeepMemoryBudget &budget) const;

 private:
  std::shared_ptr<const DeepCanonicalEncodedStorage> storage_;
  friend DeepStatus deep_encode_canonical_knots(const DeepMemoryBudget &,
                                                std::span<const DeepPageLease>,
                                                uint64_t,
                                                std::span<const double, 4>,
                                                const DeepCanonicalEncodeConfig &,
                                                DeepCanonicalEncoded &);
};

/* Pages must be immutable normalized DepthKnot pages charged to this same budget.
 * Their valid slots, in page/slot order, contain exactly knot_count knots. Every
 * original interval and same-z jump is checked, including analytic interior extrema;
 * no fixed probes, Beauty target, sample cap, or legacy writer fitter is used.
 * All temporary vectors and output/control blocks use budget. On any error output
 * is empty; InsufficientErrorBudget/DepthNotRepresentable/RadianceNotRepresentable
 * appear in the status message. Explicit zero is never raised to a hidden minimum. */
DeepStatus deep_encode_canonical_knots(const DeepMemoryBudget &budget,
                                       std::span<const DeepPageLease> pages,
                                       uint64_t knot_count,
                                       std::span<const double, 4> representation_error,
                                       const DeepCanonicalEncodeConfig &config,
                                       DeepCanonicalEncoded &output);

DeepStatus deep_encode_canonical_curve(const DeepMemoryBudget &budget,
                                       const DeepHomogeneousCurve &curve,
                                       const DeepCanonicalEncodeConfig &config,
                                       DeepCanonicalEncoded &output);

CCL_NAMESPACE_END
