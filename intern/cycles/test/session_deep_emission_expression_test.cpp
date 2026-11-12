/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "session/deep_emission_expression.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
using RGB = std::array<float, 3>;
DeepEmissionSource source(uint64_t id, float r, float g, float b, double z = 2)
{
  return {DeepCurveRadianceBasis::Point, z, z, {r, g, b}, {}, id, uint32_t(id)};
}
DeepEmissionNode term(uint32_t index)
{
  DeepEmissionNode n;
  n.operation = DeepEmissionOperation::Term;
  n.source = index;
  return n;
}
DeepEmissionNode node(DeepEmissionOperation op, uint32_t a, uint32_t b = DEEP_EMISSION_NO_OPERAND)
{
  DeepEmissionNode n;
  n.operation = op;
  n.a = a;
  n.b = b;
  return n;
}
/* Independent forward FP32 evaluator. It executes immutable native candidate values;
 * the production resolver instead distributes reverse FP64 source weights. */
RGB forward(std::span<const DeepEmissionSource> sources,
            std::span<const DeepEmissionNode> nodes,
            uint32_t head)
{
  std::vector<RGB> values;
  for (uint32_t i = 0; i <= head; i++) {
    const auto &n = nodes[i];
    RGB v{};
    for (int c = 0; c < 3; c++) {
      switch (n.operation) {
        case DeepEmissionOperation::Zero:
          break;
        case DeepEmissionOperation::Term:
          v[c] = sources[n.source].raw_rgb[c];
          break;
        case DeepEmissionOperation::Add:
          v[c] = values[n.a][c] + values[n.b][c];
          break;
        case DeepEmissionOperation::Snapshot:
          v[c] = values[n.a][c];
          break;
        case DeepEmissionOperation::Scale:
          v[c] = values[n.a][c] * n.weight[c];
          break;
        case DeepEmissionOperation::Mix: {
          /* Volatile commits separate multiply/add native FP32 operations for the
           * oracle; the conservative accounting also allows fused operations. */
          volatile float left = values[n.a][c] * (1.0f - n.weight[0]);
          volatile float right = values[n.b][c] * n.weight[0];
          v[c] = left + right;
          break;
        }
        case DeepEmissionOperation::Select:
          v[c] = values[n.select_b ? n.b : n.a][c];
          break;
      }
    }
    values.push_back(v);
  }
  return values[head];
}
DeepEmissionFilmCall call(uint64_t id,
                          uint32_t head,
                          const RGB &raw,
                          float gain = 1,
                          uint32_t routing = DEEP_EMISSION_COMBINED)
{
  DeepEmissionFilmCall c;
  c.id = id;
  c.head = head;
  c.routing = routing;
  c.clamp_gain = gain;
  for (int k = 0; k < 3; k++) {
    c.pre_clamp_rgb[k] = raw[k];
    c.post_clamp_rgb[k] = raw[k] * gain;
  }
  return c;
}
std::array<double, 3> prefix(const DeepResolvedEmission &resolved, double z)
{
  std::array<double, 3> rgb{};
  for (const auto &t : resolved.radiance()) {
    const double f = t.z == t.z_back ? (z >= t.z ? 1 : 0) :
                                       std::clamp((z - t.z) / (t.z_back - t.z), 0.0, 1.0);
    for (int c = 0; c < 3; c++) {
      const double shape = t.tau[c] == 0 ? f : -std::expm1(-t.tau[c] * f) / -std::expm1(-t.tau[c]);
      rgb[c] += t.post_clamp_rgb[c] * shape;
    }
  }
  return rgb;
}
}  // namespace

