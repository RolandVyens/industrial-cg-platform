/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_emission_expression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

CCL_NAMESPACE_BEGIN
template<typename T> using EmissionVector = std::vector<T, DeepBudgetAllocator<T>>;

struct DeepEmissionExpressionStorage {
  explicit DeepEmissionExpressionStorage(const DeepMemoryBudget &budget)
      : sources(DeepBudgetAllocator<DeepEmissionSource>(budget)),
        nodes(DeepBudgetAllocator<DeepEmissionNode>(budget)),
        calls(DeepBudgetAllocator<DeepEmissionFilmCall>(budget)),
        radiance(DeepBudgetAllocator<DeepCurveRadiance>(budget)),
        accounting(DeepBudgetAllocator<DeepEmissionAccounting>(budget))
  {
  }
  EmissionVector<DeepEmissionSource> sources;
  EmissionVector<DeepEmissionNode> nodes;
  EmissionVector<DeepEmissionFilmCall> calls;
  EmissionVector<DeepCurveRadiance> radiance;
  EmissionVector<DeepEmissionAccounting> accounting;
};

namespace {
DeepStatus fail(const char *message)
{
  return DeepStatus::fail(DeepErrorCode::InvalidPayload, message);
}
struct Weights {
  double value[3] = {}, absolute[3] = {};
};
bool add_weight(Weights &out, const Weights &in, const double *weight)
{
  for (int c = 0; c < 3; c++) {
    out.value[c] += in.value[c] * weight[c];
    out.absolute[c] += in.absolute[c] * std::abs(weight[c]);
    if (!std::isfinite(out.value[c]) || !std::isfinite(out.absolute[c]))
      return false;
  }
  return true;
}
/* Compensation protects signed HDR accounting; abs-path weights separately retain
 * intermediate cancellation error even when the final shared-source weight is zero. */
struct Sum {
  double value = 0, correction = 0;
  void add(const double x)
  {
    const double next = value + x;
    correction += std::abs(value) >= std::abs(x) ? (value - next) + x : (x - next) + value;
    value = next;
  }
  double result() const
  {
    return value + correction;
  }
};
}  // namespace

