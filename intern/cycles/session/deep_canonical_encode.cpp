/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_canonical_encode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "util/half.h"

CCL_NAMESPACE_BEGIN

struct DeepCanonicalEncodedStorage {
  explicit DeepCanonicalEncodedStorage(const DeepMemoryBudget &budget)
      : budget(budget), samples(DeepBudgetAllocator<DeepCanonicalEncodedSample>(budget))
  {
  }
  DeepMemoryBudget budget;
  std::vector<DeepCanonicalEncodedSample, DeepBudgetAllocator<DeepCanonicalEncodedSample>> samples;
  DeepCanonicalEncodeReport report;
};

DeepCanonicalEncoded::operator bool() const
{
  return bool(storage_);
}
std::span<const DeepCanonicalEncodedSample> DeepCanonicalEncoded::samples() const
{
  return storage_ ? std::span<const DeepCanonicalEncodedSample>(storage_->samples) :
                    std::span<const DeepCanonicalEncodedSample>();
}
const DeepCanonicalEncodeReport *DeepCanonicalEncoded::report() const
{
  return storage_ ? &storage_->report : nullptr;
}
bool DeepCanonicalEncoded::is_charged_to(const DeepMemoryBudget &budget) const
{
  return storage_ && storage_->budget == budget;
}

namespace {
using Value = std::array<long double, 4>;
using Knots = std::vector<DeepDepthKnot, DeepBudgetAllocator<DeepDepthKnot>>;
constexpr long double arithmetic_unit = std::numeric_limits<long double>::epsilon();
constexpr uint64_t format_sample_limit = std::numeric_limits<uint32_t>::max();

DeepStatus failure(const char *name, const char *detail)
{
  return DeepStatus::fail(DeepErrorCode::InvalidPayload, std::string(name) + ": " + detail);
}

double outward(const long double value)
{
  if (!std::isfinite(value) || value > std::numeric_limits<double>::max())
    return std::numeric_limits<double>::infinity();
  return std::nextafter(double(value), std::numeric_limits<double>::infinity());
}

Value value(const double (&input)[4])
{
  return {input[0], input[1], input[2], input[3]};
}

struct Prefix {
  Value value = {1, 0, 0, 0}, error = {};
};

bool advance(Prefix &prefix, const DeepCanonicalEncodedSample &sample, const bool alpha_only)
{
  const long double transmission = prefix.value[0];
  const long double alpha = sample.a;
  if (!alpha_only) {
    const long double rgb[3] = {sample.r, sample.g, sample.b};
    for (int c = 1; c < 4; c++) {
      const long double term = transmission * rgb[c - 1];
      prefix.error[c] += prefix.error[0] * std::abs(rgb[c - 1]) +
                         16 * arithmetic_unit * (std::abs(prefix.value[c]) + std::abs(term));
      prefix.value[c] += term;
    }
  }
  prefix.error[0] = prefix.error[0] * (1 - alpha) + 16 * arithmetic_unit * std::abs(transmission);
  prefix.value[0] = transmission * (1 - alpha);
  for (int c = 0; c < (alpha_only ? 1 : 4); c++)
    if (!std::isfinite(prefix.value[c]) || !std::isfinite(prefix.error[c]))
      return false;
  return true;
}

/* Continuous scalar EXR split, in the sample's FLOAT depth coordinates. Alpha=1
 * intervals are never emitted: that decoder is discontinuous immediately after Z. */
struct Shape {
  long double base = 0, amplitude = 0, alpha = 0, z = 0, width = 1;
  long double fraction(const long double depth) const
  {
    if (depth <= z)
      return 0;
    if (depth >= z + width)
      return 1;
    const long double f = (depth - z) / width;
    return alpha == 0 ? f : -std::expm1(std::log1p(-alpha) * f) / alpha;
  }
  long double at(const long double depth) const
  {
    return base + amplitude * fraction(depth);
  }
};

Shape shape(const Prefix &prefix, const DeepCanonicalEncodedSample &sample, const int c)
{
  const float rgb[3] = {sample.r, sample.g, sample.b};
  return {prefix.value[c],
          c == 0 ? -prefix.value[0] * sample.a : prefix.value[0] * rgb[c - 1],
          sample.a,
          sample.z,
          static_cast<long double>(sample.z_back) - sample.z};
}

/* A line minus an exponential has at most one interior stationary point. The
 * endpoint-range enclosure is the conservative fallback when the analytic root
 * cannot be evaluated stably. This checks a continuous interval, not probes. */
long double line_shape_bound(const Shape &encoded,
                             const long double a,
                             const long double b,
                             const long double reference_a,
                             const long double reference_b)
{
  const long double ya = encoded.at(a), yb = encoded.at(b);
  const long double endpoint_error = std::max(std::abs(ya - reference_a),
                                              std::abs(yb - reference_b));
  long double result = endpoint_error;
  const long double slope = (reference_b - reference_a) / (b - a);
  const long double k = std::log1p(-encoded.alpha);
  if (encoded.alpha > 0 && encoded.amplitude != 0 && a >= encoded.z &&
      b <= encoded.z + encoded.width)
  {
    const long double derivative_front = encoded.amplitude * (-k / encoded.alpha) / encoded.width;
    const long double ratio = slope / derivative_front;
    if (!std::isfinite(derivative_front) || derivative_front == 0) {
      result = std::max(std::abs(std::min(ya, yb) - std::max(reference_a, reference_b)),
                        std::abs(std::max(ya, yb) - std::min(reference_a, reference_b)));
    }
    else if (ratio > 0 && std::isfinite(ratio)) {
      const long double root = encoded.z + encoded.width * std::log(ratio) / k;
      if (std::isfinite(root) && root > a && root < b) {
        const long double reference = reference_a + slope * (root - a);
        result = std::max(result, std::abs(encoded.at(root) - reference));
      }
      else if (!std::isfinite(root)) {
        result = std::max(std::abs(std::min(ya, yb) - std::max(reference_a, reference_b)),
                          std::abs(std::max(ya, yb) - std::min(reference_a, reference_b)));
      }
    }
    else if (!std::isfinite(ratio)) {
      result = std::max(std::abs(std::min(ya, yb) - std::max(reference_a, reference_b)),
                        std::abs(std::max(ya, yb) - std::min(reference_a, reference_b)));
    }
  }
  if (encoded.alpha > 0 && encoded.amplitude != 0 && a >= encoded.z &&
      b <= encoded.z + encoded.width)
  {
    /* An independent enclosure certifies the analytic extremum calculation.
     * For e=decoder-line, max|e| <= max(endpoint errors)+max|e''|*h*h/8.
     * Express h/width locally to avoid overflowing inverse thin depths.
     * Monotone endpoint ranges supply a second rigorous enclosure. We never
     * accept solely because a numerically computed root missed an extremum. */
    const long double local_width = (b - a) / encoded.width;
    const long double curvature = std::abs(encoded.amplitude) * (-k / encoded.alpha) * (-k) *
                                  std::exp(k * ((a - encoded.z) / encoded.width)) * local_width *
                                  local_width / 8;
    const long double range = std::max(
        std::abs(std::min(ya, yb) - std::max(reference_a, reference_b)),
        std::abs(std::max(ya, yb) - std::min(reference_a, reference_b)));
    result = std::max(result, std::min(range, endpoint_error + curvature));
  }
  /* Includes root/log/expm1/enclosure and local-coordinate arithmetic. No
   * world-Z slope*Z+intercept subtraction is used. */
  return result + 256 * arithmetic_unit *
                      (1 + std::abs(reference_a) + std::abs(reference_b) + std::abs(encoded.base) +
                       std::abs(encoded.amplitude));
}

long double payload_bound(const long double amplitude,
                          const long double alpha,
                          const long double quantized_amplitude,
                          const long double quantized_alpha)
{
  /* Both shapes use the same depth and quantized prefix. Compare two
   * exponentials (or an exponential and a line), including their analytic root. */
  const auto g = [](const long double a, const long double f) {
    return a == 0 ? f : -std::expm1(std::log1p(-a) * f) / a;
  };
  long double result = std::abs(amplitude - quantized_amplitude);
  const long double k = std::log1p(-alpha), qk = std::log1p(-quantized_alpha);
  const long double d = amplitude * (alpha == 0 ? 1 : -k / alpha);
  const long double qd = quantized_amplitude * (quantized_alpha == 0 ? 1 : -qk / quantized_alpha);
  if (k != qk && d != 0 && qd / d > 0) {
    const long double root = std::log(qd / d) / (k - qk);
    if (root > 0 && root < 1 && std::isfinite(root))
      result = std::max(
          result,
          std::abs(amplitude * g(alpha, root) - quantized_amplitude * g(quantized_alpha, root)));
    else if (!std::isfinite(root))
      result = std::abs(amplitude) + std::abs(quantized_amplitude);
  }
  return result +
         128 * arithmetic_unit * (1 + std::abs(amplitude) + std::abs(quantized_amplitude));
}

float quantize(const long double input, const DeepCanonicalPrecision precision)
{
  const float f = float(input);
  return precision == DeepCanonicalPrecision::Half ? half_to_float(float_to_half(f)) : f;
}

float largest_nonopaque(const DeepCanonicalPrecision precision)
{
  return precision == DeepCanonicalPrecision::Half ? half_to_float(half(uint16_t(0x3bff))) :
                                                     std::nextafter(1.0f, 0.0f);
}

struct Work {
  double a, b;
  Value left, right;
  uint64_t source;
  uint32_t flags;
};

class Encoder {
 public:
  Encoder(const DeepMemoryBudget &budget,
          const DeepCanonicalEncodeConfig &config,
          DeepCanonicalEncodedStorage &output)
      : config(config), output(output), work(DeepBudgetAllocator<Work>(budget))
  {
  }

