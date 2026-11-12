/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_frame_production_internal.h"
#include "integrator/deep_gpu_frame_work.h"
#include "session/deep_emission_expression.h"
#include "session/buffers.h"
#include "util/half.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>

CCL_NAMESPACE_BEGIN
using namespace deep_production_detail;

namespace {
bool same_binding(const DeepNativeCaptureDomain &a, const DeepNativeCaptureDomain &b)
{
  return a.id == b.id && a.device_generation == b.device_generation &&
         a.layout_generation == b.layout_generation && a.start_sample == b.start_sample &&
         a.samples_num == b.samples_num && a.sample_offset == b.sample_offset &&
         a.require_full_optical == b.require_full_optical && a.opaque_world == b.opaque_world &&
         a.world_volume_shader == b.world_volume_shader &&
         a.world_volume_object == b.world_volume_object && a.core_x == b.core_x &&
         a.depth_kind == b.depth_kind &&
         std::equal(std::begin(a.depth_transform),
                    std::end(a.depth_transform),
                    std::begin(b.depth_transform)) &&
         a.core_y == b.core_y && a.core_width == b.core_width && a.core_height == b.core_height &&
         a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
         a.scheduled_admissions == b.scheduled_admissions;
}

DeepSampleKey sample_key(const DeepNativeCameraRecord &camera)
{
  return {camera.x,
          camera.y,
          camera.sample,
          camera.subset_offset,
          camera.rng_pixel,
          camera.rng_offset,
          DeepCameraDisposition(camera.disposition),
          camera.scheduled_sample};
}

/* Typed transport uses the same PageStore ledger as capture packets, curves and output.
 * It cannot publish any uncommitted slot or a reserved hole as valid input. */
class CapturePageWriter {
 public:
  CapturePageWriter(DeepPageStore &store,
                    const DeepRecordKind kind,
                    ProductionVector<DeepPageLease> &pages)
      : store_(store), kind_(kind), pages_(pages)
  {
  }
  template<typename T> DeepStatus append(const DeepRecordTag &tag, const T &payload)
  {
    if (!page_)
      if (const auto status = store_.allocate(kind_, page_); !status)
        return status;
    uint64_t handle;
    auto status = store_.reserve(page_, handle);
    if (status.code == DeepErrorCode::NeedPage) {
      if (status = finish(); !status)
        return status;
      if (status = store_.allocate(kind_, page_); !status)
        return status;
      status = store_.reserve(page_, handle);
    }
    if (!status)
      return status;
    if (status = store_.write(
            handle, tag, {reinterpret_cast<const uint8_t *>(&payload), sizeof(payload)});
        !status)
      return status;
    return store_.commit(handle);
  }
  DeepStatus finish()
  {
    if (!page_)
      return {};
    DeepPageLease lease;
    if (const auto status = store_.seal(page_, lease); !status)
      return status;
    if (const auto status = store_.retire(page_); !status)
      return status;
    page_ = 0;
    pages_.push_back(lease);
    return {};
  }
  ~CapturePageWriter()
  {
    if (page_) {
      DeepPageLease lease;
      if (store_.seal(page_, lease))
        store_.retire(page_);
    }
  }

 private:
  DeepPageStore &store_;
  DeepRecordKind kind_;
  ProductionVector<DeepPageLease> &pages_;
  uint32_t page_ = 0;
};
}  // namespace

