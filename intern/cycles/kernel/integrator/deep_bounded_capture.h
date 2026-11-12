/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#if __DEEP_BOUNDED_POLICY__ > 0

#  include "kernel/camera/camera.h"
#  include "kernel/integrator/deep_bounded_records.h"
#  include "kernel/integrator/state_util.h"

CCL_NAMESPACE_BEGIN

constexpr bool deep_bounded_physical = __DEEP_BOUNDED_POLICY__ == 3;
using DeepCaptureRecord = DeepBoundedRecord<deep_bounded_physical>;

ccl_device_forceinline const DeepBoundedBinding &deep_bounded_binding(KernelGlobals kg)
{
#  ifdef __KERNEL_CPU__
  return *kg->deep_bounded;
#  else
  (void)kg;
  return kernel_integrator_state.deep_bounded;
#  endif
}

ccl_device_forceinline IntegratorState deep_bounded_write_state(ConstIntegratorState state)
{
#  ifdef __KERNEL_CPU__
  /* Native CPU owns mutable path states; Film exposes a const view of them. */
  return const_cast<IntegratorStateCPU *>(state);
#  else
  return state;
#  endif
}

ccl_device_forceinline uint32_t deep_bounded_slot(const uint32_t source)
{
  return source & DEEP_BOUNDED_SLOT_MASK;
}

ccl_device_forceinline DeepBoundedSourceKind deep_bounded_kind(const uint32_t source)
{
  return DeepBoundedSourceKind(source >> 30);
}

ccl_device_forceinline bool deep_bounded_active(KernelGlobals kg)
{
#  ifdef __KERNEL_CPU__
  return kg->deep_bounded && deep_bounded_binding(kg).packet_device != 0;
#  else
  return deep_bounded_binding(kg).packet_device != 0;
#  endif
}

ccl_device_forceinline bool deep_bounded_camera_chain(KernelGlobals kg, ConstIntegratorState state)
{
  return deep_bounded_active(kg) &&
         (INTEGRATOR_STATE(state, path, visibility) & PATH_RAY_VISIBILITY_CAMERA) &&
         !(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SHADOW_CATCHER_PASS) &&
         deep_bounded_kind(INTEGRATOR_STATE(state, path, deep_bounded_slot_kind)) !=
             DeepBoundedSourceKind::EXCLUDED;
}

ccl_device_forceinline bool deep_bounded_camera_pixel(KernelGlobals kg,
                                                      ccl_private int &x,
                                                      ccl_private int &y)
{
  const auto &binding = deep_bounded_binding(kg);
  if (!binding.native_active_map) {
    return true;
  }
  const auto &control = *deep_bounded_ptr<const DeepBoundedPacketControl>(binding.packet_device);
  if (!control.native_columns) {
    return true;
  }
  const uint32_t rank = uint32_t(y) * control.native_columns + uint32_t(x);
  if (rank >= control.native_count) {
    return false; /* Padding in the last row of the scheduling grid. */
  }
  const uint32_t pixel = deep_bounded_ptr<const uint32_t>(binding.native_active_map)[rank];
  x = control.native_x + int(pixel % control.native_width);
  y = control.native_y + int(pixel / control.native_width);
  return true;
}

