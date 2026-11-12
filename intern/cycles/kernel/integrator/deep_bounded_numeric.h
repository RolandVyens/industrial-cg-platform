/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Internal scalar arithmetic shared by native CPU and CUDA owners. Include
 * after the platform address-space/error adapters inside a private namespace. */
constexpr uint32_t candidate_capacity = 4 * DEEP_BOUNDED_K;

struct Chain {
  DeepBoundedPlanes planes;
  uint64_t first, stride;
  uint32_t count;
  bool raw = false;
};

template<bool Physical>
__device__ DeepBoundedRecord<Physical> chain_record(const Chain &chain, const uint32_t index)
{
  auto record = deep_bounded_load<Physical>(chain.planes, chain.first + index * chain.stride);
  if (chain.raw) {
    deep_bounded_classify_raw_surface(record, chain.count);
  }
  else if (record.z_back > record.z &&
           (record.object & ~DEEP_BOUNDED_ELIGIBLE) != DEEP_BOUNDED_NONSURFACE)
  {
    record.object &= ~DEEP_BOUNDED_ELIGIBLE;
  }
  if (record.z_back > record.z && index + 1 < chain.count) {
    const float next_front = deep_bounded_ptr<const float>(
        chain.planes.z)[chain.first + (index + 1) * chain.stride];
    /* Independently compiled native kernels can round a shared boundary
     * differently. The following sample owns that boundary. */
    const float rounded_boundary = nextafterf(nextafterf(next_front, FLT_MAX), FLT_MAX);
    if (next_front >= record.z && next_front < record.z_back && record.z_back <= rounded_boundary)
    {
      record.z_back = next_front;
    }
  }
  return record;
}

template<bool Physical> struct Cursor {
  Chain chain;
  float weight;
  uint32_t index = 0;
  float transmission = 1.0f;
  float inside = 1.0f;
  float log_transmission = 0.0f;
  bool has_log = false;
  bool active = false;
  DeepBoundedRecord<Physical> record = {};

  __device__ void load()
  {
    if (index < chain.count) {
      record = chain_record<Physical>(chain, index);
    }
  }
  __device__ float edge() const
  {
    return index == chain.count ? FLT_MAX : active ? record.z_back : record.z;
  }
  __device__ void next()
  {
    transmission *= 1.0f - record.alpha;
    active = false;
    ++index;
    load();
  }
  __device__ void activate()
  {
    inside = 1.0f;
    has_log = false;
    active = true;
  }
  __device__ DeepBoundedRecord<Physical> point() const
  {
    auto result = record;
    result.alpha *= transmission * weight;
    if constexpr (Physical) {
      for (int c = 0; c < 3; ++c) {
        result.rgb[c] *= weight;
      }
    }
    return result;
  }
  __device__ DeepBoundedRecord<Physical> slice(const float a, const float b)
  {
    auto result = record;
    result.z = a;
    result.z_back = b;
    float fraction = (b - a) / (record.z_back - record.z);
    float mass;
    if (a == record.z && b == record.z_back) {
      mass = record.alpha;
      fraction = 1.0f;
    }
    else if (record.alpha >= 1.0f) {
      mass = inside;
      fraction = inside;
    }
    else if (record.alpha < FLT_MIN) {
      mass = record.alpha * fraction;
    }
    else {
      if (!has_log) {
        log_transmission = log1pf(-record.alpha);
        has_log = true;
      }
      const float exponent = log_transmission * fraction;
      const float a_part = -expm1f(exponent);
      mass = inside * a_part;
      fraction = mass / record.alpha;
    }
    inside = fmaxf(0.0f, inside - mass);
    result.alpha = mass * transmission * weight;
    if constexpr (Physical) {
      for (int c = 0; c < 3; ++c) {
        result.rgb[c] *= fraction * weight;
      }
    }
    return result;
  }
};

template<bool Physical> __device__ bool valid_record(const DeepBoundedRecord<Physical> &r)
{
  if (!(r.z > 0.0f && r.z_back >= r.z && r.z_back < FLT_MAX && r.alpha >= 0.0f && r.alpha <= 1.0f))
  {
    return false;
  }
  if constexpr (Physical) {
    return isfinite(r.rgb[0]) && isfinite(r.rgb[1]) && isfinite(r.rgb[2]);
  }
  return true;
}