  DeepStatus run(const Knots &knots)
  {
    for (size_t i = 0; i < knots.size();) {
      const DeepDepthKnot &knot = knots[i];
      if (const auto status = point(knot); !status)
        return with_source(status, knot.source_group, knot.z, knot.z);
      if (i + 1 == knots.size())
        break;
      if (config.depth_tolerance > 0 && i + 2 < knots.size()) {
        size_t best_end = i, end = i + 2;
        DeepCanonicalEncodedSample best_sample;
        Value best_ideal = {};
        while (true) {
          DeepCanonicalEncodedSample sample;
          Value ideal = {};
          output.report.merge_candidates++;
          if (!joined_interval(knots, i, end, sample, ideal))
            break;
          best_end = end;
          best_sample = sample;
          best_ideal = ideal;
          if (end == knots.size() - 1)
            break;
          const size_t length = end - i;
          end = length > (knots.size() - 1 - i) / 2 ? knots.size() - 1 : i + 2 * length;
        }
        if (best_end > i) {
          if (best_sample.a != 0 || best_sample.r != 0 || best_sample.g != 0 || best_sample.b != 0)
          {
            record_quantization(best_ideal, best_sample, false);
            if (const auto status = append(best_sample); !status)
              return with_source(
                  status, best_sample.source_group, best_sample.z, best_sample.z_back);
          }
          output.report.merged_intervals += best_end - i - 1;
          i = best_end;
          continue;
        }
      }
      work.push_back({knot.z,
                      knots[i + 1].z,
                      value(knot.after),
                      value(knots[i + 1].before),
                      knot.source_group,
                      knot.flags | knots[i + 1].flags});
      while (!work.empty()) {
        const Work task = work.back();
        work.pop_back();
        if (const auto status = interval(task); !status)
          return with_source(status, task.source, task.a, task.b);
      }
      i++;
    }
    return check_full_curve(knots);
  }

