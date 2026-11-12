/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/path_trace_work_gpu.h"

#include "device/device.h"

#include "integrator/deep_control_journal.h"
#include "integrator/deep_expression_journal.h"
#include "integrator/deep_input_receipt_reader.h"
#include "integrator/deep_native_capacity.h"
#include "integrator/deep_optical_work.h"
#include "integrator/deep_gpu_frame_work.h"
#include "integrator/deep_bounded_work.h"
#include "integrator/pass_accessor_gpu.h"
#include "integrator/path_trace_display.h"

#include "scene/scene.h"
#include "session/buffers.h"

#include "util/log.h"
#include "util/scoped_defer.h"
#include "util/string.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <chrono>

#include "kernel/device/gpu/block_sizes.h"
#include "kernel/integrator/shadow_state.h"
#include "kernel/types.h"

CCL_NAMESPACE_BEGIN

namespace {
using DeepDispatchClock = std::chrono::steady_clock;
double deep_dispatch_elapsed(const DeepDispatchClock::time_point start)
{
  return std::chrono::duration<double>(DeepDispatchClock::now() - start).count();
}
class DeepDispatchWall {
  double &wall_;
  DeepDispatchClock::time_point start_ = DeepDispatchClock::now();
 public:
  explicit DeepDispatchWall(double &wall) : wall_(wall) {}
  ~DeepDispatchWall() { wall_ += deep_dispatch_elapsed(start_); }
};
}  // namespace