ccl_device_forceinline void deep_bounded_bind_camera(
    KernelGlobals kg, IntegratorState state, const int x, const int y, const int scheduled_sample)
{
  if (!deep_bounded_active(kg)) {
    return;
  }
  const auto &binding = deep_bounded_binding(kg);
  const auto &control = *deep_bounded_ptr<const DeepBoundedPacketControl>(binding.packet_device);
  const auto &packet = control.packet;
  const uint32_t dx = uint32_t(x) - uint32_t(packet.owned_x);
  const uint32_t dy = uint32_t(y) - uint32_t(packet.owned_y);
  uint32_t source = deep_bounded_source(0, DeepBoundedSourceKind::EXCLUDED);
  if (dx < packet.owned_w && dy < packet.owned_h) {
    const uint32_t sample = uint32_t(scheduled_sample) - uint32_t(packet.start_sample);
    if (sample < packet.num_samples) {
      const uint32_t pixel = dy * packet.owned_pitch + dx;
      const uint32_t rank = control.root_pitch == packet.plane_pitch ?
                                pixel :
                                deep_bounded_ptr<const uint32_t>(binding.active_map)[2 + pixel];
      if (rank < control.root_pitch) {
        const uint32_t root = rank * packet.num_samples + sample;
        source = deep_bounded_source(root, DeepBoundedSourceKind::UNBOUND);
      }
      else {
        deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_RANGE);
      }
    }
    else {
      deep_bounded_error(packet.status_ptr, DEEP_BOUNDED_RANGE);
    }
  }
  INTEGRATOR_STATE_WRITE(deep_bounded_write_state(state), path, deep_bounded_slot_kind) = source;
#  if defined(__KERNEL_GPU__) && (__DEEP_BOUNDED_POLICY__ == 1 || __DEEP_BOUNDED_POLICY__ == 2)
  if (control.track_camera && deep_bounded_kind(source) != DeepBoundedSourceKind::EXCLUDED) {
    atomic_fetch_and_add_uint32(&kernel_integrator_state.queue_counter->deep_bounded_camera_live, 1);
  }
#  endif
#  if __DEEP_BOUNDED_POLICY__ == 3
  INTEGRATOR_STATE_WRITE(deep_bounded_write_state(state), path, deep_bounded_anchor_z) = 0.0f;
#  endif
}

ccl_device_forceinline void deep_bounded_retry_raster(KernelGlobals kg,
                                                      ConstIntegratorState state,
                                                      ccl_private int &x,
                                                      ccl_private int &y)
{
  if (!deep_bounded_active(kg)) {
    return;
  }
  const uint32_t source = INTEGRATOR_STATE(state, path, deep_bounded_slot_kind);
  const auto &binding = deep_bounded_binding(kg);
  const auto &control = *deep_bounded_ptr<const DeepBoundedPacketControl>(binding.packet_device);
  if (deep_bounded_kind(source) != DeepBoundedSourceKind::EXCLUDED) {
    const auto &packet = control.packet;
    const uint32_t rank = deep_bounded_slot(source) / packet.num_samples;
    const uint32_t pixel = control.root_pitch == packet.plane_pitch ?
                               rank :
                               deep_bounded_ptr<const uint32_t>(binding.active_map)[
                                   2 + uint64_t(packet.plane_pitch) + rank];
    x = packet.owned_x + int(pixel % packet.owned_pitch);
    y = packet.owned_y + int(pixel / packet.owned_pitch);
  }
  else if (control.native_columns) {
    /* A packed halo ray has native Film identity but no owned Deep slot. */
    const int64_t first = int64_t(control.native_offset) + control.native_x +
                          int64_t(control.native_y) * control.native_stride;
    const uint32_t offset = uint32_t(
        int64_t(INTEGRATOR_STATE(state, path, render_pixel_index)) - first);
    x = control.native_x + int(offset % control.native_stride);
    y = control.native_y + int(offset / control.native_stride);
  }
}

ccl_device_forceinline void deep_bounded_camera_success(KernelGlobals kg,
                                                        ConstIntegratorState state,
                                                        const uint32_t bucket)
{
  if (!deep_bounded_active(kg)) {
    return;
  }
  const uint32_t source = INTEGRATOR_STATE(state, path, deep_bounded_slot_kind);
  if (deep_bounded_kind(source) == DeepBoundedSourceKind::UNBOUND) {
    uint32_t &header = deep_bounded_ptr<uint32_t>(
        deep_bounded_binding(kg).headers)[deep_bounded_slot(source)];
    header = DEEP_BOUNDED_VALID | (bucket << DEEP_BOUNDED_BUCKET_SHIFT);
  }
}

