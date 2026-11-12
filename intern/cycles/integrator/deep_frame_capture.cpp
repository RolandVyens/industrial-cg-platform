/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_frame_capture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

CCL_NAMESPACE_BEGIN

bool deep_native_capture_validate(const DeepNativeCaptureBatch &batch,
                                  std::string &error,
                                  const bool allow_pending_optical)
{
  const auto fail = [&](const char *message) {
    error = message;
    return false;
  };
  const DeepNativeCaptureDomain &domain = batch.domain;
  if (domain.depth_kind != DeepFrozenDepthKind::Affine &&
      domain.depth_kind != DeepFrozenDepthKind::Radial)
    return fail("Native camera frozen depth kind is unknown");
  for (float value : domain.depth_transform)
    if (!std::isfinite(value))
      return fail("Native camera frozen depth transform is nonfinite");
  const auto promoted_depth = [&](const DeepNativeCameraRecord &camera, float t) {
    if (domain.depth_kind == DeepFrozenDepthKind::Affine)
      return double(camera.z_origin) + double(camera.z_direction) * t;
    double v[3];
    for (int c = 0; c < 3; c++)
      v[c] = double(camera.origin[c]) - domain.depth_transform[4 * c + 3] +
             double(camera.direction[c]) * t;
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  };
  const auto world_binding = [&](const int object, const int shader) {
    return domain.require_full_optical && domain.world_volume_shader != SHADER_NONE &&
           domain.world_volume_object != OBJECT_NONE && object == domain.world_volume_object &&
           (shader & SHADER_MASK) == (domain.world_volume_shader & SHADER_MASK);
  };
  if (domain.volume_pending)
    return fail("Native volume continuation still owns unfinished operations");
  if (domain.core_width || domain.core_height) {
    if (domain.core_width <= 0 || domain.core_height <= 0 || domain.core_x < domain.x ||
        domain.core_y < domain.y ||
        int64_t(domain.core_x) + domain.core_width > int64_t(domain.x) + domain.width ||
        int64_t(domain.core_y) + domain.core_height > int64_t(domain.y) + domain.height)
      return fail("Native output core lies outside the evaluated domain");
  }
  if (domain.auxiliary_film_calls > UINT32_MAX ||
      (domain.auxiliary_film_calls && !domain.require_full_optical))
    return fail("Auxiliary catcher routing requires a joined full Optical domain");
  if (domain.require_full_optical && !allow_pending_optical &&
      (!batch.optical_full_coverage || !batch.optical_replay_joined))
    return fail(
        "This native source family requires complete independent Optical before publication");
  if (batch.cameras.size() > UINT32_MAX || batch.homogeneous.size() > UINT32_MAX ||
      batch.roots.size() > UINT32_MAX || batch.points.size() > UINT32_MAX ||
      batch.expression_sources.size() > UINT32_MAX || batch.expression_nodes.size() > UINT32_MAX ||
      batch.expression_calls.size() > UINT32_MAX)
    return fail("Native packet exceeds its checked 32-bit transport range");
  if (!domain.id || !domain.device_generation || !domain.layout_generation ||
      !domain.main_shadow_mnee_joined || domain.error_bits ||
      domain.camera_reserved != domain.camera_written ||
      domain.camera_written != batch.cameras.size() ||
      domain.camera_written > domain.scheduled_admissions ||
      domain.homogeneous_reserved != domain.homogeneous_written ||
      domain.homogeneous_written != batch.homogeneous.size() ||
      domain.point_reserved != domain.point_written ||
      domain.point_written != batch.points.size() || domain.root_reserved != domain.root_written ||
      domain.root_written != batch.roots.size() ||
      (batch.roots.empty() && !batch.points.empty()) ||
      domain.expression_source_reserved != domain.expression_source_written ||
      domain.expression_source_written != batch.expression_sources.size() ||
      domain.expression_node_reserved != domain.expression_node_written ||
      domain.expression_node_written != batch.expression_nodes.size() ||
      domain.expression_call_reserved != domain.expression_call_written ||
      domain.expression_call_written != batch.expression_calls.size())
  {
    return fail("Native capture domain did not join or its authoritative counters disagree");
  }
  if (domain.width <= 0 || domain.height <= 0 || domain.samples_num <= 0 ||
      domain.start_sample < 0 || domain.sample_offset < 0)
  {
    return fail("Native capture domain has invalid image/sample ranges");
  }
  if (batch.optical_replay_domain || batch.optical_replay_joined ||
      !batch.optical_events.empty() || !batch.optical_ledger.empty() ||
      batch.optical_full_coverage || !batch.optical_pieces.empty() ||
      !batch.optical_visibility.empty() || !batch.optical_coverage.empty())
  {
    if (!batch.optical_replay_joined || batch.optical_replay_domain != domain.id ||
        batch.optical_ledger.size() != batch.cameras.size())
      return fail("Owned Optical replay did not join the same native domain");
    DeepNativeCaptureVector<uint32_t> counts(batch.cameras.get_allocator());
    counts.resize(batch.cameras.size(), 0);
    if (batch.optical_full_coverage) {
      if (!batch.optical_events.empty() || batch.optical_coverage.size() != batch.cameras.size())
        return fail("Full owned coverage has legacy events or a missing camera ledger");
      DeepNativeCaptureVector<uint32_t> order(batch.cameras.get_allocator());
      DeepNativeCaptureVector<uint32_t> visibility_counts(batch.cameras.get_allocator());
      visibility_counts.resize(batch.cameras.size(), 0);
      DeepNativeCaptureVector<uint32_t> last_visibility(batch.cameras.get_allocator());
      DeepNativeCaptureVector<uint32_t> clamp_counts(batch.cameras.get_allocator());
      last_visibility.resize(batch.cameras.size(), UINT32_MAX);
      clamp_counts.resize(batch.cameras.size(), 0);
      order.reserve(batch.optical_pieces.size());
      for (size_t i = 0; i < batch.optical_pieces.size(); i++) {
        const auto &piece = batch.optical_pieces[i];
        if (i > UINT32_MAX || piece.receipt >= batch.cameras.size() ||
            (piece.flags != DEEP_OWNED_OPTICAL_MEDIUM &&
             piece.flags != DEEP_OWNED_OPTICAL_VACUUM) ||
            !std::isfinite(piece.tmin) || !std::isfinite(piece.tmax) || piece.tmin < 0 ||
            piece.tmax <= piece.tmin || !std::isfinite(piece.z) || !std::isfinite(piece.zback) ||
            piece.z < 0 || piece.zback < piece.z || counts[piece.receipt] == UINT32_MAX)
          return fail("Full owned piece has invalid identity, physical support or camera depth");
        for (float tau : piece.tau)
          if (!std::isfinite(tau) || tau < 0 ||
              (piece.flags == DEEP_OWNED_OPTICAL_VACUUM && tau != 0))
            return fail("Full owned piece has invalid medium/vacuum extinction");
        if (piece.z == piece.zback &&
            !deep_owned_optical_depth_identity(
                piece.z, piece.zback, piece.tau[0], piece.tau[1], piece.tau[2]))
        {
          const auto &camera = batch.cameras[piece.receipt];
          const double front = promoted_depth(camera, piece.tmin);
          const double back = promoted_depth(camera, piece.tmax);
          const double tolerance = 64 * std::numeric_limits<float>::epsilon();
          if (!std::isfinite(front) || !std::isfinite(back) || back <= front ||
              std::abs(double(piece.z) - front) > tolerance * std::max(1., std::abs(front)) ||
              std::abs(double(piece.zback) - back) > tolerance * std::max(1., std::abs(back)))
            return fail("Collapsed Optical support disagrees with frozen physical geometry");
          for (const float tau : piece.tau)
            if ((__float_as_uint(tau) & 0x7fffffffu) != 0 && tau == 0)
              return fail("Collapsed Optical extinction cannot be lost under DAZ");
        }
        counts[piece.receipt]++;
        order.push_back(uint32_t(i));
      }
      for (size_t index = 0; index < batch.optical_visibility.size(); index++) {
        const auto &point = batch.optical_visibility[index];
        if (index > UINT32_MAX || point.receipt >= batch.cameras.size() || point.reserved ||
            point.ordinal != visibility_counts[point.receipt] ||
            visibility_counts[point.receipt] == UINT32_MAX ||
            point.physical_t < batch.cameras[point.receipt].tmin ||
            point.physical_t > batch.optical_coverage[point.receipt].cursor_t ||
            !std::isfinite(point.z) || point.z < 0 || !std::isfinite(point.physical_t))
          return fail("Owned visibility has invalid source/depth identity");
        for (int c = 0; c < 3; c++)
          if (!std::isfinite(point.transmission[c]) || point.transmission[c] < 0 ||
              point.transmission[c] > 1 || !std::isfinite(point.holdout[c]) ||
              point.holdout[c] < 0 || point.holdout[c] > 1 - point.transmission[c])
            return fail("Owned visibility is outside bounded t/q coverage");
        last_visibility[point.receipt] = uint32_t(index);
        clamp_counts[point.receipt] += bool(point.flags & DEEP_OWNED_OPTICAL_CLAMPED);
        visibility_counts[point.receipt]++;
      }
      std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return std::tie(batch.optical_pieces[a].receipt, batch.optical_pieces[a].tmin) <
               std::tie(batch.optical_pieces[b].receipt, batch.optical_pieces[b].tmin);
      });
      size_t begin = 0;
      for (size_t i = 0; i < batch.cameras.size(); i++) {
        const auto &coverage = batch.optical_coverage[i];
        const auto &camera = batch.cameras[i];
        if (!(coverage.flags & DEEP_OWNED_OPTICAL_COMPLETE) || coverage.pieces != counts[i] ||
            coverage.visibility != visibility_counts[i] || coverage.clamps != clamp_counts[i])
          return fail("Owned coverage ledger did not join its actual pieces/visibility");
        if (camera.disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK) {
          if (coverage.flags != (DEEP_OWNED_OPTICAL_COMPLETE | DEEP_OWNED_OPTICAL_NO_RAY) ||
              coverage.pieces || coverage.visibility)
            return fail("Invalid camera unexpectedly has physical owned coverage");
          continue;
        }
        float cursor = camera.tmin;
        if (coverage.front_t != cursor || !std::isfinite(coverage.cursor_t) ||
            coverage.cursor_t < cursor || coverage.cursor_t > camera.tmax ||
            !(coverage.flags & (DEEP_OWNED_OPTICAL_FAR | DEEP_OWNED_OPTICAL_VISIBILITY_END)) ||
            ((coverage.flags & DEEP_OWNED_OPTICAL_FAR) && coverage.cursor_t != camera.tmax) ||
            !std::isfinite(coverage.z_front) || !std::isfinite(coverage.z_back) ||
            coverage.z_front < 0 || coverage.z_back < coverage.z_front)
          return fail("Owned coverage does not certify actual camera clipping/terminal");
        if (coverage.flags & DEEP_OWNED_OPTICAL_VISIBILITY_END) {
          if (last_visibility[i] == UINT32_MAX)
            return fail("Owned visibility termination has no actual terminal point");
          const auto &point = batch.optical_visibility[last_visibility[i]];
          if (point.physical_t != coverage.cursor_t || point.transmission[0] != 0 ||
              point.transmission[1] != 0 || point.transmission[2] != 0)
            return fail("Owned visibility termination does not close the live branch");
        }
        while (begin < order.size() && batch.optical_pieces[order[begin]].receipt == i) {
          const auto &piece = batch.optical_pieces[order[begin++]];
          if (piece.tmin != cursor || piece.tmax > coverage.cursor_t)
            return fail("Owned Optical has a physical gap, overlap or unclosed tail");
          cursor = piece.tmax;
        }
        if (cursor != coverage.cursor_t)
          return fail("Owned Optical cursor lacks its complete physical partitions");
      }
    }
    else {
      if (!batch.optical_pieces.empty() || !batch.optical_visibility.empty() ||
          !batch.optical_coverage.empty())
        return fail("Owned full-coverage arrays have no complete-schema marker");
      for (const auto &event : batch.optical_events) {
        if (event.receipt >= batch.cameras.size() || event.reserved ||
            event.segment >= batch.optical_ledger[event.receipt].segments ||
            !std::isfinite(event.z) || !std::isfinite(event.zback) || event.z < 0 ||
            event.zback < event.z || counts[event.receipt] == UINT32_MAX)
          return fail("Owned Optical event has an invalid camera/segment/depth reference");
        for (const float tau : event.tau)
          if (!std::isfinite(tau) || tau < 0)
            return fail("Owned Optical event has invalid extinction");
        counts[event.receipt]++;
      }
    }
    for (size_t i = 0; i < batch.cameras.size(); i++) {
      const auto &ledger = batch.optical_ledger[i];
      const bool invalid = batch.cameras[i].disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK;
      if (ledger.status != (invalid ? DEEP_OPTICAL_ZERO_THROUGHPUT : DEEP_OPTICAL_COMPLETE) ||
          ledger.reserved || ledger.events != counts[i] ||
          (invalid && (ledger.events || ledger.segments)))
        return fail("Owned Optical ledger disagrees with actual captured events/disposition");
    }
  }

  /* All scratch storage uses the packet's shared quota. */
  DeepNativeCaptureVector<uint32_t> camera_order(batch.cameras.get_allocator());
  DeepNativeCaptureVector<uint32_t> segment_count(batch.cameras.get_allocator());
  DeepNativeCaptureVector<uint32_t> segment_head(batch.cameras.get_allocator());
  DeepNativeCaptureVector<uint32_t> segment_next(batch.cameras.get_allocator());
  camera_order.resize(batch.cameras.size());
  segment_count.resize(batch.cameras.size(), 0);
  segment_head.resize(batch.cameras.size(), UINT32_MAX);
  segment_next.reserve(batch.homogeneous.size());
  uint64_t emission_calls = 0;
  for (size_t i = 0; i < batch.roots.size(); i++) {
    const auto &root = batch.roots[i];
    if (root.camera_index >= batch.cameras.size() || root.ordinal != i + 1 ||
        !(root.flags & DEEP_NATIVE_ROOT_CAMERA_SUPPORT) || !std::isfinite(root.z) || root.z < 0)
      return fail("Native source root has invalid domain handle or camera support");
    const auto &camera = batch.cameras[root.camera_index];
    if (camera.disposition != DEEP_NATIVE_CAMERA_RAY)
      return fail("Nongeometric/clear camera unexpectedly owns a source root");
    if (root.kind == DEEP_NATIVE_ROOT_SURFACE || root.kind == DEEP_NATIVE_ROOT_LIGHT ||
        root.kind == DEEP_NATIVE_ROOT_VOLUME_SCATTER ||
        root.kind == DEEP_NATIVE_ROOT_VOLUME_DIRECT)
    {
      const bool candidate = root.kind == DEEP_NATIVE_ROOT_SURFACE &&
                             (root.flags & DEEP_NATIVE_ROOT_RADIANCE_ONLY);
      const bool world_support = root.flags & DEEP_NATIVE_ROOT_WORLD_SUPPORT;
      const uint32_t expected = DEEP_NATIVE_ROOT_CAMERA_SUPPORT |
                                (root.kind == DEEP_NATIVE_ROOT_SURFACE && !candidate ?
                                     DEEP_NATIVE_ROOT_OPAQUE_SURFACE :
                                     DEEP_NATIVE_ROOT_RADIANCE_ONLY) |
                                (candidate ? root.flags & DEEP_NATIVE_ROOT_MNEE_ADMISSION : 0) |
                                (world_support ? DEEP_NATIVE_ROOT_WORLD_SUPPORT : 0);
      if (world_support && ((root.kind != DEEP_NATIVE_ROOT_VOLUME_DIRECT &&
                             root.kind != DEEP_NATIVE_ROOT_VOLUME_SCATTER) ||
                            !world_binding(root.object, root.shader) || root.prim != PRIM_NONE ||
                            !(root.physical_t > camera.tmax)))
        return fail("Native World volume root has no certified producer support");
      if (root.flags != expected || root.z >= std::numeric_limits<float>::max() ||
          !std::isfinite(root.physical_t) || root.physical_t < camera.tmin ||
          (root.physical_t > camera.tmax && !world_support) ||
          (root.kind == DEEP_NATIVE_ROOT_SURFACE && !candidate &&
           camera.root_handle != root.ordinal) ||
          (candidate && !domain.require_full_optical) ||
          ((root.kind == DEEP_NATIVE_ROOT_LIGHT || root.kind == DEEP_NATIVE_ROOT_VOLUME_DIRECT) &&
           camera.root_handle == root.ordinal) ||
          (root.kind == DEEP_NATIVE_ROOT_VOLUME_SCATTER && camera.root_handle != root.ordinal) ||
          ((root.kind == DEEP_NATIVE_ROOT_VOLUME_SCATTER ||
            root.kind == DEEP_NATIVE_ROOT_VOLUME_DIRECT) &&
           !domain.require_full_optical))
        return fail("Native finite root lacks its immutable source/terminal certificate");
    }
    else if (root.kind == DEEP_NATIVE_ROOT_DISTANT) {
      if (root.flags != (DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_RADIANCE_ONLY) ||
          root.z != std::numeric_limits<float>::max() || root.physical_t != -1 ||
          camera.root_handle == root.ordinal || !domain.require_full_optical ||
          !(camera.flags & DEEP_NATIVE_CAMERA_BACKGROUND_REACHED))
        return fail("Native distant radiance lacks independent infinity support");
    }
    else if (root.kind == DEEP_NATIVE_ROOT_WORLD) {
      if (root.flags != (DEEP_NATIVE_ROOT_CAMERA_SUPPORT | DEEP_NATIVE_ROOT_WORLD_TERMINAL) ||
          root.z != std::numeric_limits<float>::max() || root.physical_t != -1 ||
          camera.root_handle != root.ordinal ||
          !(camera.flags & DEEP_NATIVE_CAMERA_BACKGROUND_REACHED))
        return fail("Native World root lacks terminal background coverage");
    }
    else
      return fail("Native root kind requires a complete independent Optical adapter");
  }
  for (size_t i = 0; i < batch.cameras.size(); ++i) {
    const DeepNativeCameraRecord &camera = batch.cameras[i];
    if (camera.root_handle > batch.roots.size())
      return fail("Camera owner references an unknown native root handle");
    const DeepNativeRootRecord root = deep_native_camera_root(batch, i);
    if (camera.root_handle && root.camera_index != i)
      return fail("Camera owner references another camera source root");
    camera_order[i] = uint32_t(i);
    if (camera.unsupported_features || !(camera.flags & DEEP_NATIVE_CAMERA_EVALUATED) ||
        !(camera.flags & DEEP_NATIVE_CAMERA_COMPLETE) || camera.population_weight != 1.0f ||
        int64_t(camera.x) < domain.x || int64_t(camera.x) >= int64_t(domain.x) + domain.width ||
        int64_t(camera.y) < domain.y || int64_t(camera.y) >= int64_t(domain.y) + domain.height ||
        camera.subset_offset != uint32_t(domain.sample_offset) ||
        camera.scheduled_sample < uint32_t(domain.start_sample) ||
        uint64_t(camera.scheduled_sample) >= uint64_t(domain.start_sample) + domain.samples_num)
    {
      return fail("Native camera metadata is incomplete, unsupported or outside its domain");
    }
    if (camera.disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK) {
      /* No geometric field is inspected: a failed camera sample has no valid Ray. */
      if (camera.throughput[0] != 0.0f || camera.throughput[1] != 0.0f ||
          camera.throughput[2] != 0.0f || root.kind != DEEP_NATIVE_ROOT_NONE)
      {
        return fail("InvalidCameraBlack has nonzero native camera throughput");
      }
      continue;
    }
    if (camera.disposition != DEEP_NATIVE_CAMERA_RAY &&
        camera.disposition != DEEP_NATIVE_CAMERA_CLEAR_TRANSPARENT)
    {
      return fail("Native camera disposition is unknown");
    }
    const uint32_t depth_flags = camera.flags & (DEEP_NATIVE_CAMERA_AFFINE_DEPTH |
                                                 DEEP_NATIVE_CAMERA_RADIAL_DEPTH);
    const uint32_t expected_depth = domain.depth_kind == DeepFrozenDepthKind::Affine ?
                                        DEEP_NATIVE_CAMERA_AFFINE_DEPTH :
                                        DEEP_NATIVE_CAMERA_RADIAL_DEPTH;
    if (depth_flags != expected_depth)
      return fail("CameraDepth: native depth flags disagree with frozen camera kind");
    if (!(camera.flags & DEEP_NATIVE_CAMERA_BACKGROUND_REACHED) &&
        root.kind != DEEP_NATIVE_ROOT_SURFACE &&
        !(domain.require_full_optical && (allow_pending_optical || batch.optical_full_coverage)))
      return fail("CameraDepth: native camera coverage is incomplete");
    if (!std::isfinite(camera.tmin) || !std::isfinite(camera.tmax) || camera.tmin < 0 ||
        camera.tmax < camera.tmin)
      return fail("CameraDepth: native camera physical clipping is invalid");
    if (!std::isfinite(camera.time))
      return fail("CameraDepth: native camera time is nonfinite");
    if (!std::isfinite(camera.dP) || !std::isfinite(camera.dD))
      return fail("NonfiniteNativeCameraDifferential: actual camera Ray cannot enter Optical");
    if (!std::isfinite(camera.z_origin) || !std::isfinite(camera.z_direction) ||
        (domain.depth_kind == DeepFrozenDepthKind::Affine && camera.z_direction <= 0) ||
        (domain.depth_kind == DeepFrozenDepthKind::Radial &&
         (camera.z_origin != 0 || camera.z_direction != 0)))
    {
      return fail("Native camera lacks complete matching depth coverage");
    }
    for (int channel = 0; channel < 3; ++channel) {
      if (!std::isfinite(camera.origin[channel]) || !std::isfinite(camera.direction[channel]) ||
          !std::isfinite(camera.throughput[channel]))
      {
        return fail("Native camera payload requires finite geometry and sensitivity");
      }
    }
    if (camera.throughput[0] == 0 && camera.throughput[1] == 0 && camera.throughput[2] == 0)
      return fail("A valid camera Ray cannot have the InvalidCameraBlack sensitivity");
    if (domain.depth_kind == DeepFrozenDepthKind::Radial) {
      double slope = 0, magnitude = 0, norm = 0;
      for (int c = 0; c < 3; c++) {
        const double d = camera.direction[c];
        const double b = (double(camera.origin[c]) - domain.depth_transform[4 * c + 3]) * d;
        const double near = d * d * camera.tmin;
        norm += d * d;
        slope += b + near;
        magnitude += std::abs(b) + near;
      }
      if (!(norm > 0))
        return fail("CameraDepth: radial direction is not invertible");
      if (slope < -64 * std::numeric_limits<double>::epsilon() * magnitude)
        return fail(
            "NonMonotonicCameraDepth: native radial branch decreases before Optical dispatch");
    }
  }

  const auto key = [&](uint32_t index) {
    const auto &camera = batch.cameras[index];
    return std::tuple(camera.x, camera.y, camera.sample, camera.subset_offset);
  };
  std::sort(camera_order.begin(), camera_order.end(), [&](uint32_t a, uint32_t b) {
    return key(a) < key(b);
  });
  for (size_t i = 1; i < camera_order.size(); ++i) {
    if (key(camera_order[i - 1]) == key(camera_order[i])) {
      return fail("Native capture contains a duplicate logical camera sample");
    }
  }

  for (const DeepNativeHomogeneousRecord &segment : batch.homogeneous) {
    if (segment.camera_index >= batch.cameras.size()) {
      return fail("Homogeneous native source references an unknown camera sample");
    }
    const auto &camera = batch.cameras[segment.camera_index];
    const bool world_support = segment.flags & DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT;
    if (world_support && (!world_binding(segment.source_object, segment.source_shader) ||
                          segment.tmax <= camera.tmax))
      return fail("Native World homogeneous support has no compiled World binding");
    segment_next.push_back(segment_head[segment.camera_index]);
    segment_head[segment.camera_index] = uint32_t(segment_next.size() - 1);
    ++segment_count[segment.camera_index];
    if (segment.unsupported_features || !(segment.flags & DEEP_NATIVE_HOMOGENEOUS_OPTICAL) ||
        !(segment.flags & DEEP_NATIVE_HOMOGENEOUS_CAMERA_SUPPORT_VERIFIED) ||
        camera.disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK || segment.pixel != camera.pixel ||
        segment.sample != camera.sample || !std::isfinite(segment.z) ||
        !std::isfinite(segment.zback) || !std::isfinite(segment.tmin) ||
        !std::isfinite(segment.tmax) || segment.z < 0.0f || segment.zback <= segment.z ||
        segment.tmin < camera.tmin || segment.tmax <= segment.tmin ||
        (segment.tmax > camera.tmax && !world_support) || segment.emission_calls > 1 ||
        !deep_native_path_writes_combined(segment.path_flags))
    {
      return fail("Homogeneous native source has invalid identity, support or film-call count");
    }
    if (bool(segment.flags & DEEP_NATIVE_HOMOGENEOUS_EMISSION_FILM_CALL) !=
        bool(segment.emission_calls))
    {
      return fail("Homogeneous source film-call flag and count disagree");
    }
    for (int channel = 0; channel < 3; ++channel) {
      /* GPU --use_fast_math may flush clamp inputs/results. Gain=1 also covers
       * a native call which did not multiply at all. This validates arithmetic;
       * it does not prune opacity or source records. */
      const auto ftz = [](const float value) {
        return std::abs(value) < std::numeric_limits<float>::min() ? 0.0f : value;
      };
      const float gradual = segment.pre_clamp_rgb[channel] * segment.clamp_gain;
      const float flushed = ftz(ftz(segment.pre_clamp_rgb[channel]) * ftz(segment.clamp_gain));
      if (!std::isfinite(segment.tau[channel]) || segment.tau[channel] < 0.0f ||
          !std::isfinite(segment.post_clamp_rgb[channel]) ||
          !std::isfinite(segment.pre_clamp_rgb[channel]) || !std::isfinite(segment.clamp_gain) ||
          segment.clamp_gain < 0 || segment.clamp_gain > 1 ||
          (segment.emission_calls && gradual != segment.post_clamp_rgb[channel] &&
           flushed != segment.post_clamp_rgb[channel]) ||
          (!segment.emission_calls &&
           (segment.post_clamp_rgb[channel] != 0.0f || segment.pre_clamp_rgb[channel] != 0.0f)))
      {
        return fail("Homogeneous native source has invalid optical depth or post-clamp RGB");
      }
    }
    emission_calls += segment.emission_calls;
  }
  if (emission_calls != domain.emission_calls) {
    return fail("Native film emission calls are missing or duplicated in the payload");
  }
  for (const auto &root : batch.roots) {
    if (root.kind != DEEP_NATIVE_ROOT_LIGHT || batch.optical_full_coverage)
      continue;
    const auto owner = deep_native_camera_root(batch, root.camera_index);
    if (owner.kind == DEEP_NATIVE_ROOT_SURFACE && root.physical_t > owner.physical_t)
      return fail("Camera-chain lamp source lies beyond its opaque owner");
    for (uint32_t segment = segment_head[root.camera_index]; segment != UINT32_MAX;
         segment = segment_next[segment])
    {
      const auto &span = batch.homogeneous[segment];
      if (span.tmin <= root.physical_t && root.physical_t <= span.tmax)
        return fail(
            "Lamp splits a native medium; complete independent Optical coverage is required");
    }
  }
  DeepNativeCaptureVector<uint32_t> point_order(batch.cameras.get_allocator());
  point_order.reserve(batch.points.size());
  for (const auto &point : batch.points) {
    if (point.camera_index >= batch.cameras.size() || !point.root_handle ||
        point.root_handle > batch.roots.size())
      return fail("Native point call references an unknown camera/root");
    const auto &camera = batch.cameras[point.camera_index];
    const auto &root = batch.roots[point.root_handle - 1];
    if (root.camera_index != point.camera_index || point.root_kind != root.kind ||
        point.ordinal >= batch.points.size() || point.root_ordinal != root.ordinal ||
        point.z != root.z || point.pixel != camera.pixel || point.sample != camera.sample ||
        point.routing != DEEP_NATIVE_POINT_COMBINED ||
        !deep_native_path_writes_combined(point.path_flags) ||
        camera.disposition != DEEP_NATIVE_CAMERA_RAY || !std::isfinite(point.clamp_gain) ||
        point.clamp_gain < 0 || point.clamp_gain > 1)
      return fail("Native point film call has invalid routing or immutable root identity");
    const auto ftz = [](const float value) {
      return std::abs(value) < std::numeric_limits<float>::min() ? 0.0f : value;
    };
    for (int c = 0; c < 3; c++) {
      const float clamped = point.pre_clamp_rgb[c] * point.clamp_gain;
      const float clamped_ftz = ftz(ftz(point.pre_clamp_rgb[c]) * ftz(point.clamp_gain));
      const float routed = point.clamp_rgb[c] * point.routing_scale[c];
      const float routed_ftz = ftz(ftz(point.clamp_rgb[c]) * ftz(point.routing_scale[c]));
      if (!std::isfinite(point.pre_clamp_rgb[c]) || !std::isfinite(point.clamp_rgb[c]) ||
          !std::isfinite(point.routing_scale[c]) || !std::isfinite(point.final_rgb[c]) ||
          (clamped != point.clamp_rgb[c] && clamped_ftz != point.clamp_rgb[c]) ||
          (routed != point.final_rgb[c] && routed_ftz != point.final_rgb[c]))
        return fail("Native point call disagrees with its actual clamp/routing arithmetic");
    }
    point_order.push_back(point.ordinal);
  }
  std::sort(point_order.begin(), point_order.end());
  if (std::adjacent_find(point_order.begin(), point_order.end()) != point_order.end())
    return fail("Native point film-call ordinal is duplicated");
  for (size_t i = 0; i < batch.cameras.size(); ++i) {
    const auto &camera = batch.cameras[i];
    if ((camera.disposition == DEEP_NATIVE_CAMERA_CLEAR_TRANSPARENT && segment_count[i]) ||
        (camera.disposition == DEEP_NATIVE_CAMERA_INVALID_BLACK && segment_count[i]))
    {
      return fail("Clear or InvalidCameraBlack sample unexpectedly owns a volume segment");
    }
  }
  if ((!batch.expression_sources.empty() || !batch.expression_nodes.empty() ||
       !batch.expression_calls.empty()) &&
      !domain.require_full_optical)
    return fail("Native emission expressions require complete independent Optical");
  for (size_t i = 0; i < batch.expression_sources.size(); i++) {
    const auto &source = batch.expression_sources[i];
    if (source.camera_index >= batch.cameras.size() || source.ordinal != i + 1 ||
        source.reserved[0] || source.reserved[1] || !std::isfinite(source.z) ||
        !std::isfinite(source.zback) || source.z < 0 || source.zback < source.z)
      return fail("Native expression term has invalid source/depth identity");
    const auto &camera = batch.cameras[source.camera_index];
    const bool world_support = source.flags & DEEP_NATIVE_EXPR_WORLD_SUPPORT;
    const uint32_t support_kind = source.flags & ~DEEP_NATIVE_EXPR_WORLD_SUPPORT;
    if (world_support && (!world_binding(source.object, source.shader) || source.root_handle ||
                          source.prim != PRIM_NONE || source.tmax <= camera.tmax ||
                          (support_kind != DEEP_NATIVE_EXPR_CAMERA_SPAN &&
                           support_kind != DEEP_NATIVE_EXPR_CAMERA_POINT)))
      return fail("Native expression World support has no compiled World binding");
    if (world_support) {
      float front = float(promoted_depth(camera, source.tmin)),
            back = float(promoted_depth(camera, source.tmax));
      if (support_kind == DEEP_NATIVE_EXPR_CAMERA_SPAN &&
          (!deep_native_world_span_depth(promoted_depth(camera, source.tmin),
                                         domain.depth_kind == DeepFrozenDepthKind::Radial,
                                         front) ||
           !deep_native_world_span_depth(promoted_depth(camera, source.tmax),
                                         domain.depth_kind == DeepFrozenDepthKind::Radial,
                                         back)))
        return fail("DepthNotRepresentable: World radial span endpoint exceeds finite encoding");
      if (source.z != front || source.zback != back)
        return fail("Native expression World depth changed its physical support");
    }
    if (camera.disposition != DEEP_NATIVE_CAMERA_RAY)
      return fail("Native expression belongs to a nongeometric camera");
    for (int c = 0; c < 3; c++)
      if (!std::isfinite(source.raw_rgb[c]) || !std::isfinite(source.tau[c]) || source.tau[c] < 0)
        return fail("Native expression source has invalid frozen arithmetic");
    if (support_kind == DEEP_NATIVE_EXPR_CAMERA_SPAN) {
      if (source.root_handle || !std::isfinite(source.tmin) || !std::isfinite(source.tmax) ||
          source.tmin < camera.tmin || (source.tmax > camera.tmax && !world_support) ||
          source.tmin >= source.tmax || source.z >= source.zback)
        return fail("Native expression Beer term lacks complete camera support");
    }
    else if (support_kind == DEEP_NATIVE_EXPR_CAMERA_POINT) {
      if (source.root_handle || !std::isfinite(source.tmin) || source.tmin != source.tmax ||
          source.tmin < camera.tmin || (source.tmax > camera.tmax && !world_support) ||
          source.z != source.zback || source.tau[0] != 0 || source.tau[1] != 0 ||
          source.tau[2] != 0)
        return fail("Native null-collision radiance point cannot certify extinction");
    }
    else if (support_kind == DEEP_NATIVE_EXPR_INHERITED_POINT) {
      if (!source.root_handle || source.root_handle > batch.roots.size())
        return fail("Native expression point has an unknown owner");
      const auto &root = batch.roots[source.root_handle - 1];
      if (root.camera_index != source.camera_index || source.z != root.z ||
          source.zback != root.z || source.tmin != root.physical_t ||
          source.tmax != root.physical_t || source.tau[0] != 0 || source.tau[1] != 0 ||
          source.tau[2] != 0)
        return fail("Native expression point changed its inherited support");
    }
    else
      return fail("Native expression source support kind is unknown");
  }
  for (size_t i = 0; i < batch.expression_nodes.size(); i++) {
    const auto &node = batch.expression_nodes[i];
    if (node.camera_index >= batch.cameras.size() || node.ordinal != i + 1 ||
        node.operation > DEEP_NATIVE_EXPR_SELECT || node.select_b > 1 || node.reserved[0] ||
        node.reserved[1] || node.a > i || node.b > i)
      return fail("Native expression graph is incomplete or not topological");
    for (const uint32_t operand : {node.a, node.b})
      if (operand && batch.expression_nodes[operand - 1].camera_index != node.camera_index)
        return fail("Native expression references another camera graph");
    for (const float weight : node.weight)
      if (!std::isfinite(weight))
        return fail("Native expression weight is not finite");
    if (node.operation == DEEP_NATIVE_EXPR_TERM) {
      if (!node.source || node.source > batch.expression_sources.size() || node.a || node.b ||
          batch.expression_sources[node.source - 1].camera_index != node.camera_index)
        return fail("Native expression Term references an unknown/foreign source");
    }
    else if (node.source)
      return fail("Native expression non-Term owns an unexpected source");
    if ((node.operation == DEEP_NATIVE_EXPR_ZERO && (node.a || node.b)) ||
        ((node.operation == DEEP_NATIVE_EXPR_SNAPSHOT ||
          node.operation == DEEP_NATIVE_EXPR_SCALE) &&
         node.b) ||
        (node.operation != DEEP_NATIVE_EXPR_SELECT && node.select_b))
      return fail("Native expression operation has unexpected operands");
    if (node.operation != DEEP_NATIVE_EXPR_SCALE && node.operation != DEEP_NATIVE_EXPR_MIX)
      for (const float weight : node.weight)
        if (weight != 1)
          return fail("Native expression contains an unconsumed weight");
    if (node.operation == DEEP_NATIVE_EXPR_MIX &&
        (node.weight[0] < 0 || node.weight[0] > 1 || node.weight[1] != node.weight[0] ||
         node.weight[2] != node.weight[0]))
      return fail("Native emission Mix requires its actual bounded scalar interpolation weight");
  }
  for (size_t i = 0; i < batch.expression_calls.size(); i++) {
    const auto &call = batch.expression_calls[i];
    if (call.camera_index >= batch.cameras.size() || call.ordinal != i + 1 || call.reserved ||
        call.root_handle || call.head > batch.expression_nodes.size() ||
        (call.head && batch.expression_nodes[call.head - 1].camera_index != call.camera_index) ||
        call.routing != DEEP_NATIVE_POINT_COMBINED ||
        !deep_native_path_writes_combined(call.path_flags) || !std::isfinite(call.clamp_gain) ||
        call.clamp_gain < 0 || call.clamp_gain > 1)
      return fail("Native expression film call has invalid graph/routing identity");
    const auto &camera = batch.cameras[call.camera_index];
    if (call.pixel != camera.pixel || call.sample != camera.sample)
      return fail("Native expression film call changed its native sample");
    const auto ftz = [](float v) {
      return std::abs(v) < std::numeric_limits<float>::min() ? 0.0f : v;
    };
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(call.pre_clamp_rgb[c]) || !std::isfinite(call.post_clamp_rgb[c]) ||
          (!call.head && call.pre_clamp_rgb[c] != 0) ||
          (call.pre_clamp_rgb[c] * call.clamp_gain != call.post_clamp_rgb[c] &&
           ftz(ftz(call.pre_clamp_rgb[c]) * ftz(call.clamp_gain)) != call.post_clamp_rgb[c]))
        return fail("Native expression call disagrees with its native film clamp");
    }
  }
  return true;
}

CCL_NAMESPACE_END
