/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

namespace {
__device__ DeepGPUFrameValue decode(const DeepGPUFrameValue front,
                                   const DeepGPUFrameSample sample,
                                   const float fraction)
{
  /* Form opacity directly: subtracting exp(x) from one loses relative accuracy
   * for tiny alpha and corrupts RGB even when the stored HALF sample is valid. */
  const float opacity = fraction == 1 ? sample.a :
                        sample.a < 1 ? -expm1f(log1pf(-sample.a) * fraction) :
                                       (fraction == 0 ? 0 : 1);
  const float weight = sample.a > 0 ? opacity / sample.a : fraction;
  return {front.t * (1 - opacity),
          fmaf(front.t * weight, sample.r, front.r),
          fmaf(front.t * weight, sample.g, front.g),
          fmaf(front.t * weight, sample.b, front.b)};
}

__device__ float residual(const DeepGPUFrameValue actual,
                          const DeepGPUFrameValue expected,
                          const DeepGPUFrameConsumer config)
{
  const float dt = fabsf(actual.t - expected.t);
  float result = config.alpha_tolerance > 0 ? dt / config.alpha_tolerance :
                                             (dt == 0 ? 0 : INFINITY);
  if (!config.alpha_only) {
    const float *a = &actual.r, *b = &expected.r;
    for (int c = 0; c < 3; c++) {
      const float budget = config.rgb_absolute + config.rgb_relative * fmaxf(1, fabsf(b[c]));
      const float delta = fabsf(a[c] - b[c]);
      result = fmaxf(result, budget > 0 ? delta / budget : (delta == 0 ? 0 : INFINITY));
    }
  }
  return isfinite(result) ? result : INFINITY;
}

__device__ bool differs(const DeepGPUFrameValue a, const DeepGPUFrameValue b,
                         const bool alpha_only)
{
  return a.t != b.t || (!alpha_only && (a.r != b.r || a.g != b.g || a.b != b.b));
}

__device__ bool sample_for(const DeepGPUFrameValue front,
                          const DeepGPUFrameValue back,
                          const float z,
                          const float z_back,
                          const DeepGPUFrameConsumer config,
                          DeepGPUFrameSample &out)
{
  if (!isfinite(front.t) || !isfinite(back.t) || front.t < 0 || front.t > 1 ||
      back.t < 0 || back.t > 1 ||
      (back.t > front.t && back.t - front.t > config.alpha_tolerance))
    return false;
  out = {0, 0, 0, 0, z, z_back};
  if (front.t > 0) {
    /* A previous HALF opacity can put the carried decoder just below the
     * reference transmission. Do not demand negative opacity to undo that
     * accepted rounding; retain zero opacity and check the true residual. */
    out.a = fminf(1, fmaxf(0, 1 - back.t / front.t));
    if (!config.alpha_only) {
      out.r = (back.r - front.r) / front.t;
      out.g = (back.g - front.g) / front.t;
      out.b = (back.b - front.b) / front.t;
    }
  }
  if (config.half) {
    out.r = __half2float(__float2half_rn(out.r));
    out.g = __half2float(__float2half_rn(out.g));
    out.b = __half2float(__float2half_rn(out.b));
    out.a = __half2float(__float2half_rn(out.a));
  }
  /* Quantization must not close a still-transmitting prefix: later native
   * radiance may be substantial even through a very small transmission. The
   * carried decoder and ordinary residual check decide whether this rounding
   * is acceptable; no radiance is discarded to make opacity fit. */
  if (out.a == 1 && back.t > 0 && front.t > 0)
    out.a = config.half ? 0.99951171875f : nextafterf(1.0f, 0.0f);
  return isfinite(out.r) && isfinite(out.g) && isfinite(out.b) &&
         isfinite(out.a) && out.a >= 0 && out.a <= 1;
}

__device__ float warp_max(float value)
{
  for (int offset = 16; offset; offset >>= 1)
    value = fmaxf(value, __shfl_xor_sync(0xffffffffu, value, offset));
  return value;
}
}  // namespace

/* One cooperative warp per consumer/pixel. Candidate residuals are checked by
 * lanes in parallel; the accepted quantized decoder state is carried forward. */