/* Kept out of the hot shading frame. The bounded scratch exists only when a
 * raw chain has reached capacity. Pressure writes never escape this camera. */
static __device__ __noinline__ uint32_t deep_bounded_make_raw_room(const DeepBoundedBinding &b,
                                                                  const uint32_t root)
{
  DeepCaptureRecord candidates[DEEP_BOUNDED_K];
  float prefix = 0.0f;
  for (uint32_t i = 0; i < DEEP_BOUNDED_K; ++i) {
    auto &record = candidates[i];
    record = deep_bounded_load<deep_bounded_physical>(b.raw, root + uint64_t(i) * b.packet.root_capacity);
    const float q = (1.0f - prefix) * record.alpha;
    record.alpha = q;
    prefix = deep_bounded_unit(prefix + q);
  }
  return deep_bounded_reduce(candidates, DEEP_BOUNDED_K, b.raw, root, b.packet.root_capacity,
                            DEEP_BOUNDED_K - 1, b.merge, false, b.packet.status_ptr);
}

ccl_device_forceinline bool deep_bounded_append(KernelGlobals kg,
                                                ConstIntegratorState state,
                                                const DeepBoundedSourceKind kind,
                                                DeepCaptureRecord record,
                                                const float anchor)
{
  const auto &binding = deep_bounded_binding(kg);
  if (!(record.z > 0.0f && record.z_back >= record.z && isfinite(record.z_back) &&
        isfinite(record.alpha)))
  {
    deep_bounded_error(binding.packet.status_ptr, DEEP_BOUNDED_NUMERIC);
    return false;
  }
  if (!binding.raw.shader && !deep_bounded_identity_fits(record)) {
    deep_bounded_error(binding.packet.status_ptr, DEEP_BOUNDED_LAYOUT);
    return false;
  }
  const uint32_t root = deep_bounded_slot(INTEGRATOR_STATE(state, path, deep_bounded_slot_kind));
  const uint64_t stride = binding.packet.root_capacity;
  uint32_t &header = deep_bounded_ptr<uint32_t>(binding.headers)[root];
  if (!(header & DEEP_BOUNDED_VALID)) {
    deep_bounded_error(binding.packet.status_ptr, DEEP_BOUNDED_RANGE);
    return false;
  }
  uint32_t count = header & DEEP_BOUNDED_COUNT_MASK;
  if (count == DEEP_BOUNDED_K) {
    count = deep_bounded_make_raw_room(binding, root);
    if (!count) {
      return false;
    }
  }
  uint32_t slot = count;
  const float *fronts = deep_bounded_ptr<const float>(binding.raw.z);
  while (slot && fronts[root + uint64_t(slot - 1) * stride] > record.z) {
    const auto previous = deep_bounded_load<deep_bounded_physical>(
        binding.raw, root + uint64_t(slot - 1) * stride);
    deep_bounded_store(binding.raw, root + uint64_t(slot) * stride, previous);
    --slot;
  }
#  if __DEEP_BOUNDED_POLICY__ == 3
  const bool ordered_ends = deep_bounded_ordered_end_insert(binding.raw,
                                                            root,
                                                            stride,
                                                            count,
                                                            slot,
                                                            record.z_back,
                                                            (header & DEEP_BOUNDED_ORDERED_ENDS) !=
                                                                0);
  if (record.object & DEEP_BOUNDED_ELIGIBLE) {
    float *pending = deep_bounded_ptr<float>(binding.no_anchor) + uint64_t(root) * 3;
    for (int c = 0; c < 3; ++c) {
      record.rgb[c] = pending[c];
      pending[c] = 0.0f;
    }
  }
  INTEGRATOR_STATE_WRITE(deep_bounded_write_state(state), path, deep_bounded_anchor_z) = anchor;
#  else
  (void)anchor;
#  endif
  deep_bounded_store(binding.raw, root + uint64_t(slot) * stride, record);
#  if __DEEP_BOUNDED_POLICY__ == 3
  header = (header & ~(DEEP_BOUNDED_COUNT_MASK | DEEP_BOUNDED_ORDERED_ENDS)) | (count + 1) |
           (ordered_ends ? DEEP_BOUNDED_ORDERED_ENDS : 0u);
#  else
  header = (header & ~DEEP_BOUNDED_COUNT_MASK) | (count + 1);
#  endif
  INTEGRATOR_STATE_WRITE(deep_bounded_write_state(state),
                         path,
                         deep_bounded_slot_kind) = deep_bounded_source(root, kind);
  return true;
}