DeepStatus deep_resolve_emission_expression(const DeepMemoryBudget &budget,
                                            const std::span<const DeepEmissionSource> sources,
                                            const std::span<const DeepEmissionNode> nodes,
                                            const std::span<const DeepEmissionFilmCall> calls,
                                            DeepResolvedEmission &output)
{
  output = {};
  if (sources.size() > UINT32_MAX || nodes.size() > UINT32_MAX)
    return DeepStatus::fail(DeepErrorCode::InvalidCount, "Emission expression index overflow");
  try {
    auto data = std::allocate_shared<DeepEmissionExpressionStorage>(
        DeepBudgetAllocator<DeepEmissionExpressionStorage>(budget), budget);
    data->sources.assign(sources.begin(), sources.end());
    data->nodes.assign(nodes.begin(), nodes.end());
    data->calls.assign(calls.begin(), calls.end());
    EmissionVector<uint64_t> identities{DeepBudgetAllocator<uint64_t>(budget)};
    for (const auto &source : sources) {
      const bool point = source.basis == DeepCurveRadianceBasis::Point;
      const bool span = source.basis == DeepCurveRadianceBasis::BeerSpan;
      if (!source.source || (!point && !span) || !std::isfinite(source.z) ||
          !std::isfinite(source.z_back) || source.z < 0 ||
          source.z_back > double(std::numeric_limits<float>::max()) ||
          (point ? source.z_back != source.z : source.z_back <= source.z))
        return fail("Emission source has missing or invalid physical support");
      for (int c = 0; c < 3; c++)
        if (!std::isfinite(source.raw_rgb[c]) || !std::isfinite(source.tau[c]) ||
            source.tau[c] < 0 || (point && source.tau[c] != 0))
          return fail("Emission source is nonfinite or has invalid optical shape");
      identities.push_back(source.source);
    }
    std::sort(identities.begin(), identities.end());
    if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
      return fail("Emission source identity is duplicated");

    /* Conservative expanded arithmetic count is used only for numerical accounting,
     * never for traversal or storage. Reject overflow/uninformative gamma_n bounds. */
    EmissionVector<uint64_t> operations{DeepBudgetAllocator<uint64_t>(budget)};
    operations.reserve(nodes.size());
    constexpr uint64_t max_operations = (uint64_t(1) << 24) - 1;
    for (size_t i = 0; i < nodes.size(); i++) {
      const auto &node = nodes[i];
      const auto earlier = [&](const uint32_t index) { return index < i; };
      uint64_t count = 0;
      switch (node.operation) {
        case DeepEmissionOperation::Zero:
          break;
        case DeepEmissionOperation::Term:
          if (node.source >= sources.size())
            return fail("Emission term references an unknown source");
          break;
        case DeepEmissionOperation::Snapshot:
        case DeepEmissionOperation::Scale:
          if (!earlier(node.a))
            return fail("Emission unary node is cyclic or refers forward");
          count = operations[node.a] + (node.operation == DeepEmissionOperation::Scale);
          break;
        case DeepEmissionOperation::Add:
        case DeepEmissionOperation::Mix:
        case DeepEmissionOperation::Select:
          if (!earlier(node.a) || !earlier(node.b))
            return fail("Emission binary node is cyclic or refers forward");
          count = node.operation == DeepEmissionOperation::Select ?
                      operations[node.select_b ? node.b : node.a] :
                      operations[node.a] + operations[node.b] +
                          (node.operation == DeepEmissionOperation::Mix ? 3 : 1);
          break;
        default:
          return fail("Unknown emission expression operation");
      }
      if (count > max_operations)
        return DeepStatus::fail(DeepErrorCode::InvalidCount,
                                "Emission accounting needs a stronger native-order witness");
      for (const float weight : node.weight)
        if (!std::isfinite(weight))
          return fail("Emission expression weight is nonfinite");
      if (node.operation == DeepEmissionOperation::Mix &&
          (node.weight[0] < 0 || node.weight[0] > 1))
        return fail("Emission mix weight is outside native interpolation range");
      operations.push_back(count);
    }
    identities.clear();
    for (const auto &call : calls) {
      if (!call.id || call.head >= nodes.size() ||
          (call.routing &
           ~(DEEP_EMISSION_COMBINED | DEEP_EMISSION_CATCHER_ONLY | DEEP_EMISSION_AUXILIARY)) ||
          !(call.routing &
            (DEEP_EMISSION_COMBINED | DEEP_EMISSION_CATCHER_ONLY | DEEP_EMISSION_AUXILIARY)) ||
          ((call.routing & DEEP_EMISSION_COMBINED) &&
           (call.routing & DEEP_EMISSION_CATCHER_ONLY)) ||
          !std::isfinite(call.clamp_gain) || call.clamp_gain < 0 || call.clamp_gain > 1)
        return fail("Emission film call has invalid identity, routing or clamp");
      for (int c = 0; c < 3; c++) {
        if (!std::isfinite(call.pre_clamp_rgb[c]) || !std::isfinite(call.post_clamp_rgb[c]))
          return fail("Emission film call is nonfinite before or after clamp");
        const float expected = call.pre_clamp_rgb[c] * call.clamp_gain;
        const auto ftz = [](const float value) {
          return std::abs(value) < std::numeric_limits<float>::min() ? 0.0f : value;
        };
        const float flushed = ftz(ftz(call.pre_clamp_rgb[c]) * ftz(call.clamp_gain));
        if (expected != call.post_clamp_rgb[c] &&
            !(call.flush_to_zero && flushed == call.post_clamp_rgb[c]))
          return fail("Emission film call post-clamp value differs from its actual gain");
      }
      identities.push_back(call.id);
    }
    std::sort(identities.begin(), identities.end());
    if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
      return fail("Emission film-call identity is duplicated");

    EmissionVector<Weights> weights{DeepBudgetAllocator<Weights>(budget)};
    EmissionVector<Weights> term_weights{DeepBudgetAllocator<Weights>(budget)};
    weights.resize(nodes.size());
    term_weights.resize(sources.size());
    for (const auto &call : calls) {
      std::fill(weights.begin(), weights.end(), Weights{});
      std::fill(term_weights.begin(), term_weights.end(), Weights{});
      weights[call.head] = {{1, 1, 1}, {1, 1, 1}};
      const double unit[3] = {1, 1, 1};
      Sum underflow[3];
      const double native_min = call.flush_to_zero ?
                                    double(std::numeric_limits<float>::min()) :
                                    double(std::numeric_limits<float>::denorm_min());
      const auto native_weight = [&](const float value) {
        return call.flush_to_zero && std::abs(value) < std::numeric_limits<float>::min() ?
                   0.0 :
                   double(value);
      };
      for (size_t i = size_t(call.head) + 1; i-- > 0;) {
        const auto &node = nodes[i];
        const auto &w = weights[i];
        if (w.absolute[0] == 0 && w.absolute[1] == 0 && w.absolute[2] == 0)
          continue;
        const auto transfer = [&](uint32_t target, const double *scale) {
          return add_weight(weights[target], w, scale);
        };
        bool valid = true;
        double underflow_factor[3] = {};
        switch (node.operation) {
          case DeepEmissionOperation::Zero:
            break;
          case DeepEmissionOperation::Term:
            valid = add_weight(term_weights[node.source], w, unit);
            break;
          case DeepEmissionOperation::Snapshot:
            valid = transfer(node.a, unit);
            break;
          case DeepEmissionOperation::Scale: {
            const double scale[3] = {native_weight(node.weight[0]),
                                     native_weight(node.weight[1]),
                                     native_weight(node.weight[2])};
            valid = transfer(node.a, scale);
            for (int c = 0; c < 3; c++)
              underflow_factor[c] = 1 + (call.flush_to_zero ? std::abs(scale[c]) : 0);
            break;
          }
          case DeepEmissionOperation::Add:
            valid = transfer(node.a, unit) && transfer(node.b, unit);
            for (double &factor : underflow_factor)
              factor = call.flush_to_zero ? 3 : 1;
            break;
          case DeepEmissionOperation::Mix: {
            const float mix_weight = float(native_weight(node.weight[0]));
            const double left[3] = {
                double(1.0f - mix_weight), double(1.0f - mix_weight), double(1.0f - mix_weight)};
            const double right[3] = {mix_weight, mix_weight, mix_weight};
            valid = transfer(node.a, left) && transfer(node.b, right);
            for (double &factor : underflow_factor)
              factor = call.flush_to_zero ? 4 : 3;
            break;
          }
          case DeepEmissionOperation::Select:
            valid = transfer(node.select_b ? node.b : node.a, unit);
            break;
        }
        if (!valid)
          return fail("Emission expression weight accumulation overflow");
        for (int c = 0; c < 3; c++)
          underflow[c].add(w.absolute[c] * underflow_factor[c] * native_min);
      }
      DeepEmissionAccounting accounting;
      accounting.call = call.id;
      accounting.float_operations = operations[call.head];
      Sum total[3], absolute[3];
      const double gain = native_weight(call.clamp_gain);
      for (size_t i = 0; i < sources.size(); i++) {
        const auto &source = sources[i];
        const auto &w = term_weights[i];
        const bool used = w.absolute[0] != 0 || w.absolute[1] != 0 || w.absolute[2] != 0;
        if (!used)
          continue;
        DeepCurveRadiance term;
        term.basis = source.basis;
        term.z = source.z;
        term.z_back = source.z_back;
        term.source = source.source;
        term.emission_call = call.id;
        term.ordinal = source.ordinal;
        term.flags = source.flags;
        for (int c = 0; c < 3; c++) {
          const double value = double(source.raw_rgb[c]) * w.value[c];
          total[c].add(value);
          absolute[c].add(std::abs(double(source.raw_rgb[c])) * w.absolute[c]);
          term.post_clamp_rgb[c] = value * gain;
          term.tau[c] = source.tau[c];
          if (!std::isfinite(term.post_clamp_rgb[c]))
            return fail("Resolved emission source overflow");
        }
        if (call.routing & DEEP_EMISSION_COMBINED)
          data->radiance.push_back(term);
      }
      /* Frozen terms include native shader/throughput computations. Propagate
       * FTZ through reverse weights: later Scale nodes may amplify underflow;
       * a bare n*FLT_MIN allowance is insufficient. */
      const double n = double(accounting.float_operations);
      const double u = std::ldexp(1.0, -24);
      const double gamma = n * u / (1 - n * u);
      for (int c = 0; c < 3; c++) {
        accounting.expression_rgb[c] = total[c].result();
        accounting.absolute_path_sum[c] = absolute[c].result();
        accounting.native_residual[c] = double(call.pre_clamp_rgb[c]) -
                                        accounting.expression_rgb[c];
        accounting.rounding_bound[c] = gamma * accounting.absolute_path_sum[c] +
                                       underflow[c].result() / (1 - n * u);
        /* Double reverse-weight/sum roundoff is separate from native FP32.
         * This covers path additions/multiplications without concealing missing terms. */
        const double host_n = double(nodes.size()) * 4 + double(sources.size()) * 4 + 8;
        const double host_u = std::ldexp(1.0, -53);
        const double host_bound = host_n * host_u / (1 - host_n * host_u) *
                                  accounting.absolute_path_sum[c];
        accounting.rounding_bound[c] += host_bound;
        if (!std::isfinite(accounting.expression_rgb[c]) ||
            !std::isfinite(accounting.absolute_path_sum[c]) ||
            !std::isfinite(accounting.rounding_bound[c]) ||
            std::abs(accounting.native_residual[c]) > accounting.rounding_bound[c])
          return fail("Emission expression does not account for its native film call");
        accounting.post_clamp_residual[c] = double(call.post_clamp_rgb[c]) -
                                            accounting.expression_rgb[c] * gain;
        accounting.post_clamp_rounding_bound[c] =
            accounting.rounding_bound[c] * gain +
            u * std::abs(double(call.pre_clamp_rgb[c]) * gain) +
            native_min * (call.flush_to_zero ? 1 + std::abs(gain) : 1);
        if (!std::isfinite(accounting.post_clamp_residual[c]) ||
            !std::isfinite(accounting.post_clamp_rounding_bound[c]) ||
            std::abs(accounting.post_clamp_residual[c]) > accounting.post_clamp_rounding_bound[c])
          return fail("Emission expression post-clamp accounting failed");
      }
      data->accounting.push_back(accounting);
    }
    output.storage_ = std::move(data);
    return {};
  }
  catch (const std::bad_alloc &) {
    return DeepStatus::fail(DeepErrorCode::BudgetExceeded,
                            "Emission expression input/scratch/output quota exhausted");
  }
  catch (const std::length_error &) {
    return DeepStatus::fail(DeepErrorCode::InvalidCount, "Emission expression count overflow");
  }
}

DeepResolvedEmission::operator bool() const
{
  return bool(storage_);
}
std::span<const DeepCurveRadiance> DeepResolvedEmission::radiance() const
{
  return storage_ ? std::span<const DeepCurveRadiance>(storage_->radiance) :
                    std::span<const DeepCurveRadiance>();
}
std::span<const DeepEmissionAccounting> DeepResolvedEmission::accounting() const
{
  return storage_ ? std::span<const DeepEmissionAccounting>(storage_->accounting) :
                    std::span<const DeepEmissionAccounting>();
}
std::span<const DeepEmissionSource> DeepResolvedEmission::sources() const
{
  return storage_ ? std::span<const DeepEmissionSource>(storage_->sources) :
                    std::span<const DeepEmissionSource>();
}
std::span<const DeepEmissionNode> DeepResolvedEmission::nodes() const
{
  return storage_ ? std::span<const DeepEmissionNode>(storage_->nodes) :
                    std::span<const DeepEmissionNode>();
}
std::span<const DeepEmissionFilmCall> DeepResolvedEmission::calls() const
{
  return storage_ ? std::span<const DeepEmissionFilmCall>(storage_->calls) :
                    std::span<const DeepEmissionFilmCall>();
}
CCL_NAMESPACE_END