bool DeepFrameProduction::complete_reference_capture(DeepNativeCaptureBatch &&batch)
{
  if (route_ != DeepProductionRoute::Reference || bounded_processing_ ||
      !storage_ || !storage_->reference)
    return fail(DeepErrorCode::InvalidState, "Reference capture requires the reference route");
  if (!storage_ || !error())
    return false;
  DeepNativeCaptureDomain mapped_domain = batch.domain;
  if (!translated_coordinate(batch.domain.x, native_to_canonical_.x, mapped_domain.x) ||
      !translated_coordinate(batch.domain.y, native_to_canonical_.y, mapped_domain.y) ||
      (batch.domain.core_width &&
       (!translated_coordinate(
            batch.domain.core_x, native_to_canonical_.x, mapped_domain.core_x) ||
        !translated_coordinate(
            batch.domain.core_y, native_to_canonical_.y, mapped_domain.core_y))))
    return fail(DeepErrorCode::InvalidLayout, "Native Deep raster mapping overflow");
  auto domain = std::find_if(storage_->domains.begin(),
                             storage_->domains.end(),
                             [&](const auto &d) { return d.binding.id == batch.domain.id; });
  if (domain == storage_->domains.end() || domain->complete ||
      !same_binding(domain->binding, mapped_domain))
    return fail(DeepErrorCode::InvalidReference,
                "Unbound, changed or repeated native Deep domain");
  if (!(batch.cameras.get_allocator() ==
        DeepBudgetAllocator<DeepNativeCameraRecord>(memory_budget())) ||
      !(batch.homogeneous.get_allocator() ==
        DeepBudgetAllocator<DeepNativeHomogeneousRecord>(memory_budget())) ||
      !(batch.roots.get_allocator() ==
        DeepBudgetAllocator<DeepNativeRootRecord>(memory_budget())) ||
      !(batch.points.get_allocator() ==
        DeepBudgetAllocator<DeepNativePointRecord>(memory_budget())) ||
      !(batch.optical_events.get_allocator() ==
        DeepBudgetAllocator<DeepOpticalEvent>(memory_budget())) ||
      !(batch.optical_ledger.get_allocator() ==
        DeepBudgetAllocator<DeepOpticalLedger>(memory_budget())) ||
      !(batch.optical_pieces.get_allocator() ==
        DeepBudgetAllocator<DeepOwnedOpticalPiece>(memory_budget())) ||
      !(batch.optical_visibility.get_allocator() ==
        DeepBudgetAllocator<DeepOwnedOpticalVisibility>(memory_budget())) ||
      !(batch.optical_coverage.get_allocator() ==
        DeepBudgetAllocator<DeepOwnedOpticalCoverage>(memory_budget())))
    return fail(DeepErrorCode::InvalidReference,
                "Native Deep packet has a foreign allocation quota");
  try {
    std::string validation_error;
    if (!deep_native_capture_validate(batch, validation_error))
      return fail(DeepErrorCode::InvalidPayload, validation_error);
    /* Only the already joined/validated packet crosses into canonical pixel space.
     * Native ray geometry, RNG, film pixel indices and every source value stay frozen. */
    for (const auto &camera : batch.cameras) {
      int x, y;
      if (!translated_coordinate(camera.x, native_to_canonical_.x, x) ||
          !translated_coordinate(camera.y, native_to_canonical_.y, y))
        return fail(DeepErrorCode::InvalidLayout, "Native Deep camera mapping overflow");
    }
    batch.domain = mapped_domain;
    for (auto &camera : batch.cameras) {
      camera.x = int(int64_t(camera.x) + native_to_canonical_.x);
      camera.y = int(int64_t(camera.y) + native_to_canonical_.y);
    }
    const char *verify_payload = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
    if (verify_payload && std::strcmp(verify_payload, "1") == 0) {
      uint64_t roots_by_kind[DEEP_NATIVE_ROOT_DISTANT + 1] = {};
      uint64_t independent_direct_calls = 0;
      for (const auto &root : batch.roots)
        roots_by_kind[root.kind]++;
      if (batch.domain.world_volume_shader != SHADER_NONE) {
        size_t world_spans = 0, world_roots = 0, world_expressions = 0;
        double max_native_t = 0, max_optical_t = 0, max_tau = 0;
        for (const auto &span : batch.homogeneous) {
          if (span.flags & DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT) {
            world_spans++;
            max_native_t = std::max(max_native_t, double(span.tmax));
            for (const double tau : span.tau)
              max_tau = std::max(max_tau, tau);
          }
        }
        for (const auto &root : batch.roots) {
          if (root.flags & DEEP_NATIVE_ROOT_WORLD_SUPPORT) {
            world_roots++;
            max_native_t = std::max(max_native_t, double(root.physical_t));
          }
        }
        for (const auto &source : batch.expression_sources) {
          if (source.flags & DEEP_NATIVE_EXPR_WORLD_SUPPORT) {
            world_expressions++;
            max_native_t = std::max(max_native_t, double(source.tmax));
            for (const double tau : source.tau)
              max_tau = std::max(max_tau, tau);
          }
        }
        for (const auto &coverage : batch.optical_coverage)
          max_optical_t = std::max(max_optical_t, double(coverage.cursor_t));
        std::fprintf(stderr,
                     "DEEP_FRAME_WORLD_SUPPORT domain=%llu shader=%d object=%d homogeneous=%zu "
                     "roots=%zu max_native_t=%.17g max_optical_t=%.17g max_tau=%.17g "
                     "expressions=%zu\n",
                     static_cast<unsigned long long>(batch.domain.id),
                     batch.domain.world_volume_shader,
                     batch.domain.world_volume_object,
                     world_spans,
                     world_roots,
                     max_native_t,
                     max_optical_t,
                     max_tau,
                     world_expressions);
      }
      for (const auto &point : batch.points)
        independent_direct_calls += point.root_kind == DEEP_NATIVE_ROOT_VOLUME_DIRECT &&
                                    batch.cameras[point.camera_index].root_handle !=
                                        point.root_handle;
      uint64_t mnee_admissions = 0, mnee_calls = 0;
      for (const auto &root : batch.roots)
        mnee_admissions += bool(root.flags & DEEP_NATIVE_ROOT_MNEE_ADMISSION);
      for (const auto &point : batch.points)
        mnee_calls += bool(batch.roots[point.root_handle - 1].flags &
                           DEEP_NATIVE_ROOT_MNEE_ADMISSION);
      if (mnee_admissions)
        std::fprintf(
            stderr,
            "DEEP_FRAME_MNEE_SOURCE domain=%llu pending_admissions=%llu joined_point_calls=%llu\n",
            static_cast<unsigned long long>(batch.domain.id),
            static_cast<unsigned long long>(mnee_admissions),
            static_cast<unsigned long long>(mnee_calls));
      std::fprintf(
          stderr,
          "DEEP_FRAME_ROOT_FAMILY domain=%llu surface=%llu world=%llu volume_scatter=%llu "
          "volume_direct=%llu light=%llu independent_direct_calls=%llu distant=%llu\n",
          static_cast<unsigned long long>(batch.domain.id),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_SURFACE]),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_WORLD]),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_VOLUME_SCATTER]),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_VOLUME_DIRECT]),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_LIGHT]),
          static_cast<unsigned long long>(independent_direct_calls),
          static_cast<unsigned long long>(roots_by_kind[DEEP_NATIVE_ROOT_DISTANT]));
      uint64_t clamped_calls = 0;
      uint64_t ftz_components = 0;
      float min_gain = 1;
      for (const auto &source : batch.homogeneous) {
        if (source.emission_calls) {
          clamped_calls += source.clamp_gain < 1;
          min_gain = std::min(min_gain, source.clamp_gain);
          for (int c = 0; c < 3; c++)
            ftz_components += source.pre_clamp_rgb[c] * source.clamp_gain != 0 &&
                              source.post_clamp_rgb[c] == 0;
        }
      }
      for (const auto &source : batch.points) {
        clamped_calls += source.clamp_gain < 1;
        min_gain = std::min(min_gain, source.clamp_gain);
        for (int c = 0; c < 3; c++)
          ftz_components += source.pre_clamp_rgb[c] * source.clamp_gain != 0 &&
                            source.clamp_rgb[c] == 0;
      }
      for (const auto &call : batch.expression_calls) {
        clamped_calls += call.clamp_gain < 1;
        min_gain = std::min(min_gain, call.clamp_gain);
        for (int c = 0; c < 3; c++)
          ftz_components += call.pre_clamp_rgb[c] * call.clamp_gain != 0 &&
                            call.post_clamp_rgb[c] == 0;
      }
      if (!batch.expression_nodes.empty())
        std::fprintf(
            stderr,
            "DEEP_FRAME_EMISSION_GRAPH domain=%llu sources=%llu nodes=%llu film_calls=%llu\n",
            static_cast<unsigned long long>(batch.domain.id),
            static_cast<unsigned long long>(batch.expression_sources.size()),
            static_cast<unsigned long long>(batch.expression_nodes.size()),
            static_cast<unsigned long long>(batch.expression_calls.size()));
      if (!batch.expression_nodes.empty()) {
        uint64_t operations[7] = {}, camera_points = 0, inherited_points = 0;
        for (const auto &node : batch.expression_nodes)
          operations[node.operation]++;
        for (const auto &source : batch.expression_sources) {
          camera_points += (source.flags & ~DEEP_NATIVE_EXPR_WORLD_SUPPORT) ==
                           DEEP_NATIVE_EXPR_CAMERA_POINT;
          inherited_points += source.flags == DEEP_NATIVE_EXPR_INHERITED_POINT;
        }
        std::fprintf(stderr,
                     "DEEP_FRAME_EMISSION_OPERATIONS domain=%llu term=%llu add=%llu snapshot=%llu "
                     "scale=%llu mix=%llu select=%llu camera_points=%llu inherited_points=%llu\n",
                     static_cast<unsigned long long>(batch.domain.id),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_TERM]),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_ADD]),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_SNAPSHOT]),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_SCALE]),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_MIX]),
                     static_cast<unsigned long long>(operations[DEEP_NATIVE_EXPR_SELECT]),
                     static_cast<unsigned long long>(camera_points),
                     static_cast<unsigned long long>(inherited_points));
      }
      std::fprintf(stderr,
                   "DEEP_FRAME_FILM_CALL domain=%llu calls=%llu clamped=%llu min_gain=%.9g "
                   "ftz_components=%llu homogeneous_calls=%llu point_calls=%llu roots=%llu\n",
                   static_cast<unsigned long long>(batch.domain.id),
                   static_cast<unsigned long long>(batch.domain.emission_calls +
                                                   batch.domain.point_written +
                                                   batch.domain.expression_call_written),
                   static_cast<unsigned long long>(clamped_calls),
                   double(min_gain),
                   static_cast<unsigned long long>(ftz_components),
                   static_cast<unsigned long long>(batch.domain.emission_calls),
                   static_cast<unsigned long long>(batch.domain.point_written),
                   static_cast<unsigned long long>(batch.domain.root_written));
      if (batch.domain.auxiliary_film_calls)
        std::fprintf(stderr,
                     "DEEP_FRAME_CATCHER_ROUTING domain=%llu auxiliary_calls=%llu "
                     "camera_population=%llu\n",
                     static_cast<unsigned long long>(batch.domain.id),
                     static_cast<unsigned long long>(batch.domain.auxiliary_film_calls),
                     static_cast<unsigned long long>(batch.cameras.size()));
    }
    const uint64_t packet_sources = uint64_t(batch.homogeneous.size()) + batch.points.size() +
                                    batch.optical_pieces.size() + batch.optical_visibility.size() +
                                    batch.expression_sources.size() +
                                    batch.expression_nodes.size() + batch.expression_calls.size();
    if (packet_sources > SIZE_MAX - storage_->reference->sources.size())
      return fail(DeepErrorCode::InvalidCount, "Native/owned source arena range overflow");
    if (batch.cameras.size() > UINT32_MAX || batch.points.size() > UINT32_MAX ||
        batch.homogeneous.size() > UINT32_MAX ||
        batch.cameras.size() > std::numeric_limits<size_t>::max() - storage_->reference->rays.size() ||
        batch.homogeneous.size() > std::numeric_limits<size_t>::max() - storage_->reference->sources.size() ||
        batch.cameras.size() >= UINT64_MAX - storage_->reference->next_ray ||
        batch.cameras.size() > UINT64_MAX - storage_->reference->active_rays ||
        packet_sources + batch.domain.root_written >= UINT64_MAX - storage_->reference->next_source)
      return fail(DeepErrorCode::InvalidCount, "Native Deep ray/source identity overflow");
    ProductionVector<DeepSampleAdmission> admissions{
        DeepBudgetAllocator<DeepSampleAdmission>(memory_budget())};
    ProductionVector<DeepPageLease> pages{DeepBudgetAllocator<DeepPageLease>(memory_budget())};
    admissions.reserve(batch.cameras.size());
    const size_t batch_index = storage_->reference->batches.size(), ray_begin = storage_->reference->rays.size();
    ProductionVector<uint64_t> root_ids{DeepBudgetAllocator<uint64_t>(memory_budget())};
    root_ids.reserve(batch.roots.size());
    for (size_t i = 0; i < batch.roots.size(); i++)
      root_ids.push_back(storage_->reference->next_source++);
    ProductionVector<size_t> camera_rays(
        batch.cameras.size(), no_index, DeepBudgetAllocator<size_t>(memory_budget()));
    const auto &d = batch.domain;
    const auto is_core = [&](const DeepNativeCameraRecord &camera) {
      return !d.core_width || (camera.x >= d.core_x && camera.y >= d.core_y &&
                               int64_t(camera.x) < int64_t(d.core_x) + d.core_width &&
                               int64_t(camera.y) < int64_t(d.core_y) + d.core_height);
    };
    storage_->reference->rays.reserve(ray_begin + batch.cameras.size());
    storage_->reference->sources.reserve(storage_->reference->sources.size() + size_t(packet_sources));
    for (size_t i = 0; i < batch.cameras.size(); i++) {
      auto &camera = batch.cameras[i];
      if (!is_core(camera))
        continue; /* Fully validated native halo, deliberately outside output population. */
      camera_rays[i] = storage_->reference->rays.size();
      if (batch.optical_full_coverage && camera.disposition != DEEP_NATIVE_CAMERA_INVALID_BLACK)
        camera.disposition = DEEP_NATIVE_CAMERA_RAY;
      size_t pixel;
      if (!storage_->index(camera.x, camera.y, pixel) || storage_->pixels[pixel].closed)
        return fail(DeepErrorCode::InvalidReference,
                    "Native camera arrived outside an open pixel");
      PixelState &owner = storage_->pixels[pixel];
      if (owner.count == UINT64_MAX)
        return fail(DeepErrorCode::InvalidCount, "Native Deep pixel population overflow");
      const uint64_t ray = storage_->reference->next_ray++;
      admissions.push_back({sample_key(camera), ray});
      storage_->reference->rays.push_back({ray, batch_index, i, owner.ray_head, no_index});
      if (camera.root_handle)
        storage_->reference->rays.back().root = root_ids[camera.root_handle - 1];
      owner.ray_head = storage_->reference->rays.size() - 1;
      owner.count++;
    }
    CapturePageWriter optical(*pipeline_.pages(), DeepRecordKind::OpticalPiece, pages);
    CapturePageWriter radiance(*pipeline_.pages(), DeepRecordKind::RadianceTerm, pages);
    CapturePageWriter visibility(*pipeline_.pages(), DeepRecordKind::VisibilityPoint, pages);
    CapturePageWriter expression_source(
        *pipeline_.pages(), DeepRecordKind::NativeExpressionSource, pages);
    CapturePageWriter expression_node(
        *pipeline_.pages(), DeepRecordKind::NativeExpressionNode, pages);
    CapturePageWriter expression_call(
        *pipeline_.pages(), DeepRecordKind::NativeExpressionCall, pages);
    const auto append_expression = [&](const auto &records,
                                       const CapturedSourceKind kind,
                                       const DeepRecordKind page_kind,
                                       CapturePageWriter &writer) {
      for (size_t i = 0; i < records.size(); i++) {
        const auto &native = records[i];
        if (camera_rays[native.camera_index] == no_index)
          continue;
        auto &ray = storage_->reference->rays[camera_rays[native.camera_index]];
        const uint64_t id = storage_->reference->next_source++;
        storage_->reference->sources.push_back({id, batch_index, i, ray.source_head, kind});
        ray.source_head = storage_->reference->sources.size() - 1;
        const DeepRecordTag tag{ray.id, id, 0, native.ordinal, uint32_t(page_kind), 0};
        if (const auto status = writer.append(tag, native); !status)
          return fail(status.code, status.message);
      }
      return true;
    };
    if (!append_expression(batch.expression_sources,
                           CapturedSourceKind::ExpressionSource,
                           DeepRecordKind::NativeExpressionSource,
                           expression_source) ||
        !append_expression(batch.expression_nodes,
                           CapturedSourceKind::ExpressionNode,
                           DeepRecordKind::NativeExpressionNode,
                           expression_node) ||
        !append_expression(batch.expression_calls,
                           CapturedSourceKind::ExpressionCall,
                           DeepRecordKind::NativeExpressionCall,
                           expression_call))
      return false;
    for (size_t i = 0; i < batch.roots.size(); i++) {
      const auto &root = batch.roots[i];
      if ((root.flags & DEEP_NATIVE_ROOT_RADIANCE_ONLY) ||
          (batch.optical_full_coverage && root.kind != DEEP_NATIVE_ROOT_WORLD))
        continue;
      if (camera_rays[root.camera_index] == no_index)
        continue;
      const auto &ray = storage_->reference->rays[camera_rays[root.camera_index]];
      const DeepVisibilityPoint point{root.z,
                                      {0, 0, 0},
                                      {0, 0, 0},
                                      uint32_t(root.object),
                                      uint32_t(root.prim),
                                      uint32_t(root.shader),
                                      root.packed_normal,
                                      root.ordinal};
      const DeepRecordTag tag{ray.id,
                              ray.root,
                              0,
                              root.ordinal,
                              uint32_t(DeepRecordKind::VisibilityPoint),
                              root.flags};
      if (const auto status = visibility.append(tag, point); !status)
        return fail(status.code, status.message);
    }
    for (size_t i = 0; i < batch.homogeneous.size(); i++) {
      const auto &native = batch.homogeneous[i];
      if (camera_rays[native.camera_index] == no_index)
        continue;
      CapturedRay &ray = storage_->reference->rays[camera_rays[native.camera_index]];
      const uint64_t source = storage_->reference->next_source++;
      storage_->reference->sources.push_back({source, batch_index, i, ray.source_head});
      ray.source_head = storage_->reference->sources.size() - 1;
      DeepRecordTag tag{
          ray.id, source, 0, native.ordinal, uint32_t(DeepRecordKind::OpticalPiece), native.flags};
      if (!batch.optical_full_coverage) {
        const DeepOpticalPiece piece{
            native.z,
            native.zback,
            {float(native.tau[0]), float(native.tau[1]), float(native.tau[2])},
            uint32_t(native.source_object),
            native.ordinal,
            native.flags};
        if (const auto status = optical.append(tag, piece); !status)
          return fail(status.code, status.message);
      }
      if (native.emission_calls) {
        const DeepRadianceTerm term{
            native.z,
            native.zback,
            {native.post_clamp_rgb[0], native.post_clamp_rgb[1], native.post_clamp_rgb[2]},
            {native.tau[0], native.tau[1], native.tau[2]},
            source,
            native.tau[0] == 0 ? 2u : 1u,
            1u}; /* M1 capture certifies raw Combined routing. */
        tag.kind = uint32_t(DeepRecordKind::RadianceTerm);
        if (const auto status = radiance.append(tag, term); !status)
          return fail(status.code, status.message);
      }
    }
    for (size_t i = 0; i < batch.points.size(); i++) {
      const auto &native = batch.points[i];
      if (camera_rays[native.camera_index] == no_index)
        continue;
      CapturedRay &ray = storage_->reference->rays[camera_rays[native.camera_index]];
      const uint64_t source = storage_->reference->next_source++;
      storage_->reference->sources.push_back(
          {source, batch_index, i, ray.source_head, CapturedSourceKind::Point});
      ray.source_head = storage_->reference->sources.size() - 1;
      const DeepRadianceTerm term{native.z,
                                  native.z,
                                  {native.final_rgb[0], native.final_rgb[1], native.final_rgb[2]},
                                  {0, 0, 0},
                                  source,
                                  0,
                                  DEEP_NATIVE_POINT_COMBINED};
      const DeepRecordTag tag{ray.id,
                              root_ids[native.root_handle - 1],
                              0,
                              native.ordinal,
                              uint32_t(DeepRecordKind::RadianceTerm),
                              DEEP_KNOT_POINT_RADIANCE};
      if (const auto status = radiance.append(tag, term); !status)
        return fail(status.code, status.message);
    }
    for (size_t i = 0; i < batch.optical_pieces.size(); i++) {
      const auto &native = batch.optical_pieces[i];
      if (camera_rays[native.receipt] == no_index)
        continue;
      auto &ray = storage_->reference->rays[camera_rays[native.receipt]];
      const uint64_t source = storage_->reference->next_source++;
      storage_->reference->sources.push_back(
          {source, batch_index, i, ray.source_head, CapturedSourceKind::OwnedOptical});
      ray.source_head = storage_->reference->sources.size() - 1;
      const DeepOpticalPiece piece{native.z,
                                   native.zback,
                                   {native.tau[0], native.tau[1], native.tau[2]},
                                   native.stack_hash[0],
                                   native.segment,
                                   native.flags};
      const DeepRecordTag tag{
          ray.id, source, 0, uint32_t(i), uint32_t(DeepRecordKind::OpticalPiece), native.flags};
      if (const auto status = optical.append(tag, piece); !status)
        return fail(status.code, status.message);
    }
    for (size_t i = 0; i < batch.optical_visibility.size(); i++) {
      const auto &native = batch.optical_visibility[i];
      if (camera_rays[native.receipt] == no_index)
        continue;
      auto &ray = storage_->reference->rays[camera_rays[native.receipt]];
      const uint64_t source = storage_->reference->next_source++;
      storage_->reference->sources.push_back(
          {source, batch_index, i, ray.source_head, CapturedSourceKind::OwnedVisibility});
      ray.source_head = storage_->reference->sources.size() - 1;
      const DeepVisibilityPoint point{
          native.z,
          {native.transmission[0], native.transmission[1], native.transmission[2]},
          {native.holdout[0], native.holdout[1], native.holdout[2]},
          uint32_t(native.object),
          uint32_t(native.prim),
          uint32_t(native.shader),
          native.packed_normal,
          native.ordinal};
      const DeepRecordTag tag{ray.id,
                              source,
                              0,
                              native.ordinal,
                              uint32_t(DeepRecordKind::VisibilityPoint),
                              native.flags};
      if (const auto status = visibility.append(tag, point); !status)
        return fail(status.code, status.message);
    }
    if (const auto status = optical.finish(); !status)
      return fail(status.code, status.message);
    if (const auto status = radiance.finish(); !status)
      return fail(status.code, status.message);
    if (const auto status = visibility.finish(); !status)
      return fail(status.code, status.message);
    if (const auto status = expression_source.finish(); !status)
      return fail(status.code, status.message);
    if (const auto status = expression_node.finish(); !status)
      return fail(status.code, status.message);
    if (const auto status = expression_call.finish(); !status)
      return fail(status.code, status.message);
    auto joined = completion(storage_->frame, batch.domain, true);
    joined.native_admitted = admissions.size();
    std::fprintf(stderr,
                 "DEEP_FRAME_NATIVE_POPULATION domain=%llu output=%llu halo=%llu\n",
                 static_cast<unsigned long long>(batch.domain.id),
                 static_cast<unsigned long long>(admissions.size()),
                 static_cast<unsigned long long>(batch.cameras.size() - admissions.size()));
    if (const auto status = pipeline_.commit_native_domain(joined, admissions, pages); !status)
      return false;
    storage_->reference->batches.push_back(std::move(batch));
    storage_->reference->active_rays += admissions.size();
    domain->complete = true;
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Native Deep packet/typed-page quota exhausted");
  }
  catch (const std::length_error &) {
    return fail(DeepErrorCode::InvalidCount, "Native Deep packet/typed-page count overflow");
  }
}