extern "C" __global__ void deep_gpu_frame_fit(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameEncodeParameters>(dispatch.a);
  const uint32_t slot = dispatch.base * p.pixel_count + blockIdx.x;
  if (slot >= p.pixel_count * p.consumer_count)
    return;
  const uint32_t lane = threadIdx.x;
  const uint32_t pixel_index = slot % p.pixel_count, consumer_index = slot / p.pixel_count;
  const auto pixel = ptr<const DeepGPUFramePixel>(p.pixels)[pixel_index];
  const auto config = ptr<const DeepGPUFrameConsumer>(p.consumers)[consumer_index];
  auto *status = ptr<uint32_t>(p.status);
  auto *ranges = ptr<DeepGPUFrameRange>(p.ranges);
  auto *counts = ptr<uint32_t>(p.scan_counts);
  if (config.reuse < consumer_index) {
    if (!lane) {
      auto previous = ranges[config.reuse * p.pixel_count + pixel_index];
      previous.consumer = consumer_index;
      ranges[slot] = previous; counts[slot] = previous.count;
    }
    return;
  }
  const uint64_t source = uint64_t(2) * (uint64_t(consumer_index) * p.cut_count + pixel.cut_begin);
  if (!pixel.cut_count || !range(pixel.cut_begin, pixel.cut_count, p.cut_count) ||
      source + uint64_t(2) * pixel.cut_count > p.sample_capacity ||
      !isfinite(config.alpha_tolerance) || config.alpha_tolerance < 0 ||
      config.alpha_tolerance > 1 || !isfinite(config.depth_tolerance) ||
      config.depth_tolerance < 0 || !isfinite(config.rgb_absolute) ||
      !isfinite(config.rgb_relative) || config.rgb_absolute < 0 || config.rgb_relative < 0)
  {
    if (!lane) {
      atomicOr(status, DEEP_GPU_FRAME_INVALID_ENCODING);
      ranges[slot] = {0, 0, uint32_t(source), consumer_index}; counts[slot] = 0;
    }
    return;
  }
  const auto *knots = ptr<const DeepGPUFrameKnot>(p.values) + pixel.cut_begin;
  const auto *cuts = ptr<const float>(p.cuts) + pixel.cut_begin;
  auto *samples = ptr<DeepGPUFrameSample>(p.sparse) + source;
  DeepGPUFrameValue decoded{1, 0, 0, 0};
  uint32_t written = 0, start = 0;
  for (;;) {
    /* Keep a real discontinuity as a point sample. Quantization is checked
     * against its actual decoded prefix instead of a hidden host certificate. */
    if (differs(knots[start].before, knots[start].after, config.alpha_only)) {
      DeepGPUFrameSample point{};
      const bool valid = sample_for(decoded, knots[start].after, cuts[start], cuts[start], config, point);
      const auto after = decode(decoded, point, 1);
      if (!valid || residual(after, knots[start].after, config) > 1) {
        if (!lane) {
          atomicOr(status, DEEP_GPU_FRAME_INVALID_ENCODING);
          const uint32_t first_error = atomicCAS(status + 3, 0u,
                                                 1u + (slot << 3) + uint32_t(!valid));
          if (!first_error && dispatch.kind)
            printf("DEEP_GPU_FIT_FAILURE slot=%u cut=%u z=%.9g valid=%u "
                   "decoded=%.9g,%.9g,%.9g,%.9g reference=%.9g,%.9g,%.9g,%.9g "
                   "sample=%.9g,%.9g,%.9g,%.9g residual=%.9g\n",
                   slot, start, cuts[start], uint32_t(valid), decoded.t, decoded.r, decoded.g,
                   decoded.b, knots[start].after.t, knots[start].after.r, knots[start].after.g,
                   knots[start].after.b, point.a, point.r, point.g, point.b,
                   residual(after, knots[start].after, config));
        }
        break;
      }
      if (!lane) samples[written] = point;
      written++; decoded = after;
    }
    if (start + 1 == pixel.cut_count)
      break;
    uint32_t accepted = start + 1, candidate = accepted;
    DeepGPUFrameSample best{};
    bool have_best = false;
    for (;;) {
      DeepGPUFrameSample sample{};
      const bool valid = sample_for(decoded, knots[candidate].before,
                                     cuts[start], cuts[candidate], config, sample);
      float worst = valid ? 0 : INFINITY;
      for (uint32_t cut = start + lane; cut <= candidate; cut += 32) {
        const float f = (cuts[cut] - cuts[start]) / (cuts[candidate] - cuts[start]);
        const auto actual = decode(decoded, sample, f);
        worst = fmaxf(worst, residual(actual,
                                       cut == start ? knots[cut].after : knots[cut].before, config));
        if (cut != start && cut != candidate &&
            differs(knots[cut].before, knots[cut].after, config.alpha_only))
        {
          /* AA camera cuts may be coalesced only under the actual colour,
           * opacity and depth quality settings. A large real jump cannot pass
           * both one-sided residuals; do not make each micro-cut permanent. */
          worst = fmaxf(worst, residual(actual, knots[cut].after, config));
          const float span = cuts[candidate] - cuts[start];
          const float scale = fmaxf(1, fminf(fabsf(cuts[start]), fabsf(cuts[candidate])));
          if (span > config.depth_tolerance * scale)
            worst = INFINITY;
        }
      }
      worst = warp_max(worst);
      if (worst > 1)
        break;
      best = sample; accepted = candidate; have_best = true;
      if (!config.depth_tolerance || candidate + 1 == pixel.cut_count)
        break;
      const uint32_t next = start + 2 * (candidate - start);
      candidate = next < pixel.cut_count ? next : pixel.cut_count - 1;
      if (candidate == accepted)
        break;
    }
    if (!have_best) {
      if (!lane) {
        atomicOr(status, DEEP_GPU_FRAME_INVALID_ENCODING);
        atomicCAS(status + 3, 0u, 3u + (slot << 3));
      }
      break;
    }
    if (best.a != 0 || best.r != 0 || best.g != 0 || best.b != 0) {
      if (!lane) samples[written] = best;
      written++;
    }
    decoded = decode(decoded, best, 1);
    start = accepted;
  }
  if (!lane) {
    ranges[slot] = {0, written, uint32_t(source), consumer_index};
    counts[slot] = written;
  }
}

