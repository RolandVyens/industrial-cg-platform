/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Integrator State
 *
 * This file defines the data structures that define the state of a path. Any state that is
 * preserved and passed between kernel executions is part of this.
 *
 * The size of this state must be kept as small as possible, to reduce cache misses and keep memory
 * usage under control on GPUs that may execute millions of kernels.
 *
 * Memory may be allocated and passed along in different ways depending on the device. There may
 * be a scalar layout, or AoS or SoA layout for batches. The state may be passed along as a pointer
 * to every kernel, or the pointer may exist at program scope or in constant memory. To abstract
 * these differences between devices and experiment with different layouts, macros are used.
 *
 * Use IntegratorState to pass a reference to the integrator state for the current path. These are
 * defined differently on the CPU and GPU. Use ConstIntegratorState instead of const
 * IntegratorState for passing state as read-only, to avoid oddities in typedef behavior.
 *
 * INTEGRATOR_STATE(state, x, y): read nested struct member x.y of IntegratorState
 * INTEGRATOR_STATE_WRITE(state, x, y): write to nested struct member x.y of IntegratorState
 *
 * INTEGRATOR_STATE_ARRAY(state, x, index, y): read x[index].y
 * INTEGRATOR_STATE_ARRAY_WRITE(state, x, index, y): write x[index].y
 */

#include "kernel/integrator/deep_camera_receipt.h"
#include "kernel/integrator/deep_compact_types.h"
#include "kernel/integrator/deep_frame_capture.h"
#include "kernel/integrator/deep_bounded_types.h"
#include "kernel/integrator/deep_optical_event.h"
#include "kernel/integrator/deep_surface_receipt.h"
#include "kernel/integrator/deep_volume_emission_span.h"
#include "kernel/integrator/deep_volume_radiance_receipt.h"
#include "kernel/types.h"

#include "util/types.h"

#if defined(__PATH_GUIDING__)
#  include "util/guiding.h"  // IWYU pragma: keep
#endif

#pragma once

CCL_NAMESPACE_BEGIN

/* Data structures */

/* Integrator State
 *
 * CPU rendering path state with AoS layout. */
struct IntegratorShadowStateCPU {
#define KERNEL_STRUCT_BEGIN(name) struct {
#define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) struct {
#define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) type name;
#define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#define KERNEL_STRUCT_ARRAY_MEMBER KERNEL_STRUCT_MEMBER
#define KERNEL_STRUCT_END(name) \
  } \
  name;
#define KERNEL_STRUCT_END_ARRAY(name, cpu_size, gpu_size) \
  } \
  name[cpu_size];
#define KERNEL_STRUCT_VOLUME_STACK_SIZE MAX_VOLUME_STACK_SIZE
#include "kernel/integrator/shadow_state_template.h"
#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
};

struct IntegratorStateCPU {
#define KERNEL_STRUCT_BEGIN(name) struct {
#define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) struct {
#define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) type name;
#define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#define KERNEL_STRUCT_ARRAY_MEMBER KERNEL_STRUCT_MEMBER
#define KERNEL_STRUCT_END(name) \
  } \
  name;
#define KERNEL_STRUCT_END_ARRAY(name, cpu_size, gpu_size) \
  } \
  name[cpu_size];
#define KERNEL_STRUCT_VOLUME_STACK_SIZE MAX_VOLUME_STACK_SIZE
#include "kernel/integrator/state_template.h"
#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_VOLUME_STACK_SIZE

  IntegratorShadowStateCPU shadow;
  IntegratorShadowStateCPU ao;
};

/* Path Queue
 *
 * Keep track of which kernels are queued to be executed next in the path
 * for GPU rendering. */