TEST(DeepEmissionExpression, shared_prefix_mix_snapshot_and_select_preserve_real_support)
{
  DeepMemoryBudget budget(1 << 20);
  std::array sources{source(10, 2, -1, 3, 1), source(11, 4, 5, -2, 2), source(12, -8, 2, 1, 3)};
  std::array nodes{term(0),
                   term(1),
                   node(DeepEmissionOperation::Add, 0, 1),
                   node(DeepEmissionOperation::Snapshot, 2),
                   term(2),
                   node(DeepEmissionOperation::Add, 3, 4),
                   node(DeepEmissionOperation::Mix, 3, 5),
                   node(DeepEmissionOperation::Select, 5, 6)};
  nodes[6].weight[0] = 0.25f;
  nodes[7].select_b = true;
  const std::array calls{call(91, 7, forward(sources, nodes, 7), 0.3f)};
  DeepResolvedEmission out;
  auto status = deep_resolve_emission_expression(budget, sources, nodes, calls, out);
  ASSERT_TRUE(status) << status.message;
  ASSERT_EQ(out.radiance().size(), 3u);
  EXPECT_EQ(out.radiance()[0].post_clamp_rgb[0], 2 * double(0.3f));
  EXPECT_EQ(out.radiance()[1].post_clamp_rgb[0], 4 * double(0.3f));
  EXPECT_EQ(out.radiance()[2].post_clamp_rgb[0], -2 * double(0.3f));
  EXPECT_EQ(prefix(out, 1.5)[0], 2 * double(0.3f));
  EXPECT_EQ(prefix(out, 2.5)[0], 6 * double(0.3f));
  EXPECT_EQ(prefix(out, 3.5)[0], 4 * double(0.3f));
  EXPECT_EQ(out.accounting()[0].native_residual[0], 0);
}

TEST(DeepEmissionExpression, unselected_candidate_is_owned_but_does_not_emit)
{
  DeepMemoryBudget budget(1 << 20);
  std::array sources{source(1, 2, 3, 4), source(2, 99999, 88888, 77777, 4)};
  std::array nodes{term(0), term(1), node(DeepEmissionOperation::Select, 0, 1)};
  const std::array calls{call(1, 2, forward(sources, nodes, 2))};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  ASSERT_EQ(out.radiance().size(), 1u);
  EXPECT_EQ(out.radiance()[0].source, 1u);
  EXPECT_EQ(out.sources().size(), 2u);
  sources[0].raw_rgb[0] = 999;
  nodes[2].select_b = true;
  EXPECT_EQ(out.sources()[0].raw_rgb[0], 2);
  EXPECT_FALSE(out.nodes()[2].select_b);
}

TEST(DeepEmissionExpression, immutable_reservoir_aliases_preserve_shared_mix_scale_and_calls)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{
      source(1, 20000, -10000, 3, 1), source(2, -12000, 8000, -5, 2), source(3, 4, -8, 7, 3)};
  std::array recorded{term(0),
                      term(1),
                      node(DeepEmissionOperation::Add, 0, 1),
                      node(DeepEmissionOperation::Snapshot, 2),
                      term(2),
                      node(DeepEmissionOperation::Add, 3, 4),
                      node(DeepEmissionOperation::Select, 3, 5),
                      node(DeepEmissionOperation::Snapshot, 6),
                      node(DeepEmissionOperation::Mix, 5, 7),
                      node(DeepEmissionOperation::Scale, 8)};
  recorded[6].select_b = false;
  recorded[8].weight[0] = 0.375f;
  recorded[9].weight[0] = 0.25f;
  recorded[9].weight[1] = 2;
  recorded[9].weight[2] = 0.125f;
  std::array aliased{term(0),
                     term(1),
                     node(DeepEmissionOperation::Add, 0, 1),
                     term(2),
                     node(DeepEmissionOperation::Add, 2, 3),
                     node(DeepEmissionOperation::Mix, 4, 2),
                     node(DeepEmissionOperation::Scale, 5)};
  aliased[5].weight[0] = recorded[8].weight[0];
  std::copy_n(recorded[9].weight, 3, aliased[6].weight);
  const std::array original_calls{call(10, 9, forward(sources, recorded, 9), 0.25f),
                                  call(11, 7, forward(sources, recorded, 7), 0.5f)};
  const std::array alias_calls{call(10, 6, forward(sources, aliased, 6), 0.25f),
                               call(11, 2, forward(sources, aliased, 2), 0.5f)};
  DeepResolvedEmission reference, folded;
  ASSERT_TRUE(
      deep_resolve_emission_expression(budget, sources, recorded, original_calls, reference));
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, aliased, alias_calls, folded));
  ASSERT_EQ(reference.radiance().size(), folded.radiance().size());
  EXPECT_EQ(folded.sources().size(), sources.size());
  for (size_t i = 0; i < reference.radiance().size(); i++) {
    const auto &a = reference.radiance()[i], &b = folded.radiance()[i];
    EXPECT_EQ(a.source, b.source);
    EXPECT_EQ(a.emission_call, b.emission_call);
    EXPECT_EQ(a.z, b.z);
    for (int c = 0; c < 3; c++)
      EXPECT_EQ(a.post_clamp_rgb[c], b.post_clamp_rgb[c]);
  }
  for (double z : {0.0, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0})
    EXPECT_EQ(prefix(reference, z), prefix(folded, z));
  EXPECT_LT(folded.nodes().size(), reference.nodes().size());
}