 private:
  const DeepCanonicalEncodeConfig &config;
  DeepCanonicalEncodedStorage &output;
  std::vector<Work, DeepBudgetAllocator<Work>> work;
  Prefix prefix;

  DeepStatus with_source(DeepStatus status,
                         const uint64_t source,
                         const double a,
                         const double b) const
  {
    status.message += " [source_group=" + std::to_string(source) + ", depth=" + std::to_string(a) +
                      ".." + std::to_string(b) + ", precision=" +
                      (config.precision == DeepCanonicalPrecision::Half ? "HALF" : "FLOAT") + "]";
    return status;
  }

  DeepStatus append(const DeepCanonicalEncodedSample &sample)
  {
    if (output.samples.size() >= format_sample_limit)
      return DeepStatus::fail(DeepErrorCode::InvalidCount, "Deep EXR sample count exceeds UINT32");
    if (!advance(prefix, sample, config.alpha_only))
      return failure("RadianceNotRepresentable", "nonfinite quantized decoded prefix");
    output.samples.push_back(sample);
    return {};
  }

  long double allowed_error(const int c, const long double a, const long double b) const
  {
    if (c == 0)
      return config.alpha_tolerance;
    const bool crosses_zero = (a <= 0 && b >= 0) || (a >= 0 && b <= 0);
    const long double minimum_abs = crosses_zero ? 0 : std::min(std::abs(a), std::abs(b));
    /* The original reference can be closer to zero than its canonical chord by
     * the upstream absolute ledger. Spend that uncertainty exactly once. */
    const long double minimum_reference = std::max(
        0.0L, minimum_abs - output.report.representation_error[c]);
    return config.rgb_absolute_tolerance +
           config.rgb_relative_tolerance * std::max(1.0L, minimum_reference);
  }

  bool acceptable(const Value &bound, const Value &a, const Value &b) const
  {
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++)
      if (!std::isfinite(bound[c]) ||
          outward(bound[c] + output.report.representation_error[c]) > allowed_error(c, a[c], b[c]))
        return false;
    return true;
  }

  void record_budget_ratio(const int c,
                           const long double bound,
                           const long double a,
                           const long double b)
  {
    const long double total = bound + output.report.representation_error[c];
    const long double allowed = allowed_error(c, a, b);
    const double ratio = allowed > 0 ? outward(total / allowed) :
                         total == 0  ? 0 :
                                       std::numeric_limits<double>::infinity();
    output.report.maximum_budget_ratio[c] = std::max(output.report.maximum_budget_ratio[c], ratio);
  }

