/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/cuda/deep_compact_trace.cuh"

namespace {
template<bool Physical> struct CompactEncodeState {
  CompactFitValue<Physical> left, reference;
  uint32_t input, phase, sequence, has_left, has_surface, reserved;
  double log_encoded, log_denominator, fraction, tau_cursor;
  uint32_t piece, pieces;
};
static_assert(sizeof(CompactEncodeState<false>) == 144);
static_assert(sizeof(CompactEncodeState<true>) == 240);

template<bool Physical>
__device__ CompactFitValue<Physical> compact_input(const DeepCompactEncodeParameters &e,
                                                 const DeepCompactParameters &p,
                                                 const uint32_t at)
{
  if constexpr (Physical) return ptr<const CompactFitValue<true>>(e.joined)[at];
  else {
    const auto &knot = ptr<const DeepCompactKnot>(p.knots)[at / 2];
    CompactFitValue<false> value{};
    value.source = knot.source;
    if (at & 1) {
      value.front = knot.z; value.back = knot.next_z; value.kind = DEEP_COMPACT_VOLUME;
      value.q = knot.interval_q; value.log_before = knot.log_after; value.log_after = knot.log_next;
    }
    else {
      value.front = value.back = knot.z; value.kind = knot.kind;
      value.q = knot.point_q; value.log_before = knot.log_before; value.log_after = knot.log_after;
    }
    return value;
  }
}

template<bool Physical>
__device__ bool compact_has_colour(const CompactFitValue<Physical> &value)
{
  if constexpr (Physical) return value.rgb[0] != 0 || value.rgb[1] != 0 || value.rgb[2] != 0;
  return false;
}

template<bool Physical>
__device__ void compact_lock_colour_epsilon(CompactFitValue<Physical> &value)
{
  if constexpr (Physical) {
    const double transmission = exp(value.log_before);
    for (int c = 0; c < 3; c++)
      value.rgb_epsilon[c] = fmax(fmax(.006 * transmission, .002 * fabs(value.rgb[c])) -
                                    .004 * transmission, .0001 * transmission);
  }
}

template<bool Physical, bool Trace>
__device__ __forceinline__ void compact_encode_pixel_state(const DeepCompactEncodeParameters &e,
                                                           const uint32_t pixel,
                                                           CompactEncodeState<Physical> &state)
{
  const auto p = *ptr<const DeepCompactParameters>(e.base);
  const auto policy = *ptr<const DeepGPUFrameConsumer>(e.policy);
  const auto range = ptr<const DeepGPUFrameRange>(e.input_ranges)[pixel];
  if (state.phase == 2) return;
  auto *controls = ptr<uint32_t>(e.controls);
  const uint32_t entry_input = state.input, entry_sequence = state.sequence;
  uint32_t work = 0;
  while (true) {
    if (++work > 4096) {
      if (state.input == entry_input && state.sequence == entry_sequence) {
        const bool report = atomicCAS(ptr<uint32_t>(p.status) + 3, 0u, 4804u) == 0;
        if constexpr (Trace) {
          if (report)
          printf("DEEP_ENCODE_NO_PROGRESS pixel=%u input=%u limit=%u phase=%u has_left=%u sequence=%u q=%.17g before=%.17g after=%.17g tau=%.17g fraction=%.17g piece=%u pieces=%u\n",
                 pixel, state.input, range.count, state.phase, state.has_left, state.sequence,
                 state.left.q, state.left.log_before, state.left.log_after, state.tau_cursor,
                 state.fraction, state.piece, state.pieces);
        }
        compact_fail(p, DEEP_GPU_FRAME_INVALID_ENCODING); return;
      }
      atomicAdd(controls + 1, 1u); atomicAdd(controls + 2, 1u); return;
    }
    if (state.phase == 0) {
      if (state.input < 2ull * range.count) {
        auto right = compact_input<Physical>(e, p, 2u * range.offset + state.input);
        if (right.kind == DEEP_COMPACT_SURFACE || right.kind == DEEP_COMPACT_HOLDOUT)
          state.has_surface = 1;
        compact_lock_colour_epsilon(right);
        if (!(right.q >= 0) || !isfinite(right.q) || !isfinite(right.front) ||
            !isfinite(right.back) || right.front < 0 || right.back < right.front ||
            isnan(right.log_before) || isnan(right.log_after) || right.log_after > right.log_before) {
          const bool report = atomicCAS(ptr<uint32_t>(p.status) + 3, 0u, 4801u) == 0;
          if constexpr (Trace) {
            if (report)
            printf("DEEP_ENCODE_INPUT_INVALID pixel=%u input=%u kind=%u q=%.17g z=%.9g back=%.9g before=%.17g after=%.17g\n",
                   pixel, state.input, right.kind, right.q, double(right.front), double(right.back), right.log_before, right.log_after);
          }
          compact_fail(p, DEEP_GPU_FRAME_NUMERIC_MASS_RANGE); return;
        }
        const bool nonzero = right.q > 0 || compact_has_colour(right);
        if (!nonzero && (right.kind == DEEP_COMPACT_VOLUME || !state.has_left)) {
          state.input++; continue;
        }
        if (!state.has_left) {
          state.left = state.reference = right; state.has_left = 1;
          state.input++; continue;
        }
        CompactFitValue<Physical> trial = state.left;
        const float2 projection = ptr<const float2>(p.projection)[pixel];
        const double footprint = compact_projection_footprint(
            projection, (double(state.left.front) + right.back) * .5);
        const bool continuous = right.back > right.front && state.left.back > state.left.front;
        const bool accepted = continuous &&
            compact_fit_extend(state.left, state.reference, right, policy, footprint, trial);
        if constexpr (Trace) {
          if (continuous && e.trace)
            trace_fit(e, p, pixel, state.left, state.reference, right, trial, policy, footprint, accepted);
        }
        if (accepted) {
          compact_fit_update_reference(state.reference, state.left, right.back);
          state.left = trial; state.input++; continue;
        }
        const auto *primitives = ptr<const DeepCompactPrimitive>(p.primitives);
        const bool point_sources = right.front == right.back && state.left.front == state.left.back &&
                                   state.left.source < p.primitive_count && right.source < p.primitive_count;
        /* Native opaque coverage grouping precedes optional point fitting.
         * A real nonempty volume interval already flushed L, so grouping never
         * moves an extinction interval through a foreground surface. */
        const bool native_coverage = point_sources &&
            compact_native_opaque_surface(primitives[state.left.source]) &&
            compact_native_opaque_surface(primitives[right.source]) &&
            compact_surface_group_compatible(primitives[state.left.source], primitives[right.source]);
        const bool merge_points = point_sources && (native_coverage ||
            compact_points_can_merge(state.left, right, policy, primitives));
        if constexpr (Trace) {
          if (point_sources && e.trace) {
          auto point_trial = state.left;
          point_trial.q += right.q; point_trial.log_after = right.log_after;
          if constexpr (Physical) {
            for (int c = 0; c < 3; c++) point_trial.rgb[c] += right.rgb[c];
          }
          trace_fit(e, p, pixel, state.left, state.reference, right, point_trial, policy, 0,
                     merge_points, 32u | (native_coverage ? 16u : 0u));
          }
        }
        if (merge_points) {
          /* Consume R as part of the mode-independent merge transaction.
           * Keep this outside the optional colour loop in both specializations. */
          state.input = state.input + 1u;
          state.left.q += right.q; state.left.log_after = right.log_after;
          if constexpr (Physical) {
            for (int c = 0; c < 3; c++) {
              state.left.rgb[c] += right.rgb[c];
              state.left.rgb_epsilon[c] = fmin(state.left.rgb_epsilon[c], right.rgb_epsilon[c]);
            }
          }
          continue;
        }
        /* R stays at the current input cursor. Only the frozen legal L is
         * represented; the next loop seeds R without recomputing upstream. */
        state.phase = 1; state.fraction = state.tau_cursor = 0; state.piece = state.pieces = 0;
      }
      else if (state.has_left) {
        state.phase = 1; state.fraction = state.tau_cursor = 0; state.piece = state.pieces = 0;
      }
      else {
        ptr<DeepCompactOutputSummary>(e.summaries)[pixel] =
            {state.log_denominator, state.log_encoded, state.has_surface, 0};
        state.phase = 2; return;
      }
    }
    const auto &left = state.left;
    if (left.q == 0 && !compact_has_colour(left)) {
      state.has_left = 0; state.phase = 0; continue;
    }
    const bool interval = left.back > left.front;
    const bool opaque = left.log_after == -INFINITY && left.log_before != -INFINITY;
    const double tau = left.log_before == left.log_after ? 0 : left.log_before - left.log_after;
    if (isnan(tau) || tau < 0 || (interval && opaque)) {
      const bool report = atomicCAS(ptr<uint32_t>(p.status) + 3, 0u, 4802u) == 0;
      if constexpr (Trace) {
        if (report)
        printf("DEEP_ENCODE_TAU_INVALID pixel=%u q=%.17g z=%.9g back=%.9g before=%.17g after=%.17g\n",
               pixel, left.q, double(left.front), double(left.back), left.log_before, left.log_after);
      }
      compact_fail(p, DEEP_GPU_FRAME_NUMERIC_MASS_RANGE); return;
    }
    const double alpha_cap = policy.half ? .99951171875 : double(nextafterf(1.0f, 0.0f));
    const double h_format = -log1p(-alpha_cap);
    double fraction = 1, log_colour_fraction = 0, next_tau = tau;
    double alpha = opaque ? 1 : -expm1(-tau);
    if (interval && tau > 0) {
      const double opacity = -expm1(-tau);
      const double current_tau = state.tau_cursor;
      double epsilon = fmax(double(policy.alpha_tolerance) - .004, .0001);
      if constexpr (Physical) {
        for (int c = 0; c < 3; c++)
          if (left.rgb[c] != 0)
            epsilon = fmin(epsilon, left.rgb_epsilon[c] * left.q / fabs(left.rgb[c]));
      }
      const double transmission = exp(left.log_before - current_tau);
      const double h_repr = transmission > 0 ? sqrt(8.0 * epsilon / transmission) : h_format;
      next_tau = fmin(tau, current_tau + fmin(h_format, h_repr));
      fraction = next_tau == tau ? 1.0 : -expm1(-next_tau) / opacity;
      if (!(next_tau > current_tau)) {
        const bool report = atomicCAS(ptr<uint32_t>(p.status) + 3, 0u, 4803u) == 0;
        if constexpr (Trace) {
          if (report)
          printf("DEEP_ENCODE_PROGRESS_INVALID pixel=%u tau=%.17g cursor=%.17g next=%.17g epsilon=%.17g\n",
                 pixel, tau, current_tau, next_tau, epsilon);
        }
        compact_fail(p, DEEP_GPU_FRAME_NUMERIC_MASS_RANGE); return;
      }
      log_colour_fraction = -current_tau + log(-expm1(-(next_tau - current_tau))) - log(opacity);
      alpha = -expm1(-(next_tau - current_tau));
    }
    else if (!interval && !opaque && tau > 0) {
      if (!state.pieces) {
        const double count = ceil(tau / h_format);
        if (!(count >= 1 && count <= UINT32_MAX)) {
          compact_fail(p, DEEP_GPU_FRAME_FORMAT_RANGE); return;
        }
        state.pieces = uint32_t(count);
      }
      const double h = tau / state.pieces;
      alpha = -expm1(-h);
      log_colour_fraction = -double(state.piece) * h + log(alpha) - log(-expm1(-tau));
    }
    DeepGPUFrameSample sample{};
    sample.z = interval ? float(double(left.front) +
                                  (double(left.back) - left.front) * state.fraction) : left.front;
    sample.z_back = interval ? float(double(left.front) +
                                       (double(left.back) - left.front) * fraction) : left.front;
    /* FLOAT depth may collapse only this representation subsegment. Its finite
     * opacity remains a same-Z packet and never closes the remaining tail. */
    sample.a = policy.half ? __half2float(__float2half_rn(float(alpha))) : float(alpha);
    if (sample.a == 1 && !opaque) { compact_fail(p, DEEP_GPU_FRAME_FORMAT_RANGE); return; }
    if (sample.a < 0 || sample.a > 1 || !isfinite(sample.a)) {
      compact_fail(p, DEEP_GPU_FRAME_INVALID_ENCODING); return;
    }
    if constexpr (Physical) {
      float *rgb = &sample.r;
      for (int c = 0; c < 3; c++) {
        double colour = 0;
        if (left.rgb[c] != 0) {
          const double magnitude = exp(log(fabs(left.rgb[c])) + log_colour_fraction - state.log_encoded);
          colour = copysign(magnitude, left.rgb[c]);
        }
        if (!isfinite(colour) || fabs(colour) > (policy.half ? 65504.0 : double(FLT_MAX))) {
          compact_fail(p, DEEP_GPU_FRAME_FORMAT_RANGE); return;
        }
        rgb[c] = policy.half ? __half2float(__float2half_rn(float(colour))) : float(colour);
      }
    }
    else if (e.simple && !policy.alpha_only && left.kind == DEEP_COMPACT_BLACK) sample.r = -1;
    const uint32_t slot = compact_colour_claim(controls, e.page_capacity);
    if (slot == UINT32_MAX) { atomicAdd(controls + 1, 1u); return; }
    if (state.sequence == UINT32_MAX) { compact_fail(p, DEEP_GPU_FRAME_CAPACITY); return; }
    ptr<DeepGPUFrameSample>(e.samples)[slot] = sample;
    if (interval && sample.z == sample.z_back) atomicAdd(controls + 3, 1u);
    ptr<uint64_t>(e.keys)[slot] = (uint64_t(pixel) << 32) | state.sequence++;
    if (sample.a > 0 && left.kind != DEEP_COMPACT_BLACK)
      state.log_denominator = compact_logadd(state.log_denominator,
                                             state.log_encoded + log(double(sample.a)));
    state.log_encoded += sample.a == 1 ? -INFINITY : log1p(-double(sample.a));
    bool done = true;
    if (interval) {
      state.fraction = fraction; state.tau_cursor = next_tau;
      done = tau == 0 || next_tau == tau;
    }
    else if (state.pieces) done = ++state.piece == state.pieces;
    if (done) { state.has_left = 0; state.phase = 0; }
  }
}

template<bool Physical, bool Trace = true>
__device__ void compact_encode_pixel(const DeepCompactEncodeParameters &e, const uint32_t pixel)
{
  auto *persistent = ptr<CompactEncodeState<Physical>>(e.states) + pixel;
  if (persistent->phase == 2) return;
  /* Only this lane owns the pixel. Keep its merge and representation state
   * private throughout this bounded quantum; publish it once at a yield or
   * completion boundary instead of repeatedly loading/storing global fields. */
  auto state = *persistent;
  compact_encode_pixel_state<Physical, Trace>(e, pixel, state);
  *persistent = state;
}
}  // namespace

