/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_CUDA

#  include <string_view>

#  include "device/cuda/kernel.h"
#  include "device/cuda/device_impl.h"

CCL_NAMESPACE_BEGIN

void CUDADeviceKernels::load(CUDADevice *device)
{
  CUmodule cuModule = device->cuModule;

  for (int i = 0; i < (int)DEVICE_KERNEL_NUM; i++) {
    CUDADeviceKernel &kernel = kernels_[i];

    if (!device_kernel_has_gpu_function((DeviceKernel)i)) {
      continue;
    }

    const std::string function_name = std::string("kernel_gpu_") +
                                      device_kernel_as_string((DeviceKernel)i);
    const bool volume = i == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME ||
                        i == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME_RAY_MARCHING;
    CUmodule selected = volume && device->cuVolumeModule ? device->cuVolumeModule : cuModule;
    if (const char *group = device->deep_module_test_group_) {
      const std::string_view name = device_kernel_as_string((DeviceKernel)i);
      const std::string_view kind = group;
      const bool surface = name.starts_with("integrator_shade_surface");
      const bool active = (kind == "camera" && name == "integrator_init_from_camera") ||
                          (kind == "volume" && volume) || (kind == "surface" && surface) ||
                          (kind == "other" && !volume && !surface &&
                           name != "integrator_init_from_camera");
      if (!active) {
        selected = volume && device->volume_policy_modules_[0] ?
                       device->volume_policy_modules_[0] :
                       device->deep_policy_modules_[0];
      }
    }
    cuda_device_assert(device,
                       cuModuleGetFunction(&kernel.function, selected, function_name.c_str()));

    if (kernel.function) {
      cuda_device_assert(device, cuFuncSetCacheConfig(kernel.function, CU_FUNC_CACHE_PREFER_L1));

      cuda_device_assert(
          device,
          cuOccupancyMaxPotentialBlockSize(
              &kernel.min_blocks, &kernel.num_threads_per_block, kernel.function, nullptr, 0, 0));
    }
    else {
      LOG_ERROR << "Unable to load kernel " << function_name;
    }
  }

  loaded = true;
}

const CUDADeviceKernel &CUDADeviceKernels::get(DeviceKernel kernel) const
{
  return kernels_[(int)kernel];
}

bool CUDADeviceKernels::available(DeviceKernel kernel) const
{
  return kernels_[(int)kernel].function != nullptr;
}

CCL_NAMESPACE_END

#endif /* WITH_CUDA */