DeepReferenceCapture *DeepFrameProduction::reference_capture()
{
  return route_ == DeepProductionRoute::Reference && !bounded_processing_ && storage_ ?
             storage_->reference.get() : nullptr;
}

bool deep_production_detail::ReferenceStorage::begin_native_capture(
    const DeepNativeCaptureDomain &domain)
{
  return owner.begin_reference_capture(domain);
}

bool deep_production_detail::ReferenceStorage::complete_native_capture(
    DeepNativeCaptureBatch &&batch)
{
  return owner.complete_reference_capture(std::move(batch));
}

bool deep_production_detail::ReferenceStorage::close_native_tile(
    const int x, const int y, const int width, const int height,
    const std::span<const uint32_t> counts, const float exposure)
{
  return owner.close_reference_native_tile(x, y, width, height, counts, exposure);
}

bool DeepFrameProduction::close_reference_native_tile(const int native_x,
                                            const int native_y,
                                            const int width,
                                            const int height,
                                            const std::span<const uint32_t> counts,
                                            const float exposure)
{
  if (route_ != DeepProductionRoute::Reference || bounded_processing_ ||
      !storage_ || !storage_->reference)
    return fail(DeepErrorCode::InvalidState, "Reference tile closure requires the reference route");
  if (!storage_ || !error())
    return false;
  int x, y;
  if (!translated_coordinate(native_x, native_to_canonical_.x, x) ||
      !translated_coordinate(native_y, native_to_canonical_.y, y))
    return fail(DeepErrorCode::InvalidLayout, "Native Deep final tile mapping overflow");
  size_t first;
  if (!storage_->tile(x, y, width, height, first) ||
      uint64_t(width) * uint64_t(height) != counts.size() || !std::isfinite(exposure) ||
      exposure < 0)
    return fail(DeepErrorCode::InvalidLayout, "Invalid final native Deep tile/counts/exposure");
  if (storage_->reference->exposure_known && exposure != storage_->reference->exposure)
    return fail(DeepErrorCode::InvalidPayload, "Native Deep frame Film exposure changed");
  for (const auto &domain : storage_->domains) {
    const auto &coverage = domain.binding;
    if (!domain.complete && int64_t(x) < int64_t(coverage.x) + coverage.width &&
        int64_t(coverage.x) < int64_t(x) + width &&
        int64_t(y) < int64_t(coverage.y) + coverage.height &&
        int64_t(coverage.y) < int64_t(y) + height)
      return fail(DeepErrorCode::Incomplete, "Final native Deep tile has an unjoined domain");
  }
  uint64_t closed_rays = 0;
  for (size_t i = 0; i < counts.size(); i++) {
    const size_t pixel = first + (i / size_t(width)) * storage_->window.width + i % size_t(width);
    if (storage_->pixels[pixel].closed)
      return fail(DeepErrorCode::DuplicatePixel, "Native Deep pixel already finally closed");
    if (!counts[i] || storage_->pixels[pixel].count != counts[i])
      return fail(DeepErrorCode::PopulationMismatch,
                  "Captured population differs from raw Film count");
    if (counts[i] > UINT64_MAX - closed_rays)
      return fail(DeepErrorCode::InvalidCount, "Native closed tile population overflow");
    closed_rays += counts[i];
  }
  if (closed_rays > storage_->reference->active_rays)
    return fail(DeepErrorCode::PopulationMismatch, "Closed tile exceeds active native population");
  try {
    const DeepMemoryBudget &budget = memory_budget();
    ProductionVector<DeepCanonicalPixel> canonical{
        DeepBudgetAllocator<DeepCanonicalPixel>(budget)};
    ProductionVector<DeepNativeCurve> curves{DeepBudgetAllocator<DeepNativeCurve>(budget)};
    ProductionVector<DeepNativeCurveRay> rays{DeepBudgetAllocator<DeepNativeCurveRay>(budget)};
    struct Range {
      size_t optical, optical_count, radiance, radiance_count, visibility, visibility_count;
    };
    ProductionVector<Range> ranges{DeepBudgetAllocator<Range>(budget)};
    ProductionVector<DeepCurveOptical> supports{DeepBudgetAllocator<DeepCurveOptical>(budget)};
    ProductionVector<DeepCurveVisibility> visibility{
        DeepBudgetAllocator<DeepCurveVisibility>(budget)};
    ProductionVector<DeepCurveRadiance> emission{DeepBudgetAllocator<DeepCurveRadiance>(budget)};
    ProductionVector<DeepEmissionSource> emission_sources{
        DeepBudgetAllocator<DeepEmissionSource>(budget)};
    ProductionVector<DeepEmissionNode> emission_nodes{
        DeepBudgetAllocator<DeepEmissionNode>(budget)};
    ProductionVector<DeepEmissionFilmCall> emission_calls{
        DeepBudgetAllocator<DeepEmissionFilmCall>(budget)};
    ProductionVector<const CapturedSource *> ordered{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    ProductionVector<const CapturedSource *> owned_optical{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    ProductionVector<const CapturedSource *> owned_vis{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    ProductionVector<const CapturedSource *> expression_source_records{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    ProductionVector<const CapturedSource *> expression_node_records{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    ProductionVector<const CapturedSource *> expression_call_records{
        DeepBudgetAllocator<const CapturedSource *>(budget)};
    struct RadiancePhysical {
      uint64_t source;
      double front, back;
      uint32_t ordinal, flags;
    };
    ProductionVector<RadiancePhysical> radiance_physical{
        DeepBudgetAllocator<RadiancePhysical>(budget)};
    ProductionVector<size_t> mapped_rays{DeepBudgetAllocator<size_t>(budget)};
    ProductionVector<size_t> final_radiance{DeepBudgetAllocator<size_t>(budget)};
    canonical.reserve(counts.size());
    curves.reserve(counts.size());
    std::shared_ptr<DeepEncodedTileStorage> encoded_tile;
    if (storage_->consumers_frozen) {
      if (counts.size() > SIZE_MAX / storage_->consumers.size())
        return fail(DeepErrorCode::InvalidCount, "Encoded consumer tile size overflow");
      encoded_tile = std::allocate_shared<DeepEncodedTileStorage>(
          DeepBudgetAllocator<DeepEncodedTileStorage>(budget), budget);
      encoded_tile->window = {x, y, uint32_t(width), uint32_t(height)};
      encoded_tile->ranges.resize(counts.size() * storage_->consumers.size());
    }
    for (size_t i = 0; i < counts.size(); i++) {
      const size_t pixel = first + (i / size_t(width)) * storage_->window.width +
                           i % size_t(width);
      rays.clear();
      ranges.clear();
      supports.clear();
      emission.clear();
      visibility.clear();
      mapped_rays.clear();
      std::optional<DeepDepthSupportBuilder> depth_builder;
      for (size_t ray_index = storage_->pixels[pixel].ray_head; ray_index != no_index;) {
        const CapturedRay &captured = storage_->reference->rays[ray_index];
        const auto &native = storage_->reference->batches[captured.batch].cameras[captured.camera];
        const auto &native_batch = storage_->reference->batches[captured.batch];
        const DeepNativeRootRecord root = deep_native_camera_root(native_batch, captured.camera);
        DeepNativeCurveRay ray;
        ray.ray = captured.id;
        ray.sample = sample_key(native);
        ray.population_weight = native.population_weight;
        ray.native_complete = true;
        /* Native error masks are checked before domain commit, never reinterpreted as
         * the builder's independent M2 feature mask. */
        if (native.unsupported_features)
          return fail(DeepErrorCode::InvalidPayload, "M1 native camera requires M2 semantics");
        const size_t begin = supports.size();
        const size_t emission_begin = emission.size();
        const size_t visibility_begin = visibility.size();
        if (storage_->window.width <= 16 && storage_->window.height <= 16 &&
            deep_frame_pipeline_requested(
                std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VALIDATION_SOURCES")))
          std::fprintf(stderr,
                       "DEEP_FRAME_REFERENCE_CAMERA ray=%llu x=%d y=%d sample=%u disposition=%u "
                       "population_weight=%.17g sensitivity_r=%.17g sensitivity_g=%.17g "
                       "sensitivity_b=%.17g\n",
                       static_cast<unsigned long long>(ray.ray),
                       native.x,
                       native.y,
                       native.sample,
                       native.disposition,
                       double(native.population_weight),
                       double(native.throughput[0]),
                       double(native.throughput[1]),
                       double(native.throughput[2]));
        emission_sources.clear();
        emission_nodes.clear();
        emission_calls.clear();
        if (native.disposition == DEEP_NATIVE_CAMERA_RAY) {
          uint64_t depth_map = 0;
          if (native_batch.optical_full_coverage) {
            if (!depth_builder)
              depth_builder.emplace(storage_->frame, budget);
            if (const auto status = depth_builder->add_map(
                    ray.ray, deep_native_depth_map(native_batch.domain, native), depth_map);
                !status)
              return fail(status.code, status.message);
            mapped_rays.push_back(rays.size());
          }
          else if (!(native.flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH))
            return fail(DeepErrorCode::Incomplete,
                        "Radial depth requires full physical Optical supports");
          ordered.clear();
          owned_optical.clear();
          owned_vis.clear();
          expression_source_records.clear();
          expression_node_records.clear();
          expression_call_records.clear();
          radiance_physical.clear();
          for (size_t source = captured.source_head; source != no_index;) {
            const auto &record = storage_->reference->sources[source];
            if (record.kind == CapturedSourceKind::Homogeneous) {
              ordered.push_back(&record);
              const auto &segment = storage_->reference->batches[record.batch].homogeneous[record.record];
              if (segment.emission_calls)
                radiance_physical.push_back(
                    {record.id,
                     segment.tmin,
                     segment.tmax,
                     segment.ordinal,
                     (segment.flags & DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT) ?
                         DEEP_PHYSICAL_WORLD_RADIANCE :
                         0u});
            }
            else if (record.kind == CapturedSourceKind::OwnedOptical) {
              owned_optical.push_back(&record);
            }
            else if (record.kind == CapturedSourceKind::OwnedVisibility) {
              owned_vis.push_back(&record);
            }
            else if (record.kind == CapturedSourceKind::ExpressionSource) {
              expression_source_records.push_back(&record);
              const auto &batch = storage_->reference->batches[record.batch];
              const auto &source = batch.expression_sources[record.record];
              const uint32_t kind = source.flags & ~DEEP_NATIVE_EXPR_WORLD_SUPPORT;
              bool infinity = false;
              bool world = source.flags & DEEP_NATIVE_EXPR_WORLD_SUPPORT;
              if (kind == DEEP_NATIVE_EXPR_INHERITED_POINT) {
                const auto &owner = batch.roots[source.root_handle - 1];
                infinity = owner.kind == DEEP_NATIVE_ROOT_WORLD ||
                           owner.kind == DEEP_NATIVE_ROOT_DISTANT;
                world = owner.flags & DEEP_NATIVE_ROOT_WORLD_SUPPORT;
              }
              radiance_physical.push_back(
                  {record.id,
                   infinity ? -1. : source.tmin,
                   infinity ? -1. : source.tmax,
                   source.ordinal,
                   (kind == DEEP_NATIVE_EXPR_CAMERA_SPAN ? 0u : DEEP_PHYSICAL_POINT) |
                       (infinity ? DEEP_PHYSICAL_INFINITY : 0u) |
                       (world ? DEEP_PHYSICAL_WORLD_RADIANCE : 0u)});
            }
            else if (record.kind == CapturedSourceKind::ExpressionNode)
              expression_node_records.push_back(&record);
            else if (record.kind == CapturedSourceKind::ExpressionCall)
              expression_call_records.push_back(&record);
            else {
              const auto &point = storage_->reference->batches[record.batch].points[record.record];
              emission.push_back(
                  {DeepCurveRadianceBasis::Point,
                   point.z,
                   point.z,
                   {point.final_rgb[0], point.final_rgb[1], point.final_rgb[2]},
                   {0, 0, 0},
                   record.id,
                   record.id,
                   point.ordinal,
                   DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_POINT_RADIANCE |
                       (point.root_kind == DEEP_NATIVE_ROOT_WORLD   ? DEEP_KNOT_WORLD_TERMINAL :
                        point.root_kind == DEEP_NATIVE_ROOT_DISTANT ? DEEP_KNOT_INFINITY_RADIANCE :
                                                                      0u) |
                       ((storage_->reference->batches[record.batch].roots[point.root_handle - 1].flags &
                         DEEP_NATIVE_ROOT_WORLD_SUPPORT) ?
                            DEEP_KNOT_NATIVE_WORLD_RADIANCE :
                            0u)});
              const auto &owner = storage_->reference->batches[record.batch].roots[point.root_handle - 1];
              const bool infinity = owner.kind == DEEP_NATIVE_ROOT_WORLD ||
                                    owner.kind == DEEP_NATIVE_ROOT_DISTANT;
              radiance_physical.push_back({record.id,
                                           infinity ? -1. : owner.physical_t,
                                           infinity ? -1. : owner.physical_t,
                                           point.ordinal,
                                           DEEP_PHYSICAL_POINT |
                                               (infinity ? DEEP_PHYSICAL_INFINITY : 0u) |
                                               ((owner.flags & DEEP_NATIVE_ROOT_WORLD_SUPPORT) ?
                                                    DEEP_PHYSICAL_WORLD_RADIANCE :
                                                    0u)});
            }
            source = storage_->reference->sources[source].next_ray;
          }
          std::sort(ordered.begin(), ordered.end(), [&](const auto *a, const auto *b) {
            const auto &left = storage_->reference->batches[a->batch].homogeneous[a->record];
            const auto &right = storage_->reference->batches[b->batch].homogeneous[b->record];
            return left.z < right.z;
          });
          if (!native_batch.optical_full_coverage && ordered.empty() &&
              root.kind == DEEP_NATIVE_ROOT_NONE &&
              (emission.size() == emission_begin ||
               !(native.flags & DEEP_NATIVE_CAMERA_BACKGROUND_REACHED)))
            return fail(DeepErrorCode::Incomplete, "Native Ray has no certified optical supports");
          std::sort(owned_vis.begin(), owned_vis.end(), [&](const auto *a, const auto *b) {
            return native_batch.optical_visibility[a->record].physical_t <
                   native_batch.optical_visibility[b->record].physical_t;
          });
          const auto mapped_depth = [&](const float physical_t, const float z) {
            const auto found = std::lower_bound(
                owned_vis.begin(),
                owned_vis.end(),
                physical_t,
                [&](const auto *source, const float t) {
                  return native_batch.optical_visibility[source->record].physical_t < t;
                });
            return found != owned_vis.end() &&
                           native_batch.optical_visibility[(*found)->record].physical_t ==
                               physical_t ?
                       double(native_batch.optical_visibility[(*found)->record].z) :
                       double(z);
          };
          double cursor =
              ordered.empty() ?
                  (root.kind == DEEP_NATIVE_ROOT_NONE ?
                       double(float(native.z_origin + native.z_direction * native.tmin)) :
                       0.0) :
                  storage_->reference->batches[ordered.front()->batch].homogeneous[ordered.front()->record].z;
          ray.coverage_front = cursor;
          for (const CapturedSource *source : ordered) {
            const auto &segment = storage_->reference->batches[source->batch].homogeneous[source->record];
            if (!native_batch.optical_full_coverage && segment.z < cursor)
              return fail(DeepErrorCode::InvalidPayload, "M1 overlapping native spans require M2");
            if (!native_batch.optical_full_coverage && segment.z > cursor) {
              if (storage_->reference->next_source == UINT64_MAX)
                return fail(DeepErrorCode::InvalidCount,
                            "Native vacuum support identity overflow");
              supports.push_back({cursor, segment.z, {0, 0, 0}, storage_->reference->next_source++});
            }
            /* The native Surface vertex and ray-expression endpoint can differ
             * by FLOAT arithmetic. An exact physical-t match pins their common
             * boundary to the preserved native Surface depth; raw captures remain owned. */
            const double back = native_batch.optical_full_coverage ?
                                    mapped_depth(segment.tmax, segment.zback) :
                                root.kind == DEEP_NATIVE_ROOT_SURFACE &&
                                        segment.tmax == root.physical_t ?
                                    double(root.z) :
                                    double(segment.zback);
            const double front = native_batch.optical_full_coverage ?
                                     mapped_depth(segment.tmin, segment.z) :
                                     double(segment.z);
            if (back <= front)
              return fail(DeepErrorCode::InvalidPayload,
                          "Native volume/Surface endpoint is not representable");
            if (!native_batch.optical_full_coverage)
              supports.push_back(
                  {segment.z, back, {segment.tau[0], segment.tau[1], segment.tau[2]}, source->id});
            if (segment.emission_calls) {
              if (emission_nodes.size() == UINT32_MAX)
                return fail(DeepErrorCode::InvalidCount, "Native emission expression overflow");
              const uint32_t index = uint32_t(emission_nodes.size());
              emission_sources.push_back(
                  {DeepCurveRadianceBasis::BeerSpan,
                   front,
                   back,
                   {segment.pre_clamp_rgb[0], segment.pre_clamp_rgb[1], segment.pre_clamp_rgb[2]},
                   {segment.tau[0], segment.tau[1], segment.tau[2]},
                   source->id,
                   segment.ordinal,
                   DEEP_KNOT_INTERVAL_BOUNDARY |
                       ((segment.flags & DEEP_NATIVE_HOMOGENEOUS_WORLD_SUPPORT) ?
                            DEEP_KNOT_NATIVE_WORLD_RADIANCE :
                            0u)});
              DeepEmissionNode node;
              node.operation = DeepEmissionOperation::Term;
              node.source = index;
              emission_nodes.push_back(node);
              emission_calls.push_back(
                  {source->id,
                   index,
                   DEEP_EMISSION_COMBINED,
                   {segment.pre_clamp_rgb[0], segment.pre_clamp_rgb[1], segment.pre_clamp_rgb[2]},
                   {segment.post_clamp_rgb[0],
                    segment.post_clamp_rgb[1],
                    segment.post_clamp_rgb[2]},
                   segment.clamp_gain,
                   true});
            }
            cursor = back;
          }
          if (!native_batch.optical_full_coverage && root.kind != DEEP_NATIVE_ROOT_NONE) {
            const double end = root.kind == DEEP_NATIVE_ROOT_SURFACE ?
                                   double(root.z) :
                                   (ordered.empty() ?
                                        double(float(native.z_origin +
                                                     native.z_direction * native.tmax)) :
                                        cursor);
            if (end < cursor)
              return fail(DeepErrorCode::InvalidPayload,
                          "Native first-source terminal precedes optical coverage");
            if (end > cursor) {
              if (storage_->reference->next_source == UINT64_MAX)
                return fail(DeepErrorCode::InvalidCount, "Native root vacuum identity overflow");
              supports.push_back({cursor, end, {0, 0, 0}, storage_->reference->next_source++});
              cursor = end;
            }
            visibility.push_back(
                {root.z,
                 {0, 0, 0},
                 {0, 0, 0},
                 captured.root,
                 root.ordinal,
                 DEEP_KNOT_PROTECTED_CUT |
                     (root.kind == DEEP_NATIVE_ROOT_WORLD ? DEEP_KNOT_WORLD_TERMINAL : 0u)});
          }
          if (!native_batch.optical_full_coverage && ordered.empty() &&
              root.kind == DEEP_NATIVE_ROOT_NONE)
          {
            /* The narrow native preflight proves this lamp-only camera path has
             * no volume/Surface coverage. Preserve its finite clip vacuum, with
             * radiance-only points and no fabricated opacity terminal. */
            const double end = double(float(native.z_origin + native.z_direction * native.tmax));
            if (end < cursor || !std::isfinite(end))
              return fail(DeepErrorCode::InvalidPayload, "Lamp camera clip coverage is invalid");
            if (end > cursor)
              supports.push_back({cursor, end, {0, 0, 0}, storage_->reference->next_source++});
            cursor = end;
          }
          if (native_batch.optical_full_coverage) {
            const auto &coverage = native_batch.optical_coverage[captured.camera];
            std::sort(
                owned_optical.begin(), owned_optical.end(), [&](const auto *a, const auto *b) {
                  return native_batch.optical_pieces[a->record].tmin <
                         native_batch.optical_pieces[b->record].tmin;
                });
            std::sort(owned_vis.begin(), owned_vis.end(), [&](const auto *a, const auto *b) {
              return native_batch.optical_visibility[a->record].ordinal <
                     native_batch.optical_visibility[b->record].ordinal;
            });
            /* Keep all raw records in their packet/pages. Only this temporary index
             * is compacted. A collapsed medium is not an identity: redistribute
             * its complete tau into an adjacent finite Optical span and charge
             * sum(max_channel(tau)) as an all-depth Lipschitz error certificate.
             * No endpoint is moved and no opacity/Surface point is fabricated. */
            ray.coverage_front = coverage.z_front;
            cursor = ray.coverage_front;
            double pending_tau[3] = {}, pending_error = 0;
            size_t positive_count = 0;
            const auto add_error = [](const double a, const double b) {
              return b == 0 ? a : std::nextafter(a + b, std::numeric_limits<double>::infinity());
            };
            for (size_t index = 0; index < owned_optical.size(); index++) {
              const auto *source = owned_optical[index];
              const auto &piece = native_batch.optical_pieces[source->record];
              const double front = mapped_depth(piece.tmin, piece.z);
              const double back = mapped_depth(piece.tmax, piece.zback);
              if (front != cursor || back < front)
                return fail(DeepErrorCode::InvalidPayload,
                            "Owned mapped depth has a gap or overlap");
              if (back == front) {
                if (deep_owned_optical_depth_identity(
                        float(front), float(back), piece.tau[0], piece.tau[1], piece.tau[2]))
                  continue;
                for (const float tau : piece.tau)
                  if ((__float_as_uint(tau) & 0x7fffffffu) != 0 && tau == 0)
                    return fail(DeepErrorCode::InvalidPayload,
                                "Collapsed Optical extinction cannot be lost under DAZ");
                double maximum = 0;
                for (int c = 0; c < 3; c++) {
                  const double tau = piece.tau[c];
                  const double next = pending_tau[c] + tau;
                  const double rounding = std::numeric_limits<double>::epsilon() *
                                          (pending_tau[c] + tau);
                  pending_tau[c] = next;
                  pending_error = add_error(pending_error, rounding);
                  maximum = std::max(maximum, tau);
                }
                pending_error = add_error(pending_error, maximum);
                continue;
              }
              DeepCurveOptical term{front,
                                    back,
                                    {piece.tau[0], piece.tau[1], piece.tau[2]},
                                    source->id,
                                    piece.segment};
              term.projection_error = pending_error;
              for (int c = 0; c < 3; c++) {
                if (pending_tau[c] != 0) {
                  term.projection_error = add_error(term.projection_error,
                                                    std::numeric_limits<double>::epsilon() *
                                                        (term.tau[c] + pending_tau[c]));
                  term.tau[c] += pending_tau[c];
                }
                pending_tau[c] = 0;
              }
              pending_error = 0;
              supports.push_back(term);
              owned_optical[positive_count++] = source;
              cursor = back;
            }
            owned_optical.resize(positive_count);
            if (pending_error != 0) {
              if (supports.size() == begin)
                return fail(DeepErrorCode::InvalidPayload,
                            "DepthNotRepresentable: no finite Optical span for collapsed medium");
              auto &term = supports.back();
              term.projection_error = add_error(term.projection_error, pending_error);
              for (int c = 0; c < 3; c++) {
                if (pending_tau[c] != 0) {
                  term.projection_error = add_error(term.projection_error,
                                                    std::numeric_limits<double>::epsilon() *
                                                        (term.tau[c] + pending_tau[c]));
                  term.tau[c] += pending_tau[c];
                }
              }
            }
            if (cursor != coverage.z_back)
              return fail(DeepErrorCode::Incomplete,
                          "Owned mapped depth does not reach the actual terminal");
            for (const auto *source : owned_vis) {
              const auto &point = native_batch.optical_visibility[source->record];
              visibility.push_back(
                  {point.z,
                   {point.transmission[0], point.transmission[1], point.transmission[2]},
                   {point.holdout[0], point.holdout[1], point.holdout[2]},
                   source->id,
                   point.ordinal,
                   DEEP_KNOT_PROTECTED_CUT});
            }
            if (root.kind == DEEP_NATIVE_ROOT_WORLD ||
                (native_batch.domain.opaque_world && (coverage.flags & DEEP_OWNED_OPTICAL_FAR)))
            {
              if (root.kind != DEEP_NATIVE_ROOT_WORLD && storage_->reference->next_source == UINT64_MAX)
                return fail(DeepErrorCode::InvalidCount,
                            "Independent World visibility identity overflow");
              const uint64_t world_source = root.kind == DEEP_NATIVE_ROOT_WORLD ?
                                                captured.root :
                                                storage_->reference->next_source++;
              visibility.push_back({double(std::numeric_limits<float>::max()),
                                    {0, 0, 0},
                                    {0, 0, 0},
                                    world_source,
                                    UINT32_MAX,
                                    DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_WORLD_TERMINAL});
            }
          }
          ray.coverage_back = cursor;
          /* Native producer + narrow scene preflight certify complete boundaries and
           * transparent black background. Only then are inter-span gaps known vacuum. */
          ray.optical_complete = native_batch.optical_full_coverage ||
                                 bool(native.flags & DEEP_NATIVE_CAMERA_BACKGROUND_REACHED) ||
                                 root.kind == DEEP_NATIVE_ROOT_SURFACE;
          ray.affine_camera_depth = bool(native.flags & DEEP_NATIVE_CAMERA_AFFINE_DEPTH);
          ray.native_world_support = native_batch.domain.world_volume_shader != SHADER_NONE;
          DeepResolvedEmission resolved;
          if (!expression_node_records.empty() || !expression_call_records.empty() ||
              !expression_source_records.empty())
          {
            const auto ordinal_order = [](const auto *a, const auto *b) {
              return a->record < b->record;
            };
            std::sort(
                expression_source_records.begin(), expression_source_records.end(), ordinal_order);
            std::sort(
                expression_node_records.begin(), expression_node_records.end(), ordinal_order);
            std::sort(
                expression_call_records.begin(), expression_call_records.end(), ordinal_order);
            if (emission_sources.size() > UINT32_MAX - expression_source_records.size() ||
                emission_nodes.size() >= UINT32_MAX - expression_node_records.size())
              return fail(DeepErrorCode::InvalidCount,
                          "Per-camera native emission graph exceeds local handles");
            const uint32_t source_base = uint32_t(emission_sources.size());
            const uint32_t zero = uint32_t(emission_nodes.size());
            emission_nodes.push_back({});
            const uint32_t node_base = uint32_t(emission_nodes.size());
            for (const auto *record : expression_source_records) {
              const auto &source = native_batch.expression_sources[record->record];
              const uint32_t kind = source.flags & ~DEEP_NATIVE_EXPR_WORLD_SUPPORT;
              const bool span = kind == DEEP_NATIVE_EXPR_CAMERA_SPAN;
              const bool world_support = (source.flags & DEEP_NATIVE_EXPR_WORLD_SUPPORT) ||
                                         (kind == DEEP_NATIVE_EXPR_INHERITED_POINT &&
                                          (native_batch.roots[source.root_handle - 1].flags &
                                           DEEP_NATIVE_ROOT_WORLD_SUPPORT));
              emission_sources.push_back(
                  {span ? DeepCurveRadianceBasis::BeerSpan : DeepCurveRadianceBasis::Point,
                   span ? mapped_depth(source.tmin, source.z) : double(source.z),
                   span ? mapped_depth(source.tmax, source.zback) : double(source.zback),
                   {source.raw_rgb[0], source.raw_rgb[1], source.raw_rgb[2]},
                   {source.tau[0], source.tau[1], source.tau[2]},
                   record->id,
                   source.ordinal,
                   (span ? DEEP_KNOT_INTERVAL_BOUNDARY :
                           DEEP_KNOT_PROTECTED_CUT | DEEP_KNOT_POINT_RADIANCE) |
                       (world_support ? DEEP_KNOT_NATIVE_WORLD_RADIANCE : 0u)});
            }
            const auto local_handle = [](const auto &records,
                                         const uint32_t handle,
                                         const uint32_t base,
                                         uint32_t &index) {
              const auto found = std::lower_bound(
                  records.begin(),
                  records.end(),
                  size_t(handle - 1),
                  [](const auto *record, const size_t wanted) { return record->record < wanted; });
              if (found == records.end() || (*found)->record != size_t(handle - 1))
                return false;
              index = base + uint32_t(found - records.begin());
              return true;
            };
            const auto operand = [&](const uint32_t handle, uint32_t &index) {
              if (!handle) {
                index = zero;
                return true;
              }
              return local_handle(expression_node_records, handle, node_base, index);
            };
            static_assert(uint32_t(DeepEmissionOperation::Select) == DEEP_NATIVE_EXPR_SELECT);
            for (const auto *record : expression_node_records) {
              const auto &native_node = native_batch.expression_nodes[record->record];
              DeepEmissionNode node;
              node.operation = DeepEmissionOperation(native_node.operation);
              node.select_b = native_node.select_b;
              std::copy(std::begin(native_node.weight), std::end(native_node.weight), node.weight);
              if (node.operation == DeepEmissionOperation::Term) {
                if (!local_handle(
                        expression_source_records, native_node.source, source_base, node.source))
                  return fail(DeepErrorCode::InvalidReference,
                              "Native expression lost its local term source");
              }
              else if (node.operation != DeepEmissionOperation::Zero) {
                if (!operand(native_node.a, node.a))
                  return fail(DeepErrorCode::InvalidReference,
                              "Native expression lost its first operand");
                if (node.operation == DeepEmissionOperation::Add ||
                    node.operation == DeepEmissionOperation::Mix ||
                    node.operation == DeepEmissionOperation::Select)
                  if (!operand(native_node.b, node.b))
                    return fail(DeepErrorCode::InvalidReference,
                                "Native expression lost its second operand");
              }
              emission_nodes.push_back(node);
            }
            for (const auto *record : expression_call_records) {
              const auto &native_call = native_batch.expression_calls[record->record];
              uint32_t head;
              if (!operand(native_call.head, head))
                return fail(DeepErrorCode::InvalidReference,
                            "Native film call lost its expression head");
              emission_calls.push_back({record->id,
                                        head,
                                        DEEP_EMISSION_COMBINED,
                                        {native_call.pre_clamp_rgb[0],
                                         native_call.pre_clamp_rgb[1],
                                         native_call.pre_clamp_rgb[2]},
                                        {native_call.post_clamp_rgb[0],
                                         native_call.post_clamp_rgb[1],
                                         native_call.post_clamp_rgb[2]},
                                        native_call.clamp_gain,
                                        true});
            }
          }
          if (const auto status = deep_resolve_emission_expression(
                  budget, emission_sources, emission_nodes, emission_calls, resolved);
              !status)
            return fail(status.code, status.message);
          emission.insert(emission.end(), resolved.radiance().begin(), resolved.radiance().end());
          if (depth_map) {
            const auto append_physical = [&](const DeepPhysicalSourceSupport &physical) {
              if (const auto status = depth_builder->add_support(ray.ray, physical); !status)
                return fail(status.code, status.message);
              if (storage_->window.width <= 16 && storage_->window.height <= 16 &&
                  deep_frame_pipeline_requested(
                      std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VALIDATION_SOURCES")))
                std::fprintf(
                    stderr,
                    "DEEP_FRAME_REFERENCE_SUPPORT ray=%llu map=%llu source=%llu "
                    "kind=%u ordinal=%u flags=%u t_front=%.17g t_back=%.17g z=%.17g back=%.17g\n",
                    static_cast<unsigned long long>(ray.ray),
                    static_cast<unsigned long long>(physical.depth_map),
                    static_cast<unsigned long long>(physical.source),
                    uint32_t(physical.kind),
                    physical.ordinal,
                    physical.flags,
                    physical.front,
                    physical.back,
                    physical.z,
                    physical.z_back);
              return true;
            };
            for (size_t j = 0; j < owned_optical.size(); j++) {
              const auto &raw = native_batch.optical_pieces[owned_optical[j]->record];
              const auto &term = supports[begin + j];
              DeepPhysicalSourceSupport physical;
              physical.depth_map = depth_map;
              physical.source = term.source;
              physical.front = raw.tmin;
              physical.back = raw.tmax;
              physical.z = term.z;
              physical.z_back = term.z_back;
              physical.ordinal = raw.segment;
              if (!append_physical(physical))
                return false;
            }
            for (size_t j = visibility_begin; j < visibility.size(); j++) {
              const auto &term = visibility[j];
              const bool infinity = term.flags & DEEP_KNOT_WORLD_TERMINAL;
              if (!infinity && j - visibility_begin >= owned_vis.size())
                return fail(DeepErrorCode::InvalidReference,
                            "Visibility physical source pairing is missing");
              const double t = infinity ?
                                   -1. :
                                   native_batch
                                       .optical_visibility[owned_vis[j - visibility_begin]->record]
                                       .physical_t;
              DeepPhysicalSourceSupport physical;
              physical.depth_map = depth_map;
              physical.source = term.source;
              physical.kind = DeepPhysicalSourceKind::Visibility;
              physical.front = physical.back = t;
              physical.z = physical.z_back = term.z;
              physical.ordinal = term.ordinal;
              physical.flags = DEEP_PHYSICAL_POINT | (infinity ? DEEP_PHYSICAL_INFINITY : 0u);
              if (!append_physical(physical))
                return false;
            }
            std::sort(radiance_physical.begin(),
                      radiance_physical.end(),
                      [](const auto &a, const auto &b) { return a.source < b.source; });
            final_radiance.clear();
            for (size_t j = emission_begin; j < emission.size(); j++)
              final_radiance.push_back(j);
            std::sort(final_radiance.begin(), final_radiance.end(), [&](size_t a, size_t b) {
              return emission[a].source < emission[b].source;
            });
            uint64_t previous = 0;
            for (size_t j : final_radiance) {
              const auto &term = emission[j];
              const auto found = std::lower_bound(
                  radiance_physical.begin(),
                  radiance_physical.end(),
                  term.source,
                  [](const auto &a, uint64_t b) { return a.source < b; });
              if (found == radiance_physical.end() || found->source != term.source ||
                  found->ordinal != term.ordinal)
                return fail(DeepErrorCode::InvalidReference,
                            "Radiance physical source pairing is missing");
              if (previous == term.source)
                continue;
              previous = term.source;
              DeepPhysicalSourceSupport physical;
              physical.depth_map = depth_map;
              physical.source = term.source;
              physical.kind = DeepPhysicalSourceKind::Radiance;
              physical.front = found->front;
              physical.back = found->back;
              physical.z = term.z;
              physical.z_back = term.z_back;
              physical.ordinal = found->ordinal;
              physical.flags = found->flags;
              if (!append_physical(physical))
                return false;
            }
          }
          if (storage_->window.width <= 16 && storage_->window.height <= 16 &&
              deep_frame_pipeline_requested(
                  std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VALIDATION_SOURCES")))
          {
            std::fprintf(stderr,
                         "DEEP_FRAME_REFERENCE_RAY ray=%llu x=%d y=%d sample=%u "
                         "origin_x=%.17g origin_y=%.17g origin_z=%.17g "
                         "dx=%.17g dy=%.17g dz=%.17g tmin=%.17g tmax=%.17g "
                         "z_origin=%.17g z_direction=%.17g depth_kind=%u "
                         "camera_x=%.17g camera_y=%.17g camera_z=%.17g "
                         "sensitivity_r=%.17g sensitivity_g=%.17g sensitivity_b=%.17g\n",
                         static_cast<unsigned long long>(ray.ray),
                         native.x,
                         native.y,
                         native.sample,
                         double(native.origin[0]),
                         double(native.origin[1]),
                         double(native.origin[2]),
                         double(native.direction[0]),
                         double(native.direction[1]),
                         double(native.direction[2]),
                         double(native.tmin),
                         double(native.tmax),
                         double(native.z_origin),
                         double(native.z_direction),
                         uint32_t(native_batch.domain.depth_kind),
                         double(native_batch.domain.depth_transform[3]),
                         double(native_batch.domain.depth_transform[7]),
                         double(native_batch.domain.depth_transform[11]),
                         double(native.throughput[0]),
                         double(native.throughput[1]),
                         double(native.throughput[2]));
            for (size_t source_index = emission_begin; source_index < emission.size();
                 source_index++)
            {
              const auto &term = emission[source_index];
              std::fprintf(stderr,
                           "DEEP_FRAME_REFERENCE_RGB ray=%llu basis=%u source=%llu call=%llu "
                           "z=%.17g back=%.17g r=%.17g g=%.17g b=%.17g "
                           "tau_r=%.17g tau_g=%.17g tau_b=%.17g flags=%u\n",
                           static_cast<unsigned long long>(ray.ray),
                           uint32_t(term.basis),
                           static_cast<unsigned long long>(term.source),
                           static_cast<unsigned long long>(term.emission_call),
                           term.z,
                           term.z_back,
                           term.post_clamp_rgb[0],
                           term.post_clamp_rgb[1],
                           term.post_clamp_rgb[2],
                           term.tau[0],
                           term.tau[1],
                           term.tau[2],
                           term.flags);
            }
          }
        }
        else if (captured.source_head != no_index)
          return fail(DeepErrorCode::InvalidPayload, "Disposition camera owns unexpected sources");
        ranges.push_back({begin,
                          supports.size() - begin,
                          emission_begin,
                          emission.size() - emission_begin,
                          visibility_begin,
                          visibility.size() - visibility_begin});
        rays.push_back(ray);
        ray_index = captured.next_pixel;
      }
      if (depth_builder) {
        DeepDepthSupportTable table;
        if (const auto status = depth_builder->finish(table); !status)
          return fail(status.code, status.message);
        for (size_t index : mapped_rays)
          if (const auto status = table.view(rays[index].ray, rays[index].depth_support); !status)
            return fail(status.code, status.message);
      }
      for (size_t r = 0; r < rays.size(); r++) {
        if (ranges[r].optical_count)
          rays[r].optical = {supports.data() + ranges[r].optical, ranges[r].optical_count};
        if (ranges[r].radiance_count)
          rays[r].radiance = {emission.data() + ranges[r].radiance, ranges[r].radiance_count};
        if (ranges[r].visibility_count)
          rays[r].visibility = {visibility.data() + ranges[r].visibility,
                                ranges[r].visibility_count};
      }
      DeepNativeCurvePixel input;
      input.frame = storage_->frame;
      input.x = int32_t(int64_t(x) + int64_t(i % size_t(width)));
      input.y = int32_t(int64_t(y) + int64_t(i / size_t(width)));
      input.native_population = counts[i];
      input.population_complete = true;
      input.exposure = exposure;
      input.rays = rays;
      DeepNativeCurve curve;
      DeepNativeCurvePolicy policy = curve_policy_;
      policy.retain_sources = retain_curve_sources_;
      if (const auto status = deep_build_native_curve(*pipeline_.pages(), input, curve, policy);
          !status)
        return fail(status.code, status.message);
      if (encoded_tile) {
        for (size_t c = 0; c < storage_->consumers.size(); c++) {
          DeepCanonicalEncoded encoded;
          if (const auto status = deep_encode_canonical_knots(budget,
                                                              curve.pages(),
                                                              curve.knot_count(),
                                                              curve.representation_error(),
                                                              storage_->consumers[c].config,
                                                              encoded);
              !status)
            return fail(status.code, "Frozen consumer encoding failed: " + status.message);
          if (!encoded.report()->normalized_checked || !encoded.report()->quantized_checked)
            return fail(DeepErrorCode::Incomplete, "Frozen consumer encoding lacks decoder proof");
          auto &range = encoded_tile->ranges[c * counts.size() + i];
          range = {encoded_tile->samples.size(), encoded.samples().size()};
          for (const auto &sample : encoded.samples())
            encoded_tile->samples.push_back(
                {sample.r, sample.g, sample.b, sample.a, sample.z, sample.z_back});
          accumulate_report(storage_->encoding_reports[c], *encoded.report());
        }
      }
      DeepCanonicalPixel output{input.x, input.y, counts[i], curve.pages().front(), {}};
      if (curve.pages().size() > 1) {
        auto sequence = std::allocate_shared<DeepPageSequence>(
            DeepBudgetAllocator<DeepPageSequence>(budget), budget);
        for (size_t p = 1; p < curve.pages().size(); p++)
          if (const auto status = sequence->append(curve.pages()[p]); !status)
            return fail(status.code, status.message);
        output.additional_knots = std::move(sequence);
      }
      canonical.push_back(std::move(output));
      curves.push_back(std::move(curve));
    }
    if (storage_->next_tile == UINT64_MAX)
      return fail(DeepErrorCode::InvalidCount, "Native final tile token overflow");
    const size_t cache_tile_index = storage_->encoded_tiles.size();
    if (encoded_tile)
      storage_->encoded_tiles.push_back(encoded_tile);
    if (const auto status = pipeline_.close_render_tile(storage_->frame,
                                                        storage_->next_tile++,
                                                        {x, y, uint32_t(width), uint32_t(height)},
                                                        canonical);
        !status)
      return false;
    for (size_t i = 0; i < counts.size(); i++) {
      const size_t pixel = first + (i / size_t(width)) * storage_->window.width +
                           i % size_t(width);
      if (encoded_tile)
        storage_->pixel_cache_tiles[pixel] = cache_tile_index;
      else
        storage_->reference->curves[pixel] = std::move(curves[i]);
      storage_->pixels[pixel].closed = true;
      storage_->pixels[pixel].ray_head = no_index;
    }
    if (encoded_tile) {
      const size_t before = budget.used();
      if (const auto status = pipeline_.retire_canonical_tile(encoded_tile->window); !status)
        return false;
      canonical.clear();
      curves.clear();
      if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
        std::fprintf(
            stderr,
            "DEEP_FRAME_ENCODED_TILE x=%d y=%d width=%d height=%d consumers=%llu "
            "samples=%llu payload_bytes=%llu canonical_before=%llu canonical_after=%llu\n",
            x,
            y,
            width,
            height,
            static_cast<unsigned long long>(storage_->consumers.size()),
            static_cast<unsigned long long>(encoded_tile->samples.size()),
            static_cast<unsigned long long>(encoded_tile->samples.capacity() *
                                            sizeof(DeepEncodedPayload)),
            static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(budget.used()));
    }
    storage_->reference->active_rays -= closed_rays;
    if (storage_->reference->active_rays == 0) {
      /* No open pixel can reference these joined packets or sparse source/ray
       * indices. Closed pixels retain canonical pages/population/error proof;
       * the next tile can reuse local indices with new frame-global IDs. */
      const size_t retired_batches = storage_->reference->batches.size();
      ProductionVector<DeepNativeCaptureBatch>(storage_->reference->batches.get_allocator())
          .swap(storage_->reference->batches);
      ProductionVector<CapturedRay>(storage_->reference->rays.get_allocator()).swap(storage_->reference->rays);
      ProductionVector<CapturedSource>(storage_->reference->sources.get_allocator()).swap(storage_->reference->sources);
      if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
        std::fprintf(stderr,
                     "DEEP_FRAME_TILE_RETIRED x=%d y=%d width=%d height=%d batches=%llu "
                     "rays=%llu bytes_used=%llu\n",
                     x,
                     y,
                     width,
                     height,
                     static_cast<unsigned long long>(retired_batches),
                     static_cast<unsigned long long>(closed_rays),
                     static_cast<unsigned long long>(budget.used()));
    }
    storage_->reference->exposure = exposure;
    storage_->reference->exposure_known = true;
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Native final tile canonical quota exhausted");
  }
  catch (const std::length_error &) {
    return fail(DeepErrorCode::InvalidCount, "Native final tile canonical count overflow");
  }
}

DeepStatus DeepFrameProduction::encode_pixel(const size_t index,
                                             const DeepCanonicalEncodeConfig &config,
                                             DeepCanonicalEncoded &encoded) const
{
  encoded = {};
  if (!storage_ || !error())
    return error();
  if (!storage_->reference || index >= storage_->reference->curves.size() || !storage_->reference->curves[index])
    return DeepStatus::fail(DeepErrorCode::Incomplete,
                            "Native Deep pixel has no closed canonical curve");
  const auto &curve = storage_->reference->curves[index];
  const auto status = deep_encode_canonical_knots(memory_budget(),
                                                  curve.pages(),
                                                  curve.knot_count(),
                                                  curve.representation_error(),
                                                  config,
                                                  encoded);
  if (!status &&
      deep_frame_pipeline_requested(std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
    std::fprintf(stderr,
                 "DEEP_FRAME_ENCODE_FAILURE pixel=%llu x=%d y=%d knots=%llu message=%s\n",
                 static_cast<unsigned long long>(index),
                 int(int64_t(storage_->window.x) + int64_t(index % storage_->window.width)),
                 int(int64_t(storage_->window.y) + int64_t(index / storage_->window.width)),
                 static_cast<unsigned long long>(curve.knot_count()),
                 status.message.c_str());
  return status;
}


CCL_NAMESPACE_END