struct IntegratorQueueCounter {
  int num_queued[DEVICE_GPU_KERNEL_INTEGRATOR_NUM];
  int cache_miss;
  uint deep_camera_receipt_count;
  uint deep_camera_receipt_overflow;
  uint deep_surface_receipt_count;
  uint deep_surface_receipt_overflow;
  uint deep_volume_radiance_receipt_count;
  uint deep_volume_radiance_receipt_error;
  uint deep_volume_emission_span_count;
  uint deep_volume_emission_span_error;
  uint deep_optical_event_count;
  uint deep_optical_error;
  uint deep_optical_completed;
  uint deep_native_camera_reserved;
  uint deep_native_camera_written;
  uint deep_compact_reserved, deep_compact_written;
  uint deep_native_homogeneous_reserved;
  uint deep_native_homogeneous_written;
  uint deep_native_emission_calls;
  uint deep_native_auxiliary_film_calls;
  uint deep_native_volume_yields, deep_native_volume_retries;
  uint deep_native_volume_pending;
  uint deep_native_root_reserved;
  uint deep_native_root_written;
  uint deep_native_point_reserved;
  uint deep_native_point_written;
  uint deep_native_expression_source_reserved, deep_native_expression_source_written;
  uint deep_native_expression_node_reserved, deep_native_expression_node_written;
  uint deep_native_expression_call_reserved, deep_native_expression_call_written;
  uint deep_owned_optical_visibility_count;
  uint deep_bounded_camera_live;
  uint deep_native_capture_error;
  uint deep_native_validation_seen;
};

#if defined(__INTEGRATOR_GPU_PACKED_STATE__) && defined(__KERNEL_GPU__)

/* Generate wrapper structs for all integrator state fields. This allows us to access state
 * uniformly, regardless of whether it stored in a packed struct or separate arrays. */