static size_t estimate_single_state_size(const uint kernel_features, const bool use_shadow_color)
{
  size_t state_size = 0;

#define KERNEL_STRUCT_BEGIN(name) \
  for (int array_index = 0;; array_index++) {

#ifdef __INTEGRATOR_GPU_PACKED_STATE__
#  define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
    state_size += (KernelFeatureRequest(feature).test(kernel_features) && \
                   (std::strcmp(#name, "deep_frame_camera_index") != 0 && \
                    std::strcmp(#name, "deep_frame_root_handle") != 0 && \
                    std::strncmp(#name, "deep_bounded_", 13) != 0)) ? \
                      sizeof(type) : \
                      0;
#  define KERNEL_STRUCT_MEMBER_PACKED(parent_struct, type, name, feature)
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) \
    KERNEL_STRUCT_BEGIN(parent_struct) \
    KERNEL_STRUCT_MEMBER(parent_struct, packed_##parent_struct, packed, feature)
#else
#  define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
    state_size += (KernelFeatureRequest(feature).test(kernel_features) && \
                   (std::strcmp(#name, "deep_frame_camera_index") != 0 && \
                    std::strcmp(#name, "deep_frame_root_handle") != 0 && \
                    std::strncmp(#name, "deep_bounded_", 13) != 0)) ? \
                      sizeof(type) : \
                      0;
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) KERNEL_STRUCT_BEGIN(parent_struct)
#endif

#define KERNEL_STRUCT_ARRAY_MEMBER(parent_struct, type, name, feature) \
  state_size += (KernelFeatureRequest(feature).test(kernel_features)) ? sizeof(type) : 0;
#define KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR(parent_struct, type, name, feature) \
  state_size += shadow_state_unshadowed_throughput_is_needed(kernel_features, use_shadow_color) ? \
                    sizeof(type) : \
                    0;
#define KERNEL_STRUCT_END(name) \
  (void)array_index; \
  break; \
  }
#define KERNEL_STRUCT_END_ARRAY(name, cpu_array_size, gpu_array_size) \
  if (array_index >= gpu_array_size - 1) { \
    break; \
  } \
  }
/* TODO(sergey): Look into better estimation for fields which depend on scene features. Maybe
 * maximum state calculation should happen as `alloc_work_memory()`, so that we can react to an
 * updated scene state here.
 * For until then use common value. Currently this size is only used for logging, but is weak to
 * rely on this. */
#define KERNEL_STRUCT_VOLUME_STACK_SIZE 4

#include "kernel/integrator/state_template.h"

#include "kernel/integrator/shadow_state_template.h"

#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_VOLUME_STACK_SIZE

  return state_size;
}

PathTraceWorkGPU::PathTraceWorkGPU(Device *device,
                                   Film *film,
                                   DeviceScene *device_scene,
                                   const bool *cancel_requested_flag)
    : PathTraceWork(device, film, device_scene, cancel_requested_flag),
      queue_(device->gpu_queue_create()),
      integrator_state_soa_kernel_features_(0),
      integrator_queue_counter_(device, "integrator_queue_counter", MEM_READ_WRITE),
      deep_native_cameras_(device, "deep_native_cameras", MEM_DEVICE_ONLY),
      deep_compact_primitives_(device, "deep_compact_primitives", MEM_DEVICE_ONLY),
      deep_compact_rays_(device, "deep_compact_rays"),
      deep_compact_stacks_(device, "deep_compact_stacks"),
      deep_compact_emission_(device, "deep_compact_emission"),
      deep_native_volume_checkpoints_(device, "deep_native_volume_checkpoints"),
      deep_native_homogeneous_(device, "deep_native_homogeneous", MEM_DEVICE_ONLY),
      deep_native_roots_(device, "deep_native_roots", MEM_DEVICE_ONLY),
      deep_native_root_resident_(device, "deep_native_root_resident", MEM_DEVICE_ONLY),
      deep_native_root_pins_(device, "deep_native_root_pins", MEM_DEVICE_ONLY),
      deep_native_points_(device, "deep_native_points", MEM_DEVICE_ONLY),
      deep_native_expression_sources_(device, "deep_native_expression_sources", MEM_DEVICE_ONLY),
      deep_native_expression_nodes_(device, "deep_native_expression_nodes", MEM_DEVICE_ONLY),
      deep_native_expression_calls_(device, "deep_native_expression_calls", MEM_DEVICE_ONLY),
      deep_native_camera_indices_(device, "deep_native_camera_indices"),
      deep_native_root_handles_(device, "deep_native_root_handles"),
      deep_native_shadow_camera_indices_(device, "deep_native_shadow_camera_indices"),
      deep_native_shadow_root_handles_(device, "deep_native_shadow_root_handles"),
      deep_camera_receipts_(device, "deep_camera_receipts", MEM_DEVICE_ONLY),
      deep_surface_receipts_(device, "deep_surface_receipts", MEM_DEVICE_ONLY),
      deep_volume_radiance_receipts_(device, "deep_volume_radiance_receipts", MEM_DEVICE_ONLY),
      deep_volume_emission_spans_(device, "deep_volume_emission_spans", MEM_DEVICE_ONLY),
      integrator_shader_sort_counter_(device, "integrator_shader_sort_counter", MEM_READ_WRITE),
      integrator_shader_raytrace_sort_counter_(
          device, "integrator_shader_raytrace_sort_counter", MEM_READ_WRITE),
      integrator_shader_sort_prefix_sum_(
          device, "integrator_shader_sort_prefix_sum", MEM_READ_WRITE),
      integrator_shader_sort_partition_key_offsets_(
          device, "integrator_shader_sort_partition_key_offsets", MEM_READ_WRITE),
      integrator_next_main_path_index_(device, "integrator_next_main_path_index", MEM_READ_WRITE),
      integrator_next_shadow_path_index_(
          device, "integrator_next_shadow_path_index", MEM_READ_WRITE),
      queued_paths_(device, "queued_paths", MEM_READ_WRITE),
      num_queued_paths_(device, "num_queued_paths", MEM_READ_WRITE),
      work_tiles_(device, "work_tiles", MEM_READ_WRITE),
      display_rgba_half_(device, "display buffer half", MEM_READ_WRITE),
      max_num_paths_(0),
      min_num_active_main_paths_(0),
      max_active_main_path_index_(0)
{
  memset(&integrator_state_gpu_, 0, sizeof(integrator_state_gpu_));
  const char *capture = getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPTS");
  deep_camera_receipts_enabled_ = capture && std::strcmp(capture, "1") == 0;
  const char *surface = getenv("CYCLES_EXPERIMENTAL_DEEP_SURFACE_RECEIPTS");
  deep_surface_receipts_enabled_ = surface && std::strcmp(surface, "1") == 0;
  const char *volume = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_RADIANCE_RECEIPTS");
  deep_volume_radiance_receipts_enabled_ = volume && std::strcmp(volume, "1") == 0;
  const char *spans = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_EMISSION_SPANS");
  deep_volume_emission_spans_enabled_ = spans && std::strcmp(spans, "1") == 0;
  const char *optical = getenv("CYCLES_EXPERIMENTAL_DEEP_INDEPENDENT_OPTICAL");
  deep_independent_optical_enabled_ = optical && std::strcmp(optical, "1") == 0;
  const char *frame_pipeline = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_PIPELINE");
  const bool new_pipeline = frame_pipeline && std::strcmp(frame_pipeline, "1") == 0;
  if (new_pipeline) {
    /* New production demand has priority and never opens diagnostic backing files. */
    deep_camera_receipts_enabled_ = false;
    deep_surface_receipts_enabled_ = false;
    deep_volume_radiance_receipts_enabled_ = false;
    deep_volume_emission_spans_enabled_ = false;
    deep_independent_optical_enabled_ = false;
  }
  const char *state_cache = getenv("CYCLES_TEST_DEEP_STATIC_STATE_CACHE");
  deep_bounded_cache_state_ = !state_cache || std::strcmp(state_cache, "0") != 0;
  static std::atomic<uint64_t> next_native_device_generation{1};
  deep_native_device_generation_ = next_native_device_generation.fetch_add(1);
}

PathTraceWorkGPU::~PathTraceWorkGPU()
{
  /* render_samples is synchronous. Its domain joins or aborts before returning. */
  if (deep_bounded_work_) {
    deep_bounded_work_->discard(integrator_state_gpu_);
  }
  release_native_capture_buffers();
}

bool PathTraceWorkGPU::set_deep_frame_capture_consumer(DeepFrameCaptureConsumer *consumer)
{
  deep_bounded_uploaded_state_.reset();
  if (deep_native_domain_active_ || (consumer && device_->info.type != DEVICE_OPTIX)) {
    return false;
  }
  if (!consumer) {
    if (deep_bounded_work_) {
      deep_bounded_work_->discard(integrator_state_gpu_);
      deep_bounded_work_.reset();
    }
    deep_bounded_projection_.reset();
    deep_gpu_frame_work_.reset();
    deep_frame_capture_consumer_ = nullptr;
    deep_frame_capture_requested_ = false;
    release_native_capture_buffers();
    deep_native_budget_.reset();
    deep_native_device_budget_.reset();
    return true;
  }
  deep_frame_capture_consumer_ = consumer;
  deep_bounded_slots_empty_ = false;
  deep_frame_capture_requested_ = !consumer->uses_gpu_processing();
  if (consumer->uses_gpu_processing()) {
    try {
      /* The locked route never creates the retained history/replay owner. */
      release_native_capture_buffers();
      deep_gpu_frame_work_.reset();
      deep_bounded_work_ = std::make_unique<DeepBoundedWork>(*device_, *queue_, *consumer);
    }
    catch (const std::bad_alloc &) {
      device_->set_error("Deep GPU owner cannot fit the frame quotas"); return false;
    }
  }
  else deep_gpu_frame_work_.reset();
  deep_camera_receipts_enabled_ = false;
  deep_surface_receipts_enabled_ = false;
  deep_volume_radiance_receipts_enabled_ = false;
  deep_volume_emission_spans_enabled_ = false;
  deep_independent_optical_enabled_ = false;
  deep_capture_group_flush_ = {};
  integrator_state_gpu_.deep_camera_receipts = nullptr;
  integrator_state_gpu_.deep_surface_receipts = nullptr;
  integrator_state_gpu_.deep_volume_radiance_receipts = nullptr;
  integrator_state_gpu_.deep_volume_emission_spans = nullptr;
  integrator_state_gpu_.deep_optical_inputs = nullptr;
  integrator_state_gpu_.deep_optical_events = nullptr;
  integrator_state_gpu_.deep_optical_ledger = nullptr;
  integrator_state_gpu_.deep_optical_event_capacity = 0;
  return true;
}

void PathTraceWorkGPU::release_native_capture_buffers()
{
  deep_native_expression_journal_.reset();
  deep_native_control_journal_.reset();
  integrator_state_gpu_.deep_native_expression_source_base = 0;
  integrator_state_gpu_.deep_native_expression_node_base = 0;
  integrator_state_gpu_.deep_native_expression_call_base = 0;
  integrator_state_gpu_.deep_native_volume_checkpoints = nullptr;
  integrator_state_gpu_.deep_native_volume_checkpoint_capacity = 0;
  integrator_state_gpu_.deep_native_volume_step_limit = 0;
  integrator_state_gpu_.deep_native_domain_id = 0;
  integrator_state_gpu_.deep_native_validation_faults = 0;
  deep_native_validation_seen_ = 0;
  deep_native_validation_fatal_after_yield_ = false;
  deep_native_volume_checkpoints_.free();
  deep_native_cameras_.free();
  deep_compact_primitives_.free();
  deep_compact_rays_.free();
  deep_compact_stacks_.free();
  deep_compact_emission_.free();
  deep_native_homogeneous_.free();
  deep_native_roots_.free();
  deep_native_root_resident_.free();
  deep_native_root_pins_.free();
  deep_native_points_.free();
  deep_native_expression_sources_.free();
  deep_native_expression_nodes_.free();
  deep_native_expression_calls_.free();
  deep_native_camera_indices_.free();
  deep_native_root_handles_.free();
  deep_native_shadow_camera_indices_.free();
  deep_native_shadow_root_handles_.free();
  integrator_state_gpu_.path.deep_frame_camera_index = nullptr;
  integrator_state_gpu_.path.deep_frame_root_handle = nullptr;
  integrator_state_gpu_.shadow_path.deep_frame_camera_index = nullptr;
  integrator_state_gpu_.shadow_path.deep_frame_root_handle = nullptr;
  integrator_state_gpu_.deep_compact_primitives = nullptr;
  integrator_state_gpu_.deep_compact_rays = nullptr;
  integrator_state_gpu_.deep_compact_stacks = nullptr;
  integrator_state_gpu_.deep_compact_cameras = nullptr;
  integrator_state_gpu_.deep_compact_camera_base = 0;
  integrator_state_gpu_.deep_compact_stack_stride = 0;
  integrator_state_gpu_.deep_compact_root_colours = nullptr;
  integrator_state_gpu_.deep_compact_root_colour_capacity = 0;
  integrator_state_gpu_.deep_compact_emission = nullptr;
  integrator_state_gpu_.deep_compact_capacity = 0;
  integrator_state_gpu_.deep_compact_has_volume = 0;
  integrator_state_gpu_.deep_native_full_optical = 0;
  integrator_state_gpu_.deep_native_capture_rgb = 0;
  integrator_state_gpu_.deep_compact_witness = nullptr;
  integrator_state_gpu_.deep_compact_trace = nullptr;
  integrator_state_gpu_.deep_native_cameras = nullptr;
  integrator_state_gpu_.deep_native_homogeneous = nullptr;
  integrator_state_gpu_.deep_native_roots = nullptr;
  integrator_state_gpu_.deep_native_root_resident = nullptr;
  integrator_state_gpu_.deep_native_homogeneous_base = 0;
  integrator_state_gpu_.deep_native_point_base = 0;
  integrator_state_gpu_.deep_native_root_base = 0;
  integrator_state_gpu_.deep_native_root_resident_count = 0;
  integrator_state_gpu_.deep_native_points = nullptr;
  integrator_state_gpu_.deep_native_expression_sources = nullptr;
  integrator_state_gpu_.deep_native_expression_nodes = nullptr;
  integrator_state_gpu_.deep_native_expression_calls = nullptr;
  integrator_state_gpu_.deep_native_camera_capacity = 0;
  integrator_state_gpu_.deep_native_homogeneous_capacity = 0;
  integrator_state_gpu_.deep_native_root_capacity = 0;
  integrator_state_gpu_.deep_native_point_capacity = 0;
  integrator_state_gpu_.deep_native_expression_source_capacity = 0;
  integrator_state_gpu_.deep_native_expression_node_capacity = 0;
  integrator_state_gpu_.deep_native_expression_call_capacity = 0;
  if (deep_native_budget_ && deep_native_allocation_charge_) {
    deep_native_budget_->relinquish(deep_native_allocation_charge_);
  }
  if (deep_native_device_budget_ && deep_native_device_charge_) {
    deep_native_device_budget_->relinquish(deep_native_device_charge_);
  }
  if (deep_native_allocation_charge_ || deep_native_device_charge_)
    std::fprintf(stderr,
                 "DEEP_FRAME_CAPTURE_RELEASE domain=%llu host_bytes=%llu device_bytes=%llu "
                 "host_used=%llu device_used=%llu\n",
                 static_cast<unsigned long long>(deep_native_domain_.id),
                 static_cast<unsigned long long>(deep_native_allocation_charge_),
                 static_cast<unsigned long long>(deep_native_device_charge_),
                 static_cast<unsigned long long>(deep_native_budget_->used()),
                 static_cast<unsigned long long>(deep_native_device_budget_->used()));
  deep_native_allocation_charge_ = 0;
  deep_native_device_charge_ = 0;
}

bool PathTraceWorkGPU::begin_native_capture(const int start_sample,
                                            const int samples_num,
                                            const int sample_offset)
{
  if (!deep_frame_capture_requested_) {
    return true;
  }
  const auto fail = [&](const char *reason) {
    device_->set_error(reason);
    return false;
  };
  if (!deep_frame_capture_consumer_ || deep_native_domain_active_ || device_->have_error() ||
      device_->info.type != DEVICE_OPTIX || has_multiple_works() || max_num_paths_ <= 0 ||
      !integrator_state_gpu_.queue_counter)
  {
    return fail("Native Deep Frame capture requires one OptiX work and a non-null frame consumer");
  }
  const KernelData &data = device_scene_->data;
  const DeepCaptureDemand demand = deep_frame_capture_consumer_->capture_demand();
  const bool capture_rgb = demand.capture_rgb();
  const char *verify_payload = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
  if (verify_payload && std::strcmp(verify_payload, "1") == 0)
    std::fprintf(stderr,
                 "DEEP_FRAME_SCENE_FEATURES motion=%d hair=%d points=%d catcher=%d\n",
                 int(bool(data.kernel_features & KERNEL_FEATURE_OBJECT_MOTION)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_HAIR)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_POINTCLOUD)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_SHADOW_CATCHER)));
  if (verify_payload && std::strcmp(verify_payload, "1") == 0)
    std::fprintf(stderr,
                 "DEEP_FRAME_CAPABILITIES osl=%d light_linking=%d shadow_linking=%d guiding=%d\n",
                 int(bool(data.kernel_features & KERNEL_FEATURE_OSL_SHADING)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING)),
                 int(bool(data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)));
  const uint unsupported = KERNEL_FEATURE_PATH_GUIDING;
  if (data.film.use_deep_output || data.film.pass_sample_count == PASS_UNUSED || data.bake.use ||
      (data.kernel_features & unsupported) ||
      (data.cam.type != CAMERA_PERSPECTIVE && data.cam.type != CAMERA_ORTHOGRAPHIC &&
       data.cam.type != CAMERA_PANORAMA && data.cam.type != CAMERA_CUSTOM))
  {
    return fail("Native Deep Frame M1 received unsupported scene/Film/camera features");
  }
  if (data.background.volume_shader != SHADER_NONE) {
    const uint shader = data.background.volume_shader & SHADER_MASK;
    if (shader >= device_scene_->shaders.size())
      return fail("Native Deep World shader binding is invalid");
  }
  /* Actual surface closures are validated after shader evaluation. The first
   * supported nontransparent Surface establishes an immutable source root. */
  for (size_t i = 0; i < device_scene_->tri_shader.size(); ++i) {
    const uint shader = device_scene_->tri_shader.data()[i] & SHADER_MASK;
    if (shader >= device_scene_->shaders.size()) {
      return fail("Native Deep Frame triangle shader binding is invalid");
    }
  }
  const BufferParams &params = effective_buffer_params_;
  if (start_sample < 0 || sample_offset < 0 || samples_num <= 0 || params.width <= 0 ||
      params.height <= 0 || uint64_t(start_sample) + samples_num > uint64_t(INT_MAX))
  {
    return fail("Native Deep Frame capture image/sample range is invalid");
  }
  const uint64_t admissions = uint64_t(params.width) * params.height * samples_num;
  const bool volume_checkpoints = data.kernel_features & KERNEL_FEATURE_VOLUME;
  /* Without Volume emission there are no per-step expression pages to drain.
   * Keep checkpoint storage for real cache-miss retries, but execute the native
   * ordered walk without artificial periodic yields. Explicit validation still
   * selects its requested step bound below. */
  uint32_t operation_steps = deep_gpu_frame_work_ && (!capture_rgb || !device_scene_->volume_may_emit) ?
                                 uint32_t(INT_MAX) : 16;
  if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_OPERATION_STEPS")) {
    char *end = nullptr;
    errno = 0;
    const long parsed = strtol(value, &end, 10);
    if (errno || !*value || !end || *end || parsed < 1 || parsed > 16)
      return fail("Native continuation step limit must be an integer from 1 through 16");
    operation_steps = uint32_t(parsed);
  }
  bool validation_fatal_after_yield = false;
  if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VALIDATION_FATAL_AFTER_YIELD")) {
    if (strcmp(value, "1") != 0)
      return fail("Explicit native fatal-after-yield validation flag must be 1");
    validation_fatal_after_yield = true;
  }
  uint32_t validation_fault = 0;
  if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VALIDATION_FAULT")) {
    if (strcmp(value, "shadow_retry") == 0)
      validation_fault = DEEP_NATIVE_FAULT_SHADOW_RETRY;
    else if (strcmp(value, "mnee_retry") == 0)
      validation_fault = DEEP_NATIVE_FAULT_MNEE_RETRY;
    else if (strcmp(value, "background_retry") == 0)
      validation_fault = DEEP_NATIVE_FAULT_BACKGROUND_RETRY;
    else if (strcmp(value, "point_deny") == 0)
      validation_fault = DEEP_NATIVE_FAULT_POINT_DENY;
    else if (strcmp(value, "expression_deny") == 0)
      validation_fault = DEEP_NATIVE_FAULT_EXPRESSION_DENY;
    else if (strcmp(value, "homogeneous_deny") == 0)
      validation_fault = DEEP_NATIVE_FAULT_HOMOGENEOUS_DENY;
    else
      return fail("Unknown explicit native validation fault");
  }
  DeepNativeCaptureCapacity capacity;
  if (!deep_native_capture_capacity(admissions,
                                    size_t(max_num_paths_),
                                    data.integrator.has_shadow_catcher,
                                    device_scene_->volume_may_emit &&
                                        (data.kernel_features & KERNEL_FEATURE_VOLUME),
                                    data.integrator.volume_ray_marching,
                                    capacity,
                                    volume_checkpoints, capture_rgb, !deep_gpu_frame_work_, bool(deep_gpu_frame_work_),
                                    data.volume_stack_size))
    return fail("Native capture reservation exceeds checked transport capacity");
  const size_t camera_capacity = capacity.cameras;
  const size_t segment_capacity = capacity.segments;
  const size_t index_capacity = capacity.indices;
  const size_t expression_source_capacity = capacity.expression_sources;
  const size_t expression_node_capacity = capacity.expression_nodes;
  const size_t expression_call_capacity = capacity.expression_calls;
  const size_t staging_bytes = deep_gpu_frame_work_ ? 0 : capacity.host_staging;
  const size_t gpu_bytes = capacity.device;
  if (verify_payload && std::strcmp(verify_payload, "1") == 0)
    std::fprintf(stderr,
                 "DEEP_FRAME_EXPRESSION_DEMAND may_emit=%d admissions=%llu sources=%zu "
                 "nodes=%zu calls=%zu\n",
                 int(device_scene_->volume_may_emit),
                 static_cast<unsigned long long>(admissions),
                 expression_source_capacity,
                 expression_node_capacity,
                 expression_call_capacity);
  static std::atomic<uint64_t> next_native_domain{1};
  deep_native_domain_ = {};
  deep_native_domain_.id = next_native_domain.fetch_add(1);
  /* Complete camera coverage is a production requirement of this frame owner.
   * Verification flags only enable diagnostics; they cannot select its semantics. */
  deep_native_domain_.require_full_optical = true;
  deep_native_domain_.demand = demand;
  deep_native_domain_.has_volume = data.kernel_features & KERNEL_FEATURE_VOLUME;
  deep_native_domain_.stack_stride = data.volume_stack_size;
  deep_native_domain_.opaque_world = !device_scene_->data.background.transparent;
  deep_native_domain_.world_volume_shader = data.background.volume_shader;
  deep_native_domain_.world_volume_object = data.background.object_index;
  const bool affine_depth = data.cam.type == CAMERA_PERSPECTIVE ||
                            data.cam.type == CAMERA_ORTHOGRAPHIC;
  deep_native_domain_.depth_kind = affine_depth ? DeepFrozenDepthKind::Affine :
                                                  DeepFrozenDepthKind::Radial;
  const Transform depth_transform = affine_depth ? data.cam.worldtocamera : data.cam.cameratoworld;
  static_assert(sizeof(depth_transform) == sizeof(deep_native_domain_.depth_transform));
  std::memcpy(deep_native_domain_.depth_transform, &depth_transform, sizeof(depth_transform));
  deep_native_domain_.device_generation = deep_native_device_generation_;
  /* This revision is used only if the newly allocated binding actually completes. Failed
   * admissions abort it and never publish it as a completed generation. */
  deep_native_domain_.layout_generation = ++deep_native_layout_generation_;
  deep_native_domain_.start_sample = start_sample;
  deep_native_domain_.samples_num = samples_num;
  deep_native_domain_.sample_offset = sample_offset;
  deep_native_domain_.x = params.full_x;
  deep_native_domain_.y = params.full_y;
  deep_native_domain_.width = params.width;
  deep_native_domain_.height = params.height;
  deep_native_domain_.core_x = params.full_x + params.window_x;
  deep_native_domain_.core_y = params.full_y + params.window_y;
  deep_native_domain_.core_width = params.window_width;
  deep_native_domain_.core_height = params.window_height;
  deep_native_domain_.scheduled_admissions = admissions;
  deep_native_domain_.gpu_requested_bytes = gpu_bytes;
  deep_native_domain_.host_staging_requested_bytes = staging_bytes;
  auto *reference = deep_frame_capture_consumer_->reference_capture();
  if (!reference || !reference->begin_native_capture(deep_native_domain_)) {
    return fail("Native Deep Frame consumer rejected domain admission");
  }
  deep_native_domain_active_ = true;
  try {
    if (deep_gpu_frame_work_ && !deep_gpu_frame_work_->begin(deep_native_domain_)) return false;
    deep_native_budget_.emplace(deep_frame_capture_consumer_->memory_budget());
    deep_native_device_budget_.emplace(deep_frame_capture_consumer_->device_memory_budget());
    deep_native_budget_->acquire(staging_bytes);
    deep_native_allocation_charge_ = staging_bytes;
    deep_native_device_budget_->acquire(gpu_bytes);
    deep_native_device_charge_ = gpu_bytes;
    if (!deep_gpu_frame_work_) {
      deep_native_expression_journal_ = std::allocate_shared<DeepExpressionJournal>(
          DeepBudgetAllocator<DeepExpressionJournal>(*deep_native_budget_),
          *deep_native_budget_,
          deep_native_domain_,
          uint32_t(camera_capacity));
      deep_native_control_journal_ = std::allocate_shared<DeepControlJournal>(
          DeepBudgetAllocator<DeepControlJournal>(*deep_native_budget_),
          *deep_native_budget_,
          deep_native_domain_,
          uint32_t(camera_capacity));
    }
    if (volume_checkpoints) {
      deep_native_volume_checkpoints_.alloc_to_device(
          camera_capacity * (DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES / sizeof(uint64_t)));
      queue_->zero_to_device(deep_native_volume_checkpoints_);
      integrator_state_gpu_.deep_native_volume_checkpoints = reinterpret_cast<void *>(
          deep_native_volume_checkpoints_.device_pointer);
      integrator_state_gpu_.deep_native_volume_checkpoint_capacity = uint(camera_capacity);
    }
    integrator_state_gpu_.deep_native_domain_id = deep_native_domain_.id;
    integrator_state_gpu_.deep_native_volume_step_limit = operation_steps;
    const auto allocate = [&](auto &buffer, const size_t count) {
      if (deep_gpu_frame_work_) buffer.alloc_device_only(count);
      else buffer.alloc(count);
    };
    allocate(deep_native_cameras_, camera_capacity);
    if (capacity.scalar_records) {
      deep_compact_primitives_.alloc_device_only(capacity.scalar_records);
      deep_compact_primitives_.alloc_to_device();
      deep_compact_rays_.alloc_to_device(capacity.scalar_states);
      if (capacity.scalar_stack) deep_compact_stacks_.alloc_to_device(capacity.scalar_stack);
      if (capacity.emission_states) {
        deep_compact_emission_.alloc_to_device(capacity.emission_states);
        integrator_state_gpu_.deep_compact_emission = reinterpret_cast<DeepCompactEmissionState *>(
            deep_compact_emission_.device_pointer);
      }
      if (device_->have_error() || !deep_compact_primitives_.device_pointer ||
          !deep_compact_rays_.device_pointer)
        return fail("Deep scalar capture allocation failed");
      integrator_state_gpu_.deep_compact_primitives = reinterpret_cast<DeepCompactPrimitive *>(
          deep_compact_primitives_.device_pointer);
      integrator_state_gpu_.deep_compact_rays = reinterpret_cast<DeepCompactRayState *>(
          deep_compact_rays_.device_pointer);
      integrator_state_gpu_.deep_compact_capacity = uint(capacity.scalar_records);
      integrator_state_gpu_.deep_compact_cameras = reinterpret_cast<DeepNativeCameraRecord *>(
          deep_native_cameras_.device_pointer);
      integrator_state_gpu_.deep_compact_stacks = reinterpret_cast<DeepCompactStackEntry *>(
          deep_compact_stacks_.device_pointer);
      integrator_state_gpu_.deep_compact_camera_base = 0;
      integrator_state_gpu_.deep_compact_stack_stride = data.volume_stack_size;
      integrator_state_gpu_.deep_compact_has_volume = deep_native_domain_.has_volume;
    }
    allocate(deep_native_homogeneous_, segment_capacity);
    allocate(deep_native_roots_, capacity.roots);
    allocate(deep_native_root_resident_, capacity.root_resident);
    allocate(deep_native_root_pins_, capacity.root_pins);
    allocate(deep_native_points_, capacity.points);
    deep_native_cameras_.alloc_to_device();
    if (deep_gpu_frame_work_)
      integrator_state_gpu_.deep_compact_cameras = reinterpret_cast<DeepNativeCameraRecord *>(
          deep_native_cameras_.device_pointer);
    if (segment_capacity) deep_native_homogeneous_.alloc_to_device();
    if (capacity.roots) deep_native_roots_.alloc_to_device();
    if (capacity.root_resident) deep_native_root_resident_.alloc_to_device();
    if (capacity.root_pins) deep_native_root_pins_.alloc_to_device();
    if (capacity.points) deep_native_points_.alloc_to_device();
    if (expression_node_capacity) {
      allocate(deep_native_expression_sources_, expression_source_capacity);
      allocate(deep_native_expression_nodes_, expression_node_capacity);
      allocate(deep_native_expression_calls_, expression_call_capacity);
      deep_native_expression_sources_.alloc_to_device();
      deep_native_expression_nodes_.alloc_to_device();
      deep_native_expression_calls_.alloc_to_device();
      if (device_->have_error() || !deep_native_expression_sources_.device_pointer ||
          !deep_native_expression_nodes_.device_pointer ||
          !deep_native_expression_calls_.device_pointer ||
          (!deep_gpu_frame_work_ && (!deep_native_expression_sources_.data() ||
                                    !deep_native_expression_nodes_.data() ||
                                    !deep_native_expression_calls_.data())) ||
          deep_native_expression_sources_.shared_pointer ||
          deep_native_expression_nodes_.shared_pointer ||
          deep_native_expression_calls_.shared_pointer)
      {
        finish_native_capture(false);
        return fail(
            "Native expression capture requires independent charged device and staging arrays");
      }
    }
    deep_native_camera_indices_.alloc_to_device(index_capacity);
    deep_native_root_handles_.alloc_to_device(index_capacity);
    deep_native_shadow_camera_indices_.alloc_to_device(max_num_paths_);
    deep_native_shadow_root_handles_.alloc_to_device(max_num_paths_);
    if (capture_rgb && !deep_gpu_frame_work_ && ((!deep_native_root_resident_.data() || !deep_native_root_pins_.data()) ||
        !deep_native_root_resident_.device_pointer || !deep_native_root_pins_.device_pointer ||
        deep_native_root_resident_.shared_pointer || deep_native_root_pins_.shared_pointer))
    {
      finish_native_capture(false);
      return fail(
          "Native root descriptors require independent quota-owned staging and device memory");
    }
    if (device_->have_error() ||
        (!deep_gpu_frame_work_ && (!deep_native_cameras_.data() ||
                                  !deep_native_homogeneous_.data() || !deep_native_roots_.data() ||
                                  !deep_native_points_.data())) || !deep_native_cameras_.device_pointer ||
        (capture_rgb && ((segment_capacity && !deep_native_homogeneous_.device_pointer) || !deep_native_roots_.device_pointer ||
                         (!deep_gpu_frame_work_ && !deep_native_points_.device_pointer))) || !deep_native_camera_indices_.device_pointer ||
        !deep_native_root_handles_.device_pointer ||
        !deep_native_shadow_camera_indices_.device_pointer ||
        !deep_native_shadow_root_handles_.device_pointer || deep_native_cameras_.shared_pointer ||
        deep_native_homogeneous_.shared_pointer || deep_native_camera_indices_.shared_pointer ||
        deep_native_root_handles_.shared_pointer || deep_native_roots_.shared_pointer ||
        deep_native_points_.shared_pointer || deep_native_shadow_camera_indices_.shared_pointer ||
        deep_native_shadow_root_handles_.shared_pointer ||
        (volume_checkpoints && (!deep_native_volume_checkpoints_.device_pointer ||
                                deep_native_volume_checkpoints_.shared_pointer)))
    {
      finish_native_capture(false);
      return fail("Native Deep Frame capture requires independent device and host staging memory");
    }
    integrator_state_gpu_.deep_native_cameras = (DeepNativeCameraRecord *)
                                                    deep_native_cameras_.device_pointer;
    integrator_state_gpu_.deep_native_homogeneous = (DeepNativeHomogeneousRecord *)
                                                        deep_native_homogeneous_.device_pointer;
    integrator_state_gpu_.deep_native_roots = (DeepNativeRootRecord *)
                                                  deep_native_roots_.device_pointer;
    integrator_state_gpu_.deep_native_points = (DeepNativePointRecord *)
                                                   deep_native_points_.device_pointer;
    integrator_state_gpu_.deep_native_camera_capacity = uint(camera_capacity);
    integrator_state_gpu_.deep_native_homogeneous_capacity = uint(segment_capacity);
    integrator_state_gpu_.deep_native_root_capacity = uint(capacity.roots);
    integrator_state_gpu_.deep_native_point_capacity = uint(capacity.points);
    integrator_state_gpu_.deep_native_expression_sources =
        (DeepNativeExpressionSource *)deep_native_expression_sources_.device_pointer;
    integrator_state_gpu_.deep_native_expression_nodes =
        (DeepNativeExpressionNode *)deep_native_expression_nodes_.device_pointer;
    integrator_state_gpu_.deep_native_expression_calls =
        (DeepNativeExpressionCall *)deep_native_expression_calls_.device_pointer;
    integrator_state_gpu_.deep_native_expression_source_capacity = uint(
        expression_source_capacity);
    integrator_state_gpu_.deep_native_expression_node_capacity = uint(expression_node_capacity);
    integrator_state_gpu_.deep_native_expression_call_capacity = uint(expression_call_capacity);
    integrator_state_gpu_.path.deep_frame_camera_index =
        (uint32_t *)deep_native_camera_indices_.device_pointer;
    integrator_state_gpu_.path.deep_frame_root_handle =
        (uint32_t *)deep_native_root_handles_.device_pointer;
    integrator_state_gpu_.shadow_path.deep_frame_camera_index =
        (uint32_t *)deep_native_shadow_camera_indices_.device_pointer;
    integrator_state_gpu_.shadow_path.deep_frame_root_handle =
        (uint32_t *)deep_native_shadow_root_handles_.device_pointer;
    integrator_state_gpu_.deep_native_root_resident = reinterpret_cast<DeepNativeRootRecord *>(
        deep_native_root_resident_.device_pointer);
    integrator_state_gpu_.deep_native_root_resident_count = 0;
    integrator_state_gpu_.deep_native_homogeneous_base = 0;
    integrator_state_gpu_.deep_native_point_base = 0;
    integrator_state_gpu_.deep_native_root_base = 0;
    integrator_state_gpu_.deep_native_validation_faults = validation_fault;
    deep_native_validation_fatal_after_yield_ = validation_fatal_after_yield;
    deep_native_validation_seen_ = 0;
    integrator_state_gpu_.deep_native_full_optical = deep_native_domain_.require_full_optical;
    integrator_state_gpu_.deep_native_capture_rgb = capture_rgb;
    if (deep_gpu_frame_work_ && capture_rgb) {
      if (!deep_gpu_frame_work_->reserve_native_root_colours(uint32_t(capacity.roots))) return false;
      integrator_state_gpu_.deep_compact_root_colours = reinterpret_cast<DeepCompactRootColour *>(
          deep_gpu_frame_work_->native_pointer(DeepGPUResidentKind::RootColour));
      integrator_state_gpu_.deep_compact_root_colour_capacity = uint(capacity.roots);
    }
    if (deep_gpu_frame_work_ && !deep_gpu_frame_work_->bind_witness(integrator_state_gpu_)) return false;
    device_->const_copy_to(
        "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    if (device_->have_error()) {
      finish_native_capture(false);
      return false;
    }
    return true;
  }
  catch (const std::bad_alloc &) {
    device_->set_error(
        string_printf("Native Deep Frame capture quota exceeded (host_requested=%zu host_used=%zu "
                      "host_quota=%zu device_requested=%zu device_used=%zu device_quota=%zu)",
                      staging_bytes,
                      deep_native_budget_->used(),
                      deep_native_budget_->quota(),
                      gpu_bytes,
                      deep_native_device_budget_->used(),
                      deep_native_device_budget_->quota()));
    finish_native_capture(false);
    return false;
  }
}

bool PathTraceWorkGPU::flush_native_gpu_pages()
{
  if (!deep_native_domain_active_ || !deep_gpu_frame_work_)
    return true;
  const DeepDispatchWall elapsed(deep_gpu_page_flush_wall_);
  auto &counter = *integrator_queue_counter_.data();
  struct Page {
    DeepGPUResidentKind kind;
    device_memory *memory;
    uint32_t *reserved, *written;
    size_t capacity;
  };
  const Page pages[] = {
      {DeepGPUResidentKind::Primitive, &deep_compact_primitives_, &counter.deep_compact_reserved,
       &counter.deep_compact_written, deep_compact_primitives_.size()},
      {DeepGPUResidentKind::Root, &deep_native_roots_, &counter.deep_native_root_reserved,
       &counter.deep_native_root_written, deep_native_roots_.size()},
      {DeepGPUResidentKind::Homogeneous, &deep_native_homogeneous_,
       &counter.deep_native_homogeneous_reserved, &counter.deep_native_homogeneous_written,
       deep_native_homogeneous_.size()},
      {DeepGPUResidentKind::Point, &deep_native_points_, &counter.deep_native_point_reserved,
       &counter.deep_native_point_written, deep_native_points_.size()},
      {DeepGPUResidentKind::Source, &deep_native_expression_sources_,
       &counter.deep_native_expression_source_reserved, &counter.deep_native_expression_source_written,
       deep_native_expression_sources_.size()},
      {DeepGPUResidentKind::Node, &deep_native_expression_nodes_,
       &counter.deep_native_expression_node_reserved, &counter.deep_native_expression_node_written,
       deep_native_expression_nodes_.size()},
      {DeepGPUResidentKind::Call, &deep_native_expression_calls_,
       &counter.deep_native_expression_call_reserved, &counter.deep_native_expression_call_written,
       deep_native_expression_calls_.size()}};
  bool changed = false;
  for (const auto &page : pages) {
    if (*page.reserved != *page.written || *page.written > page.capacity) {
      device_->set_error(string_printf(
          "Joined GPU native pages have unfinished or overflowing reservations "
          "(kind=%u reserved=%u written=%u capacity=%zu)",
          uint32_t(page.kind), *page.reserved, *page.written, page.capacity));
      return false;
    }
    changed |= *page.written != 0;
  }
  if (!changed)
    return true;
  /* Producers have already joined at the counter read. No kernel consumes an
   * intermediate binding between these independent D2D copies. Publish the
   * complete new bases and root address once, after all page copies join. */
  for (const auto &page : pages)
    if (*page.written && !deep_gpu_frame_work_->append_native(page.kind, *page.memory, *page.written))
      return false;
  if (!queue_->synchronize_for_state_rebind())
    return false;
  integrator_state_gpu_.deep_native_root_resident = reinterpret_cast<DeepNativeRootRecord *>(
      deep_gpu_frame_work_->native_pointer(DeepGPUResidentKind::Root));
  integrator_state_gpu_.deep_native_root_resident_count =
      deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Root);
  integrator_state_gpu_.deep_native_root_base = integrator_state_gpu_.deep_native_root_resident_count;
  if (integrator_state_gpu_.deep_native_capture_rgb) {
    const uint64_t required = uint64_t(integrator_state_gpu_.deep_native_root_base) +
                              integrator_state_gpu_.deep_native_root_capacity;
    if (required > UINT32_MAX || !deep_gpu_frame_work_->reserve_native_root_colours(uint32_t(required)))
      return false;
    integrator_state_gpu_.deep_compact_root_colours = reinterpret_cast<DeepCompactRootColour *>(
        deep_gpu_frame_work_->native_pointer(DeepGPUResidentKind::RootColour));
    integrator_state_gpu_.deep_compact_root_colour_capacity = uint(required);
  }
  integrator_state_gpu_.deep_native_homogeneous_base =
      deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Homogeneous);
  integrator_state_gpu_.deep_native_point_base = deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Point);
  integrator_state_gpu_.deep_native_expression_source_base =
      deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Source);
  integrator_state_gpu_.deep_native_expression_node_base =
      deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Node);
  integrator_state_gpu_.deep_native_expression_call_base =
      deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Call);
  for (const auto &page : pages)
    *page.reserved = *page.written = 0;
  queue_->copy_to_device(integrator_queue_counter_);
  if (!queue_->synchronize_for_state_rebind())
    return false;
  device_->const_copy_to("integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
  deep_gpu_page_publications_++;
  return !device_->have_error();
}