TEST(DeepEmissionExpression, zero_head_alias_is_valid_but_invalid_head_is_not_a_zero)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 1, 2, 3)};
  const std::array nodes{node(DeepEmissionOperation::Zero, DEEP_EMISSION_NO_OPERAND), term(0)};
  std::array calls{call(1, 0, {0, 0, 0})};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_TRUE(out.radiance().empty());
  EXPECT_EQ(out.sources().size(), 1u);
  calls[0].head = DEEP_EMISSION_NO_OPERAND;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_FALSE(out);
}

TEST(DeepEmissionExpression, channel_scale_signed_hdr_and_zero_tau_keep_depth_prefixes)
{
  DeepMemoryBudget budget(1 << 20);
  std::array sources{source(1, 20000, -30000, 0.125f), source(2, -0.5f, 0.3f, 10000, 4)};
  sources[0].basis = DeepCurveRadianceBasis::BeerSpan;
  sources[0].z_back = 3;
  sources[0].tau[0] = 0;
  sources[0].tau[1] = 2;
  sources[0].tau[2] = 8;
  std::array nodes{term(0),
                   node(DeepEmissionOperation::Scale, 0),
                   term(1),
                   node(DeepEmissionOperation::Add, 1, 2)};
  nodes[1].weight[0] = 0.1f;
  nodes[1].weight[1] = 2;
  nodes[1].weight[2] = -3;
  const std::array calls{call(7, 3, forward(sources, nodes, 3), 0.01f)};
  DeepResolvedEmission out;
  auto status = deep_resolve_emission_expression(budget, sources, nodes, calls, out);
  ASSERT_TRUE(status) << status.message;
  EXPECT_EQ(out.radiance().size(), 2u);
  const auto halfway = prefix(out, 2.5);
  EXPECT_NEAR(halfway[0], 20000 * double(0.1f) * double(0.01f) / 2, 1e-10);
  EXPECT_NEAR(halfway[1], -60000 * double(0.01f) * -std::expm1(-1.0) / -std::expm1(-2.0), 1e-10);
  EXPECT_EQ(out.radiance()[1].post_clamp_rgb[0], -0.5 * double(0.01f));
}

TEST(DeepEmissionExpression, one_call_clamps_whole_sum_not_individual_terms)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 100, 0, 0, 2), source(2, -99, 0, 0, 3)};
  const std::array nodes{term(0), term(1), node(DeepEmissionOperation::Add, 0, 1)};
  const std::array calls{call(9, 2, forward(sources, nodes, 2), 0.5f)};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_EQ(prefix(out, 2.5)[0], 50);
  EXPECT_EQ(prefix(out, 3.5)[0], 0.5);
  EXPECT_EQ(out.radiance()[1].post_clamp_rgb[0], -49.5);
}

