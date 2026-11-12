/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

ccl_device_noinline void deep_trace_native(const DeepCompactPrimitive &left,
                                          const DeepCompactPrimitive &reference,
                                          const DeepCompactPrimitive &right,
                                          const DeepCompactPrimitive &trial,
                                          const bool accepted)
{
  const auto *parameters = kernel_integrator_state.deep_compact_trace;
  if (!parameters) return;
  const auto &p = *parameters;
  const auto &camera = kernel_integrator_state.deep_compact_cameras[right.camera];
  const uint64_t key = (uint64_t(uint32_t(camera.x)) << 32) | uint32_t(camera.y);
  bool selected = false;
  for (uint i = 0; i < p.probe_count; i++)
    selected |= reinterpret_cast<ccl_global const uint64_t *>(p.probes)[i] == key;
  if (!selected) return;
  auto *counter = reinterpret_cast<ccl_global uint32_t *>(p.counter);
  const uint slot = atomic_fetch_and_add_uint32(counter, 1);
  if (slot >= p.capacity) { atomic_fetch_and_add_uint32(counter + 1, 1); return; }
  auto &record = reinterpret_cast<ccl_global DeepCompactTraceRecord *>(p.records)[slot];
  record.left = left; record.reference = reference; record.right = right; record.trial = trial;
  record.alpha = .004; record.depth = INFINITY; record.footprint = 0;
  record.rgb_absolute = record.rgb_relative = 0;
  for (int c = 0; c < 3; c++) {
    for (int v = 0; v < 4; v++) record.rgb[v][c] = 0;
    record.epsilon_left[c] = record.epsilon_right[c] = record.epsilon_trial[c] = 0;
  }
  record.x = camera.x; record.y = camera.y;
  record.flags = 1u | (accepted ? 8u : 0u); record.reserved = 0;
}