bool PathTraceWorkGPU::flush_native_root_page()
{
  if (!deep_native_domain_active_)
    return true;
  if (!deep_native_control_journal_) {
    device_->set_error("Missing native root history at joined dispatch");
    return false;
  }
  auto &counter = *integrator_queue_counter_.data();
  const uint32_t roots = counter.deep_native_root_written;
  const uint32_t resident = integrator_state_gpu_.deep_native_root_resident_count;
  if (deep_gpu_frame_work_) {
    if (roots != counter.deep_native_root_reserved || roots > deep_native_roots_.size()) {
      device_->set_error("Deep GPU root page has unfinished or overflowing reservations"); return false;
    }
    if (!roots) return true;
    if (!deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Root, deep_native_roots_, roots) ||
        !queue_->synchronize_for_state_rebind()) return false;
    const uint32_t total = deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Root);
    integrator_state_gpu_.deep_native_root_resident = reinterpret_cast<DeepNativeRootRecord *>(
        deep_gpu_frame_work_->native_pointer(DeepGPUResidentKind::Root));
    integrator_state_gpu_.deep_native_root_resident_count = total;
    integrator_state_gpu_.deep_native_root_base = total;
    counter.deep_native_root_reserved = counter.deep_native_root_written = 0;
    queue_->copy_to_device(integrator_queue_counter_);
    if (!queue_->synchronize_for_state_rebind()) return false;
    device_->const_copy_to("integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    return !device_->have_error();
  }
  if (roots != counter.deep_native_root_reserved || roots > deep_native_roots_.size() ||
      resident > deep_native_root_resident_.size() ||
      counter.deep_native_camera_written > counter.deep_native_camera_reserved ||
      counter.deep_native_camera_reserved > deep_native_cameras_.size())
  {
    device_->set_error("Native root window contains unfinished or overflowing reservations");
    return false;
  }
  if (!roots && !resident)
    return true;
  /* Camera cache misses leave reserved slots pending, possibly between completed slots.
   * Snapshot every reservation; pending records have no root until evaluation commits.
   * The final domain join still requires every reserved camera to complete. */
  const int camera_states = int(counter.deep_native_camera_reserved);
  int main_queued = 0, shadow_queued = 0;
  for (int kernel = 0; kernel < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++kernel) {
    const int count = counter.num_queued[kernel];
    if (count < 0) {
      device_->set_error("Negative native queue at root reference snapshot");
      return false;
    }
    if (kernel_is_shadow_path(DeviceKernel(kernel)) ||
        kernel == DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING)
      shadow_queued += count;
    else
      main_queued += count;
  }
  const int main_states = main_queued ? max_active_main_path_index_ : 0;
  const int shadow_states = shadow_queued ? integrator_next_shadow_path_index_.data()[0] : 0;
  if (main_states < 0 || shadow_states < 0 ||
      size_t(main_states) > deep_native_root_handles_.data_size ||
      size_t(shadow_states) > deep_native_shadow_root_handles_.data_size ||
      size_t(resident) + roots > deep_native_root_pins_.size())
  {
    device_->set_error(
        "Native root reference snapshot exceeds its actual state or pin allocation");
    return false;
  }
  const int work_size = std::max(camera_states, std::max(main_states, shadow_states));
  if (!work_size) {
    device_->set_error("Native resident root outlived its admitted cameras");
    return false;
  }
  try {
    queue_->zero_to_device(deep_native_root_pins_);
    const DeviceKernelArguments args(
        &main_states, &shadow_states, &camera_states, &deep_native_root_pins_.device_pointer);
    if (!queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_DEEP_NATIVE_ROOT_PINS, work_size, args))
      return false;
    queue_->copy_from_device(integrator_queue_counter_);
    if (!queue_->synchronize() || device_->have_error())
      return false;
    if (counter.deep_native_capture_error) {
      device_->set_error(
          string_printf("Native root reference snapshot lost a live handle: error_bits=%u",
                        counter.deep_native_capture_error));
      return false;
    }
    if (roots)
      deep_native_roots_.copy_from_device(0, roots, 1);
    deep_native_root_pins_.copy_from_device(0, size_t(resident) + roots, 1);
    if (device_->have_error())
      return false;
    DeepControlPage page;
    page.domain = deep_native_domain_.id;
    page.device_generation = deep_native_domain_.device_generation;
    page.layout_generation = deep_native_domain_.layout_generation;
    page.root_base = integrator_state_gpu_.deep_native_root_base;
    page.homogeneous_base = uint32_t(deep_native_control_journal_->homogeneous_count());
    page.point_base = uint32_t(deep_native_control_journal_->point_count());
    page.roots = {deep_native_roots_.data(), roots};
    if (const auto status = deep_native_control_journal_->append(page); !status) {
      device_->set_error(status.message);
      return false;
    }
    DeepNativeCaptureVector<DeepNativeRootRecord> selected{
        DeepBudgetAllocator<DeepNativeRootRecord>(*deep_native_budget_)};
    const std::span<const uint32_t> pins{deep_native_root_pins_.data(), size_t(resident) + roots};
    if (const auto status = deep_native_control_journal_->select_live_roots(
            {deep_native_root_resident_.data(), resident}, page.roots, pins, selected);
        !status)
    {
      device_->set_error(status.message);
      return false;
    }
    size_t camera_pins = 0, main_pins = 0, shadow_pins = 0, mnee_pins = 0;
    for (const uint32_t pin : pins) {
      camera_pins += bool(pin & DEEP_NATIVE_ROOT_PIN_CAMERA);
      main_pins += bool(pin & DEEP_NATIVE_ROOT_PIN_MAIN);
      shadow_pins += bool(pin & DEEP_NATIVE_ROOT_PIN_SHADOW);
      mnee_pins += bool(pin & DEEP_NATIVE_ROOT_PIN_MNEE);
    }
    const char *verify = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
    if (verify && strcmp(verify, "1") == 0 &&
        (roots || selected.size() != resident || shadow_pins || mnee_pins))
      std::fprintf(stderr,
                   "DEEP_NATIVE_ROOT_PAGE domain=%llu root_base=%u roots=%u old_resident=%u "
                   "resident=%zu camera_pins=%zu main_pins=%zu shadow_pins=%zu mnee_pins=%zu "
                   "main_states=%d shadow_states=%d\n",
                   static_cast<unsigned long long>(page.domain),
                   page.root_base,
                   roots,
                   resident,
                   selected.size(),
                   camera_pins,
                   main_pins,
                   shadow_pins,
                   mnee_pins,
                   main_states,
                   shadow_states);
    if (!roots && selected.size() == resident)
      return true; /* Every old descriptor remains pinned; no binding mutation. */
    if (!queue_->synchronize_for_state_rebind())
      return false;
    if (selected.size() > deep_native_root_resident_.size()) {
      const size_t next = selected.size();
      const size_t additional = (next - deep_native_root_resident_.size()) *
                                (sizeof(DeepNativeRootRecord) + sizeof(uint32_t));
      deep_native_budget_->acquire(additional);
      try {
        deep_native_device_budget_->acquire(additional);
      }
      catch (...) {
        deep_native_budget_->relinquish(additional);
        throw;
      }
      deep_native_allocation_charge_ += additional;
      deep_native_device_charge_ += additional;
      /* Joined producers and the selected host copy make freeing old backing safe. */
      deep_native_root_resident_.alloc(next);
      deep_native_root_pins_.alloc(next + deep_native_roots_.size());
      deep_native_root_resident_.alloc_to_device();
      deep_native_root_pins_.alloc_to_device();
    }
    if (device_->have_error() || !deep_native_root_resident_.data() ||
        !deep_native_root_resident_.device_pointer || !deep_native_root_pins_.device_pointer ||
        deep_native_root_resident_.shared_pointer || deep_native_root_pins_.shared_pointer)
    {
      device_->set_error("Native live root descriptors cannot fit independent device memory");
      return false;
    }
    std::copy(selected.begin(), selected.end(), deep_native_root_resident_.data());
    queue_->copy_to_device(deep_native_root_resident_);
    integrator_state_gpu_.deep_native_root_resident = reinterpret_cast<DeepNativeRootRecord *>(
        deep_native_root_resident_.device_pointer);
    integrator_state_gpu_.deep_native_root_resident_count = uint32_t(selected.size());
    integrator_state_gpu_.deep_native_root_base = uint32_t(
        deep_native_control_journal_->root_count());
    counter.deep_native_root_reserved = counter.deep_native_root_written = 0;
    queue_->copy_to_device(integrator_queue_counter_);
    if (!queue_->synchronize_for_state_rebind())
      return false;
    device_->const_copy_to(
        "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    return !device_->have_error();
  }
  catch (const std::bad_alloc &) {
    device_->set_error("Native live root snapshot exceeds host or device quota");
    return false;
  }
}

bool PathTraceWorkGPU::flush_native_control_page()
{
  if (!deep_native_domain_active_)
    return true;
  if (!deep_native_control_journal_) {
    device_->set_error("Missing native completed control history");
    return false;
  }
  auto &counter = *integrator_queue_counter_.data();
  const uint32_t segments = counter.deep_native_homogeneous_written;
  const uint32_t points = counter.deep_native_point_written;
  if (segments != counter.deep_native_homogeneous_reserved ||
      points != counter.deep_native_point_reserved || segments > deep_native_homogeneous_.size() ||
      points > deep_native_points_.size())
  {
    device_->set_error("Native control page has unfinished or overflowing Film reservations");
    return false;
  }
  if (!segments && !points)
    return true;
  if (deep_gpu_frame_work_) {
    if (!deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Homogeneous,
                                              deep_native_homogeneous_, segments) ||
        !deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Point, deep_native_points_, points) ||
        !queue_->synchronize_for_state_rebind()) return false;
    integrator_state_gpu_.deep_native_homogeneous_base =
        deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Homogeneous);
    integrator_state_gpu_.deep_native_point_base = deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Point);
    counter.deep_native_homogeneous_reserved = counter.deep_native_homogeneous_written = 0;
    counter.deep_native_point_reserved = counter.deep_native_point_written = 0;
    queue_->copy_to_device(integrator_queue_counter_);
    if (!queue_->synchronize_for_state_rebind()) return false;
    device_->const_copy_to("integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    return !device_->have_error();
  }
  if (segments)
    deep_native_homogeneous_.copy_from_device(0, segments, 1);
  if (points)
    deep_native_points_.copy_from_device(0, points, 1);
  if (device_->have_error())
    return false;
  DeepControlPage page;
  page.domain = deep_native_domain_.id;
  page.device_generation = deep_native_domain_.device_generation;
  page.layout_generation = deep_native_domain_.layout_generation;
  page.root_base = integrator_state_gpu_.deep_native_root_base;
  page.homogeneous_base = integrator_state_gpu_.deep_native_homogeneous_base;
  page.point_base = integrator_state_gpu_.deep_native_point_base;
  page.homogeneous = {deep_native_homogeneous_.data(), segments};
  page.points = {deep_native_points_.data(), points};
  if (const auto status = deep_native_control_journal_->append(page); !status) {
    device_->set_error(status.message);
    return false;
  }
  const char *verify = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
  if (verify && strcmp(verify, "1") == 0)
    std::fprintf(stderr,
                 "DEEP_NATIVE_CONTROL_PAGE domain=%llu homogeneous_base=%u homogeneous=%u "
                 "point_base=%u points=%u homogeneous_capacity=%zu point_capacity=%zu\n",
                 static_cast<unsigned long long>(page.domain),
                 page.homogeneous_base,
                 segments,
                 page.point_base,
                 points,
                 deep_native_homogeneous_.size(),
                 deep_native_points_.size());
  /* Homogeneous indices are local to this completed operation. Checkpoints
   * never retain them; points have no GPU reader after their Film commit. */
  if (!queue_->synchronize_for_state_rebind())
    return false;
  integrator_state_gpu_.deep_native_homogeneous_base = uint32_t(
      deep_native_control_journal_->homogeneous_count());
  integrator_state_gpu_.deep_native_point_base = uint32_t(
      deep_native_control_journal_->point_count());
  counter.deep_native_homogeneous_reserved = counter.deep_native_homogeneous_written = 0;
  counter.deep_native_point_reserved = counter.deep_native_point_written = 0;
  queue_->copy_to_device(integrator_queue_counter_);
  if (!queue_->synchronize_for_state_rebind())
    return false;
  device_->const_copy_to(
      "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
  return !device_->have_error();
}

bool PathTraceWorkGPU::flush_native_expression_page()
{
  if (!deep_native_domain_active_ || !deep_native_expression_journal_)
    return true;
  auto &counter = *integrator_queue_counter_.data();
  const uint32_t sources = counter.deep_native_expression_source_written;
  const uint32_t nodes = counter.deep_native_expression_node_written;
  const uint32_t calls = counter.deep_native_expression_call_written;
  if (sources != counter.deep_native_expression_source_reserved ||
      nodes != counter.deep_native_expression_node_reserved ||
      calls != counter.deep_native_expression_call_reserved ||
      sources > deep_native_expression_sources_.size() ||
      nodes > deep_native_expression_nodes_.size() || calls > deep_native_expression_calls_.size())
  {
    device_->set_error(
        "Native expression page cannot drain unfinished or overflowing reservations");
    return false;
  }
  if (!sources && !nodes && !calls)
    return true;
  if (deep_gpu_frame_work_) {
    if (!deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Source, deep_native_expression_sources_, sources) ||
        !deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Node, deep_native_expression_nodes_, nodes) ||
        !deep_gpu_frame_work_->append_native(DeepGPUResidentKind::Call, deep_native_expression_calls_, calls) ||
        !queue_->synchronize_for_state_rebind()) return false;
    integrator_state_gpu_.deep_native_expression_source_base =
        deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Source);
    integrator_state_gpu_.deep_native_expression_node_base =
        deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Node);
    integrator_state_gpu_.deep_native_expression_call_base =
        deep_gpu_frame_work_->native_count(DeepGPUResidentKind::Call);
    counter.deep_native_expression_source_reserved = counter.deep_native_expression_source_written = 0;
    counter.deep_native_expression_node_reserved = counter.deep_native_expression_node_written = 0;
    counter.deep_native_expression_call_reserved = counter.deep_native_expression_call_written = 0;
    queue_->copy_to_device(integrator_queue_counter_);
    if (!queue_->synchronize_for_state_rebind()) return false;
    device_->const_copy_to("integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    return !device_->have_error();
  }
  try {
    if (sources)
      deep_native_expression_sources_.copy_from_device(0, sources, 1);
    if (nodes)
      deep_native_expression_nodes_.copy_from_device(0, nodes, 1);
    if (calls)
      deep_native_expression_calls_.copy_from_device(0, calls, 1);
    if (device_->have_error())
      return false;
    DeepExpressionPage page;
    page.domain = deep_native_domain_.id;
    page.device_generation = deep_native_domain_.device_generation;
    page.layout_generation = deep_native_domain_.layout_generation;
    page.source_base = integrator_state_gpu_.deep_native_expression_source_base;
    page.node_base = integrator_state_gpu_.deep_native_expression_node_base;
    page.call_base = integrator_state_gpu_.deep_native_expression_call_base;
    page.sources = {deep_native_expression_sources_.data(), sources};
    page.nodes = {deep_native_expression_nodes_.data(), nodes};
    page.calls = {deep_native_expression_calls_.data(), calls};
    if (const auto status = deep_native_expression_journal_->append(page); !status) {
      device_->set_error(status.message);
      return false;
    }
    const size_t source_count = deep_native_expression_journal_->source_count();
    const size_t node_count = deep_native_expression_journal_->node_count();
    const size_t call_count = deep_native_expression_journal_->call_count();
    if (source_count > UINT32_MAX - deep_native_expression_sources_.size() ||
        node_count > UINT32_MAX - deep_native_expression_nodes_.size() ||
        call_count > UINT32_MAX - deep_native_expression_calls_.size())
    {
      device_->set_error("Native expression journal exhausted global transport handles");
      return false;
    }
    integrator_state_gpu_.deep_native_expression_source_base = uint32_t(source_count);
    integrator_state_gpu_.deep_native_expression_node_base = uint32_t(node_count);
    integrator_state_gpu_.deep_native_expression_call_base = uint32_t(call_count);
    counter.deep_native_expression_source_written =
        counter.deep_native_expression_source_reserved = 0;
    counter.deep_native_expression_node_written = counter.deep_native_expression_node_reserved = 0;
    counter.deep_native_expression_call_written = counter.deep_native_expression_call_reserved = 0;
    queue_->copy_to_device(integrator_queue_counter_);
    if (!queue_->synchronize_for_state_rebind())
      return false;
    device_->const_copy_to(
        "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    std::fprintf(stderr,
                 "DEEP_FRAME_EXPRESSION_PAGE domain=%llu source_base=%u node_base=%u call_base=%u "
                 "sources=%u nodes=%u calls=%u\n",
                 static_cast<unsigned long long>(page.domain),
                 page.source_base,
                 page.node_base,
                 page.call_base,
                 sources,
                 nodes,
                 calls);
    return !device_->have_error();
  }
  catch (const std::bad_alloc &) {
    device_->set_error("Native expression page readback exceeds host quota");
    return false;
  }
}

bool PathTraceWorkGPU::close_deep_tile()
{
  if (deep_bounded_work_) {
    try {
      return deep_bounded_work_->close_tile();
    }
    catch (const std::bad_alloc &) {
      device_->set_error("Bounded Deep final tile exceeds its host quota");
      return false;
    }
  }
  return false;
}

bool PathTraceWorkGPU::prepare_deep_tile()
{
  if (deep_bounded_work_) {
    return deep_bounded_work_->prepare_tile();
  }
  return false;
}

bool PathTraceWorkGPU::drain_deep_tiles()
{
  return !deep_bounded_work_ ||
         (deep_bounded_work_->publish_ready() && deep_bounded_work_->drain() &&
          deep_bounded_work_->publish_ready());
}

bool PathTraceWorkGPU::publish_ready_deep_tiles()
{
  return !deep_bounded_work_ || deep_bounded_work_->publish_ready();
}

bool PathTraceWorkGPU::migrate_bounded_deep(PathTraceWork &destination,
                                            const int x,
                                            const int y,
                                            const uint32_t width,
                                            const uint32_t height)
{
  if (!deep_bounded_work_ || destination.get_device()->info.type != DEVICE_OPTIX) {
    return false;
  }
  auto &target = static_cast<PathTraceWorkGPU &>(destination);
  return target.deep_bounded_work_ &&
         deep_bounded_work_->migrate(*target.deep_bounded_work_, x, y, width, height);
}

bool PathTraceWorkGPU::set_bounded_deep_beauty(const RenderBuffers &buffers)
{
  return deep_bounded_work_ && deep_bounded_work_->set_tile_beauty(buffers);
}

void PathTraceWorkGPU::set_deep_beauty_denoised(const bool value)
{
  if (deep_gpu_frame_work_) deep_gpu_frame_work_->set_beauty_denoised(value);
}

