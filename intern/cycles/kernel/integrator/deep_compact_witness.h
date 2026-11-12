/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Diagnostic reference uses original coefficients and an independent M/Q
 * recurrence. It never reads a compressed primitive, fitted base or payload. */
ccl_device_inline double deep_witness_logadd(const double a, const double b)
{
  if (a == -INFINITY) return b;
  if (b == -INFINITY) return a;
  const double high = a > b ? a : b, low = a > b ? b : a;
  return high + log1p(exp(low - high));
}

ccl_device_inline void deep_witness_atomic_logadd(ccl_global double *address, const double value)
{
#if defined(__KERNEL_CUDA__) || defined(__KERNEL_HIP__)
  auto *bits = reinterpret_cast<ccl_global unsigned long long *>(address);
  unsigned long long old = atomicCAS(bits, 0ull, 0ull), assumed;
  do {
    assumed = old;
    old = atomicCAS(bits, assumed, __double_as_longlong(
        deep_witness_logadd(__longlong_as_double(assumed), value)));
  } while (old != assumed);
#else
  (void)address; (void)value;
#endif
}

ccl_device_inline uint deep_witness_pixel(const DeepCompactWitnessParameters &p, const uint camera)
{
  const auto *receipt = reinterpret_cast<ccl_global const DeepNativeCameraRecord *>(p.cameras);
  const int64_t x = int64_t(receipt[camera].x) - p.x, y = int64_t(receipt[camera].y) - p.y;
  return x < 0 || y < 0 || x >= p.width || y >= p.height ? UINT_MAX : uint(y * p.width + x);
}

ccl_device_inline void deep_witness_store(const DeepCompactWitnessParameters &p,
                                         const uint pixel, const uint cut, const double log_t)
{
  if (pixel != UINT_MAX)
    deep_witness_atomic_logadd(reinterpret_cast<ccl_global double *>(p.log_t) +
                                 size_t(pixel) * p.cut_count + cut, log_t);
}

ccl_device_noinline void deep_witness_raw(KernelGlobals kg, const uint camera_index,
                                         const uint flags, const double front, const double back,
                                         const double sigma, const double log_m, const double log_q,
                                         const double za, const double zb, const double t, const double h,
                                         const uint ordinal)
{
  const auto *parameters = kernel_integrator_state.deep_compact_trace;
  if (!parameters) return;
  const auto &trace = *parameters;
  const auto &w = *kernel_integrator_state.deep_compact_witness;
  const auto &camera = reinterpret_cast<ccl_global const DeepNativeCameraRecord *>(w.cameras)[camera_index];
  const uint64_t key = (uint64_t(uint32_t(camera.x)) << 32) | uint32_t(camera.y);
  bool selected = false;
  for (uint i = 0; i < trace.probe_count; i++)
    selected |= reinterpret_cast<ccl_global const uint64_t *>(trace.probes)[i] == key;
  if (!selected) return;
  auto *counter = reinterpret_cast<ccl_global uint32_t *>(trace.raw_counter);
  const uint at = atomic_fetch_and_add_uint32(counter, 1);
  if (at >= trace.capacity) { atomic_fetch_and_add_uint32(counter + 1, 1); return; }
  auto &out = reinterpret_cast<ccl_global DeepCompactRawTrace *>(trace.raw_records)[at];
  out.front = front; out.back = back; out.sigma = sigma; out.log_m = log_m; out.log_q = log_q;
  out.z_front = za; out.z_back = zb; out.transmission = t; out.holdout = h;
  const Transform matrix = kernel_data.cam.cameratoworld;
  for (int c = 0; c < 3; c++) { out.origin[c] = camera.origin[c]; out.direction[c] = camera.direction[c]; }
  out.camera_origin[0] = matrix.x.w; out.camera_origin[1] = matrix.y.w; out.camera_origin[2] = matrix.z.w;
  out.affine_origin = camera.z_origin; out.affine_direction = camera.z_direction;
  out.x = camera.x; out.y = camera.y; out.sample = camera.sample; out.ordinal = ordinal;
  out.flags = flags | ((camera.flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH) ? 256u : 0u); out.reserved = 0;
}

ccl_device_inline void deep_witness_init(KernelGlobals kg, const uint camera, const float front, const bool valid)
{
  if (!kernel_integrator_state.deep_compact_witness) return;
  const auto &p = *kernel_integrator_state.deep_compact_witness;
  auto &ray = reinterpret_cast<ccl_global DeepCompactWitnessRay *>(p.rays)[camera];
  ray = {valid ? 0.0 : -INFINITY, -INFINITY, front, 0, uint(!valid), 0};
  if (!valid) {
    if (kernel_integrator_state.deep_compact_trace)
      deep_witness_raw(kg, camera, 8u, 0, 0, 0, -INFINITY, -INFINITY, 0, 0, 0, 0, ray.reserved++);
    const uint pixel = deep_witness_pixel(p, camera);
    for (uint cut = 0; cut < p.cut_count; cut++)
      deep_witness_store(p, pixel, cut,
          reinterpret_cast<ccl_global const double *>(p.cuts)[cut] < 0 ? 0.0 : -INFINITY);
    ray.next_cut = p.cut_count;
  }
}