  bool joined_interval(const Knots &knots,
                       const size_t first,
                       const size_t last,
                       DeepCanonicalEncodedSample &sample,
                       Value &ideal) const
  {
    for (size_t i = first + 1; i < last; i++) {
      if (knots[i].flags & DEEP_KNOT_PROTECTED_CUT)
        return false;
      for (int c = 0; c < 4; c++)
        if (knots[i].before[c] != knots[i].after[c])
          return false;
    }
    const float z = float(knots[first].z), z_back = float(knots[last].z);
    if (!std::isfinite(z) || !std::isfinite(z_back) || z_back <= z ||
        double(z) != knots[first].z || double(z_back) != knots[last].z)
      return false;
    Value target = value(knots[last].before);
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
      bool constant = true;
      for (size_t i = first; i < last; i++)
        constant &= knots[i].after[c] == knots[first].after[c] &&
                    knots[i + 1].before[c] == knots[first].after[c];
      if (constant)
        target[c] = prefix.value[c];
    }
    if (!payload(target, z, z_back, sample, ideal))
      return false;
    const long double rgb[3] = {sample.r, sample.g, sample.b};
    /* Compare against every original linear slab, including continuous interior
     * extrema. A matching final endpoint cannot hide an intermediate signed peak. */
    for (size_t i = first; i < last; i++) {
      Value bound = {};
      for (int c = 0; c < (config.alpha_only ? 1 : 4); c++)
        bound[c] = line_shape_bound(shape(prefix, sample, c),
                                    knots[i].z,
                                    knots[i + 1].z,
                                    knots[i].after[c],
                                    knots[i + 1].before[c]) +
                   prefix.error[c] + prefix.error[0] * (c == 0 ? 0 : std::abs(rgb[c - 1]));
      if (!acceptable(bound, value(knots[i].after), value(knots[i + 1].before)))
        return false;
    }
    sample.source_group = knots[first].source_group;
    sample.flags = knots[first].flags | knots[last].flags;
    return true;
  }

  void record_quantization(const Value &ideal,
                           const DeepCanonicalEncodedSample &sample,
                           const bool is_point)
  {
    const long double rgb[3] = {sample.r, sample.g, sample.b};
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
      const long double amplitude = c == 0 ? -prefix.value[0] * ideal[0] :
                                             prefix.value[0] * ideal[c];
      const long double qa = c == 0 ? -prefix.value[0] * sample.a : prefix.value[0] * rgb[c - 1];
      const long double error = is_point ? std::abs(amplitude - qa) :
                                           payload_bound(amplitude, ideal[0], qa, sample.a);
      output.report.payload_quantization_bound[c] = outward(
          static_cast<long double>(output.report.payload_quantization_bound[c]) + error);
    }
  }

  bool payload(const Value &target,
               const float z,
               const float z_back,
               DeepCanonicalEncodedSample &sample,
               Value &ideal) const
  {
    sample.z = z;
    sample.z_back = z_back;
    if (prefix.value[0] == 0) {
      ideal = {};
      for (int c = 1; c < (config.alpha_only ? 1 : 4); c++)
        if (target[c] != prefix.value[c])
          return false;
    }
    else {
      ideal[0] = std::clamp(1 - target[0] / prefix.value[0], 0.0L, 1.0L);
      for (int c = 1; c < (config.alpha_only ? 1 : 4); c++)
        ideal[c] = (target[c] - prefix.value[c]) / prefix.value[0];
    }
    sample.a = quantize(ideal[0], config.precision);
    if (sample.a == 1 && (z_back > z || target[0] > 0)) {
      sample.a = largest_nonopaque(config.precision);
      /* The new nonopaque proposal is the value actually checked and emitted. */
      ideal[0] = sample.a;
    }
    float *rgb[3] = {&sample.r, &sample.g, &sample.b};
    for (int c = 1; c < (config.alpha_only ? 1 : 4); c++) {
      const long double maximum = config.precision == DeepCanonicalPrecision::Half ?
                                      65504.0L :
                                      std::numeric_limits<float>::max();
      if (!std::isfinite(ideal[c]) || std::abs(ideal[c]) > maximum)
        return false;
      *rgb[c - 1] = quantize(ideal[c], config.precision);
    }
    return std::isfinite(sample.a) && std::isfinite(sample.r) && std::isfinite(sample.g) &&
           std::isfinite(sample.b);
  }

  DeepStatus point(const DeepDepthKnot &knot)
  {
    bool jump = knot.before[0] != knot.after[0];
    if (!config.alpha_only)
      for (int c = 1; c < 4; c++)
        jump |= knot.before[c] != knot.after[c];
    if (!jump)
      return {};
    const float z = float(knot.z);
    if (!std::isfinite(z) || double(z) != knot.z)
      return failure("DepthNotRepresentable", "protected point depth is not exact FLOAT");
    Value target = value(knot.after);
    /* A rounding difference from an earlier support remains an encoding error.
     * Do not manufacture RGB or opacity at an unrelated zero-increment point. */
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++)
      if (knot.before[c] == knot.after[c])
        target[c] = prefix.value[c];
    if (!config.alpha_only && prefix.value[0] == 0)
      for (int c = 1; c < 4; c++)
        if (target[c] != prefix.value[c])
          return failure("RadianceNotRepresentable", "point RGB follows zero scalar transmission");
    /* HDR point pieces remain at the same protected depth. Emission is split
     * before its optical jump; no interval is ever converted into a point. */
    if (!config.alpha_only && prefix.value[0] > 0) {
      const long double maximum = config.precision == DeepCanonicalPrecision::Half ?
                                      65504.0L :
                                      std::numeric_limits<float>::max();
      while (true) {
        Value local = {};
        bool needs_split = false;
        for (int c = 1; c < 4; c++) {
          local[c] = (target[c] - prefix.value[c]) / prefix.value[0];
          if (!std::isfinite(local[c]))
            return failure("RadianceNotRepresentable", "point payload ratio is nonfinite");
          needs_split |= std::abs(local[c]) > maximum;
        }
        if (!needs_split)
          break;
        const long double needed =
            std::max({std::abs(local[1]), std::abs(local[2]), std::abs(local[3])}) / maximum;
        if (needed > format_sample_limit - output.samples.size())
          return failure("RadianceNotRepresentable", "HDR point requires too many format samples");
        DeepCanonicalEncodedSample sample;
        sample.z = sample.z_back = z;
        sample.flags = knot.flags | DEEP_KNOT_POINT_RADIANCE;
        sample.source_group = knot.source_group;
        sample.r = quantize(std::clamp(local[1], -maximum, maximum), config.precision);
        sample.g = quantize(std::clamp(local[2], -maximum, maximum), config.precision);
        sample.b = quantize(std::clamp(local[3], -maximum, maximum), config.precision);
        const Value previous = prefix.value;
        for (int c = 1; c < 4; c++)
          local[c] = std::clamp(local[c], -maximum, maximum);
        record_quantization(local, sample, true);
        if (const auto status = append(sample); !status)
          return status;
        if (previous == prefix.value)
          return failure("RadianceNotRepresentable",
                         "HDR point split makes no arithmetic progress");
      }
    }
    DeepCanonicalEncodedSample sample;
    Value ideal = {};
    if (!payload(target, z, z, sample, ideal))
      return failure("RadianceNotRepresentable", "point cannot be encoded in selected precision");
    sample.flags = knot.flags;
    sample.source_group = knot.source_group;
    /* A quantized opaque point can only precede identical RGB. All future RGB
     * changes are checked again against a zero prefix, never hidden or recoloured. */
    Prefix next = prefix;
    if (!advance(next, sample, config.alpha_only))
      return failure("RadianceNotRepresentable", "point decoded prefix is nonfinite");
    Value bound = {};
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++)
      bound[c] = std::abs(next.value[c] - target[c]) + next.error[c];
    if (!acceptable(bound, value(knot.after), value(knot.after)))
      return failure("RadianceNotRepresentable",
                     "point payload quantization exceeds prefix budget");
    record_quantization(ideal, sample, true);
    return append(sample);
  }

  Value interval_bound(const Work &task, const DeepCanonicalEncodedSample &sample) const
  {
    const long double left = std::min(static_cast<long double>(task.a),
                                      static_cast<long double>(sample.z));
    const long double right = std::max(static_cast<long double>(task.b),
                                       static_cast<long double>(sample.z_back));
    std::array<long double, 6> cuts = {left, task.a, task.b, sample.z, sample.z_back, right};
    std::sort(cuts.begin(), cuts.end());
    Value result = {};
    for (size_t i = 1; i < cuts.size(); i++) {
      if (cuts[i] <= cuts[i - 1])
        continue;
      const long double a = cuts[i - 1], b = cuts[i];
      for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
        const auto reference = [&](const long double z) {
          const long double f = std::clamp((z - task.a) / (task.b - task.a), 0.0L, 1.0L);
          return task.left[c] + (task.right[c] - task.left[c]) * f;
        };
        Shape encoded = shape(prefix, sample, c);
        if (b <= sample.z || a >= sample.z_back) {
          encoded.base = encoded.at(a);
          encoded.amplitude = 0;
          encoded.alpha = 0;
        }
        const long double amplification = c == 0 ? 1 :
                                                   std::abs(c == 1 ? sample.r :
                                                            c == 2 ? sample.g :
                                                                     sample.b);
        result[c] = std::max(result[c],
                             line_shape_bound(encoded, a, b, reference(a), reference(b)) +
                                 prefix.error[c] + prefix.error[0] * amplification);
      }
    }
    return result;
  }

  DeepStatus interval(const Work &task)
  {
    Value target = task.right;
    bool constant = true;
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
      if (task.left[c] == task.right[c])
        target[c] = prefix.value[c];
      else
        constant = false;
    }
    /* In particular, a vacuum tail must not absorb a previous point's HALF
     * residual. The independent full-curve sweep still checks that carried error. */
    if (constant)
      return {};
    const float z = float(task.a), z_back = float(task.b);
    if (!std::isfinite(z) || !std::isfinite(z_back) || z_back <= z)
      return failure("DepthNotRepresentable", "positive interval collapses in FLOAT Z/ZBack");
    if (!config.alpha_only && prefix.value[0] == 0)
      for (int c = 1; c < 4; c++)
        if (task.right[c] != prefix.value[c] || task.left[c] != task.right[c])
          return failure("RadianceNotRepresentable",
                         "interval RGB follows zero scalar transmission");
    DeepCanonicalEncodedSample sample;
    Value ideal = {};
    const bool representable = payload(target, z, z_back, sample, ideal);
    if (representable && acceptable(interval_bound(task, sample), task.left, task.right)) {
      sample.flags = task.flags;
      sample.source_group = task.source;
      record_quantization(ideal, sample, false);
      return append(sample);
    }
    /* Choose a FLOAT-representable local cut. At adjacent FLOAT depths no
     * further legal interval exists; converting to a Surface is forbidden. */
    const double middle = double(float(static_cast<long double>(task.a) +
                                       (static_cast<long double>(task.b) - task.a) * 0.5L));
    if (!(middle > task.a && middle < task.b) || !(float(middle) > z && float(middle) < z_back))
      return failure(representable ? "DepthNotRepresentable" : "RadianceNotRepresentable",
                     "no finer FLOAT interval can meet the selected consumer budget");
    Value midpoint = {};
    const long double fraction = (static_cast<long double>(middle) - task.a) / (task.b - task.a);
    for (int c = 0; c < 4; c++)
      midpoint[c] = task.left[c] + (task.right[c] - task.left[c]) * fraction;
    work.push_back({middle, task.b, midpoint, task.right, task.source, task.flags});
    work.push_back({task.a, middle, task.left, midpoint, task.source, task.flags});
    output.report.subdivisions++;
    return {};
  }

  DeepStatus check_full_curve(const Knots &knots)
  {
    if (knots.empty())
      return {};
    Prefix decoded;
    size_t k = 0, s = 0;
    Value reference = {1, 0, 0, 0}, slope = {};
    long double depth = std::min(static_cast<long double>(knots.front().z),
                                 output.samples.empty() ?
                                     static_cast<long double>(knots.front().z) :
                                     static_cast<long double>(output.samples.front().z));
    const auto checkpoint = [&](const Value &actual, const Value &expected) {
      for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
        const long double bound = std::abs(actual[c] - expected[c]) + decoded.error[c];
        output.report.encoding_error[c] = std::max(
            output.report.encoding_error[c],
            outward(bound));
        record_budget_ratio(c, bound, expected[c], expected[c]);
      }
    };
    while (true) {
      /* Interval ends are continuous; grouped same-z point samples are checked
       * only at their combined left and right limits, matching canonical knots. */
      while (s < output.samples.size() && output.samples[s].z_back > output.samples[s].z &&
             output.samples[s].z_back == depth)
      {
        if (!advance(decoded, output.samples[s++], config.alpha_only))
          return failure("RadianceNotRepresentable", "nonfinite full decoder sweep");
      }
      if (k < knots.size() && knots[k].z == depth)
        reference = value(knots[k].before);
      const auto current_value = [&]() {
        Value current = decoded.value;
        if (s < output.samples.size() && output.samples[s].z < depth &&
            output.samples[s].z_back > depth)
          for (int c = 0; c < (config.alpha_only ? 1 : 4); c++)
            current[c] = shape(decoded, output.samples[s], c).at(depth);
        return current;
      };
      checkpoint(current_value(), reference);
      if (k < knots.size() && knots[k].z == depth) {
        reference = value(knots[k].after);
        slope = value(knots[k].slope_right);
        k++;
      }
      while (s < output.samples.size() && output.samples[s].z == depth &&
             output.samples[s].z_back == depth)
      {
        if (!advance(decoded, output.samples[s++], config.alpha_only))
          return failure("RadianceNotRepresentable", "nonfinite full point decoder sweep");
      }
      checkpoint(current_value(), reference);
      long double next = std::numeric_limits<long double>::infinity();
      if (k < knots.size())
        next = knots[k].z;
      const DeepCanonicalEncodedSample *active = nullptr;
      if (s < output.samples.size()) {
        const auto &sample = output.samples[s];
        if (sample.z > depth)
          next = std::min(next, static_cast<long double>(sample.z));
        else if (sample.z_back > depth) {
          active = &sample;
          next = std::min(next, static_cast<long double>(sample.z_back));
        }
        else
          return failure("DepthNotRepresentable",
                         "encoded events are not ordered and nonoverlapping");
      }
      if (!std::isfinite(next))
        break;
      if (next <= depth)
        return failure("DepthNotRepresentable", "full decoder sweep makes no depth progress");
      Value end = {};
      for (int c = 0; c < 4; c++)
        end[c] = reference[c] + slope[c] * (next - depth);
      for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
        const Shape encoded = active ? shape(decoded, *active, c) :
                                       Shape{decoded.value[c], 0, 0, depth, next - depth};
        const long double amplification = active && c != 0 ? std::abs(c == 1 ? active->r :
                                                                      c == 2 ? active->g :
                                                                               active->b) :
                                                             0;
        const long double bound = line_shape_bound(encoded, depth, next, reference[c], end[c]) +
                                  decoded.error[c] + decoded.error[0] * amplification;
        output.report.encoding_error[c] = std::max(output.report.encoding_error[c],
                                                   outward(bound));
        record_budget_ratio(c, bound, reference[c], end[c]);
      }
      reference = end;
      depth = next;
    }
    for (int c = 0; c < (config.alpha_only ? 1 : 4); c++) {
      output.report.total_error[c] = outward(
          static_cast<long double>(output.report.encoding_error[c]) +
          output.report.representation_error[c]);
      if (!std::isfinite(output.report.total_error[c]) ||
          output.report.maximum_budget_ratio[c] > 1)
        return failure("DepthNotRepresentable",
                       "complete quantized decoder bound exceeds consumer budget");
    }
    return {};
  }
};