bool PathTraceWorkGPU::finish_native_capture(bool complete)
{
  if (!deep_native_domain_active_) {
    return !deep_frame_capture_requested_;
  }
  const auto abort = [&](const string &reason, const bool cancelled = false) {
    if (cancelled)
      deep_frame_capture_consumer_->cancel_native_capture(deep_native_domain_.id);
    else
      deep_frame_capture_consumer_->abort_native_capture(deep_native_domain_.id, reason);
    deep_native_domain_active_ = false;
    release_native_capture_buffers();
    if (!device_->have_error()) {
      device_->const_copy_to(
          "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    }
    return false;
  };
  complete &= !is_cancel_requested() && !device_->have_error();
  if (complete) {
    complete = update_queue_counter_and_cache();
  }
  if (complete) {
    const IntegratorQueueCounter &counter = *integrator_queue_counter_.data();
    for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++i) {
      complete &= counter.num_queued[i] == 0;
    }
    complete &= !work_tile_scheduler_.has_remaining_work();
    deep_native_domain_.camera_reserved = counter.deep_native_camera_reserved;
    deep_native_domain_.camera_written = counter.deep_native_camera_written;
    deep_native_domain_.homogeneous_reserved =
        uint64_t(integrator_state_gpu_.deep_native_homogeneous_base) +
        counter.deep_native_homogeneous_reserved;
    deep_native_domain_.homogeneous_written =
        uint64_t(integrator_state_gpu_.deep_native_homogeneous_base) +
        counter.deep_native_homogeneous_written;
    deep_native_domain_.emission_calls = counter.deep_native_emission_calls;
    deep_native_domain_.point_reserved = uint64_t(integrator_state_gpu_.deep_native_point_base) +
                                         counter.deep_native_point_reserved;
    deep_native_domain_.point_written = uint64_t(integrator_state_gpu_.deep_native_point_base) +
                                        counter.deep_native_point_written;
    deep_native_domain_.root_reserved = uint64_t(integrator_state_gpu_.deep_native_root_base) +
                                        counter.deep_native_root_reserved;
    deep_native_domain_.root_written = uint64_t(integrator_state_gpu_.deep_native_root_base) +
                                       counter.deep_native_root_written;
    deep_native_domain_.expression_source_reserved =
        uint64_t(integrator_state_gpu_.deep_native_expression_source_base) +
        counter.deep_native_expression_source_reserved;
    deep_native_domain_.expression_source_written =
        uint64_t(integrator_state_gpu_.deep_native_expression_source_base) +
        counter.deep_native_expression_source_written;
    deep_native_domain_.expression_node_reserved =
        uint64_t(integrator_state_gpu_.deep_native_expression_node_base) +
        counter.deep_native_expression_node_reserved;
    deep_native_domain_.expression_node_written =
        uint64_t(integrator_state_gpu_.deep_native_expression_node_base) +
        counter.deep_native_expression_node_written;
    deep_native_domain_.expression_call_reserved =
        uint64_t(integrator_state_gpu_.deep_native_expression_call_base) +
        counter.deep_native_expression_call_reserved;
    deep_native_domain_.expression_call_written =
        uint64_t(integrator_state_gpu_.deep_native_expression_call_base) +
        counter.deep_native_expression_call_written;
    deep_native_domain_.auxiliary_film_calls = counter.deep_native_auxiliary_film_calls;
    deep_native_domain_.volume_yields = counter.deep_native_volume_yields;
    deep_native_domain_.volume_retries = counter.deep_native_volume_retries;
    deep_native_domain_.volume_pending = counter.deep_native_volume_pending;
    deep_native_domain_.error_bits = counter.deep_native_capture_error;
    if (deep_native_domain_.volume_yields)
      std::fprintf(
          stderr,
          "DEEP_FRAME_VOLUME_CONTINUATION domain=%llu yields=%llu retries=%llu slot_bytes=%u\n",
          static_cast<unsigned long long>(deep_native_domain_.id),
          static_cast<unsigned long long>(deep_native_domain_.volume_yields),
          static_cast<unsigned long long>(deep_native_domain_.volume_retries),
          DEEP_NATIVE_VOLUME_CHECKPOINT_BYTES);
    complete &=
        !counter.deep_native_capture_error && !counter.deep_native_volume_pending &&
        counter.deep_native_camera_reserved == counter.deep_native_camera_written &&
        counter.deep_native_homogeneous_reserved == counter.deep_native_homogeneous_written &&
        counter.deep_native_camera_written <= deep_native_cameras_.size() &&
        counter.deep_native_homogeneous_written <= deep_native_homogeneous_.size() &&
        counter.deep_native_point_reserved == counter.deep_native_point_written &&
        counter.deep_native_point_written <= deep_native_points_.size() &&
        counter.deep_native_root_reserved == counter.deep_native_root_written &&
        counter.deep_native_root_written <= deep_native_roots_.size() &&
        counter.deep_native_expression_source_reserved ==
            counter.deep_native_expression_source_written &&
        counter.deep_native_expression_source_written <= deep_native_expression_sources_.size() &&
        counter.deep_native_expression_node_reserved ==
            counter.deep_native_expression_node_written &&
        counter.deep_native_expression_node_written <= deep_native_expression_nodes_.size() &&
        counter.deep_native_expression_call_reserved ==
            counter.deep_native_expression_call_written &&
        counter.deep_native_expression_call_written <= deep_native_expression_calls_.size();
  }
  if (!complete) {
    /* A cancellation/error still orders previously submitted producers before their allocation
     * is released. Fatal-context teardown is handled by the native Device allocator. */
    const bool joined = queue_->synchronize_for_state_rebind();
    if (joined && !device_->have_error())
      integrator_queue_counter_.copy_from_device();
    const auto &counter = *integrator_queue_counter_.data();
    int queued = 0;
    for (int kernel = 0; kernel < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++kernel)
      queued += counter.num_queued[kernel];
    std::fprintf(stderr,
                 "DEEP_NATIVE_ABORT domain=%llu cancel=%d joined=%d pending=%u queued=%d "
                 "resident_roots=%u error_bits=%u\n",
                 static_cast<unsigned long long>(deep_native_domain_.id),
                 int(is_cancel_requested()),
                 int(joined),
                 counter.deep_native_volume_pending,
                 queued,
                 integrator_state_gpu_.deep_native_root_resident_count,
                 counter.deep_native_capture_error);
    if (!is_cancel_requested() && !device_->have_error()) {
      device_->set_error("Native Deep Frame domain cannot publish incomplete queues or counters");
    }
    const bool cancelled = is_cancel_requested() && joined && !device_->have_error() &&
                           !counter.deep_native_capture_error;
    return abort("Native Deep Frame domain cancelled, failed, overflowed or did not join",
                 cancelled);
  }
  try {
    deep_native_domain_.main_shadow_mnee_joined = true;
    const size_t camera_count = size_t(deep_native_domain_.camera_written);
    const size_t segment_count = integrator_queue_counter_.data()->deep_native_homogeneous_written;
    const size_t point_count = integrator_queue_counter_.data()->deep_native_point_written;
    const size_t root_count = integrator_queue_counter_.data()->deep_native_root_written;
    if (camera_count) {
      deep_native_cameras_.copy_from_device(0, camera_count, 1);
    }
    if (root_count) {
      deep_native_roots_.copy_from_device(0, root_count, 1);
    }
    if (segment_count) {
      deep_native_homogeneous_.copy_from_device(0, segment_count, 1);
    }
    if (point_count) {
      deep_native_points_.copy_from_device(0, point_count, 1);
    }
    if (device_->have_error()) {
      return abort("Native Deep Frame capture readback failed");
    }
    DeepNativeCaptureBatch batch(*deep_native_budget_);
    batch.domain = deep_native_domain_;
    if (camera_count) {
      batch.cameras.assign(deep_native_cameras_.data(),
                           deep_native_cameras_.data() + camera_count);
    }
    if (!deep_native_control_journal_)
      return abort("Missing native control journal at domain join");
    DeepControlPage control;
    control.root_base = integrator_state_gpu_.deep_native_root_base;
    control.homogeneous_base = integrator_state_gpu_.deep_native_homogeneous_base;
    control.point_base = integrator_state_gpu_.deep_native_point_base;
    control.domain = batch.domain.id;
    control.device_generation = batch.domain.device_generation;
    control.layout_generation = batch.domain.layout_generation;
    control.roots = {deep_native_roots_.data(), root_count};
    control.homogeneous = {deep_native_homogeneous_.data(), segment_count};
    control.points = {deep_native_points_.data(), point_count};
    if (const auto status = deep_native_control_journal_->append(control); !status) {
      device_->set_error(status.message);
      return abort(status.message);
    }
    if (const auto status = deep_native_control_journal_->release_to(batch); !status) {
      device_->set_error(status.message);
      return abort(status.message);
    }
    if (!deep_native_expression_journal_) {
      device_->set_error("Missing native expression journal at domain join");
      return abort("Missing native expression journal");
    }
    if (const auto status = deep_native_expression_journal_->release_to(batch); !status) {
      device_->set_error(status.message);
      return abort(status.message);
    }
    DeepNativeCaptureVector<uint32_t> segments_per_camera{
        DeepBudgetAllocator<uint32_t>(*deep_native_budget_)};
    segments_per_camera.resize(camera_count, 0);
    for (const auto &segment : batch.homogeneous) {
      if (segment.camera_index < camera_count) {
        ++segments_per_camera[segment.camera_index];
      }
    }
    for (const auto &root : batch.roots) {
      if (root.camera_index < camera_count) {
        ++segments_per_camera[root.camera_index];
      }
    }
    for (const auto &source : batch.expression_sources) {
      if (source.camera_index < camera_count)
        ++segments_per_camera[source.camera_index];
    }
    size_t retried_cameras = 0;
    for (size_t i = 0; i < camera_count; ++i) {
      auto &camera = batch.cameras[i];
      retried_cameras += bool(camera.flags & DEEP_NATIVE_CAMERA_CACHE_RETRY);
      camera.flags |= DEEP_NATIVE_CAMERA_COMPLETE;
      if (camera.disposition == DEEP_NATIVE_CAMERA_RAY && !segments_per_camera[i]) {
        camera.disposition = DEEP_NATIVE_CAMERA_CLEAR_TRANSPARENT;
      }
    }
    if (const char *verify = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")) {
      if (std::strcmp(verify, "1") == 0)
        std::fprintf(stderr,
                     "DEEP_FRAME_CAMERA_RETRY domain=%llu cameras=%u retried=%zu\n",
                     static_cast<unsigned long long>(batch.domain.id),
                     camera_count,
                     retried_cameras);
    }
    release_native_capture_buffers();
    device_->const_copy_to(
        "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
    string error;
    if (!deep_native_capture_validate(batch, error, batch.domain.require_full_optical)) {
      device_->set_error(error);
      return abort(error);
    }
    {
      auto replay = std::allocate_shared<DeepOpticalWork>(
          DeepBudgetAllocator<DeepOpticalWork>(*deep_native_budget_),
          device_,
          queue_.get(),
          *deep_native_budget_,
          *deep_native_device_budget_);
      if (!replay->replay_owned(device_scene_,
                                integrator_state_gpu_,
                                batch,
                                cancel_requested_flag_,
                                buffers_.get(),
                                true,
                                [this]() { return poll_cancel_requested(); }) ||
          !deep_native_capture_validate(batch, error))
      {
        if (is_cancel_requested() && !device_->have_error())
          return abort("Native domain cancelled during owned Optical replay");
        if (!device_->have_error())
          device_->set_error(error);
        return abort(error.empty() ? "Owned Optical replay did not qualify the native domain" :
                                     error);
      }
    }
    deep_native_domain_active_ = false;
    auto *reference = deep_frame_capture_consumer_->reference_capture();
    if (!reference || !reference->complete_native_capture(std::move(batch))) {
      device_->set_error("Native Deep Frame consumer rejected a completed domain");
      return false;
    }
    return true;
  }
  catch (const std::bad_alloc &) {
    device_->set_error("Native Deep Frame readback/validation exceeds the shared memory quota");
    return abort("Native Deep Frame host packet allocation failed");
  }
}

void PathTraceWorkGPU::alloc_integrator_soa()
{
  /* IntegrateState allocated as structure of arrays. */

  /* Check if we already allocated memory for the required features.
   * Note that both disabling and enabling features may require memory
   * allocations, so we check for equality. */
  const int requested_volume_stack_size = device_scene_->data.volume_stack_size;
  const uint kernel_features = device_scene_->data.kernel_features;
  const bool use_shadow_color = device_scene_->data.integrator.light_flags &
                                KERNEL_INTEGRATOR_SHADOW_COLOR;
  if (integrator_state_soa_kernel_features_ == kernel_features &&
      integrator_state_soa_use_shadow_color_ == use_shadow_color &&
      integrator_state_soa_volume_stack_size_ >= requested_volume_stack_size)
  {
    return;
  }
  deep_bounded_slots_empty_ = false;
  deep_bounded_uploaded_state_.reset();
  integrator_state_soa_kernel_features_ = kernel_features;
  integrator_state_soa_use_shadow_color_ = use_shadow_color;
  integrator_state_soa_volume_stack_size_ = max(integrator_state_soa_volume_stack_size_,
                                                requested_volume_stack_size);

  /* Determine the number of path states. Deferring this for as long as possible allows the
   * back-end to make better decisions about memory availability. */
  if (max_num_paths_ == 0) {
    const size_t single_state_size = estimate_single_state_size(kernel_features, use_shadow_color);

    max_num_paths_ = queue_->num_concurrent_states(single_state_size);
    min_num_active_main_paths_ = queue_->num_concurrent_busy_states(single_state_size);

    /* Limit number of active paths to the half of the overall state. This is due to the logic in
     * the path compaction which relies on the fact that regeneration does not happen sooner than
     * half of the states are available again. */
    min_num_active_main_paths_ = min(min_num_active_main_paths_, max_num_paths_ / 2);
  }

  /* Allocate a device only memory buffer before for each struct member, and then
   * write the pointers into a struct that resides in constant memory.
   *
   * TODO: store float3 in separate XYZ arrays. */
#define KERNEL_STRUCT_BEGIN(name) \
  for (int array_index = 0;; array_index++) {
#define KERNEL_STRUCT_MEMBER_IMPL(parent_struct, type, name, condition) \
  if ((condition) && \
      (std::strcmp(#name, "deep_frame_camera_index") != 0 && \
       std::strcmp(#name, "deep_frame_root_handle") != 0 && \
                    std::strncmp(#name, "deep_bounded_", 13) != 0) && \
      (integrator_state_gpu_.parent_struct.name == nullptr)) \
  { \
    string name_str = string_printf("%sintegrator_state_" #parent_struct "_" #name, \
                                    shadow ? "shadow_" : ""); \
    auto array = make_unique<device_only_memory<type>>(device_, name_str.c_str()); \
    array->alloc_to_device(max_num_paths_); \
    memcpy(&integrator_state_gpu_.parent_struct.name, \
           &array->device_pointer, \
           sizeof(array->device_pointer)); \
    integrator_state_soa_.emplace_back(std::move(array)); \
  }
#define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
  KERNEL_STRUCT_MEMBER_IMPL( \
      parent_struct, type, name, KernelFeatureRequest(feature).test(kernel_features))
#define KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR(parent_struct, type, name, feature) \
  KERNEL_STRUCT_MEMBER_IMPL( \
      parent_struct, \
      type, \
      name, \
      shadow_state_unshadowed_throughput_is_needed(kernel_features, use_shadow_color))
#ifdef __INTEGRATOR_GPU_PACKED_STATE__
#  define KERNEL_STRUCT_MEMBER_PACKED(parent_struct, type, name, feature) \
    if ((KernelFeatureRequest(feature).test(kernel_features))) { \
      string name_str = string_printf("%sintegrator_state_" #parent_struct "_" #name, \
                                      shadow ? "shadow_" : ""); \
      LOG_TRACE << "Skipping " << name_str \
                << " -- data is packed inside integrator_state_" #parent_struct "_packed"; \
    }
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) \
    KERNEL_STRUCT_BEGIN(parent_struct) \
    KERNEL_STRUCT_MEMBER(parent_struct, packed_##parent_struct, packed, feature)
#else
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) KERNEL_STRUCT_BEGIN(parent_struct)
#endif

#define KERNEL_STRUCT_ARRAY_MEMBER(parent_struct, type, name, feature) \
  if ((KernelFeatureRequest(feature).test(kernel_features)) && \
      (integrator_state_gpu_.parent_struct[array_index].name == nullptr)) \
  { \
    string name_str = string_printf( \
        "%sintegrator_state_" #name "_%d", shadow ? "shadow_" : "", array_index); \
    auto array = make_unique<device_only_memory<type>>(device_, name_str.c_str()); \
    array->alloc_to_device(max_num_paths_); \
    memcpy(&integrator_state_gpu_.parent_struct[array_index].name, \
           &array->device_pointer, \
           sizeof(array->device_pointer)); \
    integrator_state_soa_.emplace_back(std::move(array)); \
  }
#define KERNEL_STRUCT_END(name) \
  (void)array_index; \
  break; \
  }
#define KERNEL_STRUCT_END_ARRAY(name, cpu_array_size, gpu_array_size) \
  if (array_index >= gpu_array_size - 1) { \
    break; \
  } \
  }
#define KERNEL_STRUCT_VOLUME_STACK_SIZE (integrator_state_soa_volume_stack_size_)

  bool shadow = false;
#include "kernel/integrator/state_template.h"
  shadow = true;
#include "kernel/integrator/shadow_state_template.h"

#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_IMPL
#undef KERNEL_STRUCT_MEMBER_RUNTIME_SHADOW_COLOR
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_VOLUME_STACK_SIZE

  ++deep_native_layout_generation_;

  if (LOG_IS_ON(LOG_LEVEL_TRACE)) {
    size_t total_soa_size = 0;
    for (auto &&soa_memory : integrator_state_soa_) {
      total_soa_size += soa_memory->memory_size();
    }

    LOG_TRACE << "GPU SoA state size: " << string_human_readable_size(total_soa_size);
  }
}

void PathTraceWorkGPU::alloc_integrator_queue()
{
  if (integrator_queue_counter_.size() == 0) {
    integrator_queue_counter_.alloc(1);
    integrator_queue_counter_.zero_to_device();
    integrator_queue_counter_.copy_from_device();
    integrator_state_gpu_.queue_counter = (IntegratorQueueCounter *)
                                              integrator_queue_counter_.device_pointer;
  }

  /* Allocate data for active path index arrays. */
  if (num_queued_paths_.size() == 0) {
    num_queued_paths_.alloc(1);
    num_queued_paths_.zero_to_device();
  }

  if (queued_paths_.size() == 0) {
    queued_paths_.alloc(max_num_paths_);
    /* TODO: this could be skip if we had a function to just allocate on device. */
    queued_paths_.zero_to_device();
  }
}

void PathTraceWorkGPU::alloc_integrator_sorting()
{
  num_sort_partitions_ = queue_->num_sort_partitions(max_num_paths_,
                                                     device_scene_->data.max_shaders);

  integrator_state_gpu_.sort_partition_divisor = (int)divide_up(max_num_paths_,
                                                                num_sort_partitions_);

  if (num_sort_partitions_ > 1 && queue_->supports_local_atomic_sort()) {
    /* Allocate array for partitioned shader sorting using local atomics. */
    const int num_offsets = (device_scene_->data.max_shaders + 1) * num_sort_partitions_;
    if (integrator_shader_sort_partition_key_offsets_.size() < num_offsets) {
      integrator_shader_sort_partition_key_offsets_.alloc(num_offsets);
      integrator_shader_sort_partition_key_offsets_.zero_to_device();
    }
    integrator_state_gpu_.sort_partition_key_offsets =
        (int *)integrator_shader_sort_partition_key_offsets_.device_pointer;
  }
  else {
    /* Allocate arrays for shader sorting. */
    const int sort_buckets = device_scene_->data.max_shaders * num_sort_partitions_;
    if (integrator_shader_sort_counter_.size() < sort_buckets) {
      integrator_shader_sort_counter_.alloc(sort_buckets);
      integrator_shader_sort_counter_.zero_to_device();
      integrator_state_gpu_.sort_key_counter[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE] =
          (int *)integrator_shader_sort_counter_.device_pointer;

      integrator_shader_sort_prefix_sum_.alloc(sort_buckets);
      integrator_shader_sort_prefix_sum_.zero_to_device();
    }

    if (device_scene_->data.kernel_features & KERNEL_FEATURE_NODE_RAYTRACE) {
      if (integrator_shader_raytrace_sort_counter_.size() < sort_buckets) {
        integrator_shader_raytrace_sort_counter_.alloc(sort_buckets);
        integrator_shader_raytrace_sort_counter_.zero_to_device();
        integrator_state_gpu_.sort_key_counter[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE] =
            (int *)integrator_shader_raytrace_sort_counter_.device_pointer;
      }
    }
  }
}

void PathTraceWorkGPU::alloc_integrator_path_split()
{
  if (integrator_next_shadow_path_index_.size() == 0) {
    integrator_next_shadow_path_index_.alloc(1);
    integrator_next_shadow_path_index_.zero_to_device();

    integrator_state_gpu_.next_shadow_path_index =
        (int *)integrator_next_shadow_path_index_.device_pointer;
  }

  if (integrator_next_main_path_index_.size() == 0) {
    integrator_next_main_path_index_.alloc(1);
    integrator_next_shadow_path_index_.data()[0] = 0;
    integrator_next_main_path_index_.zero_to_device();

    integrator_state_gpu_.next_main_path_index =
        (int *)integrator_next_main_path_index_.device_pointer;
  }
}

void PathTraceWorkGPU::alloc_work_memory()
{
  alloc_integrator_soa();
  alloc_integrator_queue();
  alloc_integrator_sorting();
  alloc_integrator_path_split();
  if (deep_bounded_work_ &&
      !deep_bounded_work_->bind_sources(uint32_t(max_num_paths_), integrator_state_gpu_))
  {
    return;
  }
  alloc_camera_receipts();
  alloc_surface_receipts();
  alloc_volume_radiance_receipts();
  alloc_volume_emission_spans();
}

void PathTraceWorkGPU::alloc_volume_emission_spans()
{
  if (!deep_volume_emission_spans_enabled_ || device_->have_error()) {
    return;
  }
  if (!deep_volume_radiance_receipts_enabled_ ||
      !integrator_state_gpu_.deep_volume_radiance_receipts)
  {
    device_->set_error("Volume emission spans require native Camera/Surface/Volume capture");
    return;
  }
  if (deep_volume_emission_spans_.size() == 0) {
    size_t capacity = (64 * 1024 * 1024) / sizeof(DeepVolumeEmissionSpan);
    if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_EMISSION_SPAN_CAPACITY")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long long parsed = std::strtoull(value, &end, 10);
      if (errno || end == value || *end || parsed == 0 || parsed > 16777216) {
        device_->set_error("Invalid Volume emission span budget (1..16777216)");
        return;
      }
      capacity = size_t(parsed);
    }
    size_t total = 0, available = 0;
    device_->get_device_memory_info(total, available);
    const size_t bytes = capacity * sizeof(DeepVolumeEmissionSpan);
    if (!total || bytes > available || available - bytes < 256 * 1024 * 1024) {
      device_->set_error("Volume emission span arena exceeds available device budget");
      return;
    }
    deep_volume_emission_spans_.alloc(capacity);
    deep_volume_emission_spans_.alloc_to_device();
    if (device_->have_error() || !deep_volume_emission_spans_.device_pointer ||
        deep_volume_emission_spans_.shared_pointer)
    {
      device_->set_error("Volume emission spans require native device allocation");
      return;
    }
    std::fprintf(stderr,
                 "DEEP_VOLUME_EMISSION_SPAN_ALLOCATION bytes=%zu capacity=%zu native=1\n",
                 bytes,
                 capacity);
  }
  integrator_state_gpu_.deep_volume_emission_spans =
      (DeepVolumeEmissionSpan *)deep_volume_emission_spans_.device_pointer;
  integrator_state_gpu_.deep_volume_emission_span_capacity = uint(
      deep_volume_emission_spans_.size());
}

void PathTraceWorkGPU::alloc_volume_radiance_receipts()
{
  if (!deep_volume_radiance_receipts_enabled_ || device_->have_error()) {
    return;
  }
  if (!deep_surface_receipts_enabled_ || !integrator_state_gpu_.deep_surface_receipts) {
    device_->set_error("Volume radiance receipts require Deep-off Camera/Surface capture");
    return;
  }
  if (device_scene_->data.integrator.has_shadow_catcher) {
    /* Catcher splits can route contributions into their separate film pass. Their composited
     * ownership is not yet represented by this limited Combined-radiance diagnostic. */
    device_->set_error("Volume radiance receipts do not yet support shadow-catcher film routing");
    return;
  }
  if (deep_volume_radiance_receipts_.size() == 0) {
    size_t capacity = (64 * 1024 * 1024) / sizeof(DeepVolumeRadianceReceipt);
    if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_RADIANCE_CAPACITY")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long long parsed = std::strtoull(value, &end, 10);
      if (errno || end == value || *end || parsed == 0 || parsed > 16777216) {
        device_->set_error("Invalid Volume radiance total record budget (1..16777216)");
        return;
      }
      capacity = size_t(parsed);
    }
    size_t total = 0, available = 0;
    device_->get_device_memory_info(total, available);
    const size_t bytes = capacity * sizeof(DeepVolumeRadianceReceipt);
    if (!total || bytes > available || available - bytes < 256 * 1024 * 1024) {
      device_->set_error("Volume radiance arena exceeds available device budget");
      return;
    }
    deep_volume_radiance_receipts_.alloc(capacity);
    deep_volume_radiance_receipts_.alloc_to_device();
    if (device_->have_error() || !deep_volume_radiance_receipts_.device_pointer ||
        deep_volume_radiance_receipts_.shared_pointer)
    {
      device_->set_error("Volume radiance receipts require native device allocation");
      return;
    }
    std::fprintf(stderr,
                 "DEEP_VOLUME_RADIANCE_ALLOCATION bytes=%zu capacity=%zu native=1\n",
                 bytes,
                 capacity);
  }
  integrator_state_gpu_.deep_volume_radiance_receipts =
      (DeepVolumeRadianceReceipt *)deep_volume_radiance_receipts_.device_pointer;
  integrator_state_gpu_.deep_volume_radiance_receipt_capacity = uint(
      deep_volume_radiance_receipts_.size());
}

void PathTraceWorkGPU::alloc_surface_receipts()
{
  if (!deep_surface_receipts_enabled_ || device_->have_error()) {
    return;
  }
  if (!deep_camera_receipts_enabled_ || !integrator_state_gpu_.deep_camera_receipts) {
    device_->set_error("Surface receipts require native Deep-off camera receipt capture");
    return;
  }
  if (deep_surface_receipts_.size() == 0) {
    /* A domain can rotate native state batches and visit arbitrarily many transparent layers.
     * This total arena budget is independent of max_num_paths; overflow fails the whole domain. */
    size_t capacity = (256 * 1024 * 1024) / sizeof(DeepSurfaceReceipt);
    if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_SURFACE_RECEIPT_CAPACITY")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long long parsed = std::strtoull(value, &end, 10);
      if (errno || end == value || *end || parsed == 0 || parsed > 16 * 1024 * 1024) {
        device_->set_error("Invalid Surface receipt total record budget (1..16777216)");
        return;
      }
      capacity = size_t(parsed);
    }
    size_t total = 0, available = 0;
    device_->get_device_memory_info(total, available);
    const size_t bytes = capacity * sizeof(DeepSurfaceReceipt);
    if (!total || bytes > available || available - bytes < 256 * 1024 * 1024) {
      device_->set_error("Surface receipt arena exceeds available device budget");
      return;
    }
    deep_surface_receipts_.alloc(capacity);
    deep_surface_receipts_.alloc_to_device();
    if (device_->have_error() || !deep_surface_receipts_.device_pointer ||
        deep_surface_receipts_.shared_pointer)
    {
      device_->set_error(
          "Surface receipts require native device allocation without mapped fallback");
      return;
    }
    std::fprintf(stderr,
                 "DEEP_SURFACE_RECEIPT_ALLOCATION bytes=%zu capacity=%zu native=1\n",
                 bytes,
                 capacity);
  }
  integrator_state_gpu_.deep_surface_receipts = (DeepSurfaceReceipt *)
                                                    deep_surface_receipts_.device_pointer;
  integrator_state_gpu_.deep_surface_receipt_capacity = uint(deep_surface_receipts_.size());
}

void PathTraceWorkGPU::alloc_camera_receipts()
{
  if (!deep_camera_receipts_enabled_ || device_->have_error()) {
    return;
  }
  const char *root = getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPT_ROOT");
  if (!root || !path_is_directory(root) ||
      (device_->info.type != DEVICE_CUDA && device_->info.type != DEVICE_OPTIX) ||
      device_scene_->data.film.use_deep_output || device_scene_->data.bake.use ||
      device_scene_->data.cam.type == CAMERA_CUSTOM ||
      (device_scene_->data.kernel_features & KERNEL_FEATURE_OSL_SHADING))
  {
    device_->set_error(
        "Camera receipts require an existing exclusive output root, native "
        "CUDA/OptiX camera and Deep-off demand (bake/custom/OSL not supported)");
    return;
  }
  if (deep_camera_receipts_.size() == 0) {
    /* Capacity follows the native admission bound; do not reduce Beauty occupancy for Deep.
     *
     * Refuse insufficient memory instead of allowing fallback to mapped host allocation. */
    const size_t bytes = size_t(max_num_paths_) * sizeof(DeepCameraReceipt);
    size_t total = 0, available = 0;
    device_->get_device_memory_info(total, available);
    if (max_num_paths_ <= 0 || !total || bytes > available ||
        available - bytes < 256 * 1024 * 1024)
    {
      device_->set_error("Camera receipt native-capacity buffer exceeds available device budget");
      return;
    }
    deep_camera_receipts_.alloc(max_num_paths_);
    deep_camera_receipts_.alloc_to_device();
    if (device_->have_error() || !deep_camera_receipts_.device_pointer ||
        deep_camera_receipts_.shared_pointer)
    {
      device_->set_error(
          "Camera receipts require native device allocation without mapped fallback");
      return;
    }
    std::fprintf(stderr,
                 "DEEP_CAMERA_RECEIPT_ALLOCATION bytes=%zu available_before=%zu "
                 "native=1 path_capacity=%d\n",
                 bytes,
                 available,
                 max_num_paths_);
  }
  integrator_state_gpu_.deep_camera_receipts = (DeepCameraReceipt *)
                                                   deep_camera_receipts_.device_pointer;
  integrator_state_gpu_.deep_camera_receipt_capacity = uint(deep_camera_receipts_.size());
}

bool PathTraceWorkGPU::begin_camera_receipts(const int start_sample,
                                             const int samples_num,
                                             const int sample_offset)
{
  if (!deep_camera_receipts_enabled_) {
    return true;
  }
  if (device_->have_error() || !integrator_state_gpu_.deep_camera_receipts) {
    return false;
  }
  /* This diagnostic domain ID is unique within this process. The external manifest binds
   *
   * scene/runtime identity; it is not yet a production SceneEpoch or frame-completion token. */
  static std::atomic<uint64_t> next_domain{0};
  const uint64_t domain = next_domain.fetch_add(1);
  const auto &p = effective_buffer_params_;
  uint32_t header[32] = {0x44435248, 1, sizeof(DeepCameraReceipt), uint32_t(max_num_paths_)};
  header[4] = uint32_t(domain);
  header[5] = uint32_t(domain >> 32);
  header[6] = uint32_t(device_->info.type);
  header[7] = uint32_t(device_->info.num);
  header[8] = p.width;
  header[9] = p.height;
  header[10] = p.full_x;
  header[11] = p.full_y;
  header[12] = p.full_width;
  header[13] = p.full_height;
  header[14] = p.offset;
  header[15] = p.stride;
  header[16] = p.window_x;
  header[17] = p.window_y;
  header[18] = p.window_width;
  header[19] = p.window_height;
  header[20] = start_sample;
  header[21] = samples_num;
  header[22] = sample_offset;
  header[23] = device_scene_->data.integrator.seed;
  header[24] = device_scene_->data.integrator.sampling_pattern;
  header[25] = device_scene_->data.cam.width;
  header[26] = device_scene_->data.cam.height;
  header[27] = device_scene_->data.cam.type;
  header[28] = device_scene_->data.kernel_features;
  header[29] = PRNG_BOUNCE_NUM;
  const string path = path_join(
      getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPT_ROOT"),
      string_printf("camera-domain-%llu.receipts", static_cast<unsigned long long>(domain)));
  if (!deep_camera_receipt_writer_.begin(path, header)) {
    device_->set_error("Cannot exclusively create camera receipt domain: " + path);
    return false;
  }
  if (deep_surface_receipts_enabled_) {
    header[0] = 0x44535248;
    header[2] = sizeof(DeepSurfaceReceipt);
    header[3] = uint32_t(deep_surface_receipts_.size());
    const string surface_path = path_join(
        getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPT_ROOT"),
        string_printf("surface-domain-%llu.receipts", static_cast<unsigned long long>(domain)));
    if (!integrator_state_gpu_.deep_surface_receipts ||
        !deep_surface_receipt_writer_.begin(surface_path, header))
    {
      deep_camera_receipt_writer_.finish(false);
      device_->set_error("Cannot exclusively create Surface receipt domain: " + surface_path);
      return false;
    }
    std::fprintf(stderr,
                 "DEEP_SURFACE_RECEIPTS domain=%llu path=%s\n",
                 static_cast<unsigned long long>(domain),
                 surface_path.c_str());
  }
  if (deep_volume_radiance_receipts_enabled_) {
    header[0] = 0x44565248;
    header[2] = sizeof(DeepVolumeRadianceReceipt);
    header[3] = uint32_t(deep_volume_radiance_receipts_.size());
    const string volume_path = path_join(getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPT_ROOT"),
                                         string_printf("volume-radiance-domain-%llu.receipts",
                                                       static_cast<unsigned long long>(domain)));
    if (!integrator_state_gpu_.deep_volume_radiance_receipts ||
        !deep_volume_radiance_receipt_writer_.begin(volume_path, header))
    {
      deep_surface_receipt_writer_.finish(false);
      deep_camera_receipt_writer_.finish(false);
      device_->set_error("Cannot exclusively create Volume radiance domain: " + volume_path);
      return false;
    }
    std::fprintf(stderr,
                 "DEEP_VOLUME_RADIANCE_RECEIPTS domain=%llu path=%s\n",
                 static_cast<unsigned long long>(domain),
                 volume_path.c_str());
  }
  if (deep_volume_emission_spans_enabled_) {
    header[0] = 0x44455248;
    header[2] = sizeof(DeepVolumeEmissionSpan);
    header[3] = uint32_t(deep_volume_emission_spans_.size());
    const string span_path = path_join(getenv("CYCLES_EXPERIMENTAL_DEEP_CAMERA_RECEIPT_ROOT"),
                                       string_printf("emission-span-domain-%llu.receipts",
                                                     static_cast<unsigned long long>(domain)));
    if (!integrator_state_gpu_.deep_volume_emission_spans ||
        !deep_volume_emission_span_writer_.begin(span_path, header))
    {
      deep_volume_radiance_receipt_writer_.finish(false);
      deep_surface_receipt_writer_.finish(false);
      deep_camera_receipt_writer_.finish(false);
      device_->set_error("Cannot exclusively create Volume emission span domain: " + span_path);
      return false;
    }
    std::fprintf(stderr,
                 "DEEP_VOLUME_EMISSION_SPANS domain=%llu path=%s\n",
                 static_cast<unsigned long long>(domain),
                 span_path.c_str());
  }
  std::fprintf(stderr,
               "DEEP_CAMERA_RECEIPTS domain=%llu capacity=%d record_bytes=%zu path=%s\n",
               static_cast<unsigned long long>(domain),
               max_num_paths_,
               sizeof(DeepCameraReceipt),
               path.c_str());
  return true;
}

bool PathTraceWorkGPU::drain_camera_receipts()
{
  if (!deep_camera_receipts_enabled_) {
    return true;
  }
  IntegratorQueueCounter *counter = integrator_queue_counter_.data();
  const uint count = counter->deep_camera_receipt_count;
  if (counter->deep_camera_receipt_overflow || count > deep_camera_receipts_.size()) {
    device_->set_error("Camera receipt append overflow: population is incomplete");
    return false;
  }
  if (count) {
    /* The producer stream has already completed. This exact prefix transfer is synchronous;
     *
     * the buffer/counter cannot be reused until the backing append has also completed. */
    deep_camera_receipts_.copy_from_device(0, count, 1);
    if (device_->have_error() ||
        !deep_camera_receipt_writer_.append(deep_camera_receipts_.data(), count))
    {
      device_->set_error("Camera receipt backing append failed; domain cannot be sealed");
      return false;
    }
    counter->deep_camera_receipt_count = 0;
    counter->deep_camera_receipt_overflow = 0;
    queue_->copy_to_device(integrator_queue_counter_);
  }
  return true;
}

bool PathTraceWorkGPU::finish_surface_receipts(bool complete)
{
  if (!deep_surface_receipts_enabled_) {
    return true;
  }
  if (complete) {
    /* All producer queues are synchronized and empty. Shadows cannot write these addresses
     * any longer. Do not drain/reset the arena at intermediate camera batch boundaries. */
    const uint count = integrator_queue_counter_.data()->deep_surface_receipt_count;
    if (count) {
      deep_surface_receipts_.copy_from_device(0, count, 1);
      complete = !device_->have_error() &&
                 deep_surface_receipt_writer_.append(deep_surface_receipts_.data(), count);
    }
  }
  const bool finished = deep_surface_receipt_writer_.finish(complete);
  std::fprintf(stderr,
               "DEEP_SURFACE_RECEIPTS_END complete=%d records=%llu\n",
               int(complete && finished),
               static_cast<unsigned long long>(deep_surface_receipt_writer_.count()));
  return complete && finished;
}

bool PathTraceWorkGPU::finish_volume_radiance_receipts(bool complete)
{
  if (!deep_volume_radiance_receipts_enabled_) {
    return true;
  }
  if (complete) {
    const uint count = integrator_queue_counter_.data()->deep_volume_radiance_receipt_count;
    if (count) {
      /* Stable addresses remain live through every native shadow and camera batch. */
      deep_volume_radiance_receipts_.copy_from_device(0, count, 1);
      complete = !device_->have_error();
      for (uint i = 0; complete && i < count; ++i) {
        const auto &receipt = deep_volume_radiance_receipts_.data()[i];
        for (int channel = 0; channel < 3; ++channel) {
          complete &= std::isfinite(receipt.emission[channel]) &&
                      std::isfinite(receipt.unowned_path[channel]) &&
                      std::isfinite(receipt.unowned_shadow[channel]) &&
                      std::isfinite(receipt.origin[channel]) &&
                      std::isfinite(receipt.direction[channel]);
        }
        complete &= std::isfinite(receipt.tmin) && std::isfinite(receipt.tmax) &&
                    std::isfinite(receipt.time) && receipt.tmin >= 0 &&
                    receipt.tmax >= receipt.tmin;
      }
      if (!complete && !device_->have_error()) {
        device_->set_error("Volume radiance contains non-finite totals or invalid Ray metadata");
      }
      complete = complete && deep_volume_radiance_receipt_writer_.append(
                                 deep_volume_radiance_receipts_.data(), count);
    }
  }
  const bool finished = deep_volume_radiance_receipt_writer_.finish(complete);
  std::fprintf(stderr,
               "DEEP_VOLUME_RADIANCE_RECEIPTS_END complete=%d records=%llu\n",
               int(complete && finished),
               static_cast<unsigned long long>(deep_volume_radiance_receipt_writer_.count()));
  return complete && finished;
}

bool PathTraceWorkGPU::finish_volume_emission_spans(bool complete)
{
  if (!deep_volume_emission_spans_enabled_) {
    return true;
  }
  if (complete) {
    const uint count = integrator_queue_counter_.data()->deep_volume_emission_span_count;
    if (count) {
      deep_volume_emission_spans_.copy_from_device(0, count, 1);
      complete = !device_->have_error() && deep_volume_emission_span_writer_.append(
                                               deep_volume_emission_spans_.data(), count);
    }
  }
  const bool finished = deep_volume_emission_span_writer_.finish(complete);
  std::fprintf(stderr,
               "DEEP_VOLUME_EMISSION_SPANS_END complete=%d records=%llu\n",
               int(complete && finished),
               static_cast<unsigned long long>(deep_volume_emission_span_writer_.count()));
  return complete && finished;
}

static_assert(uint32_t(~SHADER_MASK) == 0xdfc00000u,
              "Deep compact identity must preserve every native shader flag");

void PathTraceWorkGPU::init_execution()
{
  queue_->init_execution();

  if (deep_bounded_work_ && !device_->have_error()) {
    deep_bounded_work_->after_native_init();
    if (!deep_bounded_projection_) {
      deep_bounded_projection_ = 0.0f;
      const auto &cam = device_scene_->data.cam;
      if (cam.type == CAMERA_PERSPECTIVE) {
        const float3 origin = transform_perspective(&cam.rastertocamera, make_float3(0, 0, 0));
        const float3 dx = transform_perspective(&cam.rastertocamera, make_float3(1, 0, 0));
        const float3 dy = transform_perspective(&cam.rastertocamera, make_float3(0, 1, 0));
        if (origin.z != 0.0f && dx.z != 0.0f && dy.z != 0.0f) {
          const float kx = std::abs(dx.x / dx.z - origin.x / origin.z);
          const float ky = std::abs(dy.y / dy.z - origin.y / origin.z);
          const float scale = 0.5f * std::min(kx, ky);
          if (std::isfinite(scale) && scale > 0.0f) {
            deep_bounded_projection_ = scale;
          }
        }
      }
    }
    if (!deep_bounded_work_->configure(effective_big_tile_params_, effective_buffer_params_,
                                       uint32_t(max_num_paths_),
                                       deep_frame_capture_consumer_->bounded_max_samples(),
                                       device_scene_->data.background.transparent != 0,
                                       *deep_bounded_projection_, integrator_state_gpu_,
                                       device_scene_->objects.size() <= 8191 &&
                                           device_scene_->shaders.size() <= 512))
    {
      return;
    }
  }

  /* One rendering Work owns the integrator constants on each Device. The
   * frame/allocator boundaries above invalidate the snapshot. Native R6 packet
   * changes are published separately through its stable device control. */
  const bool cache = deep_bounded_work_ && deep_bounded_cache_state_;
  const bool cached = cache && !device_->have_error() && deep_bounded_uploaded_state_ &&
                      std::memcmp(&*deep_bounded_uploaded_state_, &integrator_state_gpu_,
                                    sizeof(integrator_state_gpu_)) == 0;
  if (const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_WORK_TIMINGS");
      deep_bounded_work_ && trace && std::strcmp(trace, "1") == 0)
  {
    std::fprintf(stderr, "DEEP_STATIC_BINDING bytes=%zu cache_active=%d uploaded=%d\n",
                 sizeof(integrator_state_gpu_), int(cache), int(!cached));
  }
  if (cached) return;
  device_->const_copy_to(
      "integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
  if (cache && !device_->have_error()) {
    deep_bounded_uploaded_state_.emplace();
    std::memcpy(&*deep_bounded_uploaded_state_, &integrator_state_gpu_,
                 sizeof(integrator_state_gpu_));
  }
  else {
    deep_bounded_uploaded_state_.reset();
  }
}

DeepBoundedDeviceAdmission PathTraceWorkGPU::deep_bounded_admission_snapshot() const
{
  auto result = deep_bounded_work_ ? deep_bounded_work_->admission_snapshot(
      uint32_t(deep_bounded_camera_limits().x),
      device_scene_->objects.size() <= 8191 && device_scene_->shaders.size() <= 512) :
      DeepBoundedDeviceAdmission{};
  size_t area = 0;
  if (result.pack_active && effective_buffer_params_.width > 0 && effective_buffer_params_.height > 0 &&
      deep_bounded_mul(size_t(effective_buffer_params_.width),
                       size_t(effective_buffer_params_.height), area) &&
      deep_bounded_mul(area, sizeof(uint32_t), area))
    result.adaptive_target = area;
  return result;
}

void PathTraceWorkGPU::set_deep_bounded_admission_limit(const uint32_t samples, const size_t bytes,
                                                         const size_t auxiliary_growth)
{
  if (deep_bounded_work_) deep_bounded_work_->set_admission_limit(samples, bytes, auxiliary_growth);
}

int2 PathTraceWorkGPU::deep_bounded_camera_limits() const
{
  const int states = max_num_paths_ ? max_num_paths_ : queue_->num_concurrent_states(0);
  const int busy = min_num_active_main_paths_ ? min_num_active_main_paths_ :
                                               queue_->num_concurrent_busy_states(0);
  return make_int2(states, std::min(states / 2, busy));
}

bool PathTraceWorkGPU::update_queue_counter_and_cache()
{
  const auto read_start = DeepDispatchClock::now();
  /* Copy stats from the device. */
  queue_->copy_from_device(integrator_queue_counter_);

  if (!queue_->synchronize()) {
    return false;
  }
  if (deep_gpu_frame_work_ && deep_native_domain_active_) {
    deep_gpu_queue_sync_wall_ += deep_dispatch_elapsed(read_start);
    deep_gpu_queue_updates_++;
  }

  if (deep_native_domain_active_) {
    const auto &counter = *integrator_queue_counter_.data();
    if (deep_native_validation_fatal_after_yield_ && counter.deep_native_volume_pending &&
        counter.deep_native_expression_source_written)
    {
      deep_native_validation_fatal_after_yield_ = false;
      std::fprintf(stderr,
                   "DEEP_NATIVE_FATAL_DISPOSAL domain=%llu pending=%u sources=%u "
                   "producer_joined=1 simulated_device_failure=1 actual_context_loss=0\n",
                   static_cast<unsigned long long>(deep_native_domain_.id),
                   counter.deep_native_volume_pending,
                   counter.deep_native_expression_source_written);
      device_->set_error("Native validation fatal device failure after checkpoint join");
      return false;
    }

    if (counter.deep_native_validation_seen != deep_native_validation_seen_) {
      deep_native_validation_seen_ = counter.deep_native_validation_seen;
      std::fprintf(stderr,
                   "DEEP_NATIVE_FAULT_WITNESS domain=%llu configured=%u seen=%u pending=%u\n",
                   static_cast<unsigned long long>(deep_native_domain_.id),
                   integrator_state_gpu_.deep_native_validation_faults,
                   counter.deep_native_validation_seen,
                   counter.deep_native_volume_pending);
    }
    const uint32_t denied = DEEP_NATIVE_FAULT_POINT_DENY | DEEP_NATIVE_FAULT_EXPRESSION_DENY |
                            DEEP_NATIVE_FAULT_HOMOGENEOUS_DENY;
    if (counter.deep_native_capture_error && (counter.deep_native_validation_seen & denied)) {
      /* This explicit fault fixture starts with no Combined contribution.
       * Read actual Film values before installing the sticky device error. */
      const int stride = buffers_->params.pass_stride;
      const int combined = device_scene_->data.film.pass_combined;
      if (stride <= 0 || combined < 0 || combined + 3 > stride || !buffers_->copy_from_device()) {
        device_->set_error("Cannot inspect actual Film after native reservation denial");
        return false;
      }
      const float *film = buffers_->buffer.data();
      const size_t pixels = buffers_->buffer.size() / size_t(stride);
      size_t nonzero = 0;
      for (size_t pixel = 0; pixel < pixels; ++pixel)
        for (int c = 0; c < 3; ++c)
          nonzero += film[pixel * stride + combined + c] != 0.0f;
      std::fprintf(stderr,
                   "DEEP_NATIVE_DENIAL_FILM domain=%llu pixels=%zu components=%zu nonzero=%zu "
                   "point_written=%u expression_written=%u\n",
                   static_cast<unsigned long long>(deep_native_domain_.id),
                   pixels,
                   pixels * 3,
                   nonzero,
                   counter.deep_native_point_written,
                   counter.deep_native_expression_call_written);
    }
    if (counter.deep_native_capture_error ||
        counter.deep_native_camera_reserved > deep_native_cameras_.size() ||
        counter.deep_native_homogeneous_reserved > deep_native_homogeneous_.size())
    {
      device_->set_error(string_printf("%s: error_bits=%u",
          counter.deep_native_capture_error & DEEP_NATIVE_ERROR_MASS_RANGE ?
              "NumericMassRange" : "Native Deep Frame capture hard failure", counter.deep_native_capture_error));
      return false;
    }
  }

  if (deep_native_domain_active_ && poll_cancel_requested())
    return false; /* Producer is joined; preserve pending identities until abort disposal. */

  if (deep_surface_receipts_enabled_) {
    const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
    if (counter->deep_surface_receipt_overflow ||
        counter->deep_surface_receipt_count > deep_surface_receipts_.size())
    {
      device_->set_error("Surface receipt overflow or invalid RGB reference: domain incomplete");
      return false;
    }
    if (counter->cache_miss) {
      device_->set_error(
          "Surface receipt cache retry requires an unimplemented transaction; "
          "diagnostic domain cannot be sealed");
      return false;
    }
  }

  if (deep_volume_radiance_receipts_enabled_) {
    const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
    if (counter->deep_volume_radiance_receipt_error ||
        counter->deep_volume_radiance_receipt_count > deep_volume_radiance_receipts_.size())
    {
      device_->set_error(string_printf("Volume radiance domain incomplete: error_bits=%u",
                                       counter->deep_volume_radiance_receipt_error));
      return false;
    }
  }
  if (deep_volume_emission_spans_enabled_) {
    const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
    if (counter->deep_volume_emission_span_error ||
        counter->deep_volume_emission_span_count > deep_volume_emission_spans_.size())
    {
      device_->set_error(string_printf("Volume emission span domain incomplete: error_bits=%u",
                                       counter->deep_volume_emission_span_error));
      return false;
    }
  }
  if (!drain_camera_receipts()) {
    return false;
  }

  /* Update image cache if needed. */
  /* TODO: If the number of kernels with cache misses is small compared to the total
   * number of queued kernels, we could try asynchronously updating the image cache
   * while continuing to work on the majority of states? */
  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();
  if (queue_counter->cache_miss) {
    LOG_DEBUG << "Image cache miss in GPU kernel, updating to load requested tiles";
    device_->image_load_requested_gpu(*queue_);
    queue_counter->cache_miss = 0;
    queue_->copy_to_device(integrator_queue_counter_);
  }

  if (deep_gpu_frame_work_)
    return flush_native_gpu_pages();
  return flush_native_root_page() && flush_native_control_page() && flush_native_expression_page();
}

bool PathTraceWorkGPU::set_deep_capture_group_flush(std::function<bool()> flush,
                                                    const int max_camera_paths,
                                                    const int max_ready_paths)
{
  deep_capture_group_flush_ = std::move(flush);
  deep_capture_group_limit_ = max_camera_paths;
  deep_capture_ready_limit_ = max_ready_paths;
  deep_capture_group_admitted_ = 0;
  return bool(deep_capture_group_flush_) && max_camera_paths > 0;
}

void PathTraceWorkGPU::render_samples(RenderStatistics &statistics,
                                      const int start_sample,
                                      const int samples_num,
                                      const int sample_offset)
{
  statistics.occupancy = 0;
  if (deep_bounded_work_) {
    if (deep_bounded_work_->has_owned_pixels()) {
      render_bounded_samples(statistics, start_sample, samples_num, sample_offset);
    }
    else if (!device_->have_error()) {
      deep_bounded_slots_empty_ = false;
      render_samples_batch(statistics, start_sample, samples_num, sample_offset);
    }
    return;
  }
  if (!deep_frame_capture_requested_) {
    render_samples_batch(statistics, start_sample, samples_num, sample_offset);
    return;
  }
  if (!deep_frame_capture_consumer_ || samples_num <= 0 || start_sample < 0 ||
      uint64_t(start_sample) + uint64_t(samples_num) > INT_MAX)
  {
    device_->set_error("Invalid bounded native sample range/consumer");
    return;
  }
  float occupancy = 0;
  for (int done = 0; done < samples_num;) {
    if (is_cancel_requested() || device_->have_error())
      break;
    const auto &host = deep_frame_capture_consumer_->memory_budget();
    const auto &device = deep_frame_capture_consumer_->device_memory_budget();
    const auto &data = device_scene_->data;
    const uint64_t pixels = uint64_t(effective_buffer_params_.width) *
                            effective_buffer_params_.height;
    /* Leave host space for joined packet validation/Optical and consumer caches.
     * No adaptive or guiding update occurs between these internal domains. */
    size_t memory_total = 0, memory_free = 0;
    device_->get_device_memory_info(memory_total, memory_free);
    const size_t quota_available = device.quota() - device.used();
    const size_t device_available = memory_total ? std::min(quota_available, memory_free) : quota_available;
    const int batch = deep_native_batch_samples(pixels,
                                                samples_num - done,
                                                size_t(max_num_paths_),
                                                data.integrator.has_shadow_catcher,
                                                device_scene_->volume_may_emit &&
                                                    (data.kernel_features & KERNEL_FEATURE_VOLUME),
                                                data.integrator.volume_ray_marching,
                                                deep_gpu_frame_work_ ? SIZE_MAX :
                                                    (host.quota() - host.used()) / 4,
                                                device_available /
                                                    (deep_gpu_frame_work_ ? 2 : 1),
                                                data.kernel_features & KERNEL_FEATURE_VOLUME,
                                                deep_frame_capture_consumer_->capture_demand().capture_rgb(),
                                                !deep_gpu_frame_work_, bool(deep_gpu_frame_work_), data.volume_stack_size);
    if (!batch) {
      device_->set_error("Native Deep quota cannot admit one sample of the bounded render tile");
      break;
    }
    std::fprintf(
        stderr,
        "DEEP_FRAME_NATIVE_BATCH start=%d samples=%d requested_start=%d requested_samples=%d "
        "offset=%d pixels=%llu host_available=%llu device_available=%llu\n",
        start_sample + done,
        batch,
        start_sample,
        samples_num,
        sample_offset,
        static_cast<unsigned long long>(pixels),
        static_cast<unsigned long long>(host.quota() - host.used()),
        static_cast<unsigned long long>(device.quota() - device.used()));
    RenderStatistics current = {};
    render_samples_batch(current, start_sample + done, batch, sample_offset);
    occupancy += current.occupancy * batch;
    done += batch;
  }
  statistics.occupancy = occupancy / samples_num;
}

void PathTraceWorkGPU::render_bounded_samples(RenderStatistics &statistics,
                                             const int start_sample,
                                             const int samples_num,
                                             const int sample_offset)
{
  auto dispatch = [&]() {
    const auto bounded_start = DeepDispatchClock::now();
    uint32_t packets = 0;
    if (samples_num <= 0 || device_->have_error()) {
      return;
    }
    if (start_sample < 0 || uint64_t(start_sample) + uint64_t(samples_num) > INT_MAX) {
      device_->set_error("Invalid native Work sample range for Deep");
      return;
    }
    const char *batch_shadow = getenv("CYCLES_TEST_DEEP_BATCH_SHADOWS");
    /* Defer small shadow waves within this packet for every mode. Physical
     * keeps its source buffers alive until the full native queue join below;
     * the separate cross-packet tail carry remains disabled for that mode. */
    prefer_bounded_main_queues_ = !batch_shadow || std::strcmp(batch_shadow, "0") != 0;
    SCOPED_DEFER(prefer_bounded_main_queues_ = false);
    const char *fill = getenv("CYCLES_TEST_DEEP_FILL_SHADOW_CAPACITY");
    bounded_shadow_queue_limit_ = fill && std::strcmp(fill, "0") == 0 ? 0 : max_num_paths_;
    BufferParams camera_params = effective_buffer_params_;
    if (!deep_bounded_work_->prepare_work(start_sample,
                                         uint32_t(samples_num),
                                         *buffers_,
                                         device_scene_->data.film.pass_adaptive_aux_buffer,
                                         camera_params))
    {
      return;
    }
    /* A larger scheduling unit keeps the admitted Physical packet coherent.
     * Admission and shadow-state limits still bound the actual scheduled work. */
    const char *wide = getenv("CYCLES_TEST_DEEP_WIDE_NATIVE_WORK");
    const bool wide_native_work = (!wide || std::strcmp(wide, "0") != 0);
    const int tile_divisor = wide_native_work ||
                                     device_->deep_bounded_policy() ==
                                         uint32_t(DeepBoundedPolicy::PHYSICAL) ?
                                 2 : 8;
    work_tile_scheduler_.set_max_num_path_states(max_num_paths_ / tile_divisor);
    work_tile_scheduler_.set_accelerated_rt(true);
    /* Termination and compaction clear occupied slots. Once every queue has
     * joined at zero, only counters need resetting for the next Work. */
    enqueue_reset(!deep_bounded_slots_empty_);
    deep_bounded_slots_empty_ = false;
    const char *overlap = getenv("CYCLES_TEST_DEEP_OVERLAP_SHADOWS");
    const bool overlap_shadow_tail = (!overlap || std::strcmp(overlap, "0") != 0) &&
        device_->deep_bounded_policy() != uint32_t(DeepBoundedPolicy::PHYSICAL);
    const char *camera_tail = getenv("CYCLES_TEST_DEEP_OVERLAP_INDIRECT");
    const bool overlap_indirect_tail = (!camera_tail || std::strcmp(camera_tail, "0") != 0) &&
        device_->deep_bounded_policy() != uint32_t(DeepBoundedPolicy::PHYSICAL);
    bool keep_shadow_queue = false;
    uint64_t busy = 0, iterations = 0;
    int done = 0;
    while (done < samples_num && !is_cancel_requested() && !device_->have_error()) {
      const uint32_t packet = deep_bounded_work_->packet_samples(uint32_t(samples_num - done));
      if (!deep_bounded_work_->begin_packet(start_sample + done,
                                            packet,
                                            sample_offset,
                                            integrator_state_gpu_,
                                            keep_shadow_queue,
                                            overlap_indirect_tail && int(packet) < samples_num - done))
      {
        break;
      }
      ++packets;
      work_tile_scheduler_.reset(camera_params, start_sample + done, packet, sample_offset,
                                 device_scene_->data.integrator.scrambling_distance);
      const bool admit_new_packet = keep_shadow_queue;
      if (!keep_shadow_queue) {
        max_active_main_path_index_ = 0;
        integrator_next_shadow_path_index_.data()[0] = 0;
      }
      keep_shadow_queue = false;
      bool closed = false;
      while (!is_cancel_requested() && !device_->have_error()) {
        bool finished = false;
        const bool camera = enqueue_work_tiles(finished, admit_new_packet);
        if (camera) {
          if (!update_queue_counter_and_cache()) {
            break;
          }
        }
        if (finished && !work_tile_scheduler_.has_remaining_work()) {
          closed = true;
          for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++i) {
            closed &= integrator_queue_counter_.data()->num_queued[i] == 0;
          }
          if (closed) {
            break;
          }
        }
        /* Simple and Alpha shadows write only native Film. The joined queue
         * counters prove all camera-root writers are gone before raw reuse. */
        const bool producer_closed = overlap_indirect_tail ?
            integrator_queue_counter_.data()->deep_bounded_camera_live == 0 :
            overlap_shadow_tail && num_active_main_paths_paths() == 0;
        if (producer_closed && int(packet) < samples_num - done &&
            !work_tile_scheduler_.has_remaining_work())
        {
          closed = true;
          keep_shadow_queue = true;
          break;
        }
        const bool path = enqueue_path_iteration();
        if (path && !update_queue_counter_and_cache()) {
          break;
        }
        if (!camera && !path) {
          device_->set_error("Deep packet has unfinished native work and no dispatchable queue");
          break;
        }
        busy += num_active_main_paths_paths();
        ++iterations;
      }
      if (!closed || is_cancel_requested() || device_->have_error() ||
          !deep_bounded_work_->accumulate_packet())
      {
        break;
      }
      done += int(packet);
    }
    /* Native counter joins close each producer. Same-stream ordering protects
     * packet AA and raw reuse without another host fence for each packet. */
    const bool joined = queue_->synchronize();
    deep_bounded_slots_empty_ = joined && done == samples_num && !is_cancel_requested() &&
                                !device_->have_error();
    deep_bounded_work_->after_native_init();
    if (iterations) {
      statistics.occupancy = float(double(busy) / double(iterations) / double(max_num_paths_));
    }
    if (const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_WORK_TIMINGS");
        trace && strcmp(trace, "1") == 0)
    {
      std::fprintf(stderr,
                   "DEEP_BOUNDED_WORK_TIMING start=%d samples=%d width=%d height=%d "
                   "packets=%u iterations=%llu total_wall=%.9f\n",
                   start_sample, samples_num, effective_buffer_params_.width,
                   effective_buffer_params_.height, packets,
                   static_cast<unsigned long long>(iterations), deep_dispatch_elapsed(bounded_start));
    }
  };
  queue_->run_with_deep_context(
      [](void *data) { (*static_cast<decltype(dispatch) *>(data))(); }, &dispatch);
}

void PathTraceWorkGPU::render_samples_batch(RenderStatistics &statistics,
                                            const int start_sample,
                                            const int samples_num,
                                            const int sample_offset)
{
  const auto batch_start = DeepDispatchClock::now();
  deep_gpu_queue_sync_wall_ = deep_gpu_page_flush_wall_ = 0;
  deep_gpu_queue_updates_ = deep_gpu_page_publications_ = 0;
  static std::mutex native_domain_mutex;
  std::unique_lock<std::mutex> native_domain_lease(native_domain_mutex, std::defer_lock);
  if (deep_frame_capture_requested_ && !native_domain_lease.try_lock()) {
    device_->set_error("Native Deep Frame capture does not permit overlapping device bindings");
    return;
  }
  if (!begin_native_capture(start_sample, samples_num, sample_offset)) {
    return;
  }
  /* Device constants are shared by queues. Hold the diagnostic lease across Beauty production,
   * replay and restoration; reject multi-work and overlapping domains before any GPU dispatch. */
  static std::mutex optical_domain_mutex;
  std::unique_lock<std::mutex> optical_domain_lease(optical_domain_mutex, std::defer_lock);
  if (deep_independent_optical_enabled_) {
    if (!deep_camera_receipts_enabled_ || !deep_surface_receipts_enabled_ ||
        has_multiple_works() || !optical_domain_lease.try_lock())
    {
      device_->set_error("Independent Optical requires one non-overlapping Camera/Surface work");
      return;
    }
  }
  if (!begin_camera_receipts(start_sample, samples_num, sample_offset)) {
    return;
  }
  /* Limit number of states for the tile and rely on a greedy scheduling of tiles. This allows to
   * add more work (because tiles are smaller, so there is higher chance that more paths will
   * become busy after adding new tiles). This is especially important for the shadow catcher which
   * schedules work in halves of available number of paths. */
  work_tile_scheduler_.set_max_num_path_states(
      deep_capture_group_flush_ ? min(max_num_paths_ / 8, deep_capture_group_limit_) :
                                  max_num_paths_ / 8);
  work_tile_scheduler_.set_accelerated_rt(
      (device_->get_bvh_layout_mask(device_scene_->data.kernel_features) & BVH_LAYOUT_OPTIX) != 0);
  work_tile_scheduler_.reset(effective_buffer_params_,
                             start_sample,
                             samples_num,
                             sample_offset,
                             device_scene_->data.integrator.scrambling_distance);

  enqueue_reset();

  const auto option_enabled = [](const char *name) {
    const char *value = getenv(name);
    return value && std::strcmp(value, "1") == 0;
  };
  deep_optical_wavefront_ = device_scene_->data.film.use_deep_output &&
                            !(device_scene_->data.kernel_features & KERNEL_FEATURE_OSL_SHADING) &&
                            (device_->info.type == DEVICE_CUDA ||
                             device_->info.type == DEVICE_OPTIX) &&
                            option_enabled("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE") &&
                            option_enabled("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_ONLY_KERNEL") &&
                            option_enabled("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WARP_KERNEL") &&
                            option_enabled("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTICAL_WAVEFRONT");
  deep_optical_phase_ = deep_optical_wavefront_;
  deep_beauty_coalesce_ = deep_optical_wavefront_ && deep_capture_group_flush_ &&
                          option_enabled("CYCLES_EXPERIMENTAL_DEEP_GPU_BEAUTY_COALESCE");
  bool coalesced_beauty_inflight = false;
  deep_capture_group_admitted_ = 0;

  int num_iterations = 0;
  uint64_t num_busy_accum = 0;
  bool camera_domain_complete = false;
  const double setup_wall = deep_dispatch_elapsed(batch_start);
  const auto loop_start = DeepDispatchClock::now();

  /* TODO: set a hard limit in case of undetected kernel failures? */
  while (true) {
    if (deep_optical_wavefront_) {
      const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
      int pending = 0;
      for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; i++) {
        pending += counter->num_queued[i];
      }
      if (pending == 0) {
        /* Both optical and restored Beauty states are dead. Their surface references can no
         * longer mutate the append pool. The counter read has completed this producer queue. */
        if (deep_capture_group_flush_ &&
            (deep_capture_group_admitted_ || coalesced_beauty_inflight))
        {
          if (!deep_capture_group_flush_())
            break;
          if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE"))
            std::fprintf(stderr,
                         "DEEP_CAPTURE_GROUP cameras=%d limit=%d\n",
                         deep_capture_group_admitted_,
                         deep_capture_group_limit_);
          deep_capture_group_admitted_ = 0;
          coalesced_beauty_inflight = false;
        }
        deep_optical_phase_ = true;
      }
      else if (deep_optical_phase_ &&
               pending == counter->num_queued[DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY])
      {
        if (deep_beauty_coalesce_) {
          /* Optical completion marks its last raw record before path_state_init_integrator
           * clears deep_surface_sample_idx and restores the original ray into READY. These
           * states hold no append-pool indices. Drain/freeze only the optical records here,
           * then retain the restored states while admitting another bounded camera group. */
          if (deep_capture_group_admitted_) {
            if (!deep_capture_group_flush_())
              break;
            if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE"))
              std::fprintf(stderr,
                           "DEEP_CAPTURE_GROUP cameras=%d limit=%d ready=%d optical_only=1\n",
                           deep_capture_group_admitted_,
                           deep_capture_group_limit_,
                           pending);
            deep_capture_group_admitted_ = 0;
          }
        }
        else {
          /* Every optical traversal is complete. Release their restored Beauty states before
           * allowing another group of new camera paths. */
          deep_optical_phase_ = false;
        }
      }
    }
    /* Enqueue work from the scheduler, on start or when there are not enough
     * paths to keep the device occupied. */
    bool finished = false;
    if (enqueue_work_tiles(finished)) {
      if (!update_queue_counter_and_cache()) {
        break; /* Stop on error. */
      }
    }
    else if (deep_beauty_coalesce_ && deep_optical_phase_) {
      const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
      const int ready = counter->num_queued[DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY];
      int pending = 0;
      for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; i++)
        pending += counter->num_queued[i];
      if (ready && pending == ready) {
        /* The native state capacity or remaining scheduler work cannot admit another group.
         * Release existing restored rays directly. Surface records stay live until all Beauty
         * and shadow queues drain, so their indices are never recycled prematurely. */
        deep_optical_phase_ = false;
        coalesced_beauty_inflight = true;
        if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE"))
          std::fprintf(
              stderr, "DEEP_BEAUTY_COALESCE ready=%d state_capacity=%d\n", ready, max_num_paths_);
      }
    }

    if (is_cancel_requested()) {
      break;
    }

    /* Stop if no more work remaining. */
    if (finished) {
      camera_domain_complete = true;
      break;
    }

    /* Enqueue on of the path iteration kernels. */
    if (enqueue_path_iteration()) {
      if (!update_queue_counter_and_cache()) {
        break; /* Stop on error. */
      }
    }

    if (is_cancel_requested()) {
      break;
    }

    num_busy_accum += num_active_main_paths_paths();
    ++num_iterations;
  }
  const double loop_wall = deep_dispatch_elapsed(loop_start);

  if (deep_camera_receipts_enabled_) {
    bool complete = camera_domain_complete && !is_cancel_requested() && !device_->have_error();
    if (complete) {
      complete = update_queue_counter_and_cache();
      if (complete) {
        const IntegratorQueueCounter *counter = integrator_queue_counter_.data();
        for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; ++i) {
          if (counter->num_queued[i] != 0) {
            device_->set_error("Receipt domain cannot seal with live main/shadow/MNEE queues");
            complete = false;
            break;
          }
        }
      }
      if (complete && work_tile_scheduler_.has_remaining_work()) {
        device_->set_error("Receipt domain cannot seal with remaining camera scheduler work");
        complete = false;
      }
    }
    if (deep_volume_emission_spans_enabled_) {
      const bool spans_complete = finish_volume_emission_spans(complete);
      if (complete && !spans_complete) {
        device_->set_error("Volume emission span backing or close-success seal failed");
      }
      complete &= spans_complete;
    }
    if (deep_volume_radiance_receipts_enabled_) {
      const bool volume_complete = finish_volume_radiance_receipts(complete);
      if (complete && !volume_complete) {
        device_->set_error("Volume radiance backing or close-success seal failed");
      }
      complete &= volume_complete;
    }
    if (deep_surface_receipts_enabled_) {
      const bool surface_complete = finish_surface_receipts(complete);
      if (complete && !surface_complete) {
        device_->set_error("Surface receipt backing or close-success seal failed");
      }
      complete &= surface_complete;
    }
    complete &= !is_cancel_requested() && !device_->have_error();
    const bool camera_finished = deep_camera_receipt_writer_.finish(complete);
    if (!camera_finished) {
      device_->set_error("Camera receipt domain finish failed");
    }
    complete &= camera_finished && !device_->have_error();
    std::fprintf(stderr,
                 "DEEP_CAMERA_RECEIPTS_END complete=%d samples=%llu\n",
                 int(complete && !device_->have_error()),
                 static_cast<unsigned long long>(deep_camera_receipt_writer_.count()));
    if (complete && deep_independent_optical_enabled_) {
      if (deep_volume_radiance_receipts_enabled_) {
        uint32_t camera_header[32] = {}, volume_header[32] = {};
        complete = DeepCameraReceiptReader::sealed_header(deep_camera_receipt_writer_.path(),
                                                          0x44435248,
                                                          sizeof(DeepCameraReceipt),
                                                          deep_camera_receipt_writer_.count(),
                                                          camera_header) &&
                   DeepCameraReceiptReader::sealed_header(
                       deep_volume_radiance_receipt_writer_.path(),
                       0x44565248,
                       sizeof(DeepVolumeRadianceReceipt),
                       deep_volume_radiance_receipt_writer_.count(),
                       volume_header) &&
                   memcmp(camera_header + 4, volume_header + 4, 28 * sizeof(uint32_t)) == 0;
        if (!complete) {
          device_->set_error("Volume radiance joint close-success identity is invalid");
          return;
        }
      }
      if (deep_volume_emission_spans_enabled_) {
        uint32_t camera_header[32] = {}, span_header[32] = {};
        complete = DeepCameraReceiptReader::sealed_header(deep_camera_receipt_writer_.path(),
                                                          0x44435248,
                                                          sizeof(DeepCameraReceipt),
                                                          deep_camera_receipt_writer_.count(),
                                                          camera_header) &&
                   DeepCameraReceiptReader::sealed_header(
                       deep_volume_emission_span_writer_.path(),
                       0x44455248,
                       sizeof(DeepVolumeEmissionSpan),
                       deep_volume_emission_span_writer_.count(),
                       span_header) &&
                   memcmp(camera_header + 4, span_header + 4, 28 * sizeof(uint32_t)) == 0;
        if (!complete) {
          device_->set_error("Volume emission span joint close-success identity is invalid");
          return;
        }
      }
      DeepOpticalWork optical(device_);
      const bool optical_complete = optical.replay(device_scene_,
                                                   integrator_state_gpu_,
                                                   deep_camera_receipt_writer_.path(),
                                                   deep_surface_receipt_writer_.path(),
                                                   deep_camera_receipt_writer_.count(),
                                                   deep_surface_receipt_writer_.count(),
                                                   cancel_requested_flag_,
                                                   buffers_.get());
      if (!optical_complete && !is_cancel_requested() && !device_->have_error()) {
        device_->set_error("Independent Optical domain is incomplete");
      }
    }
  }

  const auto finish_start = DeepDispatchClock::now();
  if (deep_native_domain_active_) {
    finish_native_capture(camera_domain_complete);
  }
  const double finish_wall = deep_dispatch_elapsed(finish_start);

  if (num_iterations) {
    statistics.occupancy = float(num_busy_accum) / num_iterations / max_num_paths_;
  }
  else {
    statistics.occupancy = 0.0f;
  }
  const char *trace = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_CHAIN_TRACE");
  if (deep_gpu_frame_work_ && trace && strcmp(trace, "1") == 0)
    std::fprintf(stderr,
                 "DEEP_GPU_PRODUCER domain=%llu setup_wall=%.9f loop_wall=%.9f "
                 "queue_sync_wall=%.9f page_flush_wall=%.9f finish_wall=%.9f "
                 "total_wall=%.9f queue_updates=%llu page_publications=%llu iterations=%d "
                 "nested_queue_and_page=1 wall_scopes_not_pure_gpu=1\n",
                 static_cast<unsigned long long>(deep_native_domain_.id), setup_wall, loop_wall,
                 deep_gpu_queue_sync_wall_, deep_gpu_page_flush_wall_, finish_wall,
                 deep_dispatch_elapsed(batch_start),
                 static_cast<unsigned long long>(deep_gpu_queue_updates_),
                 static_cast<unsigned long long>(deep_gpu_page_publications_), num_iterations);
}

