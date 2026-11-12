/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

#include "kernel/integrator/deep_bounded_types.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Checked host arithmetic is shared by admission, binding and migration. It
 * never estimates the native/driver reservation or available host RAM as zero. */
inline bool deep_bounded_add(const size_t a, const size_t b, size_t &out)
{
  if (b > std::numeric_limits<size_t>::max() - a) {
    return false;
  }
  out = a + b;
  return true;
}
inline bool deep_bounded_mul(const size_t a, const size_t b, size_t &out)
{
  if (a && b > std::numeric_limits<size_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

struct DeepBoundedArenaLayout {
  size_t bytes = 0;
  bool valid = true;

  size_t reserve(const size_t count, const size_t width, const size_t alignment = 16)
  {
    size_t padded, payload, end;
    if (!valid || !alignment || (alignment & (alignment - 1)) ||
        !deep_bounded_add(bytes, alignment - 1, padded) ||
        !deep_bounded_mul(count, width, payload))
    {
      valid = false;
      return 0;
    }
    padded &= ~(alignment - 1);
    if (!deep_bounded_add(padded, payload, end)) {
      valid = false;
      return 0;
    }
    bytes = end;
    return padded;
  }

  DeepBoundedPlanes planes(const uint64_t base, const size_t records, const bool physical,
                             const bool compact_identity = false)
  {
    DeepBoundedPlanes p;
    p.z = base + reserve(records, 4);
    p.z_back = base + reserve(records, 4);
    p.rgba = base + reserve(records, physical ? 16 : 4);
    p.object = base + reserve(records, 4);
    p.shader = compact_identity ? 0 : base + reserve(records, 4);
    p.normal = base + reserve(records, 4);
    return p;
  }
};

struct DeepBoundedAdmission {
  uint32_t pixels = 0, packet_samples = 0, roots = 0;
  size_t collector = 0, phase = 0, encoded = 0, source = 0, control = sizeof(DeepBoundedPacketControl), known_total = 0;

  static bool calculate(const uint32_t pixels,
                        const uint32_t samples,
                        const uint32_t native_states,
                        const DeepBoundedPolicy policy,
                        const bool holdout,
                        const uint32_t families,
                        DeepBoundedAdmission &out,
                        const bool compact_identity = false)
  {
    if (!pixels || !samples || !families || policy == DeepBoundedPolicy::NONE) {
      return false;
    }
    size_t roots, slots, total;
    if (!deep_bounded_mul(pixels, samples, roots) || roots >= (size_t(1) << 30) ||
        (!compact_identity && roots > std::max(size_t(pixels), size_t(native_states))) ||
        !deep_bounded_mul(pixels, 512, slots))
    {
      return false;
    }
    const bool physical = policy == DeepBoundedPolicy::PHYSICAL;
    DeepBoundedAdmission r;
    r.pixels = pixels;
    r.packet_samples = samples;
    r.roots = uint32_t(roots);
    DeepBoundedArenaLayout c, raw, final, encoded, source;
    c.planes(0, slots, physical, compact_identity);
    c.reserve(size_t(pixels) * 16, 4);
    c.reserve(size_t(pixels) * 16, 4);
    c.reserve(size_t(pixels) * 2 + 2, sizeof(uint32_t));
    raw.planes(0, roots * 32, physical, compact_identity);
    raw.reserve(roots, 4);
    if (holdout) {
      raw.reserve(roots, 8);
    }
    if (physical) {
      raw.reserve(roots, 12);
    }
    final.planes(0, size_t(pixels) * 32 * std::min(families, 2u), physical, compact_identity);
    final.reserve(size_t(pixels) * std::min(families, 2u), 4);
    final.reserve(pixels, 8, 8);
    /* A bounded list and count for cooperative final pressure trials. */
    final.reserve(pixels, 4);
    final.reserve(1, 4);
    /* Recursive scan totals/offsets include every level and uniform-add input. */
    for (size_t n = (size_t(pixels) + 255) / 256; n; n = n == 1 ? 0 : (n + 255) / 256) {
      final.reserve(n, 8, 8);
      final.reserve(n, 8, 8);
    }
    encoded.reserve(1, sizeof(DeepBoundedEncodedHeader));
    encoded.reserve(pixels, 4);
    encoded.reserve(size_t(pixels) * 32, 4); /* Z */
    encoded.reserve(size_t(pixels) * 32, 4); /* ZBack */
    encoded.reserve(size_t(pixels) * 32, physical ? 16 : 4);
    if (policy == DeepBoundedPolicy::SIMPLE) {
      encoded.reserve(pixels, 4);
    }
    source.reserve(native_states, 4);
    if (physical) {
      source.reserve(native_states, 4);
      source.reserve(native_states, 4);
      source.reserve(native_states, 4);
    }
    if (!c.valid || !raw.valid || !final.valid || !encoded.valid || !source.valid) {
      return false;
    }
    r.collector = c.bytes;
    r.phase = std::max({raw.bytes, final.bytes, physical ? size_t(18560) : size_t(12416)});
    r.encoded = encoded.bytes;
    r.source = source.bytes;
    if (!deep_bounded_add(r.collector, r.phase, total) ||
        !deep_bounded_add(total, r.source, total) ||
        !deep_bounded_add(total, r.encoded, total) ||
        !deep_bounded_add(total, r.encoded, total) ||
        !deep_bounded_add(total, r.control, r.known_total))
    {
      return false;
    }
    out = r;
    return true;
  }
};

inline DeepBoundedAdmission deep_bounded_choose_packet(const uint32_t pixels,
                                                       const uint32_t max_samples,
                                                       const uint32_t native_states,
                                                       const DeepBoundedPolicy policy,
                                                       const bool holdout,
                                                       const uint32_t families,
                                                       const size_t available,
                                                       const DeepBoundedAdmission &retained = {},
                                                       const bool exact = false,
                                                       const bool compact_identity = false)
{
  DeepBoundedAdmission best;
  uint32_t lo = 1;
  uint32_t hi = max_samples;
  while (lo <= hi) {
    const uint32_t b = exact ? lo + (hi - lo) / 2 : lo;
    DeepBoundedAdmission candidate;
    if (!DeepBoundedAdmission::calculate(pixels, b, native_states, policy, holdout, families,
                                         candidate, compact_identity))
    {
      if (!exact) {
        break;
      }
      hi = b - 1;
      continue;
    }
    /* The reusable arenas only grow. A smaller edge Tile may admit a larger
     * packet, but still coexists with the previously allocated collector. */
    size_t resident = std::max(candidate.source, retained.source);
    const size_t collector = std::max(candidate.collector, retained.collector);
    const size_t phase = std::max(candidate.phase, retained.phase);
    const size_t encoded = std::max(candidate.encoded, retained.encoded);
    const size_t control = std::max(candidate.control, retained.control);
    if (!deep_bounded_add(resident, collector, resident) ||
        !deep_bounded_add(resident, phase, resident) ||
        !deep_bounded_add(resident, encoded, resident) ||
        !deep_bounded_add(resident, encoded, resident) ||
        !deep_bounded_add(resident, control, resident) || resident > available)
    {
      if (!exact) {
        break;
      }
      hi = b - 1;
      continue;
    }
    best = candidate;
    if (!exact && b > max_samples / 2) {
      break;
    }
    lo = exact ? b + 1 : b * 2;
  }
  return best;
}

/* Cold snapshot of one real device owner, not a division of one logical Tile.
 * Already charged owners are reusable by max(capacity, demand), never summed
 * twice or assumed absent just because the current slice is empty. */
struct DeepBoundedDeviceAdmission {
  uint32_t native_states = 0;
  DeepBoundedAdmission retained = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  size_t physical_capacity = SIZE_MAX;
  size_t adaptive_bytes = 0, adaptive_target = 0;
  bool compact_identity = false;
  bool pack_active = false;
};

inline bool deep_bounded_resident(const DeepBoundedAdmission &a,
                                 const DeepBoundedAdmission &retained,
                                 size_t &bytes)
{
  bytes = std::max(a.source, retained.source);
  return deep_bounded_add(bytes, std::max(a.collector, retained.collector), bytes) &&
         deep_bounded_add(bytes, std::max(a.phase, retained.phase), bytes) &&
         deep_bounded_add(bytes, std::max(a.encoded, retained.encoded), bytes) &&
         deep_bounded_add(bytes, std::max(a.encoded, retained.encoded), bytes) &&
         deep_bounded_add(bytes, std::max(a.control, retained.control), bytes);
}

/* Returns a common feasible packet size; caller may collect each owner's arena
 * cap using the same inputs. No allocation, device query or synchronization. */
inline DeepBoundedAdmission deep_bounded_choose_devices(
    const uint32_t pixels,
    const uint32_t max_samples,
    const DeepBoundedPolicy policy,
    const bool holdout,
    const uint32_t families,
    const size_t shared_remaining,
    const std::span<const DeepBoundedDeviceAdmission> devices)
{
  DeepBoundedAdmission best;
  uint32_t lo = 1, hi = max_samples;
  while (lo <= hi && !devices.empty()) {
    const uint32_t samples = lo + (hi - lo) / 2;
    size_t additional = 0;
    bool fits = true;
    DeepBoundedAdmission candidate;
    for (const auto &device : devices) {
      size_t before, after;
      before = device.retained.known_total;
      if (!DeepBoundedAdmission::calculate(pixels, samples, device.native_states, policy,
                                           holdout, families, candidate, device.compact_identity) ||
          !deep_bounded_resident(candidate, device.retained, after))
      {
        fits = false;
        break;
      }
      const size_t growth = after >= before ? after - before : SIZE_MAX;
      if (after < before ||
          growth > device.physical_capacity || !deep_bounded_add(additional, growth, additional) ||
          additional > shared_remaining)
      {
        fits = false;
        break;
      }
    }
    if (fits) {
      best = candidate;
      lo = samples + 1;
    }
    else {
      hi = samples - 1;
    }
  }
  return best;
}

struct DeepBoundedMigrationChunk {
  uint64_t pixels = 0, pixels_per_chunk = 0, bytes_per_pixel = 0;
  uint64_t payload_bytes = 0, chunks = 0;

  bool initialize(const uint64_t count,
                  const bool physical,
                  const uint64_t source_arena,
                  const uint64_t target_arena)
  {
    pixels = count;
    bytes_per_pixel = physical ? 18560 : 12416;
    if (!count) {
      pixels_per_chunk = payload_bytes = chunks = 0;
      return true;
    }
    if (count > UINT64_MAX / bytes_per_pixel) {
      return false;
    }
    const uint64_t cap = std::min({uint64_t(16 << 20), source_arena, target_arena,
                                   count * bytes_per_pixel});
    pixels_per_chunk = cap / bytes_per_pixel;
    if (!pixels_per_chunk) {
      return false;
    }
    payload_bytes = pixels_per_chunk * bytes_per_pixel;
    chunks = 1 + (count - 1) / pixels_per_chunk;
    return true;
  }
};

CCL_NAMESPACE_END