DeepStatus read_checked_knots(const DeepMemoryBudget &budget,
                              const std::span<const DeepPageLease> pages,
                              const uint64_t knot_count,
                              Knots &knots)
{
  if (knot_count > std::numeric_limits<size_t>::max() / sizeof(DeepDepthKnot))
    return DeepStatus::fail(DeepErrorCode::InvalidCount,
                            "Canonical knot count overflows scratch bytes");
  knots.reserve(size_t(knot_count));
  const DeepFrameKey *frame = nullptr;
  for (const DeepPageLease &page : pages) {
    const DeepPageDescriptor *descriptor = page.descriptor();
    if (!descriptor || descriptor->kind != DeepRecordKind::DepthKnot ||
        !page.is_charged_to(budget) || !page.frame_key())
      return DeepStatus::fail(DeepErrorCode::InvalidLayout,
                              "Encoder requires owned DepthKnot pages in its shared budget");
    if (frame && !(*frame == *page.frame_key()))
      return DeepStatus::fail(DeepErrorCode::InvalidFrame, "Canonical pages mix FrameKeys");
    frame = page.frame_key();
    if (descriptor->valid > knot_count - knots.size())
      return DeepStatus::fail(DeepErrorCode::InvalidCount,
                              "Canonical page valid count exceeds authoritative knot count");
    uint64_t read_count = 0;
    for (uint32_t slot = 0; slot < descriptor->capacity; slot++) {
      const uint64_t handle = page.valid_handle_at(slot);
      if (!handle)
        continue;
      DeepRecordTag tag;
      std::span<const uint8_t> payload;
      if (const auto status = page.read(handle, tag, payload); !status)
        return status;
      if (payload.size() != sizeof(DeepDepthKnot) ||
          tag.kind != uint32_t(DeepRecordKind::DepthKnot))
        return DeepStatus::fail(DeepErrorCode::InvalidLayout,
                                "Canonical payload/tag layout mismatch");
      DeepDepthKnot knot;
      std::memcpy(&knot, payload.data(), sizeof(knot));
      if (knots.size() == knot_count)
        return DeepStatus::fail(DeepErrorCode::InvalidCount,
                                "Canonical slot count exceeds authoritative count");
      knots.push_back(knot);
      read_count++;
    }
    if (read_count != descriptor->valid)
      return DeepStatus::fail(DeepErrorCode::InvalidCount,
                              "Canonical page valid slots disagree with descriptor");
  }
  if (knots.size() != knot_count)
    return DeepStatus::fail(DeepErrorCode::InvalidCount, "Canonical knot count is incomplete");
  for (size_t i = 0; i < knots.size(); i++) {
    const auto &knot = knots[i];
    if (!std::isfinite(knot.z) || knot.z < 0 || (i > 0 && knot.z <= knots[i - 1].z))
      return failure("InvalidNormalizedCurve",
                     "knot depths must be finite, nonnegative and strictly ordered");
    for (int c = 0; c < 4; c++) {
      if (!std::isfinite(knot.before[c]) || !std::isfinite(knot.after[c]) ||
          !std::isfinite(knot.slope_right[c]))
        return failure("InvalidNormalizedCurve", "nonfinite T/R/G/B or slope");
      if (i == 0 && knot.before[c] != (c == 0 ? 1 : 0))
        return failure("InvalidNormalizedCurve",
                       "initial prefix is not normalized transparent black");
      if (i + 1 == knots.size() && knot.slope_right[c] != 0)
        return failure("InvalidNormalizedCurve", "last knot leaves an unbounded slope");
      if (i > 0) {
        const auto &previous = knots[i - 1];
        const long double predicted = static_cast<long double>(previous.after[c]) +
                                      static_cast<long double>(previous.slope_right[c]) *
                                          (static_cast<long double>(knot.z) - previous.z);
        const long double allowance = 128 * std::numeric_limits<double>::epsilon() *
                                      (1 + std::abs(predicted) + std::abs(knot.before[c]) +
                                       std::abs(previous.after[c]));
        if (std::abs(predicted - knot.before[c]) > allowance)
          return failure("InvalidNormalizedCurve",
                         "right slope does not reach the next left limit");
      }
    }
    if (knot.before[0] < 0 || knot.before[0] > 1 || knot.after[0] < 0 ||
        knot.after[0] > knot.before[0] || knot.slope_right[0] > 0 ||
        (i > 0 && knot.before[0] > knots[i - 1].after[0]))
    {
      char detail[512];
      std::snprintf(detail,
                    sizeof(detail),
                    "transmission is outside normalized monotone range "
                    "(knot=%llu z=%.17g before=%.17g after=%.17g slope=%.17g "
                    "previous_z=%.17g previous_after=%.17g previous_slope=%.17g)",
                    static_cast<unsigned long long>(i),
                    knot.z,
                    knot.before[0],
                    knot.after[0],
                    knot.slope_right[0],
                    i ? knots[i - 1].z : 0,
                    i ? knots[i - 1].after[0] : 1,
                    i ? knots[i - 1].slope_right[0] : 0);
      return failure("InvalidNormalizedCurve", detail);
    }
  }
  return {};
}
}  // namespace