bool PathTraceWorkGPU::bounded_shadows_can_wait() const
{
  if (!prefer_bounded_main_queues_) {
    return false;
  }
  const auto &counter = *integrator_queue_counter_.data();
  const int queued = counter.num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE] +
                     counter.num_queued[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW] +
                     counter.num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW];
  /* Allocation-producing kernels retain their original compact/capacity
   * guard. Let those real bounds determine when native shadows must drain;
   * otherwise preserve main waves and already-authorized packet tail carry. */
  const int threshold = bounded_shadow_queue_limit_ ? bounded_shadow_queue_limit_ :
      std::max(1, std::min(min_num_active_main_paths_ / 2, max_num_paths_ / 2));
  return queued < threshold;
}

DeviceKernel PathTraceWorkGPU::get_most_queued_kernel() const
{
  const IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int max_num_queued = 0, max_shadow_queued = 0;
  DeviceKernel kernel = DEVICE_KERNEL_NUM, shadow_kernel = DEVICE_KERNEL_NUM;
  const bool defer_shadows = bounded_shadows_can_wait();

  for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; i++) {
    /* SHADOW_PATH_MNEE_PENDING is a sentinel marker on shadow slots holding an MNEE precompute
     * payload; there is no kernel to dispatch for it. The slot transitions to a real shadow
     * kernel (or terminates) when integrator_shade_surface runs on the main path. */
    if (i == DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING) {
      continue;
    }
    if (deep_optical_phase_ && i == DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY) {
      continue;
    }
    if (defer_shadows && kernel_is_shadow_path(DeviceKernel(i))) {
      if (queue_counter->num_queued[i] > max_shadow_queued) {
        shadow_kernel = DeviceKernel(i);
        max_shadow_queued = queue_counter->num_queued[i];
      }
      continue;
    }
    if (queue_counter->num_queued[i] > max_num_queued) {
      kernel = (DeviceKernel)i;
      max_num_queued = queue_counter->num_queued[i];
    }
  }

  return kernel == DEVICE_KERNEL_NUM ? shadow_kernel : kernel;
}

