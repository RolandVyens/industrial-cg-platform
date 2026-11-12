/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_OPTIX

#  include "device/optix/queue.h"
#  include "device/optix/device_impl.h"
#  include <cstdlib>
#  include <cstring>

#  define __KERNEL_OPTIX__
#  include "kernel/device/optix/globals.h"

CCL_NAMESPACE_BEGIN

/* CUDADeviceQueue */

OptiXDeviceQueue::OptiXDeviceQueue(OptiXDevice *device)
    : CUDADeviceQueue(device, 64 * 65536 + 64 * sizeof(void *))
{
  const char *option = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_OPTIX_PARAMETER_STAGING");
  const char *frame = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_PIPELINE");
  const bool legacy_route = frame && std::strcmp(frame, "1") == 0;
  const char *defaults = getenv("CYCLES_TEST_DEEP_DEMAND_DEFAULTS");
  const bool demand_defaults = !defaults || std::strcmp(defaults, "0") != 0;
  parameter_staging_ = option ? std::strcmp(option, "1") == 0 :
                                legacy_route || demand_defaults;
  parameter_staging_requires_deep_policy_ = !option && !legacy_route;
}

OptiXDeviceQueue::~OptiXDeviceQueue()
{
  if (parameter_block_count_) {
    const CUDAContextScope scope(cuda_device_);
    /* Also drain after submission/readback failure before any host snapshot is freed. */
    cuda_device_assert(cuda_device_, cuStreamSynchronize(cuda_stream_));
    for (size_t i = 0; i < parameter_block_count_; ++i)
      free_deep_auxiliary(parameter_blocks_[i], 65536);
  }
}

bool OptiXDeviceQueue::synchronize()
{
  const bool complete = CUDADeviceQueue::synchronize();
  if (complete) parameter_cursor_ = 0;
  return complete;
}

const void *OptiXDeviceQueue::stage_parameter(const void *source, const size_t bytes)
{
  constexpr size_t block_bytes = 65536, max_blocks = 64;
  if (!bytes || bytes > block_bytes) {
    cuda_device_->set_error("Invalid Deep OptiX parameter snapshot range"); return nullptr;
  }
  const size_t aligned = (bytes + 15) & ~size_t(15);
  if (parameter_cursor_ % block_bytes + aligned > block_bytes)
    parameter_cursor_ = (parameter_cursor_ + block_bytes - 1) & ~(block_bytes - 1);
  const size_t index = parameter_cursor_ / block_bytes;
  if (index >= max_blocks) {
    cuda_device_->set_error("Deep OptiX parameter staging exceeds bounded pinned capacity");
    return nullptr;
  }
  if (index == parameter_block_count_) {
    void *block = allocate_deep_auxiliary(block_bytes);
    if (!block) return nullptr;
    /* Fixed metadata registration cannot throw after the allocation. */
    parameter_blocks_[parameter_block_count_++] = block;
  }
  char *destination = static_cast<char *>(parameter_blocks_[index]) + parameter_cursor_ % block_bytes;
  std::memcpy(destination, source, bytes);
  parameter_cursor_ += aligned;
  return destination;
}

void OptiXDeviceQueue::init_execution()
{
  CUDADeviceQueue::init_execution();
}

static bool is_optix_specific_kernel(DeviceKernel kernel, bool osl_shading, bool osl_camera)
{
#  ifdef WITH_OSL
  /* OSL uses direct callables to execute, so shading needs to be done in OptiX if OSL is used. */
  if (osl_shading && device_kernel_has_shading(kernel)) {
    return true;
  }
  if (osl_camera && kernel == DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA) {
    return true;
  }
#  else
  (void)osl_shading;
  (void)osl_camera;
#  endif

  return device_kernel_has_intersection(kernel);
}