template<bool Physical>
__device__ uint32_t
scale_chain(const Chain &input, const Chain &output, const float weight, const uint64_t status)
{
  float native_prefix = 0.0f;
  float prefix = 0.0f;
  for (uint32_t i = 0; i < input.count; ++i) {
    auto r = chain_record<Physical>(input, i);
    if (!valid_record(r)) {
      deep_bounded_error(status, DEEP_BOUNDED_NUMERIC);
      return 0;
    }
    const float q = (1.0f - native_prefix) * r.alpha;
    native_prefix = deep_bounded_unit(native_prefix + q);
    r.alpha = weight == 1.0f ? r.alpha : deep_bounded_local_alpha(weight * q, prefix);
    prefix = deep_bounded_unit(prefix + weight * q);
    if constexpr (Physical) {
      for (int c = 0; c < 3; ++c) {
        r.rgb[c] *= weight;
      }
    }
    deep_bounded_store(output.planes, output.first + i * output.stride, r);
  }
  return input.count;
}

template<bool Physical>
__device__ __forceinline__ uint32_t generate_candidates(const Chain &a,
                                                        const Chain &b,
                                                        const float wa,
                                                        const float wb,
                                                        DeepBoundedRecord<Physical> *candidates,
                                                        const uint32_t capacity,
                                                        const uint64_t status)
{
  Cursor<Physical> left{a, wa}, right{b, wb};
  left.load();
  right.load();
  uint32_t count = 0;
  float last = fminf(left.edge(), right.edge());
  while (left.index < a.count || right.index < b.count) {
    if ((left.index < a.count && !valid_record(left.record)) ||
        (right.index < b.count && !valid_record(right.record)))
    {
      deep_bounded_error(status, DEEP_BOUNDED_NUMERIC);
      return UINT32_MAX;
    }
    if constexpr (!Physical) {
      if (!left.active && !right.active) {
        const bool left_first = left.index < a.count &&
                                (right.index == b.count || left.record.z <= right.record.z);
        if (left_first && left.record.z == left.record.z_back) {
          if (left.record.z < last || count >= capacity) {
            deep_bounded_error(status, DEEP_BOUNDED_RANGE);
            return UINT32_MAX;
          }
          last = left.record.z;
          candidates[count++] = left.point();
          left.next();
          continue;
        }
        if (!left_first && right.record.z == right.record.z_back) {
          if (right.record.z < last || count >= capacity) {
            deep_bounded_error(status, DEEP_BOUNDED_RANGE);
            return UINT32_MAX;
          }
          last = right.record.z;
          candidates[count++] = right.point();
          right.next();
          continue;
        }
      }
    }
    const float edge = fminf(left.edge(), right.edge());
    if (edge < last || count >= capacity) {
      deep_bounded_error(status, DEEP_BOUNDED_RANGE);
      return UINT32_MAX;
    }
    if (edge > last && (left.active || right.active)) {
      auto r = left.active ? left.slice(last, edge) : right.slice(last, edge);
      if (left.active && right.active) {
        const auto other = right.slice(last, edge);
        const uint32_t eligible = (deep_bounded_mature_eligible(r.object) ||
                                   deep_bounded_mature_eligible(other.object)) ?
                                      DEEP_BOUNDED_ELIGIBLE :
                                      0u;
        if (other.alpha > r.alpha) {
          r.object = other.object;
          r.shader = other.shader;
          r.normal = other.normal;
        }
        const uint32_t object = r.object & ~DEEP_BOUNDED_ELIGIBLE;
        r.object = object | (object == DEEP_BOUNDED_NONSURFACE ? eligible : 0u);
        r.alpha += other.alpha;
        if constexpr (Physical) {
          for (int c = 0; c < 3; ++c) {
            r.rgb[c] += other.rgb[c];
          }
        }
      }
      candidates[count++] = r;
    }
    Cursor<Physical> *streams[2] = {&left, &right};
    for (auto *cursor : streams) {
      if (cursor->active && cursor->record.z_back == edge) {
        cursor->next();
      }
      while (!cursor->active && cursor->index < cursor->chain.count && cursor->record.z == edge) {
        if (!valid_record(cursor->record)) {
          deep_bounded_error(status, DEEP_BOUNDED_NUMERIC);
          return UINT32_MAX;
        }
        if (cursor->record.z_back != edge) {
          cursor->activate();
          break;
        }
        if (count == capacity) {
          deep_bounded_error(status, DEEP_BOUNDED_CAPACITY);
          return UINT32_MAX;
        }
        candidates[count++] = cursor->point();
        cursor->next();
      }
    }
    last = edge;
  }
  return count;
}