TEST(DeepEmissionExpression, catcher_only_and_auxiliary_are_accounted_without_combined_duplicates)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 2, 3, 4)};
  const std::array nodes{term(0)};
  const std::array calls{
      call(1, 0, {2, 3, 4}, 1, DEEP_EMISSION_COMBINED | DEEP_EMISSION_AUXILIARY),
      call(2, 0, {2, 3, 4}, 1, DEEP_EMISSION_CATCHER_ONLY),
      call(3, 0, {2, 3, 4}, 1, DEEP_EMISSION_AUXILIARY)};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_EQ(out.radiance().size(), 1u);
  EXPECT_EQ(out.accounting().size(), 3u);
  EXPECT_EQ(prefix(out, 3)[0], 2);
}

TEST(DeepEmissionExpression, native_rounding_is_reported_but_missing_terms_fail)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{
      source(1, 16777216, 0, 0), source(2, 1, 0, 0), source(3, -16777216, 0, 0)};
  const std::array nodes{term(0),
                         term(1),
                         node(DeepEmissionOperation::Add, 0, 1),
                         term(2),
                         node(DeepEmissionOperation::Add, 2, 3)};
  std::array calls{call(1, 4, forward(sources, nodes, 4))};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_EQ(out.accounting()[0].native_residual[0], -1);
  EXPECT_GE(out.accounting()[0].rounding_bound[0], 1);
  EXPECT_EQ(prefix(out, 3)[0], 1); /* No residual/correction source. */
  calls[0].pre_clamp_rgb[0] = calls[0].post_clamp_rgb[0] = 100;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_FALSE(out);
}

TEST(DeepEmissionExpression, cancellation_keeps_absolute_path_error_ledger)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 9999999, 1, -1)};
  std::array nodes{
      term(0), node(DeepEmissionOperation::Scale, 0), node(DeepEmissionOperation::Add, 0, 1)};
  for (auto &w : nodes[1].weight)
    w = -1;
  const std::array calls{call(1, 2, forward(sources, nodes, 2))};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  ASSERT_EQ(out.radiance().size(), 1u);
  EXPECT_EQ(out.radiance()[0].post_clamp_rgb[0], 0);
  EXPECT_EQ(out.accounting()[0].absolute_path_sum[0], 19999998);
  EXPECT_GT(out.accounting()[0].rounding_bound[0], 0);
}

TEST(DeepEmissionExpression, cyclic_unknown_or_duplicate_identities_are_hard_errors)
{
  DeepMemoryBudget budget(1 << 20);
  std::array sources{source(1, 1, 2, 3), source(2, 2, 3, 4)};
  std::array nodes{term(0), node(DeepEmissionOperation::Add, 0, 0)};
  std::array calls{call(1, 1, {2, 4, 6})};
  DeepResolvedEmission out;
  nodes[1].b = 1;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  nodes[1].b = 0;
  nodes[0].source = 99;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  nodes[0].source = 0;
  sources[1].source = 1;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  sources[1].source = 2;
  const std::array duplicated_calls{calls[0], calls[0]};
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, duplicated_calls, out));
}

TEST(DeepEmissionExpression, nonfinite_preclamp_bad_gain_and_routing_are_not_sanitized)
{
  DeepMemoryBudget budget(1 << 20);
  std::array sources{source(1, 1, 2, 3)};
  const std::array nodes{term(0)};
  std::array calls{call(1, 0, {1, 2, 3})};
  DeepResolvedEmission out;
  sources[0].raw_rgb[0] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  sources[0].raw_rgb[0] = 1;
  calls[0].clamp_gain = 2;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  calls[0].clamp_gain = 1;
  calls[0].post_clamp_rgb[0] = 0;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  calls[0].post_clamp_rgb[0] = 1;
  calls[0].routing = DEEP_EMISSION_COMBINED | DEEP_EMISSION_CATCHER_ONLY;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
}

