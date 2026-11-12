/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

template<bool Physical>
__device__ void trace_value(DeepCompactPrimitive &out, const CompactFitValue<Physical> &in,
                            const DeepCompactParameters &p)
{
  out = {};
  if (in.source < p.primitive_count) out = ptr<const DeepCompactPrimitive>(p.primitives)[in.source];
  out.front = in.front; out.back = in.back; out.q = in.q;
  out.log_before = in.log_before; out.log_after = in.log_after;
  out.kind = in.kind;
}

template<bool Physical>
__device__ __noinline__ void trace_fit(const DeepCompactEncodeParameters &e,
                                      const DeepCompactParameters &p, const uint32_t pixel,
                                      const CompactFitValue<Physical> &left,
                                      const CompactFitValue<Physical> &reference,
                                      const CompactFitValue<Physical> &right,
                                      const CompactFitValue<Physical> &trial,
                                      const DeepGPUFrameConsumer &policy, const double footprint,
                                      const bool accepted, const uint32_t flags = 0)
{
  if (!e.trace) return;
  const auto t = *ptr<const DeepCompactTraceParameters>(e.trace);
  const int32_t x = int32_t(int64_t(t.x) + pixel % t.width);
  const int32_t y = int32_t(int64_t(t.y) + pixel / t.width);
  const uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
  bool selected = false;
  for (uint32_t i = 0; i < t.probe_count; i++) selected |= ptr<const uint64_t>(t.probes)[i] == key;
  if (!selected) return;
  const uint32_t at = atomicAdd(ptr<uint32_t>(t.counter), 1u);
  if (at >= t.capacity) { atomicAdd(ptr<uint32_t>(t.counter) + 1, 1u); return; }
  auto &record = ptr<DeepCompactTraceRecord>(t.records)[at];
  trace_value(record.left, left, p); trace_value(record.reference, reference, p);
  trace_value(record.right, right, p); trace_value(record.trial, trial, p);
  record.alpha = policy.alpha_tolerance; record.depth = policy.depth_tolerance;
  record.rgb_absolute = policy.rgb_absolute; record.rgb_relative = policy.rgb_relative;
  record.footprint = footprint; record.x = x; record.y = y;
  record.flags = 2u | (Physical ? 4u : 0u) | (accepted ? 8u : 0u) | flags;
  record.reserved = 0;
  for (int c = 0; c < 3; c++) {
    if constexpr (Physical) {
      record.rgb[0][c] = left.rgb[c]; record.rgb[1][c] = reference.rgb[c];
      record.rgb[2][c] = right.rgb[c]; record.rgb[3][c] = trial.rgb[c];
      record.epsilon_left[c] = left.rgb_epsilon[c]; record.epsilon_right[c] = right.rgb_epsilon[c];
      record.epsilon_trial[c] = trial.rgb_epsilon[c];
    }
    else {
      for (int v = 0; v < 4; v++) record.rgb[v][c] = 0;
      record.epsilon_left[c] = record.epsilon_right[c] = record.epsilon_trial[c] = 0;
    }
  }
}
