/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

namespace {
__device__ uint32_t compact_geometry_hash(const uint32_t camera, const float z,
                                        const uint32_t object, const uint32_t prim,
                                        const uint32_t shader)
{
  uint32_t hash = camera * 0x9e3779b9u ^ __float_as_uint(z);
  hash = (hash ^ object) * 0x85ebca6bu;
  hash = (hash ^ prim) * 0xc2b2ae35u;
  return hash ^ shader;
}

/* A lookup interpolates only the frozen linear-T base. It never evaluates a ray
 * or changes AA knots when colour introduces another endpoint. */
__device__ double compact_base_log(const DeepCompactParameters &p, const uint32_t pixel,
                                 const float z, const bool before)
{
  const auto range = ptr<const DeepGPUFrameRange>(p.event_ranges)[pixel];
  const auto *knots = ptr<const DeepCompactKnot>(p.knots);
  uint32_t lo = range.offset, hi = lo + range.count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (knots[mid].z < z || (!before && knots[mid].z == z)) lo = mid + 1;
    else hi = mid;
  }
  if (before && lo < range.offset + range.count && knots[lo].z == z)
    return knots[lo].log_before;
  if (lo == range.offset) return 0;
  const auto &knot = knots[lo - 1];
  if (!(knot.next_z > knot.z) || z >= knot.next_z) return knot.log_next;
  const double f = (double(z) - knot.z) / (double(knot.next_z) - knot.z);
  return compact_logadd(knot.log_after + log1p(-f), knot.log_next + log(f));
}

__device__ float compact_source_depth(const DeepCompactColourSource &source, const double t)
{
  return source.flags & 0x80000000u ?
      float(sqrt((t - source.map_a) * (t - source.map_a) + source.map_b)) :
      float(source.map_a + source.map_b * t);
}

__device__ double compact_base_mass(const DeepCompactParameters &p, const uint32_t pixel,
                                  const float front, const float back)
{
  if (!(back > front)) return 0;
  const auto range = ptr<const DeepGPUFrameRange>(p.event_ranges)[pixel];
  const auto *knots = ptr<const DeepCompactKnot>(p.knots);
  uint32_t lo = range.offset, hi = lo + range.count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (knots[mid].z <= front) lo = mid + 1;
    else hi = mid;
  }
  if (lo == range.offset) return 0;
  const auto &knot = knots[lo - 1];
  return knot.next_z > knot.z ? knot.interval_q *
      ((double(back) - front) / (double(knot.next_z) - knot.z)) : 0;
}

__device__ bool compact_colour_nonzero(const DeepCompactColourSource &source)
{
  return source.rgb[0] != 0 || source.rgb[1] != 0 || source.rgb[2] != 0;
}

__device__ uint32_t compact_colour_claim(uint32_t *counter, const uint32_t capacity)
{
  uint32_t current = atomicAdd(counter, 0u);
  while (current < capacity) {
    const uint32_t old = atomicCAS(counter, current, current + 1);
    if (old == current) return current;
    current = old;
  }
  return UINT32_MAX;
}
}  // namespace

extern "C" __global__ void deep_gpu_frame_compact_colour_hash(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  const auto &primitive = ptr<const DeepCompactPrimitive>(d.a)[i];
  if (primitive.front != primitive.back || primitive.kind == DEEP_COMPACT_VOLUME) return;
  uint32_t slot = compact_geometry_hash(primitive.camera, primitive.front, primitive.object,
                                        primitive.prim, primitive.shader) % d.m;
  for (uint32_t probe = 0; probe < d.m; probe++, slot = (slot + 1) % d.m) {
    if (atomicCAS(ptr<uint32_t>(d.b) + slot, 0u, i + 1) == 0u) return;
  }
  atomicOr(ptr<uint32_t>(d.c), DEEP_GPU_FRAME_CAPACITY);
}

/* A source keeps its physical cursor on page exhaustion. A coefficient is read
 * once per source launch; no count pass replays the local discretization. */