ccl_device_inline double deep_witness_physical(const DeepNativeCameraRecord &camera,
                                              const double depth, KernelGlobals kg)
{
  if (camera.flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH)
    return (depth - camera.z_origin) / camera.z_direction;
  const Transform matrix = kernel_data.cam.cameratoworld;
  const double x = double(camera.origin[0]) - matrix.x.w;
  const double y = double(camera.origin[1]) - matrix.y.w;
  const double z = double(camera.origin[2]) - matrix.z.w;
  const double along = x * camera.direction[0] + y * camera.direction[1] + z * camera.direction[2];
  const double perpendicular = fmax(0.0, x * x + y * y + z * z - along * along);
  return sqrt(fmax(0.0, depth * depth - perpendicular)) - along;
}

ccl_device_inline double deep_witness_depth(const DeepNativeCameraRecord &camera,
                                           const double t, KernelGlobals kg)
{
  if (camera.flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH) return camera.z_origin + double(camera.z_direction) * t;
  const Transform matrix = kernel_data.cam.cameratoworld;
  const double x = double(camera.origin[0]) - matrix.x.w + double(camera.direction[0]) * t;
  const double y = double(camera.origin[1]) - matrix.y.w + double(camera.direction[1]) * t;
  const double z = double(camera.origin[2]) - matrix.z.w + double(camera.direction[2]) * t;
  return sqrt(x * x + y * y + z * z);
}

ccl_device_inline void deep_witness_cell(KernelGlobals kg, const uint camera_index,
                                        const float front, const float back, const double sigma)
{
  if (!kernel_integrator_state.deep_compact_witness) return;
  const auto &p = *kernel_integrator_state.deep_compact_witness;
  auto &ray = reinterpret_cast<ccl_global DeepCompactWitnessRay *>(p.rays)[camera_index];
  if (ray.done || back <= ray.cursor) return;
  const uint pixel = deep_witness_pixel(p, camera_index);
  const auto &camera = reinterpret_cast<ccl_global const DeepNativeCameraRecord *>(p.cameras)[camera_index];
  const auto *cuts = reinterpret_cast<ccl_global const double *>(p.cuts);
  const double a = fmax(double(front), double(ray.cursor)), b = back;
  const double za = deep_witness_depth(camera, a, kg), zb = deep_witness_depth(camera, b, kg);
  if (kernel_integrator_state.deep_compact_trace)
    deep_witness_raw(kg, camera_index, 1u, a, b, sigma, ray.log_m, ray.log_q, za, zb, 1, 0, ray.reserved++);
  while (ray.next_cut < p.cut_count && cuts[ray.next_cut] < zb) {
    const double t = cuts[ray.next_cut] <= za ? a :
        fmin(b, fmax(a, deep_witness_physical(camera, cuts[ray.next_cut], kg)));
    deep_witness_store(p, pixel, ray.next_cut++, deep_witness_logadd(ray.log_m - sigma * (t - a), ray.log_q));
  }
  ray.log_m -= sigma * (b - a); ray.cursor = back;
}

ccl_device_inline void deep_witness_point(KernelGlobals kg, const uint camera, const float depth,
                                         const float physical, const double transmission,
                                         const double holdout)
{
  if (!kernel_integrator_state.deep_compact_witness) return;
  const auto &p = *kernel_integrator_state.deep_compact_witness;
  auto &ray = reinterpret_cast<ccl_global DeepCompactWitnessRay *>(p.rays)[camera];
  if (ray.done) return;
  const uint pixel = deep_witness_pixel(p, camera);
  const auto *cuts = reinterpret_cast<ccl_global const double *>(p.cuts);
  if (kernel_integrator_state.deep_compact_trace)
    deep_witness_raw(kg, camera, 2u, physical, physical, 0, ray.log_m, ray.log_q,
                      depth, depth, transmission, holdout, ray.reserved++);
  while (ray.next_cut < p.cut_count && cuts[ray.next_cut] < depth)
    deep_witness_store(p, pixel, ray.next_cut++, deep_witness_logadd(ray.log_m, ray.log_q));
  if (holdout > 0) ray.log_q = deep_witness_logadd(ray.log_q, ray.log_m + log(holdout));
  ray.log_m = transmission > 0 ? ray.log_m + log(transmission) : -INFINITY;
  ray.cursor = physical;
  while (ray.next_cut < p.cut_count && (cuts[ray.next_cut] == depth || ray.log_m == -INFINITY))
    deep_witness_store(p, pixel, ray.next_cut++, deep_witness_logadd(ray.log_m, ray.log_q));
  if (ray.log_m == -INFINITY) ray.done = 1;
}

ccl_device_inline void deep_witness_background(KernelGlobals kg, const uint camera, const bool transparent)
{
  if (!kernel_integrator_state.deep_compact_witness) return;
  const auto &p = *kernel_integrator_state.deep_compact_witness;
  auto &ray = reinterpret_cast<ccl_global DeepCompactWitnessRay *>(p.rays)[camera];
  if (ray.done) return;
  const uint pixel = deep_witness_pixel(p, camera);
  const auto *cuts = reinterpret_cast<ccl_global const double *>(p.cuts);
  if (kernel_integrator_state.deep_compact_trace)
    deep_witness_raw(kg, camera, 4u, ray.cursor, ray.cursor, 0, ray.log_m, ray.log_q,
                      FLT_MAX, FLT_MAX, transparent ? 1 : 0, 0, ray.reserved++);
  while (ray.next_cut < p.cut_count) {
    const uint cut = ray.next_cut++;
    const double tail = !transparent && cuts[cut] == FLT_MAX ? ray.log_q :
        deep_witness_logadd(ray.log_m, ray.log_q);
    deep_witness_store(p, pixel, cut, tail);
  }
  ray.done = 1;
}