bool OptiXDeviceQueue::enqueue(DeviceKernel kernel,
                               const int work_size,
                               const DeviceKernelArguments &args)
{
  OptiXDevice *const optix_device = static_cast<OptiXDevice *>(cuda_device_);

#  ifdef WITH_OSL
  const OSLGlobals *og = static_cast<const OSLGlobals *>(optix_device->get_cpu_osl_memory());
  const bool osl_shading = og->use_shading;
  const bool osl_camera = og->use_camera;
#  else
  const bool osl_shading = false;
  const bool osl_camera = false;
#  endif

  if (!is_optix_specific_kernel(kernel, osl_shading, osl_camera)) {
    return CUDADeviceQueue::enqueue(kernel, work_size, args);
  }

  if (cuda_device_->have_error()) {
    return false;
  }

  debug_enqueue_begin(kernel, work_size);

  const CUDAContextScope scope(cuda_device_);

  const device_ptr sbt_data_ptr = optix_device->sbt_data.device_pointer;
  const device_ptr launch_params_ptr = optix_device->launch_params.device_pointer;
  const bool use_parameter_staging = parameter_staging_ &&
      (!parameter_staging_requires_deep_policy_ || cuda_device_->deep_bounded_policy());

  auto set_launch_param = [&](size_t offset, size_t size, int arg) {
    if (cuda_device_->have_error()) return;
    const void *source = use_parameter_staging ? stage_parameter(args.values[arg], size) : args.values[arg];
    if (!source) return;
    cuda_device_assert(
        cuda_device_,
        cuMemcpyHtoDAsync(launch_params_ptr + offset, source, size, cuda_stream_));
  };

  set_launch_param(offsetof(KernelParamsOptiX, path_index_array), sizeof(device_ptr), 0);

  if (kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST || device_kernel_has_shading(kernel)) {
    set_launch_param(offsetof(KernelParamsOptiX, render_buffer), sizeof(device_ptr), 1);
  }
  if (kernel == DEVICE_KERNEL_SHADER_EVAL_DISPLACE ||
      kernel == DEVICE_KERNEL_SHADER_EVAL_BACKGROUND ||
      kernel == DEVICE_KERNEL_SHADER_EVAL_CURVE_SHADOW_TRANSPARENCY ||
      kernel == DEVICE_KERNEL_SHADER_EVAL_VOLUME_DENSITY)
  {
    set_launch_param(offsetof(KernelParamsOptiX, shader_eval_cache_miss), sizeof(device_ptr), 2);
    set_launch_param(offsetof(KernelParamsOptiX, shader_eval_offset), sizeof(int32_t), 3);
  }

  if (kernel == DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA) {
    /* Tile-less native retries carry compacted state indices in argument four.
     * Receipt replay also has no tiles, but its wrapper disposition takes precedence. */
    const int num_tiles = *static_cast<const int *>(args.values[1]);
    if (num_tiles == 0) {
      set_launch_param(offsetof(KernelParamsOptiX, path_index_array), sizeof(device_ptr), 4);
    }
    set_launch_param(offsetof(KernelParamsOptiX, num_tiles), sizeof(int32_t), 1);
    set_launch_param(offsetof(KernelParamsOptiX, render_buffer), sizeof(device_ptr), 2);
    set_launch_param(offsetof(KernelParamsOptiX, max_tile_work_size), sizeof(int32_t), 3);
  }

  if (!use_parameter_staging)
    cuda_device_assert(cuda_device_, cuStreamSynchronize(cuda_stream_));
  if (cuda_device_->have_error()) return false;

  OptixPipeline pipeline = nullptr;
  OptixShaderBindingTable sbt_params = {};

  switch (kernel) {
    case DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_BACKGROUND * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_LIGHT_NEE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_FORWARD:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_LIGHT_FORWARD * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_SURFACE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_SURFACE_RAYTRACE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_MNEE:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INTERSECT_MNEE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_VOLUME * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr +
                                PG_RGEN_SHADE_VOLUME_RAY_MARCHING * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEEP_VOLUME_OPTICAL:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_DEEP_VOLUME_OPTICAL * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_SHADOW * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_SHADE_DEDICATED_LIGHT:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_SHADE_DEDICATED_LIGHT * sizeof(SbtRecord);
      break;

    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST:
      pipeline = optix_device->pipelines[PIP_INTERSECT];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INTERSECT_CLOSEST * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW:
      pipeline = optix_device->pipelines[PIP_INTERSECT];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INTERSECT_SHADOW * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE:
      pipeline = optix_device->pipelines[PIP_INTERSECT];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INTERSECT_SUBSURFACE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK:
      pipeline = optix_device->pipelines[PIP_INTERSECT];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INTERSECT_VOLUME_STACK * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_DEDICATED_LIGHT:
      pipeline = optix_device->pipelines[PIP_INTERSECT];
      sbt_params.raygenRecord = sbt_data_ptr +
                                PG_RGEN_INTERSECT_DEDICATED_LIGHT * sizeof(SbtRecord);
      break;

    case DEVICE_KERNEL_SHADER_EVAL_DISPLACE:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_EVAL_DISPLACE * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_SHADER_EVAL_BACKGROUND:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_EVAL_BACKGROUND * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_SHADER_EVAL_CURVE_SHADOW_TRANSPARENCY:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr +
                                PG_RGEN_EVAL_CURVE_SHADOW_TRANSPARENCY * sizeof(SbtRecord);
      break;
    case DEVICE_KERNEL_SHADER_EVAL_VOLUME_DENSITY:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_EVAL_VOLUME_DENSITY * sizeof(SbtRecord);
      break;

    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA:
      pipeline = optix_device->pipelines[PIP_SHADE];
      sbt_params.raygenRecord = sbt_data_ptr + PG_RGEN_INIT_FROM_CAMERA * sizeof(SbtRecord);
      break;

    default:
      LOG_ERROR << "Invalid kernel " << device_kernel_as_string(kernel)
                << " is attempted to be enqueued.";
      return false;
  }

  sbt_params.missRecordBase = sbt_data_ptr + MISS_PROGRAM_GROUP_OFFSET * sizeof(SbtRecord);
  sbt_params.missRecordStrideInBytes = sizeof(SbtRecord);
  sbt_params.missRecordCount = NUM_MISS_PROGRAM_GROUPS;
  sbt_params.hitgroupRecordBase = sbt_data_ptr + HIT_PROGAM_GROUP_OFFSET * sizeof(SbtRecord);
  sbt_params.hitgroupRecordStrideInBytes = sizeof(SbtRecord);
  sbt_params.hitgroupRecordCount = NUM_HIT_PROGRAM_GROUPS;
  sbt_params.callablesRecordBase = sbt_data_ptr + CALLABLE_PROGRAM_GROUPS_BASE * sizeof(SbtRecord);
  sbt_params.callablesRecordCount = NUM_CALLABLE_PROGRAM_GROUPS;
  sbt_params.callablesRecordStrideInBytes = sizeof(SbtRecord);

#  ifdef WITH_OSL
  if (osl_shading || osl_camera) {
    sbt_params.callablesRecordCount += static_cast<unsigned int>(optix_device->osl_groups.size());
  }
#  endif

  /* Launch the ray generation program. */
  if (!begin_native_timing(kernel, work_size)) return false;
  optix_device_assert(optix_device,
                      optixLaunch(pipeline,
                                  cuda_stream_,
                                  launch_params_ptr,
                                  optix_device->launch_params.data_elements,
                                  &sbt_params,
                                  work_size,
                                  1,
                                  1));
  if (!end_native_timing()) return false;

  debug_enqueue_end();

  return !(optix_device->have_error());
}

CCL_NAMESPACE_END

#endif /* WITH_OPTIX */