extern "C" __global__ void deep_gpu_frame_compact_colour_page(const DeepGPUFrameDispatch d)
{
  const auto c = *ptr<const DeepCompactColourParameters>(d.a);
  const auto p = *ptr<const DeepCompactParameters>(c.base);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= c.source_count) return;
  const auto &source = ptr<const DeepCompactColourSource>(c.sources)[i];
  auto &cursor = ptr<DeepCompactColourCursor>(c.cursors)[i];
  auto *control = ptr<uint32_t>(c.controls);
  if (cursor.done) return;
  if (source.camera >= p.camera_count) {
    compact_fail(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
  }
  const uint32_t rank = ptr<const uint32_t>(p.camera_map)[source.camera];
  if (rank == UINT32_MAX || !compact_colour_nonzero(source)) { cursor.done = 1; return; }
  const uint32_t pixel = uint32_t(ptr<const uint64_t>(p.camera_keys)[rank] >> 32);
  const uint32_t population = ptr<const DeepGPUFramePixel>(p.pixels)[pixel].population;
  if (!population) { compact_fail(p, DEEP_GPU_FRAME_INVALID_POPULATION); return; }
  const double scale = double(c.exposure) / population;
  if (source.kind == 0) {
    uint32_t target = UINT32_MAX;
    if (c.hash_capacity) {
      uint32_t slot = compact_geometry_hash(source.camera, source.z, source.object,
                                            source.prim, source.shader) % c.hash_capacity;
      for (uint32_t probe = 0; probe < c.hash_capacity; probe++, slot = (slot + 1) % c.hash_capacity) {
        const uint32_t entry = ptr<const uint32_t>(c.hash)[slot];
        if (!entry) break;
        const auto &primitive = ptr<const DeepCompactPrimitive>(p.primitives)[entry - 1];
        if (primitive.camera == source.camera && primitive.front == source.z &&
            primitive.back == source.z && primitive.object == source.object &&
            primitive.prim == source.prim && primitive.shader == source.shader) {
          target = entry - 1; break;
        }
      }
    }
    if (target != UINT32_MAX) {
      for (int channel = 0; channel < 3; channel++)
        atomicAdd(ptr<double>(c.point_rgb) + 3ull * target + channel, double(source.rgb[channel]) * scale);
      cursor.done = 1; return;
    }
    const uint32_t slot = compact_colour_claim(control, c.page_capacity);
    if (slot == UINT32_MAX) { atomicAdd(control + 1, 1u); return; }
    DeepCompactColourPiece piece{};
    piece.front = piece.back = source.z; piece.pixel = pixel; piece.target = UINT32_MAX;
    piece.camera = source.camera; piece.kind = DEEP_COMPACT_COLOUR;
    for (int channel = 0; channel < 3; channel++) piece.rgb[channel] = source.rgb[channel] * scale;
    ptr<DeepCompactColourPiece>(c.pieces)[slot] = piece;
    cursor.done = 1; return;
  }
  const double front = source.physical_front, back = source.physical_back, length = back - front;
  if (!(length > 0)) { compact_fail(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return; }
  const double opacity = -expm1(-(double(source.sigma[0]) + source.sigma[1] + source.sigma[2]) * length / 3.0);
  const float encoded = c.alpha_half ? __half2float(__float2half_rn(float(opacity))) : float(opacity);
  if (encoded == 0) { cursor.done = 1; return; }
  const double log_t = compact_base_log(p, pixel, source.z, true);
  const double transmission = exp(log_t);
  double at = cursor.reserved ? cursor.physical : front;
  while (at < back) {
    double step = back - at;
    for (int channel = 0; channel < 3; channel++) {
      const double total = double(source.rgb[channel]) * scale;
      if (total == 0) continue;
      const double sigma = source.sigma[channel];
      const double budget = fmax(fmax(.006 * transmission, .002 * fabs(total)) -
                                  .004 * transmission, .0001 * transmission);
      if (!(budget > 0)) { compact_fail(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return; }
      if (source.flags & 0x80000000u) {
        /* Bounding absolute colour change also bounds the chord error under
         * the nonlinear radial-depth map. The zero-sigma limit is explicit. */
        const double density = sigma > 0 ? fabs(total) * sigma * exp(-sigma * (at - front)) /
                                              -expm1(-sigma * length) : fabs(total) / length;
        if (density > 0) step = fmin(step, budget / density);
      }
      else if (sigma > 0) {
        const double amplitude = fabs(total) * exp(-sigma * (at - front)) / -expm1(-sigma * length);
        if (amplitude > 0) step = fmin(step, sqrt(8.0 * budget / amplitude) / sigma);
      }
    }
    const double next = fmin(back, at + step);
    const float a = at == front ? source.z : compact_source_depth(source, at);
    const float b = next == back ? source.z_back : compact_source_depth(source, next);
    if (!(next > at) || b < a || !isfinite(a) || !isfinite(b)) {
      compact_fail(p, DEEP_GPU_FRAME_INVALID_RADIANCE); return;
    }
    const uint32_t slot = compact_colour_claim(control, c.page_capacity);
    if (slot == UINT32_MAX) { atomicAdd(control + 1, 1u); return; }
    DeepCompactColourPiece piece{};
    piece.front = a; piece.back = b; piece.pixel = pixel; piece.target = UINT32_MAX;
    piece.camera = source.camera;
    piece.kind = b == a ? DEEP_COMPACT_COLLAPSED_VOLUME : DEEP_COMPACT_VOLUME;
    for (int channel = 0; channel < 3; channel++) {
      const double sigma = source.sigma[channel];
      const double fraction = sigma > 0 ? exp(-sigma * (at - front)) * -expm1(-sigma * (next - at)) /
                                                -expm1(-sigma * length) : (next - at) / length;
      piece.rgb[channel] = double(source.rgb[channel]) * scale * fraction;
    }
    ptr<DeepCompactColourPiece>(c.pieces)[slot] = piece;
    at = next; cursor.physical = at; cursor.reserved = 1;
  }
  cursor.done = 1;
}

extern "C" __global__ void deep_gpu_frame_compact_join_events(const DeepGPUFrameDispatch d)
{
  const auto p = *ptr<const DeepCompactParameters>(d.a);
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  DeepCompactJoinEvent event{};
  event.pixel = event.base = UINT32_MAX;
  if (i < p.event_count) {
    /* The retained primary event key is the immutable pixel assignment. */
    event.pixel = uint32_t(ptr<const uint64_t>(d.g)[i] >> 32);
    if (event.pixel != UINT32_MAX) {
      const auto &base = ptr<const DeepCompactKnot>(p.knots)[i];
      event.z = base.z; event.base = i; event.kind = base.kind;
      if (base.kind != DEEP_COMPACT_VOLUME && base.kind != DEEP_COMPACT_BLACK &&
          base.source < p.primitive_count)
        for (int c = 0; c < 3; c++) event.jump[c] = ptr<const double>(d.c)[3ull * base.source + c];
    }
  }
  else {
    const uint32_t index = i - p.event_count;
    const auto &piece = ptr<const DeepCompactColourPiece>(d.b)[index / 2];
    event.z = index & 1 ? piece.back : piece.front;
    event.pixel = piece.pixel; event.kind = piece.kind;
    if (piece.back > piece.front) {
      event.kind = index & 1 ? UINT32_MAX - 2u : UINT32_MAX - 1u;
      for (int c = 0; c < 3; c++)
        event.slope[c] = (index & 1 ? -1.0 : 1.0) * piece.rgb[c] /
                         (double(piece.back) - piece.front);
    }
    else if (!(index & 1)) {
      for (int c = 0; c < 3; c++) event.jump[c] = piece.rgb[c];
    }
    else event.pixel = UINT32_MAX;
  }
  ptr<DeepCompactJoinEvent>(d.d)[i] = event;
  ptr<uint64_t>(d.e)[i] = event.pixel == UINT32_MAX ? UINT64_MAX :
      (uint64_t(event.base != UINT32_MAX) << 32) | i;
  ptr<uint32_t>(d.f)[i] = i;
}

/* The second stable sort puts pure colour jumps before scalar absorption at
 * their Z, while keeping the scalar packet's original order. */
extern "C" __global__ void deep_gpu_frame_compact_join_keys(const DeepGPUFrameDispatch d)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= d.n) return;
  const auto &event = ptr<const DeepCompactJoinEvent>(d.a)[ptr<const uint32_t>(d.c)[i]];
  ptr<uint64_t>(d.b)[i] = event.pixel == UINT32_MAX ? UINT64_MAX :
      (uint64_t(event.pixel) << 32) | __float_as_uint(event.z);
}