DeepCanonicalEncodeConfig deep_production_encode_config(const double depth,
                                                        const double alpha,
                                                        const DeepCanonicalPrecision precision,
                                                        const bool alpha_only)
{
  DeepCanonicalEncodeConfig config;
  config.depth_tolerance = depth;
  config.alpha_tolerance = alpha;
  config.precision = precision;
  config.alpha_only = alpha_only;
  config.rgb_relative_tolerance = .002;
  return config;
}

DeepStatus deep_validate_canonical_encode_config(const DeepCanonicalEncodeConfig &config)
{
  if (!std::isfinite(config.depth_tolerance) || config.depth_tolerance < 0 ||
      !std::isfinite(config.alpha_tolerance) || config.alpha_tolerance < 0 ||
      !std::isfinite(config.rgb_absolute_tolerance) || config.rgb_absolute_tolerance < 0 ||
      !std::isfinite(config.rgb_relative_tolerance) || config.rgb_relative_tolerance < 0 ||
      (config.precision != DeepCanonicalPrecision::Half &&
       config.precision != DeepCanonicalPrecision::Float))
    return failure("InvalidConsumerConfig", "tolerances/precision must be finite and valid");
  return {};
}

DeepStatus deep_encode_canonical_knots(const DeepMemoryBudget &budget,
                                       const std::span<const DeepPageLease> pages,
                                       const uint64_t knot_count,
                                       const std::span<const double, 4> representation_error,
                                       const DeepCanonicalEncodeConfig &config,
                                       DeepCanonicalEncoded &output)
{
  output = {};
  if (const auto status = deep_validate_canonical_encode_config(config); !status)
    return status;
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(representation_error[c]) || representation_error[c] < 0)
      return failure("InvalidRepresentationLedger",
                     "upstream error must be finite and nonnegative");
    if ((c == 0 || !config.alpha_only) &&
        representation_error[c] >
            (c == 0 ? config.alpha_tolerance :
                      config.rgb_absolute_tolerance + config.rgb_relative_tolerance))
      return failure("InsufficientErrorBudget",
                     "upstream representation already exceeds consumer budget");
  }
  try {
    auto storage = std::allocate_shared<DeepCanonicalEncodedStorage>(
        DeepBudgetAllocator<DeepCanonicalEncodedStorage>(budget), budget);
    storage->report.input_knots = knot_count;
    std::copy(representation_error.begin(),
              representation_error.end(),
              storage->report.representation_error);
    Knots knots{DeepBudgetAllocator<DeepDepthKnot>(budget)};
    if (const auto status = read_checked_knots(budget, pages, knot_count, knots); !status)
      return status;
    storage->report.normalized_checked = true;
    Encoder encoder(budget, config, *storage);
    if (const auto status = encoder.run(knots); !status)
      return status;
    storage->report.output_samples = storage->samples.size();
    storage->report.quantized_checked = true;
    for (int c = 0; c < 4; c++)
      if (storage->report.total_error[c] == 0)
        storage->report.total_error[c] = storage->report.representation_error[c];
    output.storage_ = std::move(storage);
    return {};
  }
  catch (const std::bad_alloc &) {
    return DeepStatus::fail(DeepErrorCode::BudgetExceeded,
                            "DeepCanonicalEncode scratch/output exceeds shared byte quota [used=" +
                                std::to_string(budget.used()) +
                                ", quota=" + std::to_string(budget.quota()) + "]");
  }
  catch (const std::length_error &) {
    return DeepStatus::fail(
        DeepErrorCode::InvalidCount,
        "DeepCanonicalEncode checked allocation count exceeds container limits");
  }
}

DeepStatus deep_encode_canonical_curve(const DeepMemoryBudget &budget,
                                       const DeepHomogeneousCurve &curve,
                                       const DeepCanonicalEncodeConfig &config,
                                       DeepCanonicalEncoded &output)
{
  output = {};
  if (!curve)
    return DeepStatus::fail(DeepErrorCode::Incomplete,
                            "DeepCanonicalEncode requires a completed homogeneous curve");
  return deep_encode_canonical_knots(
      budget, curve.pages(), curve.knot_count(), curve.representation_error(), config, output);
}

CCL_NAMESPACE_END
