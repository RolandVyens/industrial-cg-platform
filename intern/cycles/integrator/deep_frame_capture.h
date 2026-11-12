/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <span>
#include <string>
#include <vector>

#include "kernel/integrator/deep_frame_capture.h"
#include "kernel/integrator/deep_compact_types.h"
#include "kernel/integrator/deep_optical_event.h"
#include "session/deep_capture_types.h"
#include "session/deep_memory_budget.h"

CCL_NAMESPACE_BEGIN

struct DeepCaptureDemand {
  bool simple_mode = true;
  bool needs_rgb = false;
  bool capture_rgb() const { return needs_rgb && !simple_mode; }
};

struct DeepNativeCaptureDomain {
  uint64_t id = 0;
  /* Producer/Device association and the actual native+capture binding revision. */
  uint64_t device_generation = 0, layout_generation = 0;
  int start_sample = 0, samples_num = 0, sample_offset = 0;
  /* Absolute raster coordinates, including overscan. Pixel is the native buffer index. */
  int x = 0, y = 0, width = 0, height = 0;
  /* Native adaptive halo is evaluated and validated but is not output population.
   * Zero core dimensions mean the complete domain (reference adapters). */
  int core_x = 0, core_y = 0, core_width = 0, core_height = 0;
  uint64_t scheduled_admissions = 0;
  uint64_t camera_reserved = 0, camera_written = 0;
  uint64_t homogeneous_reserved = 0, homogeneous_written = 0, emission_calls = 0;
  uint64_t point_reserved = 0, point_written = 0, root_reserved = 0, root_written = 0;
  uint64_t expression_source_reserved = 0, expression_source_written = 0;
  uint64_t expression_node_reserved = 0, expression_node_written = 0;
  uint64_t expression_call_reserved = 0, expression_call_written = 0;
  uint64_t auxiliary_film_calls = 0;
  uint64_t volume_yields = 0, volume_retries = 0;
  uint64_t volume_pending = 0;
  uint64_t gpu_requested_bytes = 0, host_staging_requested_bytes = 0;
  uint32_t error_bits = 0;
  bool main_shadow_mnee_joined = false;
  bool require_full_optical = false;
  DeepCaptureDemand demand;
  bool has_volume = false;
  uint32_t stack_stride = 0;
  bool opaque_world = false;
  /* Immutable compiled binding certifies native World sources outside camera Optical clipping. */
  int world_volume_shader = SHADER_NONE, world_volume_object = OBJECT_NONE;
  DeepFrozenDepthKind depth_kind = DeepFrozenDepthKind::Affine;
  float depth_transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

inline DeepFrozenDepthMap deep_native_depth_map(const DeepNativeCaptureDomain &domain,
                                                const DeepNativeCameraRecord &camera)
{
  DeepFrozenDepthMap map;
  map.kind = domain.depth_kind;
  for (int i = 0; i < 12; i++)
    map.transform[i] = domain.depth_transform[i];
  for (int i = 0; i < 3; i++) {
    map.origin[i] = camera.origin[i];
    map.direction[i] = camera.direction[i];
  }
  map.tmin = camera.tmin;
  map.tmax = camera.tmax;
  if (map.kind == DeepFrozenDepthKind::Affine) {
    map.affine_origin = camera.z_origin;
    map.affine_slope = camera.z_direction;
  }
  return map;
}

template<typename T> using DeepNativeCaptureVector = std::vector<T, DeepBudgetAllocator<T>>;

struct DeepNativeCaptureBatch {
  explicit DeepNativeCaptureBatch(const DeepMemoryBudget &budget)
      : cameras(DeepBudgetAllocator<DeepNativeCameraRecord>(budget)),
        homogeneous(DeepBudgetAllocator<DeepNativeHomogeneousRecord>(budget)),
        roots(DeepBudgetAllocator<DeepNativeRootRecord>(budget)),
        points(DeepBudgetAllocator<DeepNativePointRecord>(budget)),
        expression_sources(DeepBudgetAllocator<DeepNativeExpressionSource>(budget)),
        expression_nodes(DeepBudgetAllocator<DeepNativeExpressionNode>(budget)),
        expression_calls(DeepBudgetAllocator<DeepNativeExpressionCall>(budget)),
        optical_events(DeepBudgetAllocator<DeepOpticalEvent>(budget)),
        optical_ledger(DeepBudgetAllocator<DeepOpticalLedger>(budget)),
        optical_pieces(DeepBudgetAllocator<DeepOwnedOpticalPiece>(budget)),
        optical_visibility(DeepBudgetAllocator<DeepOwnedOpticalVisibility>(budget)),
        optical_coverage(DeepBudgetAllocator<DeepOwnedOpticalCoverage>(budget))
  {
  }
  DeepNativeCaptureDomain domain;
  DeepNativeCaptureVector<DeepNativeCameraRecord> cameras;
  DeepNativeCaptureVector<DeepNativeHomogeneousRecord> homogeneous;
  DeepNativeCaptureVector<DeepNativeRootRecord> roots;
  DeepNativeCaptureVector<DeepNativePointRecord> points;
  DeepNativeCaptureVector<DeepNativeExpressionSource> expression_sources;
  DeepNativeCaptureVector<DeepNativeExpressionNode> expression_nodes;
  DeepNativeCaptureVector<DeepNativeExpressionCall> expression_calls;
  DeepNativeCaptureVector<DeepOpticalEvent> optical_events;
  DeepNativeCaptureVector<DeepOpticalLedger> optical_ledger;
  DeepNativeCaptureVector<DeepOwnedOpticalPiece> optical_pieces;
  DeepNativeCaptureVector<DeepOwnedOpticalVisibility> optical_visibility;
  DeepNativeCaptureVector<DeepOwnedOpticalCoverage> optical_coverage;
  uint64_t optical_replay_domain = 0;
  bool optical_replay_joined = false;
  bool optical_full_coverage = false;
};

/* The non-null consumer is installed by the frame owner before native work starts.
 * Reference begin/complete transfers independently owned host payloads; each successful
 * begin receives one completion or abort. Active bounded CPU/OptiX instead uses frozen
 * consumers and joined Tile publication. All shared publication is single Host owner;
 * Work-local DMA retirement must not mutate this consumer. Diagnostic files are separate. */
struct DeepFrozenConsumer;
struct DeepBoundedHostTile;
class RenderBuffers;

/* Reference-only capability. Bounded producers cannot obtain it, including
 * between construction and backend enable. Borrow only for the immediate call. */
class DeepReferenceCapture {
 public:
  virtual ~DeepReferenceCapture() = default;
  virtual bool begin_native_capture(const DeepNativeCaptureDomain &domain) = 0;
  virtual bool complete_native_capture(DeepNativeCaptureBatch &&batch) = 0;
  virtual bool close_native_tile(int x, int y, int width, int height,
                                 std::span<const uint32_t> counts, float exposure) = 0;
};

class DeepFrameCaptureConsumer {
 public:
  virtual ~DeepFrameCaptureConsumer() = default;
  virtual const DeepMemoryBudget &memory_budget() const = 0;
  virtual const DeepMemoryBudget &device_memory_budget() const
  {
    return memory_budget();
  }
  /* Configure consumers, select backend once, then run bounded producer work. */
  virtual bool enable_gpu_processing() { return false; }
  virtual bool uses_gpu_processing() const { return false; }
  virtual bool enable_cpu_processing()
  {
    return false;
  }
  virtual bool uses_cpu_processing() const
  {
    return false;
  }
  virtual bool uses_bounded_processing() const
  {
    return uses_gpu_processing() || uses_cpu_processing();
  }
  virtual std::span<const DeepFrozenConsumer> bounded_consumers() const { return {}; }
  virtual DeepCaptureDemand capture_demand() const { return {false, true}; }
  virtual uint32_t bounded_max_samples() const { return 1; }
  /* Only the joined PathTrace owner calls active publication. A successful
   * publish transfers payload ownership; failure keeps the first sticky error. */
  virtual bool publish_bounded_tile(DeepBoundedHostTile &&) { return false; }
  virtual void defer_bounded_beauty() {}
  virtual bool set_final_bounded_beauty(const RenderBuffers &)
  {
    return false;
  }
  virtual void abort_native_capture(uint64_t domain_id, const std::string &reason) = 0;
  virtual void cancel_native_capture(uint64_t domain_id)
  {
    abort_native_capture(domain_id, "Native Deep capture cancelled");
  }
  virtual bool capture_cancelled() const
  {
    return false;
  }
  virtual std::string capture_error_message() const
  {
    return {};
  }
  virtual DeepReferenceCapture *reference_capture() { return nullptr; }
};

/* CPU checks used before publication. No silent filtering or survivor normalization. */
bool deep_native_capture_validate(const DeepNativeCaptureBatch &batch,
                                  std::string &error,
                                  bool allow_pending_optical = false);

/* Used only after validation has proved this domain-local owner handle. */
inline DeepNativeRootRecord deep_native_camera_root(const DeepNativeCaptureBatch &batch,
                                                    const size_t camera)
{
  const uint32_t handle = batch.cameras[camera].root_handle;
  return handle ? batch.roots[handle - 1] : DeepNativeRootRecord{};
}

CCL_NAMESPACE_END