void PathTraceWorkGPU::enqueue_reset(const bool clear_path_slots)
{
  if (clear_path_slots) {
    const DeviceKernelArguments args(&max_num_paths_);
    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_RESET, max_num_paths_, args);
  }
  queue_->zero_to_device(integrator_queue_counter_);
  if (integrator_shader_sort_counter_.size() != 0) {
    queue_->zero_to_device(integrator_shader_sort_counter_);
  }
  if (device_scene_->data.kernel_features & KERNEL_FEATURE_NODE_RAYTRACE &&
      integrator_shader_raytrace_sort_counter_.size() != 0)
  {
    queue_->zero_to_device(integrator_shader_raytrace_sort_counter_);
  }

  /* Tiles enqueue need to know number of active paths, which is based on this counter. Zero the
   * counter on the host side because `zero_to_device()` is not doing it. */
  if (integrator_queue_counter_.host_pointer) {
    memset(integrator_queue_counter_.data(), 0, integrator_queue_counter_.memory_size());
  }
}

bool PathTraceWorkGPU::enqueue_path_iteration()
{
  /* Find kernel to execute, with max number of queued paths. */
  const IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int num_active_paths = 0;
  for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; i++) {
    num_active_paths += queue_counter->num_queued[i];
  }

  if (num_active_paths == 0) {
    return false;
  }

  /* Find kernel to execute, with max number of queued paths. */
  DeviceKernel kernel = get_most_queued_kernel();
  if (deep_native_domain_active_) {
    /* Drain executable pending sources before creating optional new roots.
     * MNEE_PENDING remains pinned until its owning main operation resolves. */
    for (const DeviceKernel shadow : {DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE,
                                      DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW,
                                      DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW})
    {
      if (queue_counter->num_queued[shadow]) {
        kernel = shadow;
        break;
      }
    }
  }
  if (kernel == DEVICE_KERNEL_NUM) {
    return false;
  }

  /* For kernels that add shadow paths, check if there is enough space available.
   * If not, schedule shadow kernels first to clear out the shadow paths. */
  int num_paths_limit = INT_MAX;

  if (kernel_creates_shadow_paths(kernel)) {
    compact_shadow_paths();

    /* Number of shadow paths that may be created for every scheduled path. */
    const int shadow_paths_per_path = kernel_creates_ao_paths(kernel) ? 2 : 1;

    int available_shadow_paths = max_num_paths_ - integrator_next_shadow_path_index_.data()[0];

    if (available_shadow_paths < queue_counter->num_queued[kernel] * shadow_paths_per_path) {
      if (queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE]) {
        enqueue_path_iteration(DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE);
        return true;
      }
      if (queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW]) {
        enqueue_path_iteration(DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
        return true;
      }
      if (queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW]) {
        enqueue_path_iteration(DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
        return true;
      }

      /* Rare corner case where there are not even enough available shadow paths
       * to run the kernel with 1 path, in this case we must force compaction. */
      if (available_shadow_paths < shadow_paths_per_path) {
        compact_shadow_paths(true);
        available_shadow_paths = max_num_paths_ - integrator_next_shadow_path_index_.data()[0];
      }
    }

    /* Limit number of scheduled paths to the available shadow path space. */
    num_paths_limit = available_shadow_paths / shadow_paths_per_path;

    if (kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE) {
      /* MNEE should not fill in more than half the available shadow paths,
       * so there is room to run integrator_shade_surface, which is the only
       * kernel that can release MNEE shadow paths. */
      const int num_mnee_paths =
          queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING];
      num_paths_limit = min(num_paths_limit, max(max_num_paths_ / 2 - num_mnee_paths, 1));
    }
  }

  /* Schedule kernel with maximum number of queued items. */
  enqueue_path_iteration(kernel, num_paths_limit);

  /* Update next shadow path index for kernels that can add shadow paths. */
  if (kernel_creates_shadow_paths(kernel)) {
    queue_->copy_from_device(integrator_next_shadow_path_index_);
  }


  return true;
}

