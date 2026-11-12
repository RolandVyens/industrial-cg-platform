/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* Explicit qualification only; state stays in its own global allocation. */
extern "C" __global__ void deep_gpu_frame_compact_qualification_cases(const DeepGPUFrameDispatch d)
{
  const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
  if (id >= 8) return;
  auto &out = ptr<DeepGPUFrameQualificationCase>(d.a)[id];
  out.id = id;
  out.policy = {1, 0, .01f, .01f, .006f, .002f, UINT32_MAX};
  out.encode.base = uint64_t(&out.base); out.encode.policy = uint64_t(&out.policy);
  out.encode.input_ranges = uint64_t(&out.range); out.encode.states = uint64_t(out.state);
  out.encode.summaries = uint64_t(&out.summary); out.encode.controls = uint64_t(out.controls);
  out.encode.samples = uint64_t(out.samples); out.encode.keys = uint64_t(out.keys);
  out.encode.pixel_count = 1; out.encode.page_capacity = 256;
  out.base.status = uint64_t(&out.error); out.base.knots = uint64_t(&out.knot);
  out.base.projection = uint64_t(out.projection);
  auto &state = *reinterpret_cast<CompactEncodeState<true> *>(out.state);
  state.log_denominator = -INFINITY;
  state.phase = 1; state.has_left = 1;
  state.left.kind = DEEP_COMPACT_SURFACE;
  state.left.front = state.left.back = 1;
  state.left.q = .5; state.left.log_after = log(.5);
  if (id == 0) {
    const auto &captured = *ptr<const DeepCompactCaptureCase>(d.b);
    if (captured.claimed != 3 || captured.status || captured.count != 2) {
      out.error = DEEP_GPU_FRAME_INVALID_RANGE; return;
    }
    auto *input = reinterpret_cast<CompactFitValue<true> *>(out.joined);
    for (uint32_t i = 0; i < 2; i++) {
      const auto &v = captured.primitives[i];
      input[i].front = v.front; input[i].back = v.back; input[i].q = v.q;
      input[i].log_before = v.log_before; input[i].log_after = v.log_after;
      input[i].kind = v.kind;
    }
    state.phase = state.has_left = 0; out.range.count = 1;
    out.encode.joined = uint64_t(out.joined);
  }
  else if (id == 1) {
    state.left.q = 4095.0 / 4096; state.left.log_after = -log(4096.0);
  }
  else if (id == 2) {
    state.left.front = float(.979946213312); state.left.back = 1;
    state.left.kind = DEEP_COMPACT_VOLUME;
    state.left.q = 0; state.left.log_before = state.left.log_after = -1;
    state.left.rgb[0] = .011788645156; state.log_encoded = -1;
  }
  else if (id == 3) {
    state.left.rgb[0] = -2; state.left.rgb[1] = 8; state.left.rgb[2] = 1;
  }
  else if (id == 4) {
    state.left.q = 1; state.left.log_after = -1000;
  }
  else if (id == 5) {
    state.left.front = 0; state.left.back = 1; state.left.kind = DEEP_COMPACT_VOLUME;
    state.left.q = .9; state.left.log_after = log(.1);
    compact_lock_colour_epsilon(state.left);
  }
  else if (id == 6) {
    out.knot.point_q = -1; out.knot.log_before = out.knot.log_after = 0;
    out.range.count = 1;
    auto &scalar = *reinterpret_cast<CompactEncodeState<false> *>(out.state);
    scalar = {}; scalar.log_denominator = -INFINITY;
    compact_encode_pixel<false>(out.encode, 0);
    out.count = out.controls[0]; out.phase = scalar.phase;
    out.encode = {}; out.base = {};
    return;
  }
  else if (id == 7) {
    state.log_encoded = -1000; state.left.rgb[0] = 1;
  }
  compact_encode_pixel<true>(out.encode, 0);
  out.count = out.controls[0]; out.phase = state.phase;
  if (id == 0) {
    CompactFitValue<true> left{}, right{}, trial{};
    left.front = 1; left.back = 2; left.q = .1; left.log_after = log(.9);
    right.front = 2.005f; right.back = 3.005f; right.q = .1;
    right.log_before = left.log_after; right.log_after = log(.8);
    left.rgb[0] = right.rgb[0] = .1;
    compact_lock_colour_epsilon(left); compact_lock_colour_epsilon(right);
    auto policy = out.policy; policy.depth_tolerance = 0;
    if (!compact_fit_extend(left, left, right, policy, 10, trial)) out.controls[2] |= 1;
    policy.depth_tolerance = .01f;
    if (compact_fit_extend(left, left, right, policy, 0, trial)) out.controls[2] |= 2;
    policy.depth_tolerance = .001f;
    if (!compact_fit_extend(left, left, right, policy, 0, trial)) out.controls[2] |= 4;
    if (compact_fit_extend(left, left, right, policy, 10, trial)) out.controls[2] |= 8;
    right.rgb[0] = 10;
    compact_lock_colour_epsilon(right);
    if (!compact_fit_extend(left, left, right, policy, 10, trial)) out.controls[2] |= 16;
    right.kind = DEEP_COMPACT_HOLDOUT;
    if (!compact_fit_extend(left, left, right, policy, 10, trial)) out.controls[2] |= 32;
  }
  out.encode = {}; out.base = {};
}
