/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "session/deep_native_curve.h"

CCL_NAMESPACE_BEGIN

constexpr uint32_t DEEP_EMISSION_NO_OPERAND = UINT32_MAX;
enum class DeepEmissionOperation : uint32_t { Zero, Term, Add, Snapshot, Scale, Mix, Select };
enum DeepEmissionRouting : uint32_t {
  DEEP_EMISSION_COMBINED = 1u << 0,
  DEEP_EMISSION_CATCHER_ONLY = 1u << 1,
  DEEP_EMISSION_AUXILIARY = 1u << 2,
};

/* Frozen native source terms are already throughput/MIS weighted. A linear span is
 * BeerSpan with tau=0; sampled null collisions are Point, never optical intervals. */
struct DeepEmissionSource {
  DeepCurveRadianceBasis basis = DeepCurveRadianceBasis::Point;
  double z = 0, z_back = 0;
  float raw_rgb[3] = {};
  double tau[3] = {};
  uint64_t source = 0;
  uint32_t ordinal = 0, flags = DEEP_KNOT_PROTECTED_CUT;
};

/* Append-only topological indices. Snapshot and Select refer to immutable earlier
 * heads; a candidate never copies or mutates the shared history. Mix uses the actual
 * float w and float (1-w), matching native component arithmetic. */
struct DeepEmissionNode {
  DeepEmissionOperation operation = DeepEmissionOperation::Zero;
  uint32_t a = DEEP_EMISSION_NO_OPERAND, b = DEEP_EMISSION_NO_OPERAND;
  uint32_t source = DEEP_EMISSION_NO_OPERAND;
  float weight[3] = {1, 1, 1};
  bool select_b = false;
};

struct DeepEmissionFilmCall {
  uint64_t id = 0;
  uint32_t head = DEEP_EMISSION_NO_OPERAND, routing = 0;
  float pre_clamp_rgb[3] = {}, post_clamp_rgb[3] = {}, clamp_gain = 1;
  /* Freeze the actual native arithmetic mode; CUDA/OptiX uses fast math. */
  bool flush_to_zero = false;
};
struct DeepEmissionAccounting {
  uint64_t call = 0, float_operations = 0;
  double expression_rgb[3] = {}, absolute_path_sum[3] = {};
  double native_residual[3] = {}, rounding_bound[3] = {};
  double post_clamp_residual[3] = {}, post_clamp_rounding_bound[3] = {};
};
struct DeepEmissionExpressionStorage;

/* CPU fallback resolver. Reverse weights visit each shared node once per film call,
 * then apply that call's actual clamp once to every surviving source. Owned input,
 * output and scratch allocations all use the frame's ledger. No endpoint calibration
 * or residual source is created. This is not a native DAG capture implementation. */
class DeepResolvedEmission {
 public:
  explicit operator bool() const;
  std::span<const DeepCurveRadiance> radiance() const;
  std::span<const DeepEmissionAccounting> accounting() const;
  std::span<const DeepEmissionSource> sources() const;
  std::span<const DeepEmissionNode> nodes() const;
  std::span<const DeepEmissionFilmCall> calls() const;

 private:
  std::shared_ptr<const DeepEmissionExpressionStorage> storage_;
  friend DeepStatus deep_resolve_emission_expression(const DeepMemoryBudget &,
                                                     std::span<const DeepEmissionSource>,
                                                     std::span<const DeepEmissionNode>,
                                                     std::span<const DeepEmissionFilmCall>,
                                                     DeepResolvedEmission &);
};

DeepStatus deep_resolve_emission_expression(const DeepMemoryBudget &budget,
                                            std::span<const DeepEmissionSource> sources,
                                            std::span<const DeepEmissionNode> nodes,
                                            std::span<const DeepEmissionFilmCall> calls,
                                            DeepResolvedEmission &output);

CCL_NAMESPACE_END