void PathTraceWorkGPU::enqueue_path_iteration(DeviceKernel kernel, const int num_paths_limit)
{
  device_ptr d_path_index = 0;

  int dispatch_limit = num_paths_limit;
  if (deep_native_domain_active_) {
    dispatch_limit = deep_native_control_dispatch_limit(
        integrator_state_gpu_.deep_native_capture_rgb ? deep_native_roots_.size() : SIZE_MAX,
                                                        integrator_state_gpu_.deep_native_capture_rgb &&
                                                            !integrator_state_gpu_.deep_compact_root_colours ?
                                                                deep_native_points_.size() : SIZE_MAX,
                                                        deep_native_homogeneous_.size() ? deep_native_homogeneous_.size() : SIZE_MAX,
                                                        deep_native_expression_calls_.size(),
                                                        bool(deep_native_expression_calls_.size()),
                                                        num_paths_limit);
    if (integrator_state_gpu_.deep_compact_capacity)
      dispatch_limit = std::min(dispatch_limit, int(deep_compact_primitives_.size() / 2));
    if (dispatch_limit <= 0) {
      device_->set_error("Native control window cannot hold one complete operation");
      return;
    }
  }
  /* Create array of path indices for which this kernel is queued to be executed. */
  int work_size = kernel_max_active_main_path_index(kernel);

  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();
  const int num_queued = queue_counter->num_queued[kernel];

  if (kernel_uses_sorting(kernel)) {
    /* Compute array of active paths, sorted by shader. */
    work_size = num_queued;
    d_path_index = queued_paths_.device_pointer;

    compute_sorted_queued_paths(kernel, dispatch_limit);
  }
  else if (num_queued < work_size) {
    work_size = num_queued;
    d_path_index = queued_paths_.device_pointer;

    if (kernel_is_shadow_path(kernel)) {
      /* Compute array of active shadow paths for specific kernel. */
      compute_queued_paths(DEVICE_KERNEL_INTEGRATOR_QUEUED_SHADOW_PATHS_ARRAY, kernel);
    }
    else {
      /* Compute array of active paths for specific kernel. */
      compute_queued_paths(DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY, kernel);
    }
  }

  work_size = min(work_size, dispatch_limit);

  DCHECK_LE(work_size, max_num_paths_);

  switch (kernel) {
    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA: {
      const int num_path_indices = work_size;
      device_ptr null_tiles = 0;
      int zero = 0;
      const DeviceKernelArguments args(&null_tiles,
                                       &zero,
                                       &buffers_->buffer.device_pointer,
                                       &zero,
                                       &d_path_index,
                                       &num_path_indices);

      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY: {
      const DeviceKernelArguments args(&d_path_index, &work_size);
      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST: {
      /* Closest ray intersection kernels with integrator state and render buffer. */
      const DeviceKernelArguments args(
          &d_path_index, &buffers_->buffer.device_pointer, &work_size);

      queue_->enqueue(kernel, work_size, args);
      break;
    }

    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW: {
      const DeviceKernelArguments args(&d_path_index, &work_size, &buffers_->buffer.device_pointer);
      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_DEDICATED_LIGHT:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE: {
      /* Ray intersection kernels with integrator state. */
      const DeviceKernelArguments args(&d_path_index, &work_size);

      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_FORWARD:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL_WARP:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEDICATED_LIGHT: {
      /* Shading kernels with integrator state and render buffer. */
      if (deep_native_domain_active_ && kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_DEDICATED_LIGHT) {
        const char *verify = std::getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD");
        if (verify && std::strcmp(verify, "1") == 0)
          std::fprintf(stderr,
                       "DEEP_FRAME_DEDICATED_LIGHT domain=%llu paths=%d\n",
                       static_cast<unsigned long long>(deep_native_domain_.id),
                       work_size);
      }
      const DeviceKernelArguments args(
          &d_path_index, &buffers_->buffer.device_pointer, &work_size);

      queue_->enqueue(kernel, work_size, args);
      break;
    }
    default:
      LOG_FATAL << "Unhandled kernel " << device_kernel_as_string(kernel)
                << " used for path iteration, should never happen.";
      break;
  }
}

void PathTraceWorkGPU::compute_sorted_queued_paths(DeviceKernel queued_kernel,
                                                   const int num_paths_limit)
{
  int d_queued_kernel = queued_kernel;

  /* Launch kernel to fill the active paths arrays. */
  if (num_sort_partitions_ > 1 && queue_->supports_local_atomic_sort()) {
    const int work_size = kernel_max_active_main_path_index(queued_kernel);
    device_ptr d_queued_paths = queued_paths_.device_pointer;

    int partition_size = (int)integrator_state_gpu_.sort_partition_divisor;

    const DeviceKernelArguments args(
        &work_size, &partition_size, &num_paths_limit, &d_queued_paths, &d_queued_kernel);

    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_SORT_BUCKET_PASS,
                    GPU_PARALLEL_SORT_BLOCK_SIZE * num_sort_partitions_,
                    args);
    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_SORT_WRITE_PASS,
                    GPU_PARALLEL_SORT_BLOCK_SIZE * num_sort_partitions_,
                    args);
    return;
  }

  device_ptr d_counter = (device_ptr)integrator_state_gpu_.sort_key_counter[d_queued_kernel];
  device_ptr d_prefix_sum = integrator_shader_sort_prefix_sum_.device_pointer;
  assert(d_counter != 0 && d_prefix_sum != 0);

  /* Compute prefix sum of number of active paths with each shader. */
  {
    const int work_size = 1;
    int sort_buckets = device_scene_->data.max_shaders * num_sort_partitions_;

    const DeviceKernelArguments args(&d_counter, &d_prefix_sum, &sort_buckets);

    queue_->enqueue(DEVICE_KERNEL_PREFIX_SUM, work_size, args);
  }

  /* The native sorted-index helper never reads or updates num_indices.
   * Real active/compaction users retain their own required zeroing. */
  if (!deep_bounded_work_) queue_->zero_to_device(num_queued_paths_);

  /* Launch kernel to fill the active paths arrays. */
  {
    /* TODO: this could be smaller for terminated paths based on amount of work we want
     * to schedule, and also based on num_paths_limit.
     *
     * Also, when the number paths is limited it may be better to prefer paths from the
     * end of the array since compaction would need to do less work. */
    const int work_size = kernel_max_active_main_path_index(queued_kernel);

    device_ptr d_queued_paths = queued_paths_.device_pointer;
    device_ptr d_num_queued_paths = num_queued_paths_.device_pointer;

    const DeviceKernelArguments args(&work_size,
                                     &num_paths_limit,
                                     &d_queued_paths,
                                     &d_num_queued_paths,
                                     &d_counter,
                                     &d_prefix_sum,
                                     &d_queued_kernel);

    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY, work_size, args);
  }
}

void PathTraceWorkGPU::compute_queued_paths(DeviceKernel kernel, DeviceKernel queued_kernel)
{
  int d_queued_kernel = queued_kernel;

  /* Launch kernel to fill the active paths arrays. */
  const int work_size = kernel_max_active_main_path_index(queued_kernel);
  device_ptr d_queued_paths = queued_paths_.device_pointer;
  device_ptr d_num_queued_paths = num_queued_paths_.device_pointer;

  const DeviceKernelArguments args(
      &work_size, &d_queued_paths, &d_num_queued_paths, &d_queued_kernel);

  queue_->zero_to_device(num_queued_paths_);
  queue_->enqueue(kernel, work_size, args);
}

void PathTraceWorkGPU::compact_main_paths(const int num_active_paths)
{
  /* Early out if there is nothing that needs to be compacted. */
  if (num_active_paths == 0) {
    max_active_main_path_index_ = 0;
    return;
  }
  if (deep_native_domain_active_ &&
      size_t(max_active_main_path_index_) > deep_native_camera_indices_.data_size)
  {
    device_->set_error("Native Deep Frame main-state address exceeds its capture index envelope");
    return;
  }

  const int min_compact_paths = 32;
  if (max_active_main_path_index_ == num_active_paths ||
      max_active_main_path_index_ < min_compact_paths)
  {
    return;
  }

  /* Compact. */
  compact_paths(num_active_paths,
                max_active_main_path_index_,
                DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY,
                DEVICE_KERNEL_INTEGRATOR_COMPACT_PATHS_ARRAY,
                DEVICE_KERNEL_INTEGRATOR_COMPACT_STATES);

  /* Adjust max active path index now we know which part of the array is actually used. */
  max_active_main_path_index_ = num_active_paths;
}

void PathTraceWorkGPU::compact_shadow_paths(const bool force)
{
  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();
  const int num_active_paths =
      queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE] +
      queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW] +
      queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW] +
      queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING];

  /* Early out if there is nothing that needs to be compacted. */
  if (num_active_paths == 0) {
    if (integrator_next_shadow_path_index_.data()[0] != 0) {
      integrator_next_shadow_path_index_.data()[0] = 0;
      queue_->copy_to_device(integrator_next_shadow_path_index_);
    }
    return;
  }

  /* Compact if we can reduce the space used by half. Not always unless forced, since
   * compaction has a cost. */
  const float max_overhead_factor = 2.0f;
  const int min_compact_paths = 32;
  const int num_total_paths = integrator_next_shadow_path_index_.data()[0];
  if (!force && (num_total_paths < num_active_paths * max_overhead_factor ||
                 num_total_paths < min_compact_paths))
  {
    return;
  }

  /* Compact. */
  compact_paths(num_active_paths,
                num_total_paths,
                DEVICE_KERNEL_INTEGRATOR_TERMINATED_SHADOW_PATHS_ARRAY,
                DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_PATHS_ARRAY,
                DEVICE_KERNEL_INTEGRATOR_COMPACT_SHADOW_STATES);

  /* Adjust max active path index now we know which part of the array is actually used. */
  integrator_next_shadow_path_index_.data()[0] = num_active_paths;
  queue_->copy_to_device(integrator_next_shadow_path_index_);
}