ccl_device_forceinline float deep_bounded_surface_alpha(KernelGlobals kg,
                                                        ConstIntegratorState state,
                                                        const float t,
                                                        const float h)
{
  const auto &binding = deep_bounded_binding(kg);
  if (!binding.has_holdout) {
    return deep_bounded_unit(1.0f - t);
  }
  const uint32_t root = deep_bounded_slot(INTEGRATOR_STATE(state, path, deep_bounded_slot_kind));
  float *mq = deep_bounded_ptr<float>(binding.holdout_mq) + uint64_t(root) * 2;
  const float m = mq[0];
  const float l = m + mq[1];
  const float a = l > 0.0f ? deep_bounded_unit(m * (1.0f - t - h) / l) : 0.0f;
  mq[1] += m * h;
  mq[0] = m * t;
  return a;
}

ccl_device_forceinline void deep_bounded_surface(KernelGlobals kg,
                                                 ConstIntegratorState state,
                                                 const ccl_private ShaderData *sd,
                                                 const Spectrum transparency,
                                                 const Spectrum holdout,
                                                 const bool source_eligible)
{
  const float3 t_rgb = spectrum_to_rgb(transparency);
  const float3 h_rgb = spectrum_to_rgb(holdout);
  const float tx = deep_bounded_unit(t_rgb.x);
  const float ty = deep_bounded_unit(t_rgb.y);
  const float tz = deep_bounded_unit(t_rgb.z);
  const float t = (tx + ty + tz) * (1.0f / 3.0f);
  const float h = fminf(1.0f - t, (fminf(deep_bounded_unit(h_rgb.x), 1.0f - tx) +
                                 fminf(deep_bounded_unit(h_rgb.y), 1.0f - ty) +
                                 fminf(deep_bounded_unit(h_rgb.z), 1.0f - tz)) * (1.0f / 3.0f));
  const float a = deep_bounded_surface_alpha(kg, state, t, h);
  /* Full holdout terminates this source. Do not retain a false Surface source
   * if scalar transparency/holdout subtraction leaves a rounding residual. */
  if (!source_eligible) {
    return;
  }
  if (sd->object < 0 || uint32_t(sd->object) >= DEEP_BOUNDED_NONSURFACE) {
    deep_bounded_error(deep_bounded_binding(kg).packet.status_ptr, DEEP_BOUNDED_RANGE);
    return;
  }
  DeepCaptureRecord record = {};
  /* Use the same ray equation as the neighboring Volume endpoint. A refined
   * surface position can move one float ULP inside its preceding interval. */
  Intersection isect;
  integrator_state_read_isect(state, &isect);
  Ray ray;
  integrator_state_read_ray(state, &ray);
  record.z = record.z_back = camera_z_depth(kg, ray.P + ray.D * isect.t);
  record.alpha = a;
  record.object = uint32_t(sd->object) | DEEP_BOUNDED_ELIGIBLE;
  record.shader = uint32_t(sd->shader);
  record.normal = deep_bounded_pack_normal(sd->Ng.x, sd->Ng.y, sd->Ng.z);
  deep_bounded_append(kg, state, DeepBoundedSourceKind::POINT, record, record.z);
}