extern "C" __global__ void deep_gpu_frame_compact_encode_init(const DeepGPUFrameDispatch d)
{
  const auto e = *ptr<const DeepCompactEncodeParameters>(d.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= e.pixel_count) return;
  if (e.physical) {
    CompactEncodeState<true> state{}; state.log_denominator = -INFINITY;
    ptr<CompactEncodeState<true>>(e.states)[i] = state;
  }
  else {
    CompactEncodeState<false> state{}; state.log_denominator = -INFINITY;
    ptr<CompactEncodeState<false>>(e.states)[i] = state;
  }
}

extern "C" __global__ void deep_gpu_frame_compact_encode_page(const DeepGPUFrameDispatch d)
{
  const auto e = *ptr<const DeepCompactEncodeParameters>(d.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= e.pixel_count) return;
  if (e.physical) compact_encode_pixel<true>(e, i);
  else compact_encode_pixel<false>(e, i);
}

#define DEEP_COMPACT_ENCODE_ENTRY(Name, Physical, Trace) \
  extern "C" __global__ void Name(const DeepGPUFrameDispatch d) \
  { \
    const auto e = *ptr<const DeepCompactEncodeParameters>(d.a); \
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; \
    if (i < e.pixel_count) compact_encode_pixel<Physical, Trace>(e, i); \
  }
DEEP_COMPACT_ENCODE_ENTRY(deep_gpu_frame_compact_encode_simple, false, false)
DEEP_COMPACT_ENCODE_ENTRY(deep_gpu_frame_compact_encode_physical, true, false)
DEEP_COMPACT_ENCODE_ENTRY(deep_gpu_frame_compact_encode_simple_trace, false, true)
DEEP_COMPACT_ENCODE_ENTRY(deep_gpu_frame_compact_encode_physical_trace, true, true)
#undef DEEP_COMPACT_ENCODE_ENTRY

extern "C" __global__ void deep_gpu_frame_compact_final_ranges(const DeepGPUFrameDispatch d)
{
  const uint32_t pixel = blockIdx.x * blockDim.x + threadIdx.x;
  if (pixel >= d.n) return;
  const auto input = ptr<const DeepGPUFrameRange>(d.a)[pixel];
  const uint32_t population = ptr<const DeepGPUFramePixel>(d.c)[pixel].population;
  ptr<DeepGPUFrameFinalRange>(d.b)[pixel] = {input.offset + d.base, input.count, population, d.kind};
}

extern "C" __global__ void deep_gpu_frame_compact_payload_ids(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < d.n) ptr<uint32_t>(d.c)[i] = i;
}