void PathTraceWorkGPU::compact_paths(const int num_active_paths,
                                     const int max_active_path_index,
                                     DeviceKernel terminated_paths_kernel,
                                     DeviceKernel compact_paths_kernel,
                                     DeviceKernel compact_kernel)
{
  /* Compact fragmented path states into the start of the array, moving any paths
   * with index higher than the number of active paths into the gaps. */
  device_ptr d_compact_paths = queued_paths_.device_pointer;
  device_ptr d_num_queued_paths = num_queued_paths_.device_pointer;

  /* Create array with terminated paths that we can write to. */
  {
    /* TODO: can the work size be reduced here? */
    int offset = num_active_paths;
    const int work_size = num_active_paths;

    const DeviceKernelArguments args(&work_size, &d_compact_paths, &d_num_queued_paths, &offset);

    queue_->zero_to_device(num_queued_paths_);
    queue_->enqueue(terminated_paths_kernel, work_size, args);
  }

  /* Create array of paths that we need to compact, where the path index is bigger
   * than the number of active paths. */
  {
    const int work_size = max_active_path_index;

    const DeviceKernelArguments args(
        &work_size, &d_compact_paths, &d_num_queued_paths, &num_active_paths);

    queue_->zero_to_device(num_queued_paths_);
    queue_->enqueue(compact_paths_kernel, work_size, args);
  }

  int work_size;
  device_ptr device_count = 0;
  if (deep_bounded_work_) {
    /* A move needs a hole below the split and a live state above it. Launch the
     * smaller range and let the move kernel consume the same-stream GPU count. */
    work_size = min(num_active_paths, max_active_path_index - num_active_paths);
    device_count = d_num_queued_paths;
  }
  else {
    queue_->copy_from_device(num_queued_paths_);
    if (!queue_->synchronize()) {
      return;
    }
    work_size = num_queued_paths_.data()[0];
  }

  /* Move paths into gaps. */
  if (work_size > 0) {
    int active_states_offset = 0;
    int terminated_states_offset = num_active_paths;

    const DeviceKernelArguments args(
        &d_compact_paths, &active_states_offset, &terminated_states_offset, &work_size,
        &device_count);

    queue_->enqueue(compact_kernel, work_size, args);
  }
}

bool PathTraceWorkGPU::enqueue_work_tiles(bool &finished, const bool admit_new_packet)
{
  if (deep_optical_wavefront_ && !deep_optical_phase_) {
    return false;
  }
  /* If there are existing paths wait them to go to intersect closest kernel, which will align the
   * wavefront of the existing and newly added paths. */
  /* TODO: Check whether counting new intersection kernels here will have positive affect on the
   * performance. */
  const DeviceKernel kernel = get_most_queued_kernel();
  const bool admit_with_shadow = kernel_is_shadow_path(kernel) && bounded_shadows_can_wait();
  if (kernel != DEVICE_KERNEL_NUM && kernel != DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST &&
      !admit_new_packet && !admit_with_shadow)
  {
    return false;
  }
  const int num_active_paths = num_active_main_paths_paths();

  /* Don't schedule more work if canceling. */
  if (is_cancel_requested()) {
    if (num_active_paths == 0) {
      finished = true;
    }
    return false;
  }

  finished = false;

  vector<KernelWorkTile> work_tiles;

  int max_num_camera_paths = max_num_paths_;
  int num_predicted_splits = 0;
  int scheduled_possible_catcher_splits = 0;

  if (has_shadow_catcher()) {
    /* When there are shadow catchers in the scene bounce from them will split the state. So we
     * make sure there is enough space in the path states array to fit split states.
     *
     * Basically, when adding N new paths we ensure that there is 2*N available path states, so
     * that all the new paths can be split.
     *
     * Note that it is possible that some of the current states can still split, so need to make
     * sure there is enough space for them as well. */

    /* Number of currently in-flight states which can still split. */
    const int num_scheduled_possible_split = shadow_catcher_count_possible_splits();
    scheduled_possible_catcher_splits = num_scheduled_possible_split;

    const int num_available_paths = max_num_paths_ - num_active_paths;
    const int num_new_paths = num_available_paths / 2;
    max_num_camera_paths = max(num_active_paths,
                               num_active_paths + num_new_paths - num_scheduled_possible_split);
    num_predicted_splits += num_scheduled_possible_split + num_new_paths;
  }

  if (deep_optical_wavefront_ && deep_capture_group_flush_) {
    if (deep_beauty_coalesce_ && deep_optical_phase_ && deep_capture_ready_limit_ > 0) {
      /* Optical batches drain before READY rays are released. Their subsequent primary-surface

       * * records share one live append pool until Beauty/shadow completion, so native state

       * * capacity alone is not a sufficient bound with a small pool. Shadow-catcher splitting

       * * reserves space for both resulting paths. Multi-layer surfaces retain explicit overflow.
       */
      const int ready_limit = has_shadow_catcher() ? deep_capture_ready_limit_ / 2 :
                                                     deep_capture_ready_limit_;
      max_num_camera_paths = min(max_num_camera_paths, ready_limit);
    }
    /* Count all cameras admitted since the last drain, including already terminated empty
     *
     * rays. Active-state occupancy alone is not an append-capacity bound. */
    const int available = max(0, deep_capture_group_limit_ - deep_capture_group_admitted_);
    max_num_camera_paths = min(max_num_camera_paths, num_active_paths + available);
  }

  /* Schedule when we're out of paths or there are too few paths to keep the
   * device occupied. */
  int num_paths = num_active_paths;
  const bool admit_with_ready =
      deep_beauty_coalesce_ && deep_optical_phase_ &&
      integrator_queue_counter_.data()->num_queued[DEVICE_KERNEL_INTEGRATOR_DEEP_BEAUTY_READY] ==
          num_active_paths;
  if (num_paths == 0 || num_paths < min_num_active_main_paths_ || admit_with_ready) {
    /* Get work tiles until the maximum number of path is reached. */
    while (num_paths < max_num_camera_paths) {
      KernelWorkTile work_tile;
      if (work_tile_scheduler_.get_work(&work_tile, max_num_camera_paths - num_paths)) {
        work_tiles.push_back(work_tile);
        num_paths += work_tile.w * work_tile.h * work_tile.num_samples;
      }
      else {
        break;
      }
    }

    /* If we couldn't get any more tiles, we're done. */
    if (work_tiles.empty() && num_paths == 0) {
      if (deep_optical_wavefront_ && deep_capture_group_flush_ &&
          work_tile_scheduler_.has_remaining_work())
      {
        device_->set_error(
            "Deep camera group cannot schedule remaining work within capture capacity");
      }
      finished = true;
      return false;
    }
  }

  /* Initialize paths from work tiles. */
  if (work_tiles.empty()) {
    return false;
  }
  if (deep_optical_wavefront_ && deep_capture_group_flush_) {
    deep_capture_group_admitted_ += num_paths - num_active_paths;
  }

  if (deep_native_domain_active_ && has_shadow_catcher()) {
    /* Only admitted cameras and live unsplit paths can split. Keep compaction
     * within their checked two-state envelope instead of unused pool slots. */
    num_predicted_splits = min(num_predicted_splits,
                               scheduled_possible_catcher_splits + num_paths - num_active_paths);
  }

  /* Compact state array when number of paths becomes small relative to the
   * known maximum path index, which makes computing active index arrays slow. */
  compact_main_paths(num_active_paths);

  if (has_shadow_catcher()) {
    integrator_next_main_path_index_.data()[0] = num_paths;
    queue_->copy_to_device(integrator_next_main_path_index_);
  }

  enqueue_work_tiles((device_scene_->data.bake.use) ? DEVICE_KERNEL_INTEGRATOR_INIT_FROM_BAKE :
                                                      DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA,
                     work_tiles.data(),
                     work_tiles.size(),
                     num_active_paths,
                     num_predicted_splits);

  return true;
}

void PathTraceWorkGPU::enqueue_work_tiles(DeviceKernel kernel,
                                          const KernelWorkTile work_tiles[],
                                          const int num_work_tiles,
                                          const int num_active_paths,
                                          const int num_predicted_splits)
{
  /* Copy work tiles to device. */
  if (work_tiles_.size() < num_work_tiles) {
    work_tiles_.alloc(num_work_tiles);
  }

  int path_index_offset = num_active_paths;
  int max_tile_work_size = 0;
  for (int i = 0; i < num_work_tiles; i++) {
    KernelWorkTile &work_tile = work_tiles_.data()[i];
    work_tile = work_tiles[i];

    const int tile_work_size = work_tile.w * work_tile.h * work_tile.num_samples;

    work_tile.path_index_offset = path_index_offset;
    work_tile.work_size = tile_work_size;

    path_index_offset += tile_work_size;

    max_tile_work_size = max(max_tile_work_size, tile_work_size);
  }

  queue_->copy_to_device(work_tiles_);

  if (deep_native_domain_active_ &&
      size_t(path_index_offset + num_predicted_splits) > deep_native_camera_indices_.data_size)
  {
    device_->set_error("Native Deep Frame camera scheduling exceeds its capture index envelope");
    return;
  }

  const device_ptr d_work_tiles = work_tiles_.device_pointer;
  device_ptr d_render_buffer = buffers_->buffer.device_pointer;

  /* Launch kernel. */
  device_ptr null_ptr = 0;
  int zero = 0;
  const DeviceKernelArguments args(
      &d_work_tiles, &num_work_tiles, &d_render_buffer, &max_tile_work_size, &null_ptr, &zero);

  queue_->enqueue(kernel, max_tile_work_size * num_work_tiles, args);

  max_active_main_path_index_ = path_index_offset + num_predicted_splits;
}

int PathTraceWorkGPU::num_active_main_paths_paths()
{
  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int num_paths = 0;
  for (int i = 0; i < DEVICE_GPU_KERNEL_INTEGRATOR_NUM; i++) {
    DCHECK_GE(queue_counter->num_queued[i], 0)
        << "Invalid number of queued states for kernel "
        << device_kernel_as_string(static_cast<DeviceKernel>(i));

    if (!kernel_is_shadow_path((DeviceKernel)i)) {
      num_paths += queue_counter->num_queued[i];
    }
  }

  return num_paths;
}

bool PathTraceWorkGPU::should_use_graphics_interop(PathTraceDisplay *display)
{
  /* There are few aspects with the graphics interop when using multiple devices caused by the fact
   * that the PathTraceDisplay has a single texture:
   *
   *   CUDA will return `CUDA_ERROR_NOT_SUPPORTED` from `cuGraphicsGLRegisterBuffer()` when
   *   attempting to register OpenGL PBO which has been mapped. Which makes sense, because
   *   otherwise one would run into a conflict of where the source of truth is. */
  if (has_multiple_works()) {
    return false;
  }

  if (!interop_use_checked_) {
    Device *device = queue_->device;
    interop_use_ = device->should_use_graphics_interop(display->graphics_interop_get_device(),
                                                       true);

    if (interop_use_) {
      LOG_INFO << "Using graphics interop GPU display update.";
    }
    else {
      LOG_INFO << "Using naive GPU display update.";
    }

    interop_use_checked_ = true;
  }

  return interop_use_;
}

void PathTraceWorkGPU::copy_to_display(PathTraceDisplay *display,
                                       PassMode pass_mode,
                                       const int num_samples)
{
  if (device_->have_error()) {
    /* Don't attempt to update GPU display if the device has errors: the error state will make
     * wrong decisions to happen about interop, causing more chained bugs. */
    return;
  }

  if (!buffers_->buffer.device_pointer) {
    LOG_WARNING << "Request for GPU display update without allocated render buffers.";
    return;
  }

  if (should_use_graphics_interop(display)) {
    if (copy_to_display_interop(display, pass_mode, num_samples)) {
      return;
    }

    /* If error happens when trying to use graphics interop fallback to the native implementation
     * and don't attempt to use interop for the further updates. */
    interop_use_ = false;
  }

  copy_to_display_naive(display, pass_mode, num_samples);
}

void PathTraceWorkGPU::copy_to_display_naive(PathTraceDisplay *display,
                                             PassMode pass_mode,
                                             const int num_samples)
{
  const BufferParams &effective_big_tile_params = (pass_mode == PassMode::DENOISED) ?
                                                      effective_denoised_big_tile_params_ :
                                                      effective_big_tile_params_;
  const BufferParams &effective_buffer_params = (pass_mode == PassMode::DENOISED) ?
                                                    effective_denoised_buffer_params_ :
                                                    effective_buffer_params_;

  const int full_x = effective_buffer_params.full_x;
  const int full_y = effective_buffer_params.full_y;
  const int width = effective_buffer_params.window_width;
  const int height = effective_buffer_params.window_height;
  const int final_width = buffers_->params.window_width;
  const int final_height = buffers_->params.window_height;

  const int texture_x = full_x - effective_big_tile_params.full_x +
                        effective_buffer_params.window_x - effective_big_tile_params.window_x;
  const int texture_y = full_y - effective_big_tile_params.full_y +
                        effective_buffer_params.window_y - effective_big_tile_params.window_y;

  /* Re-allocate display memory if needed, and make sure the device pointer is allocated.
   *
   * NOTE: allocation happens to the final resolution so that no re-allocation happens on every
   * change of the resolution divider. However, if the display becomes smaller, shrink the
   * allocated memory as well. */
  if (display_rgba_half_.data_width != final_width ||
      display_rgba_half_.data_height != final_height)
  {
    display_rgba_half_.alloc(final_width, final_height);
    /* TODO(sergey): There should be a way to make sure device-side memory is allocated without
     * transferring zeroes to the device. */
    queue_->zero_to_device(display_rgba_half_);
  }

  PassAccessor::Destination destination(film_->get_display_pass(), pass_mode);
  destination.d_pixels_half_rgba = display_rgba_half_.device_pointer;

  get_render_tile_film_pixels(destination, pass_mode, num_samples);

  queue_->copy_from_device(display_rgba_half_);
  queue_->synchronize();

  display->copy_pixels_to_texture(display_rgba_half_.data(), texture_x, texture_y, width, height);
}

bool PathTraceWorkGPU::copy_to_display_interop(PathTraceDisplay *display,
                                               PassMode pass_mode,
                                               const int num_samples)
{
  if (!device_graphics_interop_) {
    device_graphics_interop_ = queue_->graphics_interop_create();
  }

  GraphicsInteropBuffer &interop_buffer = display->graphics_interop_get_buffer();
  device_graphics_interop_->set_buffer(interop_buffer);

  const device_ptr d_rgba_half = device_graphics_interop_->map();
  if (!d_rgba_half) {
    return false;
  }

  PassAccessor::Destination destination = get_display_destination_template(display, pass_mode);
  destination.d_pixels_half_rgba = d_rgba_half;

  get_render_tile_film_pixels(destination, pass_mode, num_samples);

  device_graphics_interop_->unmap();

  return true;
}

void PathTraceWorkGPU::destroy_gpu_resources(PathTraceDisplay *display)
{
  if (!device_graphics_interop_) {
    return;
  }
  display->graphics_interop_activate();
  device_graphics_interop_ = nullptr;
  display->graphics_interop_deactivate();
}

void PathTraceWorkGPU::get_render_tile_film_pixels(const PassAccessor::Destination &destination,
                                                   PassMode pass_mode,
                                                   const int num_samples)
{
  const KernelFilm &kfilm = device_scene_->data.film;

  const PassAccessor::PassAccessInfo pass_access_info = get_display_pass_access_info(pass_mode);
  if (pass_access_info.type == PASS_NONE) {
    return;
  }

  const BufferParams &effective_buffer_params = (pass_mode == PassMode::DENOISED) ?
                                                    effective_denoised_buffer_params_ :
                                                    effective_buffer_params_;

  const PassAccessorGPU pass_accessor(queue_.get(), pass_access_info, kfilm.exposure, num_samples);

  pass_accessor.get_render_tile_pixels(buffers_.get(), effective_buffer_params, destination);
}

int PathTraceWorkGPU::adaptive_sampling_converge_filter_count_active(const float threshold,
                                                                     bool reset)
{
  const int num_active_pixels = adaptive_sampling_convergence_check_count_active(threshold, reset);

  if (num_active_pixels) {
    enqueue_adaptive_sampling_filter_x();
    enqueue_adaptive_sampling_filter_y();
    queue_->synchronize();
  }

  return num_active_pixels;
}

int PathTraceWorkGPU::adaptive_sampling_convergence_check_count_active(const float threshold,
                                                                       bool reset)
{
  device_vector<uint> num_active_pixels(device_, "num_active_pixels", MEM_READ_WRITE);
  num_active_pixels.alloc(1);

  queue_->zero_to_device(num_active_pixels);

  const int work_size = effective_buffer_params_.width * effective_buffer_params_.height;
  if (!work_size) {
    return 0;
  }

  const int reset_int = reset; /* No bool kernel arguments. */

  const DeviceKernelArguments args(&buffers_->buffer.device_pointer,
                                   &effective_buffer_params_.full_x,
                                   &effective_buffer_params_.full_y,
                                   &effective_buffer_params_.width,
                                   &effective_buffer_params_.height,
                                   &threshold,
                                   &reset_int,
                                   &effective_buffer_params_.offset,
                                   &effective_buffer_params_.stride,
                                   &num_active_pixels.device_pointer);

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_CHECK, work_size, args);

  queue_->copy_from_device(num_active_pixels);
  queue_->synchronize();

  return num_active_pixels.data()[0];
}

void PathTraceWorkGPU::enqueue_adaptive_sampling_filter_x()
{
  const int work_size = effective_buffer_params_.height;
  DCHECK_GT(work_size, 0);

  const DeviceKernelArguments args(&buffers_->buffer.device_pointer,
                                   &effective_buffer_params_.full_x,
                                   &effective_buffer_params_.full_y,
                                   &effective_buffer_params_.width,
                                   &effective_buffer_params_.height,
                                   &effective_buffer_params_.offset,
                                   &effective_buffer_params_.stride);

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_X, work_size, args);
}

void PathTraceWorkGPU::enqueue_adaptive_sampling_filter_y()
{
  const int work_size = effective_buffer_params_.width;
  DCHECK_GT(work_size, 0);

  const DeviceKernelArguments args(&buffers_->buffer.device_pointer,
                                   &effective_buffer_params_.full_x,
                                   &effective_buffer_params_.full_y,
                                   &effective_buffer_params_.width,
                                   &effective_buffer_params_.height,
                                   &effective_buffer_params_.offset,
                                   &effective_buffer_params_.stride);

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_Y, work_size, args);
}

void PathTraceWorkGPU::cryptomatte_postproces()
{
  const int work_size = effective_buffer_params_.width * effective_buffer_params_.height;
  if (!work_size) {
    return;
  }

  const DeviceKernelArguments args(&buffers_->buffer.device_pointer,
                                   &work_size,
                                   &effective_buffer_params_.offset,
                                   &effective_buffer_params_.stride);

  queue_->enqueue(DEVICE_KERNEL_CRYPTOMATTE_POSTPROCESS, work_size, args);
}

void PathTraceWorkGPU::denoise_volume_guiding_buffers()
{
  const DeviceKernelArguments args(&buffers_->buffer.device_pointer,
                                   &effective_buffer_params_.full_x,
                                   &effective_buffer_params_.full_y,
                                   &effective_buffer_params_.width,
                                   &effective_buffer_params_.height,
                                   &effective_buffer_params_.offset,
                                   &effective_buffer_params_.stride);

  {
    const int work_size = effective_buffer_params_.width * effective_buffer_params_.height;
    DCHECK_GT(work_size, 0);
    queue_->enqueue(DEVICE_KERNEL_VOLUME_GUIDING_FILTER_X, work_size, args);
  }

  {
    const int work_size = effective_buffer_params_.width;
    DCHECK_GT(work_size, 0);
    queue_->enqueue(DEVICE_KERNEL_VOLUME_GUIDING_FILTER_Y, work_size, args);
  }
}

bool PathTraceWorkGPU::copy_render_buffers_from_device()
{
  /* May not exist if cancelled before rendering started. */
  if (!buffers_->buffer.device_pointer) {
    return false;
  }

  queue_->copy_from_device(buffers_->buffer);

  /* Synchronize so that the CPU-side buffer is available at the exit of this function. */
  return queue_->synchronize();
}

bool PathTraceWorkGPU::copy_render_buffers_to_device()
{
  queue_->copy_to_device(buffers_->buffer);

  /* NOTE: The direct device access to the buffers only happens within this path trace work. The
   * rest of communication happens via API calls which involves `copy_render_buffers_from_device()`
   * which will perform synchronization as needed. */

  return true;
}

bool PathTraceWorkGPU::zero_render_buffers()
{
  queue_->zero_to_device(buffers_->buffer);

  return true;
}

bool PathTraceWorkGPU::has_shadow_catcher() const
{
  return device_scene_->data.integrator.has_shadow_catcher;
}

int PathTraceWorkGPU::shadow_catcher_count_possible_splits()
{
  if (max_active_main_path_index_ == 0) {
    return 0;
  }

  if (!has_shadow_catcher()) {
    return 0;
  }

  queue_->zero_to_device(num_queued_paths_);

  const int work_size = max_active_main_path_index_;
  device_ptr d_num_queued_paths = num_queued_paths_.device_pointer;

  const DeviceKernelArguments args(&work_size, &d_num_queued_paths);

  queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_SHADOW_CATCHER_COUNT_POSSIBLE_SPLITS, work_size, args);
  queue_->copy_from_device(num_queued_paths_);
  queue_->synchronize();

  return num_queued_paths_.data()[0];
}

bool PathTraceWorkGPU::kernel_uses_sorting(DeviceKernel kernel)
{
  return (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE);
}

bool PathTraceWorkGPU::kernel_creates_shadow_paths(DeviceKernel kernel)
{
  return (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE ||
          kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_DEDICATED_LIGHT);
}

bool PathTraceWorkGPU::kernel_creates_ao_paths(DeviceKernel kernel)
{
  return (device_scene_->data.kernel_features & KERNEL_FEATURE_AO) &&
         (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE);
}

bool PathTraceWorkGPU::kernel_is_shadow_path(DeviceKernel kernel)
{
  return (kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW ||
          kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE);
}

int PathTraceWorkGPU::kernel_max_active_main_path_index(DeviceKernel kernel)
{
  return (kernel_is_shadow_path(kernel)) ? integrator_next_shadow_path_index_.data()[0] :
                                           max_active_main_path_index_;
}

CCL_NAMESPACE_END