ccl_device_forceinline void deep_bounded_emissive_point(KernelGlobals kg,
                                                        IntegratorState state,
                                                        const float depth,
                                                        const uint32_t object,
                                                        const uint32_t shader,
                                                        const uint32_t normal)
{
  if (deep_bounded_camera_chain(kg, state)) {
    DeepCaptureRecord record = {};
    record.z = record.z_back = depth;
    record.object = object | DEEP_BOUNDED_ELIGIBLE;
    record.shader = shader;
    record.normal = normal;
    deep_bounded_append(kg, state, DeepBoundedSourceKind::POINT, record, depth);
  }
}

ccl_device_forceinline void deep_bounded_background(KernelGlobals kg,
                                                    ConstIntegratorState state,
                                                    const bool colour_only = false)
{
  if (!deep_bounded_camera_chain(kg, state)) {
    return;
  }
  Ray ray;
  integrator_state_read_ray(state, &ray);
  const float t = ray.tmax;
  if (!(t >= ray.tmin && t < FLT_MAX && isfinite(t))) {
    deep_bounded_error(deep_bounded_binding(kg).packet.status_ptr, DEEP_BOUNDED_NUMERIC);
    return;
  }
  DeepCaptureRecord record = {};
  record.z = record.z_back = camera_z_depth(kg, ray.P + ray.D * t);
  record.alpha = colour_only ? 0.0f : deep_bounded_surface_alpha(kg, state, 0.0f, 0.0f);
  record.object = DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE;
  record.shader = 1;
  deep_bounded_append(kg, state, DeepBoundedSourceKind::POINT, record, record.z);
}

enum DeepBoundedVolumeReason : uint32_t {
  BOUNDED_VOLUME_INACTIVE,
  BOUNDED_VOLUME_TRACKING,
  BOUNDED_VOLUME_INTERVAL,
  BOUNDED_VOLUME_ZERO_EVENT,
};

ccl_device_forceinline bool deep_bounded_commit_volume(KernelGlobals kg,
                                                       ConstIntegratorState state,
                                                       const ccl_private Ray *ray,
                                                       const ccl_private ShaderData *sd,
                                                       const DeepBoundedBridge &bridge,
                                                       const Spectrum throughput,
                                                       const bool scattered,
                                                       const float scatter_t,
                                                       const bool has_radiance)
{
  if (!deep_bounded_camera_chain(kg, state)) {
    /* Indirect whole contributions keep their earlier real surface/volume source. */
    return true;
  }
  if (sd->object == OBJECT_NONE ||
      (bridge.reason != BOUNDED_VOLUME_INTERVAL && bridge.reason != BOUNDED_VOLUME_ZERO_EVENT))
  {
    return false;
  }
  const auto &binding = deep_bounded_binding(kg);
  const uint32_t flags = INTEGRATOR_STATE(state, path, flag);
  const bool terminated = (flags & PATH_RAY_TERMINATE_IN_NEXT_VOLUME) ||
                          INTEGRATOR_STATE(state, path, continuation_probability) == 0.0f;
  const float incoming = average(INTEGRATOR_STATE(state, path, throughput));
  const float outgoing = average(throughput);
  const float event_t = scattered ? scatter_t : bridge.event_t;
  if (!isfinite(event_t) || (!scattered && (!isfinite(incoming) || !isfinite(outgoing)))) {
    deep_bounded_error(binding.packet.status_ptr, DEEP_BOUNDED_NUMERIC);
    return false;
  }
  /* Native scatter ends the transparent camera path. Its sampled depth
   * supplies the coverage event. On transmission use the native weighted
   * throughput which can actually contribute transparency at the background. */
  const float t = scattered || terminated ? 0.0f :
                  incoming > 0.0f ? deep_bounded_unit(outgoing / incoming) : 1.0f;
  if (t == 1.0f &&
      (__DEEP_BOUNDED_POLICY__ == 1 || !bridge.has_extinction || !has_radiance))
  {
    /* A transmitted sample can carry radiance from a medium with real
     * extinction even when this draw contributes no opacity. */
    return false;
  }
  const float end = fminf(ray->tmax, fmaxf(ray->tmin, event_t));
  const bool coverage_event = scattered || terminated || bridge.reason == BOUNDED_VOLUME_ZERO_EVENT;
  const float begin = coverage_event ? end : ray->tmin;
  DeepCaptureRecord record = {};
  record.z = camera_z_depth(kg, ray->P + ray->D * begin);
  record.z_back = camera_z_depth(kg, ray->P + ray->D * end);
  record.alpha = deep_bounded_surface_alpha(kg, state, deep_bounded_unit(t), 0.0f);
  record.object = DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE;
  return deep_bounded_append(kg, state, DeepBoundedSourceKind::VOLUME, record, record.z_back);
}