struct CompactColourSlope {
  double rgb[3];
  int active;
};
struct CompactColourSlopeAdd {
  __device__ CompactColourSlope operator()(CompactColourSlope a, const CompactColourSlope b) const
  {
    for (int c = 0; c < 3; c++) a.rgb[c] += b.rgb[c];
    a.active += b.active;
    return a;
  }
};

/* Each colour endpoint queries the immutable piecewise-linear T with a binary
 * search; the RGB slope scan itself is linear in the joined endpoint count. */
extern "C" __global__ void deep_gpu_frame_compact_join_sweep(const DeepGPUFrameDispatch d)
{
  const auto p = *ptr<const DeepCompactParameters>(d.a);
  const uint32_t pixel = blockIdx.x;
  if (pixel >= p.pixel_count) return;
  using Scan = cub::BlockScan<CompactColourSlope, 256>;
  __shared__ typename Scan::TempStorage storage;
  __shared__ CompactColourSlope carry;
  if (!threadIdx.x) carry = {};
  __syncthreads();
  const auto range = ptr<const DeepGPUFrameRange>(d.d)[pixel];
  for (uint32_t start = 0; start < range.count; start += 256) {
    const uint32_t local = start + threadIdx.x, at = range.offset + local;
    DeepCompactJoinEvent event{};
    if (local < range.count)
      event = ptr<const DeepCompactJoinEvent>(d.b)[ptr<const uint32_t>(d.c)[at]];
    const int active = event.kind == UINT32_MAX - 1u ? 1 :
                       event.kind == UINT32_MAX - 2u ? -1 : 0;
    CompactColourSlope slope{{event.slope[0], event.slope[1], event.slope[2]}, active}, prefix;
    Scan(storage).InclusiveScan(slope, prefix, CompactColourSlopeAdd{});
    prefix = CompactColourSlopeAdd{}(carry, prefix);
    if (local < range.count) {
      CompactFitValue<true> point{}, interval{};
      point.front = point.back = event.z; point.kind = event.kind; point.source = UINT32_MAX;
      point.log_before = point.log_after = compact_base_log(p, pixel, event.z, true);
      if (event.base != UINT32_MAX) {
        const auto &base = ptr<const DeepCompactKnot>(p.knots)[event.base];
        point.q = base.point_q; point.log_before = base.log_before;
        point.log_after = base.log_after; point.source = base.source;
      }
      interval.front = interval.back = event.z; interval.kind = DEEP_COMPACT_VOLUME;
      interval.source = UINT32_MAX;
      interval.log_before = interval.log_after = compact_base_log(p, pixel, event.z, false);
      if (local + 1 < range.count) {
        const auto &next = ptr<const DeepCompactJoinEvent>(d.b)[ptr<const uint32_t>(d.c)[at + 1]];
        interval.back = next.z;
        interval.q = compact_base_mass(p, pixel, interval.front, interval.back);
        /* Coincident packets have no intervening scalar interval. Looking up
         * before the first point at Z after looking up after the last point at
         * that same Z would otherwise manufacture an increasing transmission.
         * A zero-mass base interval also has exactly constant transmission. */
        if (interval.q > 0)
          interval.log_after = compact_base_log(p, pixel, next.z, true);
        if (interval.q < 0 || !isfinite(interval.q)) compact_fail(p, DEEP_GPU_FRAME_INVALID_OPTICAL);
      }
      for (int c = 0; c < 3; c++) {
        point.rgb[c] = event.jump[c];
        interval.rgb[c] = prefix.active ? prefix.rgb[c] * (double(interval.back) - interval.front) : 0;
        point.rgb_epsilon[c] = interval.rgb_epsilon[c] = INFINITY;
      }
      ptr<CompactFitValue<true>>(d.e)[2ull * at] = point;
      ptr<CompactFitValue<true>>(d.e)[2ull * at + 1] = interval;
    }
    __syncthreads();
    if (threadIdx.x == 255) {
      carry = prefix;
      if (!carry.active) for (int c = 0; c < 3; c++) carry.rgb[c] = 0;
    }
    __syncthreads();
  }
}