/* Hierarchical scans take O(n) work. The host submits levels using dimensions
 * only; it neither reads counts nor computes a prefix. */
extern "C" __global__ void deep_gpu_frame_scan(const DeepGPUFrameDispatch p)
{
  using Scan = cub::BlockScan<uint32_t, 256>;
  __shared__ typename Scan::TempStorage temporary;
  const uint32_t i = blockIdx.x * 256 + threadIdx.x;
  const auto *input = ptr<const uint32_t>(p.a);
  auto *output = ptr<uint32_t>(p.b), *totals = ptr<uint32_t>(p.c);
  const uint32_t value = i < p.n ? input[i] : 0;
  uint32_t prefix, total;
  Scan(temporary).ExclusiveSum(value, prefix, total);
  if (i < p.n) output[i] = prefix;
  if (!threadIdx.x) totals[blockIdx.x] = total;
}

extern "C" __global__ void deep_gpu_frame_scan_add(const DeepGPUFrameDispatch p)
{
  const uint32_t i = blockIdx.x * 256 + threadIdx.x;
  if (i < p.n)
    ptr<uint32_t>(p.a)[i] += ptr<const uint32_t>(p.b)[blockIdx.x];
}

extern "C" __global__ void deep_gpu_frame_pack(const DeepGPUFrameDispatch dispatch)
{
  const auto p = *ptr<const DeepGPUFrameEncodeParameters>(dispatch.a);
  const uint32_t slot = blockIdx.x;
  const uint32_t count = p.pixel_count * p.consumer_count;
  if (slot >= count)
    return;
  auto *ranges = ptr<DeepGPUFrameRange>(p.ranges);
  auto item = ranges[slot];
  const uint32_t offset = ptr<const uint32_t>(p.scan_offsets)[slot];
  if (offset > p.sample_capacity || item.count > p.sample_capacity - offset) {
    if (!threadIdx.x) atomicOr(ptr<uint32_t>(p.status), DEEP_GPU_FRAME_CAPACITY);
    return;
  }
  const auto *source = ptr<const DeepGPUFrameSample>(p.sparse) + item.source;
  auto *destination = ptr<DeepGPUFrameSample>(p.packed) + offset;
  for (uint32_t i = threadIdx.x; i < item.count; i += blockDim.x)
    destination[i] = source[i];
  __syncthreads();
  if (!threadIdx.x) {
    ranges[slot].offset = offset;
    /* The final range carries population after its sparse source copy joins. */
    ranges[slot].source = ptr<const DeepGPUFramePixel>(p.pixels)[slot % p.pixel_count].population;
    if (slot + 1 == count) *ptr<uint32_t>(p.output_count) = offset + item.count;
  }
}