#  define KERNEL_STRUCT_BEGIN(name)
#  define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
    struct Wrapped_##parent_struct##_##name { \
      type name; \
    };
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) \
    KERNEL_STRUCT_BEGIN(parent_struct) \
    KERNEL_STRUCT_MEMBER(parent_struct, packed_##parent_struct, packed, feature)
#  define KERNEL_STRUCT_ARRAY_MEMBER KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_END(name)
#  define KERNEL_STRUCT_END_ARRAY(name, cpu_size, gpu_size)
#  define KERNEL_STRUCT_VOLUME_STACK_SIZE MAX_VOLUME_STACK_SIZE

#  include "kernel/integrator/shadow_state_template.h"
#  include "kernel/integrator/state_template.h"

#  undef KERNEL_STRUCT_BEGIN
#  undef KERNEL_STRUCT_BEGIN_PACKED
#  undef KERNEL_STRUCT_MEMBER
#  undef KERNEL_STRUCT_MEMBER_PACKED
#  undef KERNEL_STRUCT_ARRAY_MEMBER
#  undef KERNEL_STRUCT_END
#  undef KERNEL_STRUCT_END_ARRAY
#  undef KERNEL_STRUCT_VOLUME_STACK_SIZE

#endif

/* Integrator State GPU
 *
 * GPU rendering path state with SoA layout. */
struct IntegratorStateGPU {
#define KERNEL_STRUCT_BEGIN(name) struct {

#ifdef __INTEGRATOR_GPU_PACKED_STATE__

#  ifdef __KERNEL_GPU__

/* If we've opted in to packed layouts, generate member functions that return a pointer to a
 * wrapper type so we can access state using uniform syntax. */
#    define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) \
      ccl_global Wrapped_##parent_struct##_##name *name; \
      ccl_device_inline ccl_global Wrapped_##parent_struct##_##name *name##_fn() ccl_constant \
      { \
        return (ccl_global Wrapped_##parent_struct##_##name *)name; \
      }
#    define KERNEL_STRUCT_MEMBER_PACKED(parent_struct, type, name, feature) \
      ccl_device_inline ccl_global packed_##parent_struct *name##_fn() ccl_constant \
      { \
        return (ccl_global packed_##parent_struct *)packed; \
      }
#  else
#    define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) ccl_global type *name;
#    define KERNEL_STRUCT_MEMBER_PACKED(parent_struct, type, name, feature)
#  endif

#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) \
    KERNEL_STRUCT_BEGIN(parent_struct) \
    KERNEL_STRUCT_MEMBER(parent_struct, packed_##parent_struct, packed, feature)

#else
#  define KERNEL_STRUCT_MEMBER(parent_struct, type, name, feature) ccl_global type *name;
#  define KERNEL_STRUCT_MEMBER_PACKED KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_BEGIN_PACKED(parent_struct, feature) KERNEL_STRUCT_BEGIN(parent_struct)
#endif
#define KERNEL_STRUCT_ARRAY_MEMBER KERNEL_STRUCT_MEMBER
#define KERNEL_STRUCT_END(name) \
  } \
  name;
#define KERNEL_STRUCT_END_ARRAY(name, cpu_size, gpu_size) \
  } \
  name[gpu_size];
#define KERNEL_STRUCT_VOLUME_STACK_SIZE MAX_VOLUME_STACK_SIZE

#include "kernel/integrator/state_template.h"

#include "kernel/integrator/shadow_state_template.h"

#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_BEGIN_PACKED
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_PACKED
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
#undef KERNEL_STRUCT_VOLUME_STACK_SIZE

  /* Count number of queued kernels. */
  ccl_global IntegratorQueueCounter *queue_counter;

  /* Count number of kernels queued for specific shaders. */
  ccl_global int *sort_key_counter[DEVICE_GPU_KERNEL_INTEGRATOR_NUM];

  /* Index of shadow path which will be used by a next shadow path. */
  ccl_global int *next_shadow_path_index;

  /* Index of main path which will be used by a next shadow catcher split. */
  ccl_global int *next_main_path_index;

  /* Partition/key offsets used when writing sorted active indices. */
  ccl_global int *sort_partition_key_offsets;

  /* Divisor used to partition active indices by locality when sorting by material. */
  uint sort_partition_divisor;

  /* Optional camera-input capture. No Optical traversal lives in these Beauty states. */
  ccl_global DeepCameraReceipt *deep_camera_receipts;
  uint deep_camera_receipt_capacity;
  /* Addresses remain stable until every main/shadow/MNEE queue in the domain is dead. */
  ccl_global DeepSurfaceReceipt *deep_surface_receipts;
  uint deep_surface_receipt_capacity;
  ccl_global DeepVolumeRadianceReceipt *deep_volume_radiance_receipts;
  uint deep_volume_radiance_receipt_capacity;
  ccl_global DeepVolumeEmissionSpan *deep_volume_emission_spans;
  uint deep_volume_emission_span_capacity;
  /* These pointers are populated ONLY in the separately allocated Optical descriptor. */
  ccl_global DeepCameraReceipt *deep_optical_inputs;
  ccl_global DeepOpticalEvent *deep_optical_events;
  ccl_global DeepOpticalLedger *deep_optical_ledger;
  uint deep_optical_event_capacity;
  /* Separate production arrays. No legacy receipt, Optical READY or Deep Film buffer owns them. */
  ccl_global DeepNativeCameraRecord *deep_native_cameras;
  ccl_global DeepNativeHomogeneousRecord *deep_native_homogeneous;
  ccl_global DeepNativeRootRecord *deep_native_roots;
  ccl_global DeepNativeRootRecord *deep_native_root_resident;
  uint deep_native_root_base, deep_native_root_resident_count;
  ccl_global DeepNativePointRecord *deep_native_points;
  ccl_global DeepNativeExpressionSource *deep_native_expression_sources;
  ccl_global DeepNativeExpressionNode *deep_native_expression_nodes;
  ccl_global DeepNativeExpressionCall *deep_native_expression_calls;
  uint deep_native_camera_capacity;
  uint deep_native_homogeneous_capacity;
  uint deep_native_root_capacity;
  uint deep_native_point_capacity;
  uint deep_native_homogeneous_base, deep_native_point_base;
  uint deep_native_validation_faults;
  uint deep_native_expression_source_capacity, deep_native_expression_node_capacity;
  uint deep_native_expression_call_capacity;
  uint deep_native_expression_source_base, deep_native_expression_node_base;
  uint deep_native_expression_call_base;
  ccl_global void *deep_native_volume_checkpoints;
  uint deep_native_volume_checkpoint_capacity, deep_native_volume_step_limit;
  uint64_t deep_native_domain_id;
  ccl_global DeepOwnedOpticalPiece *deep_owned_optical_pieces;
  ccl_global DeepOwnedOpticalVisibility *deep_owned_optical_visibility;
  ccl_global DeepOwnedOpticalCoverage *deep_owned_optical_coverage;
  ccl_global DeepOpticalCheckpoint *deep_owned_optical_checkpoints;
  uint deep_owned_optical_operation_steps;
  uint deep_owned_optical_visibility_capacity;
  uint deep_native_full_optical;
  uint deep_native_capture_rgb;
  const DeepCompactWitnessParameters *deep_compact_witness;
  const DeepCompactTraceParameters *deep_compact_trace;
  ccl_global DeepCompactPrimitive *deep_compact_primitives;
  ccl_global DeepCompactRayState *deep_compact_rays;
  uint deep_compact_capacity;
  uint deep_compact_has_volume;
  ccl_global DeepCompactStackEntry *deep_compact_stacks;
  ccl_global DeepNativeCameraRecord *deep_compact_cameras;
  uint deep_compact_camera_base, deep_compact_stack_stride;
  ccl_global DeepCompactRootColour *deep_compact_root_colours;
  uint deep_compact_root_colour_capacity;
  ccl_global DeepCompactEmissionState *deep_compact_emission;
  /* Immutable packet binding, replaced only after all native queues and AA join. */
  DeepBoundedBinding deep_bounded;
};

/* Abstraction
 *
 * Macros to access data structures on different devices.
 *
 * Note that there is a special access function for the shadow catcher state. This access is to
 * happen from a kernel which operates on a "main" path. Attempt to use shadow catcher accessors
 * from a kernel which operates on a shadow catcher state will cause bad memory access. */

#ifndef __KERNEL_GPU__

/* Scalar access on CPU. */

using IntegratorState = IntegratorStateCPU *;
using ConstIntegratorState = const IntegratorStateCPU *;
using IntegratorShadowState = IntegratorShadowStateCPU *;
using ConstIntegratorShadowState = const IntegratorShadowStateCPU *;
struct IntegratorBakeState {};
using ConstIntegratorBakeState = IntegratorBakeState;

#  define INTEGRATOR_STATE(state, nested_struct, member) ((state)->nested_struct.member)
#  define INTEGRATOR_STATE_WRITE(state, nested_struct, member) ((state)->nested_struct.member)

#  define INTEGRATOR_STATE_ARRAY(state, nested_struct, array_index, member) \
    ((state)->nested_struct[array_index].member)
#  define INTEGRATOR_STATE_ARRAY_WRITE(state, nested_struct, array_index, member) \
    ((state)->nested_struct[array_index].member)

#else /* !__KERNEL_GPU__ */

/* Array access on GPU with Structure-of-Arrays. */

using IntegratorState = int;
using ConstIntegratorState = int;

/* Shadow state is wrapped in a struct to support function overloading and templates. */
struct IntegratorShadowState {
  ccl_device_inline_method IntegratorShadowState() {}
  ccl_device_inline_method IntegratorShadowState(int state) : state(state) {}
  ccl_device_inline_method operator int() const
  {
    return state;
  }
  int state;
};
using ConstIntegratorShadowState = IntegratorShadowState;

struct IntegratorBakeState {};
using ConstIntegratorBakeState = IntegratorBakeState;

#  ifdef __INTEGRATOR_GPU_PACKED_STATE__

/* If we've opted in to packed layouts, we use the generated accessor functions (member##_fn) to
 * resolve different layouts (packed vs separate). */
#    define INTEGRATOR_STATE(state, nested_struct, member) \
      kernel_integrator_state.nested_struct.member##_fn()[state].member
#    define INTEGRATOR_STATE_ARRAY(state, nested_struct, array_index, member) \
      kernel_integrator_state.nested_struct[array_index].member##_fn()[state].member
#  else
#    define INTEGRATOR_STATE(state, nested_struct, member) \
      kernel_integrator_state.nested_struct.member[state]
#    define INTEGRATOR_STATE_ARRAY(state, nested_struct, array_index, member) \
      kernel_integrator_state.nested_struct[array_index].member[state]
#  endif

#  define INTEGRATOR_STATE_WRITE(state, nested_struct, member) \
    INTEGRATOR_STATE(state, nested_struct, member)
#  define INTEGRATOR_STATE_ARRAY_WRITE(state, nested_struct, array_index, member) \
    INTEGRATOR_STATE_ARRAY(state, nested_struct, array_index, member)

#endif /* !__KERNEL_GPU__ */

CCL_NAMESPACE_END