#  if __DEEP_BOUNDED_POLICY__ == 3
struct DeepBoundedFilmSource {
  uint32_t source = deep_bounded_source(0, DeepBoundedSourceKind::EXCLUDED);
  float anchor = 0.0f;
};

ccl_device_forceinline DeepBoundedFilmSource deep_bounded_film_source(KernelGlobals kg,
                                                                      ConstIntegratorState state,
                                                                      const bool permit = true)
{
  if (!deep_bounded_active(kg) || !permit ||
      (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SHADOW_CATCHER_PASS))
  {
    return {};
  }
  return {INTEGRATOR_STATE(state, path, deep_bounded_slot_kind),
          INTEGRATOR_STATE(state, path, deep_bounded_anchor_z)};
}

ccl_device_forceinline DeepBoundedFilmSource
deep_bounded_film_source(KernelGlobals kg, ConstIntegratorShadowState state)
{
  if (!deep_bounded_active(kg) ||
      (INTEGRATOR_STATE(state, shadow_path, flag) & PATH_RAY_SHADOW_CATCHER_PASS))
  {
    return {};
  }
  return {INTEGRATOR_STATE(state, shadow_path, deep_bounded_slot_kind),
          INTEGRATOR_STATE(state, shadow_path, deep_bounded_anchor_z)};
}

ccl_device_forceinline void deep_bounded_write_rgb(KernelGlobals kg,
                                                   const DeepBoundedFilmSource source,
                                                   const Spectrum contribution)
{
  const auto kind = deep_bounded_kind(source.source);
  if (kind == DeepBoundedSourceKind::EXCLUDED || is_zero(contribution)) {
    return;
  }
  const auto &b = deep_bounded_binding(kg);
  const uint32_t root = deep_bounded_slot(source.source);
  const float3 rgb = spectrum_to_rgb(contribution);
  if (root >= b.packet.root_capacity || !isfinite(rgb.x) || !isfinite(rgb.y) || !isfinite(rgb.z)) {
    deep_bounded_error(b.packet.status_ptr, DEEP_BOUNDED_NUMERIC);
    return;
  }
  const uint32_t header = deep_bounded_ptr<const uint32_t>(b.headers)[root];
  const uint32_t count = header & DEEP_BOUNDED_COUNT_MASK;
  const uint64_t best = deep_bounded_rgb_source_index_ordered(
      b.raw,
      root,
      b.packet.root_capacity,
      count,
      kind,
      source.anchor,
      (header & DEEP_BOUNDED_ORDERED_ENDS) != 0);
  if (best != UINT64_MAX) {
    auto &record = deep_bounded_ptr<DeepBoundedRGBA>(b.raw.rgba)[best];
    atomicAdd(&record.r, rgb.x);
    atomicAdd(&record.g, rgb.y);
    atomicAdd(&record.b, rgb.z);
  }
  else {
    float *pending = deep_bounded_ptr<float>(b.no_anchor) + uint64_t(root) * 3;
    atomicAdd(pending + 0, rgb.x);
    atomicAdd(pending + 1, rgb.y);
    atomicAdd(pending + 2, rgb.z);
  }
}
#  endif

CCL_NAMESPACE_END

#endif