template<bool Physical>
__device__ __forceinline__ uint32_t reduce_numeric(DeepBoundedRecord<Physical> *input,
                                                   const uint32_t count,
                                                   const DeepBoundedPlanes &output,
                                                   const uint64_t first,
                                                   const uint64_t stride,
                                                   const uint32_t capacity,
                                                   const DeepBoundedMergePolicy &policy,
                                                   const bool mature_colour,
                                                   const uint64_t status)
{
  if (mature_colour) {
    return deep_bounded_reduce_mature<Physical, false>(
        input, count, output, first, stride, capacity, policy);
  }
  return deep_bounded_reduce(input, count, output, first, stride, capacity, policy, false, status);
}

template<bool Physical, uint32_t Capacity>
__device__ __noinline__ uint32_t merge_nonempty_chains(const Chain &a,
                                                       const Chain &b,
                                                       const Chain &output,
                                                       const float wa,
                                                       const float wb,
                                                       const DeepBoundedMergePolicy merge,
                                                       const uint64_t status)
{
  DeepBoundedRecord<Physical> candidates[Capacity];
  const uint32_t count = generate_candidates(a, b, wa, wb, candidates, Capacity, status);
  if (count == UINT32_MAX)
    return 0;
  return reduce_numeric(candidates,
                        count,
                        output.planes,
                        output.first,
                        output.stride,
                        DEEP_BOUNDED_K,
                        merge,
                        true,
                        status);
}

/* Two surface points need no interval cursor or 128-record candidate frame.
 * The same stable depth order, mass weighting and ordinary gates still apply. */
template<bool Physical>
__device__ __noinline__ uint32_t merge_point_pair(const Chain &a,
                                                  const Chain &b,
                                                  const Chain &output,
                                                  const float wa,
                                                  const float wb,
                                                  const DeepBoundedMergePolicy merge,
                                                  const uint64_t status)
{
  DeepBoundedRecord<Physical> pair[2] = {chain_record<Physical>(a, 0),
                                         chain_record<Physical>(b, 0)};
  if (!valid_record(pair[0]) || !valid_record(pair[1])) {
    deep_bounded_error(status, DEEP_BOUNDED_NUMERIC);
    return 0;
  }
  pair[0].alpha *= wa;
  pair[1].alpha *= wb;
  if constexpr (Physical) {
    for (int c = 0; c < 3; ++c) {
      pair[0].rgb[c] *= wa;
      pair[1].rgb[c] *= wb;
    }
  }
  if (pair[1].z < pair[0].z) {
    const auto first = pair[0];
    pair[0] = pair[1];
    pair[1] = first;
  }
  return reduce_numeric(
      pair, 2, output.planes, output.first, output.stride, DEEP_BOUNDED_K, merge, true, status);
}

template<bool Physical>
__device__ uint32_t merge_chains(const Chain &a,
                                 const Chain &b,
                                 const Chain &output,
                                 const float wa,
                                 const float wb,
                                 const DeepBoundedMergePolicy merge,
                                 const uint64_t status)
{
  if (!a.count) {
    return scale_chain<Physical>(b, output, wb, status);
  }
  if (!b.count) {
    return scale_chain<Physical>(a, output, wa, status);
  }
  if (a.count == 1 && b.count == 1 &&
      deep_bounded_ptr<const float>(a.planes.z)[a.first] ==
          deep_bounded_ptr<const float>(a.planes.z_back)[a.first] &&
      deep_bounded_ptr<const float>(b.planes.z)[b.first] ==
          deep_bounded_ptr<const float>(b.planes.z_back)[b.first])
  {
    return merge_point_pair<Physical>(a, b, output, wa, wb, merge, status);
  }
  /* At most two endpoints per input record. A point and a split interval
   * each consume one endpoint. Leave one spare slot for the cursor guard. */
  if (a.count + b.count <= 4) {
    return merge_nonempty_chains<Physical, 9>(a, b, output, wa, wb, merge, status);
  }
  return merge_nonempty_chains<Physical, candidate_capacity>(a, b, output, wa, wb, merge, status);
}