TEST(DeepEmissionExpression, long_snapshot_history_uses_linear_owned_storage_and_quota)
{
  DeepMemoryBudget budget(4 << 20);
  const size_t baseline = budget.used();
  std::vector<DeepEmissionSource> sources;
  std::vector<DeepEmissionNode> nodes;
  nodes.emplace_back();
  float native_sum = 0;
  for (uint32_t i = 0; i < 1000; i++) {
    sources.push_back(source(i + 1, 0.001f, 0, 0, double(i + 1)));
    const uint32_t previous = uint32_t(nodes.size() - 1);
    nodes.push_back(term(i));
    nodes.push_back(node(DeepEmissionOperation::Add, previous, uint32_t(nodes.size() - 1)));
    nodes.push_back(node(DeepEmissionOperation::Snapshot, uint32_t(nodes.size() - 1)));
    native_sum += 0.001f;
  }
  const std::array calls{call(1, uint32_t(nodes.size() - 1), {native_sum, 0, 0})};
  DeepResolvedEmission out, retained;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_EQ(out.radiance().size(), 1000u);
  EXPECT_EQ(out.nodes().size(), 3001u);
  EXPECT_LT(budget.used() - baseline, 1u << 20);
  retained = out;
  out = {};
  EXPECT_EQ(retained.calls()[0].head, 3000u);
  retained = {};
  EXPECT_EQ(budget.used(), baseline);
  DeepMemoryBudget tiny(512);
  const size_t tiny_baseline = tiny.used();
  auto status = deep_resolve_emission_expression(tiny, sources, nodes, calls, out);
  EXPECT_EQ(status.code, DeepErrorCode::BudgetExceeded);
  EXPECT_FALSE(out);
  EXPECT_EQ(tiny.used(), tiny_baseline);
}

TEST(DeepEmissionExpression, repeated_native_calls_share_source_but_keep_separate_clamps)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 4, 2, -1)};
  const std::array nodes{term(0)};
  const std::array calls{call(1, 0, {4, 2, -1}, 0.5f), call(2, 0, {4, 2, -1}, 0.25f)};
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  ASSERT_EQ(out.radiance().size(), 2u);
  EXPECT_EQ(out.radiance()[0].source, out.radiance()[1].source);
  EXPECT_NE(out.radiance()[0].emission_call, out.radiance()[1].emission_call);
  EXPECT_EQ(prefix(out, 3)[0], 3);
}

TEST(DeepEmissionExpression, native_ftz_clamp_is_explicit_and_does_not_prune_the_source)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 2e-38f, 2, -3)};
  const std::array nodes{term(0)};
  std::array calls{call(1, 0, {2e-38f, 2, -3}, 0.25f)};
  calls[0].post_clamp_rgb[0] = 0; /* GPU multiplication flushed its result. */
  DeepResolvedEmission out;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  calls[0].flush_to_zero = true;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  ASSERT_EQ(out.radiance().size(), 1u);
  EXPECT_GT(out.radiance()[0].post_clamp_rgb[0], 0);
  EXPECT_NE(out.accounting()[0].post_clamp_residual[0], 0);
  EXPECT_LE(std::abs(out.accounting()[0].post_clamp_residual[0]),
            out.accounting()[0].post_clamp_rounding_bound[0]);
  calls[0].post_clamp_rgb[0] = 0.001f;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
}

TEST(DeepEmissionExpression, ftz_error_is_weighted_by_later_native_scale)
{
  DeepMemoryBudget budget(1 << 20);
  const std::array sources{source(1, 2e-38f, 0, 0)};
  std::array nodes{
      term(0), node(DeepEmissionOperation::Scale, 0), node(DeepEmissionOperation::Scale, 1)};
  nodes[1].weight[0] = 0.25f;
  nodes[2].weight[0] = 1e30f;
  std::array calls{call(1, 2, {0, 0, 0})};
  calls[0].flush_to_zero = true;
  DeepResolvedEmission out;
  ASSERT_TRUE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
  EXPECT_GT(std::abs(out.accounting()[0].native_residual[0]), 1e-10);
  EXPECT_LE(std::abs(out.accounting()[0].native_residual[0]),
            out.accounting()[0].rounding_bound[0]);
  calls[0].flush_to_zero = false;
  EXPECT_FALSE(deep_resolve_emission_expression(budget, sources, nodes, calls, out));
}

CCL_NAMESPACE_END
