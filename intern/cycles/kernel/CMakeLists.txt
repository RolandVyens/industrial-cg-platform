# SPDX-FileCopyrightText: 2011-2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

set(INC
  ..
)

set(INC_SYS

)

set(SRC_KERNEL_DEVICE_GPU_HEADERS
  device/gpu/block_sizes.h
  device/gpu/image.h
  device/gpu/kernel.h
  device/gpu/parallel_active_index.h
  device/gpu/parallel_prefix_sum.h
  device/gpu/parallel_sorted_index.h
  device/gpu/work_stealing.h
)

set(SRC_KERNEL_CLOSURE_HEADERS
  closure/alloc.h
  closure/bsdf.h
  closure/bsdf_ashikhmin_velvet.h
  closure/bsdf_burley.h
  closure/bsdf_diffuse.h
  closure/bsdf_diffuse_ramp.h
  closure/bsdf_microfacet.h
  closure/bsdf_oren_nayar.h
  closure/bsdf_phong_ramp.h
  closure/bsdf_ray_portal.h
  closure/bsdf_sheen.h
  closure/bsdf_toon.h
  closure/bsdf_transparent.h
  closure/bsdf_util.h
  closure/bsdf_ashikhmin_shirley.h
  closure/bsdf_hair.h
  closure/bssrdf.h
  closure/emissive.h
  closure/volume.h
  closure/volume_util.h
  closure/volume_henyey_greenstein.h
  closure/volume_rayleigh.h
  closure/volume_fournier_forand.h
  closure/volume_draine.h
  closure/bsdf_principled_hair_chiang.h
  closure/bsdf_principled_hair_huang.h
)

set(SRC_KERNEL_SVM_HEADERS
  svm/svm.h
  svm/ao.h
  svm/aov.h
  svm/attribute.h
  svm/bevel.h
  svm/blackbody.h
  svm/boolean_math.h
  svm/bump.h
  svm/camera.h
  svm/clamp.h
  svm/closure.h
  svm/convert.h
  svm/checker.h
  svm/color_util.h
  svm/brick.h
  svm/displace.h
  svm/fresnel.h
  svm/wireframe.h
  svm/wavelength.h
  svm/gabor.h
  svm/gamma.h
  svm/brightness.h
  svm/geometry.h
  svm/gradient.h
  svm/hsv.h
  svm/ies.h
  svm/image.h
  svm/invert.h
  svm/light_path.h
  svm/magic.h
  svm/map_range.h
  svm/mapping.h
  svm/mapping_util.h
  svm/math.h
  svm/math_util.h
  svm/integer_math.h
  svm/mix.h
  svm/node_types.h
  svm/node_types_template.h
  svm/noise.h
  svm/noisetex.h
  svm/normal.h
  svm/ramp.h
  svm/ramp_util.h
  svm/radial_tiling.h
  svm/radial_tiling_shared.h
  svm/raycast.h
  svm/scene_time.h
  svm/sepcomb_color.h
  svm/sepcomb_vector.h
  svm/sky.h
  svm/tex_coord.h
  svm/fractal_noise.h
  svm/types.h
  svm/util.h
  svm/value.h
  svm/vector_rotate.h
  svm/vector_transform.h
  svm/voronoi.h
  svm/wave.h
  svm/white_noise.h
)

if(WITH_CYCLES_OSL)
  set(SRC_KERNEL_OSL_HEADERS
    osl/osl.h
    osl/camera.h
    osl/closures_setup.h
    osl/closures_template.h
    osl/services_gpu.h
    osl/strings.h
    osl/types.h
  )
endif()

set(SRC_KERNEL_GEOM_HEADERS
  geom/attribute.h
  geom/curve.h
  geom/curve_intersect.h
  geom/geom_intersect.h
  geom/gsplat.h
  geom/gsplat_intersect.h
  geom/motion_curve.h
  geom/motion_gsplat.h
  geom/motion_point.h
  geom/motion_triangle.h
  geom/motion_triangle_intersect.h
  geom/motion_triangle_shader.h
  geom/object.h
  geom/point.h
  geom/point_intersect.h
  geom/primitive.h
  geom/shader_data.h
  geom/triangle.h
  geom/triangle_intersect.h
  geom/volume.h
)

set(SRC_KERNEL_BAKE_HEADERS
  bake/bake.h
)

set(SRC_KERNEL_BVH_HEADERS
  bvh/bvh.h
  bvh/intersect_filter.h
  bvh/nodes.h
  bvh/shadow_all.h
  bvh/local.h
  bvh/traversal.h
  bvh/types.h
  bvh/util.h
  bvh/volume.h
  bvh/volume_all.h
)

set(SRC_KERNEL_CAMERA_HEADERS
  camera/camera.h
  camera/projection.h
)

set(SRC_KERNEL_FILM_HEADERS
  film/adaptive_sampling.h
  film/aov_passes.h
  film/data_passes.h
  film/deep_write.h
  film/denoising_passes.h
  film/volume_guiding_denoise.h
  film/cryptomatte_passes.h
  film/light_passes.h
  film/read.h
  film/write.h
)

set(SRC_KERNEL_INTEGRATOR_HEADERS
  integrator/deep_bounded_schema.h
  integrator/deep_bounded_types.h
  integrator/deep_bounded_capture.h
  integrator/deep_bounded_records.h
  integrator/deep_bounded_numeric.h
  integrator/deep_camera_receipt.h
  integrator/deep_surface_receipt.h
  integrator/deep_volume_radiance_receipt.h
  integrator/deep_volume_emission_span.h
  integrator/deep_compact_capture.h
  integrator/deep_compact_witness.h
  integrator/deep_compact_trace.h
  integrator/deep_compact_cases.h
  integrator/deep_compact_types.h
  integrator/deep_gpu_frame.h
    integrator/deep_optical_capture.h
  integrator/deep_optical_event.h
  integrator/displacement_shader.h
  integrator/init_from_bake.h
  integrator/init_from_camera.h
  integrator/intersect_dedicated_light.h
  integrator/intersect_closest.h
  integrator/intersect_mnee.h
  integrator/intersect_shadow.h
  integrator/intersect_subsurface.h
  integrator/intersect_volume_stack.h
  integrator/guiding.h
  integrator/megakernel.h
  integrator/mnee.h
  integrator/path_state.h
  integrator/shade_background.h
  integrator/shade_light.h
  integrator/shade_shadow.h
  integrator/shade_surface.h
  integrator/shade_volume.h
  integrator/shade_dedicated_light.h
  integrator/shadow_catcher.h
  integrator/shadow_color.h
  integrator/shadow_linking.h
  integrator/shadow_state.h
  integrator/shadow_state_template.h
  integrator/state_flow.h
  integrator/state.h
  integrator/state_template.h
  integrator/state_util.h
  integrator/subsurface_disk.h
  integrator/subsurface.h
  integrator/subsurface_random_walk.h
  integrator/surface_shader.h
  integrator/volume_shader.h
  integrator/volume_stack.h
)

set(SRC_KERNEL_LIGHT_HEADERS
  light/area.h
  light/background.h
  light/common.h
  light/distribution.h
  light/light.h
  light/point.h
  light/sample.h
  light/spot.h
  light/sun.h
  light/tree.h
  light/triangle.h
)

set(SRC_KERNEL_SAMPLE_HEADERS
  sample/lcg.h
  sample/mapping.h
  sample/mis.h
  sample/pattern.h
  sample/sobol_burley.h
  sample/tabulated_sobol.h
  sample/util.h
)

set(SRC_KERNEL_UTIL_HEADERS
  util/colorspace.h
  util/differential.h
  util/ies.h
  util/image.h
  util/image_2d.h
  util/image_3d.h
  util/lookup_table.h
  util/nanovdb.h
  util/profiler.h
)

set(SRC_KERNEL_TYPES_HEADERS
  constants.h
  data_arrays.h
  data_template.h
  features.h
  globals.h
  image.h
  tables.h
  types.h
)

set(SRC_KERNEL_HEADERS
  ${SRC_KERNEL_BAKE_HEADERS}
  ${SRC_KERNEL_BVH_HEADERS}
  ${SRC_KERNEL_CAMERA_HEADERS}
  ${SRC_KERNEL_CLOSURE_HEADERS}
  ${SRC_KERNEL_FILM_HEADERS}
  ${SRC_KERNEL_GEOM_HEADERS}
  ${SRC_KERNEL_INTEGRATOR_HEADERS}
  ${SRC_KERNEL_LIGHT_HEADERS}
  ${SRC_KERNEL_OSL_HEADERS}
  ${SRC_KERNEL_SAMPLE_HEADERS}
  ${SRC_KERNEL_SVM_HEADERS}
  ${SRC_KERNEL_TYPES_HEADERS}
  ${SRC_KERNEL_UTIL_HEADERS}
)

set(SRC_UTIL_HEADERS
  ../util/atomic.h
  ../util/color.h
  ../util/defines.h
  ../util/guiding.h
  ../util/half.h
  ../util/hash.h
  ../util/math.h
  ../util/math_base.h
  ../util/math_fast.h
  ../util/math_intersect.h
  ../util/math_float2.h
  ../util/math_float3.h
  ../util/math_float4.h
  ../util/math_float8.h
  ../util/math_int2.h
  ../util/math_int3.h
  ../util/math_int4.h
  ../util/math_int8.h
  ../util/math_dual.h
  ../util/math_float3x3.h
  ../util/math_quaternion.h
  ../util/math_spherical_harmonics.h
  ../util/projection.h
  ../util/projection_inverse.h
  ../util/rect.h
  ../util/static_assert.h
  ../util/transform.h
  ../util/types.h
  ../util/types_base.h
  ../util/types_float2.h
  ../util/types_float3.h
  ../util/types_float4.h
  ../util/types_float8.h
  ../util/types_gpu_compressed.h
  ../util/types_image.h
  ../util/types_int2.h
  ../util/types_int3.h
  ../util/types_int4.h
  ../util/types_int8.h
  ../util/types_normal.h
  ../util/types_spectrum.h
  ../util/types_spherical_harmonics.h
  ../util/types_rgbe.h
  ../util/types_uchar2.h
  ../util/types_uchar3.h
  ../util/types_uchar4.h
  ../util/types_uint2.h
  ../util/types_uint3.h
  ../util/types_uint4.h
  ../util/types_ushort4.h
  ../util/types_dual.h
  ../util/types_float3x3.h
  ../util/types_quaternion.h
)

set(LIB

)

# `Zstd` compressor for kernels.
add_executable(zstd_compress ../cmake/zstd_compress.cpp)

target_link_libraries(zstd_compress PRIVATE bf::dependencies::zstd)
target_link_libraries(zstd_compress PRIVATE bf::dependencies::pthreads)

if(NOT WITH_BLENDER)
  # For the Cycles standalone put libraries next to the Cycles application.
  set(cycles_kernel_runtime_lib_target_path ${CYCLES_INSTALL_PATH})
else()
  # For Blender put the libraries next to the Blender executable.
  #
  # Note that the installation path in the delayed_install is relative to the versioned folder,
  # which means we need to go one level up.
  set(cycles_kernel_runtime_lib_target_path "../")
endif()

if(UNIX AND NOT APPLE)
  set(cycles_kernel_runtime_lib_target_path ${cycles_kernel_runtime_lib_target_path}/lib)
endif()

if(WITH_COMPILER_ASAN)
  if((CMAKE_C_COMPILER_ID STREQUAL "GNU") AND (NOT WITH_CYCLES_KERNEL_ASAN))
    # GCC hangs compiling the big kernel files with ASAN and release, so disable by default.
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -fno-sanitize=all")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fno-sanitize=vptr")
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # With OSL, Cycles disables RTTI in some modules, which then breaks at linking
    # when trying to use `vptr` sanitizer (included into 'undefined' general option).
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -fno-sanitize=vptr")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fno-sanitize=vptr")
  endif()
endif()

# -----------------------------------------------------------------------------
# Create module with headers common for all compute backends.

add_library(cycles_kernel INTERFACE)
target_sources(cycles_kernel INTERFACE
  ${SRC_KERNEL_HEADERS}
  ${SRC_KERNEL_DEVICE_GPU_HEADERS}
  ${SRC_KERNEL_UTIL_HEADERS}
)

source_group("bake" FILES ${SRC_KERNEL_BAKE_HEADERS})
source_group("bvh" FILES ${SRC_KERNEL_BVH_HEADERS})
source_group("camera" FILES ${SRC_KERNEL_CAMERA_HEADERS})
source_group("closure" FILES ${SRC_KERNEL_CLOSURE_HEADERS})
source_group("device\\gpu" FILES ${SRC_KERNEL_DEVICE_GPU_HEADERS})
source_group("film" FILES ${SRC_KERNEL_FILM_HEADERS})
source_group("geom" FILES ${SRC_KERNEL_GEOM_HEADERS})
source_group("integrator" FILES ${SRC_KERNEL_INTEGRATOR_HEADERS})
source_group("kernel" FILES ${SRC_KERNEL_TYPES_HEADERS})
source_group("light" FILES ${SRC_KERNEL_LIGHT_HEADERS})
source_group("osl" FILES ${SRC_KERNEL_OSL_HEADERS})
source_group("sample" FILES ${SRC_KERNEL_SAMPLE_HEADERS})
source_group("svm" FILES ${SRC_KERNEL_SVM_HEADERS})
source_group("util" FILES ${SRC_KERNEL_UTIL_HEADERS})

set(_cycles_prev_gpu_kernel_target)
macro(cycles_add_gpu_kernel_dependencies GPU_KERNEL_TARGET)
  if(TARGET ${GPU_KERNEL_TARGET})
    add_dependencies(cycles_kernel ${GPU_KERNEL_TARGET})
    if(_cycles_prev_gpu_kernel_target)
      add_dependencies(${GPU_KERNEL_TARGET} ${_cycles_prev_gpu_kernel_target})
    endif()
    set(_cycles_prev_gpu_kernel_target ${GPU_KERNEL_TARGET})
  endif()
endmacro()

cycles_add_gpu_kernel_dependencies(cycles_kernel_cuda)
cycles_add_gpu_kernel_dependencies(cycles_kernel_optix)
cycles_add_gpu_kernel_dependencies(cycles_kernel_hip)
cycles_add_gpu_kernel_dependencies(cycles_kernel_hiprt)
cycles_add_gpu_kernel_dependencies(cycles_kernel_oneapi)

# Device-resident Deep numerical module, built and installed with the owning
# OptiX runtime. Do not rely on a retained experimental PTX from another ABI.
if(WITH_CYCLES_DEVICE_OPTIX AND CUDA_NVCC_EXECUTABLE)
  set(deep_bounded_ptx "${CMAKE_CURRENT_BINARY_DIR}/deep_bounded_7.ptx")
  set(deep_bounded_source "${CMAKE_CURRENT_SOURCE_DIR}/../device/cuda/deep_bounded.cu")
  add_custom_command(
    OUTPUT "${deep_bounded_ptx}"
    COMMAND ${CUDA_NVCC_EXECUTABLE} -std=c++17 --ptx -arch=compute_50
      -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
      -I "${CMAKE_CURRENT_SOURCE_DIR}/.." -o "${deep_bounded_ptx}" "${deep_bounded_source}"
    DEPENDS "${deep_bounded_source}" ${SRC_KERNEL_INTEGRATOR_HEADERS}
    VERBATIM
  )
  set(deep_bounded_binaries "${deep_bounded_ptx}")
  get_filename_component(deep_bounded_cuda_bin "${CUDA_NVCC_EXECUTABLE}" DIRECTORY)
  set(deep_bounded_ptxas "${deep_bounded_cuda_bin}/ptxas${CMAKE_EXECUTABLE_SUFFIX}")
  foreach(arch ${CYCLES_CUDA_BINARIES_ARCH})
    if(arch MATCHES "^sm_[0-9]+$")
      set(deep_bounded_cubin "${CMAKE_CURRENT_BINARY_DIR}/deep_bounded_7_${arch}.cubin")
      add_custom_command(
        OUTPUT "${deep_bounded_cubin}"
        COMMAND "${deep_bounded_ptxas}" -arch=${arch} -O3
          "${deep_bounded_ptx}" -o "${deep_bounded_cubin}"
        DEPENDS "${deep_bounded_ptx}"
        VERBATIM
      )
      list(APPEND deep_bounded_binaries "${deep_bounded_cubin}")
    endif()
  endforeach()
  add_custom_target(cycles_kernel_deep_bounded ALL DEPENDS ${deep_bounded_binaries})
  add_dependencies(cycles_kernel cycles_kernel_deep_bounded)
  if(WITH_BLENDER)
    set(deep_bounded_runtime
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${BLENDER_VERSION}/${CYCLES_INSTALL_PATH}/lib")
    add_custom_command(TARGET cycles_kernel_deep_bounded POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${deep_bounded_runtime}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${deep_bounded_binaries} "${deep_bounded_runtime}"
      VERBATIM
    )
  endif()
  delayed_install("${CMAKE_CURRENT_BINARY_DIR}" "${deep_bounded_binaries}" "${CYCLES_INSTALL_PATH}/lib")
endif()

if(WITH_CYCLES_DEEP_REFERENCE_GPU AND
   (NOT WITH_CYCLES_DEVICE_OPTIX OR NOT CUDA_NVCC_EXECUTABLE))
  message(FATAL_ERROR "Deep GPU reference module requires OptiX and NVCC")
endif()

if(WITH_CYCLES_DEEP_REFERENCE_GPU)
  set(deep_frame_ptx "${CMAKE_CURRENT_BINARY_DIR}/deep_frame_reduce_44474603.ptx")
  set(deep_frame_source_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
  set(deep_frame_sources
    "${deep_frame_source_root}/device/cuda/deep_frame_reduce.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_encode.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_native.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_grid.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_compact.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_colour.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_simple.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_compact_encode.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_sort.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_archive.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_witness.cu"
    "${deep_frame_source_root}/device/cuda/deep_frame_cases.cu"
    "${deep_frame_source_root}/device/cuda/deep_compact_fit.cuh"
    "${deep_frame_source_root}/device/cuda/deep_compact_trace.cuh"
    "${deep_frame_source_root}/kernel/integrator/deep_compact_types.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/integrator/deep_gpu_frame.h"
  )
  add_custom_command(OUTPUT "${deep_frame_ptx}"
    COMMAND "${CUDA_NVCC_EXECUTABLE}" --ptx --gpu-architecture=compute_75
            --std=c++17 --fmad=true --ftz=false -Wno-deprecated-gpu-targets
            "-I${deep_frame_source_root}"
            "${deep_frame_source_root}/device/cuda/deep_frame_reduce.cu"
            -o "${deep_frame_ptx}"
    DEPENDS ${deep_frame_sources}
    VERBATIM
  )
  add_custom_target(cycles_kernel_deep_frame ALL DEPENDS "${deep_frame_ptx}")
  add_dependencies(cycles_kernel cycles_kernel_deep_frame)
  if(WITH_BLENDER)
    set(deep_frame_runtime
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${BLENDER_VERSION}/${CYCLES_INSTALL_PATH}/lib")
    add_custom_command(TARGET cycles_kernel_deep_frame POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${deep_frame_runtime}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${deep_frame_ptx}" "${deep_frame_runtime}"
      VERBATIM
    )
  endif()
  delayed_install("${CMAKE_CURRENT_BINARY_DIR}" "${deep_frame_ptx}" "${CYCLES_INSTALL_PATH}/lib")
endif()

# Copy precompiled kernel binaries into the Blender add-on runtime folder for local builds.
if(WITH_BLENDER)
  set(cycles_kernel_runtime_build_lib
      "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${BLENDER_VERSION}/${CYCLES_INSTALL_PATH}/lib")

  set(cycles_kernel_runtime_files_cuda)
  set(cycles_kernel_runtime_files_optix)
  set(cycles_kernel_runtime_files_hip)
  set(cycles_kernel_runtime_files_hiprt)

  if(WITH_CYCLES_CUDA_BINARIES)
    foreach(_file IN LISTS cuda_cubins)
      list(APPEND cycles_kernel_runtime_files_cuda "${_file}")
    endforeach()
  endif()

  if(WITH_CYCLES_DEVICE_OPTIX)
    foreach(_file IN LISTS optix_ptx)
      list(APPEND cycles_kernel_runtime_files_optix "${_file}")
    endforeach()
    foreach(_file IN LISTS cuda_cubins)
      list(APPEND cycles_kernel_runtime_files_optix "${_file}")
    endforeach()
  endif()

  if(WITH_CYCLES_DEVICE_HIP)
    foreach(_file IN LISTS hip_fatbins)
      list(APPEND cycles_kernel_runtime_files_hip "${_file}")
    endforeach()
  endif()

  if(WITH_CYCLES_DEVICE_HIPRT)
    foreach(_file IN LISTS hiprt_hipfb)
      list(APPEND cycles_kernel_runtime_files_hiprt "${_file}")
    endforeach()
  endif()

  function(cycles_kernel_normalize_files out_var)
    set(_out)
    foreach(_file IN LISTS ARGN)
      if(IS_ABSOLUTE "${_file}")
        list(APPEND _out "${_file}")
      else()
        list(APPEND _out "${CMAKE_CURRENT_BINARY_DIR}/${_file}")
      endif()
    endforeach()
    set(${out_var} "${_out}" PARENT_SCOPE)
  endfunction()

  function(cycles_add_runtime_copy target file_list_var)
    if(TARGET ${target})
      set(_files "${${file_list_var}}")
      if(_files)
        cycles_kernel_normalize_files(_normalized ${_files})
        if(NOT _normalized)
          return()
        endif()
        set(_copy_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_runtime_copy.stamp")
        add_custom_command(
          OUTPUT ${_copy_stamp}
          COMMAND ${CMAKE_COMMAND} -E make_directory "${cycles_kernel_runtime_build_lib}"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  ${_normalized}
                  "${cycles_kernel_runtime_build_lib}"
          COMMAND ${CMAKE_COMMAND} -E touch ${_copy_stamp}
          DEPENDS ${_normalized}
          COMMENT "Copying Cycles kernel binaries for ${target}"
          VERBATIM
        )
        add_custom_target(${target}_runtime_copy DEPENDS ${_copy_stamp})
        add_dependencies(${target} ${target}_runtime_copy)
      endif()
    endif()
  endfunction()

  cycles_add_runtime_copy(cycles_kernel_cuda cycles_kernel_runtime_files_cuda)
  cycles_add_runtime_copy(cycles_kernel_optix cycles_kernel_runtime_files_optix)
  cycles_add_runtime_copy(cycles_kernel_hip cycles_kernel_runtime_files_hip)
  cycles_add_runtime_copy(cycles_kernel_hiprt cycles_kernel_runtime_files_hiprt)
endif()

# -----------------------------------------------------------------------------
# Install kernel source for runtime compilation.

delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_BAKE_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/bake)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_BVH_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/bvh)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_CAMERA_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/camera)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_CLOSURE_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/closure)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_DEVICE_GPU_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/device/gpu)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_FILM_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/film)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_GEOM_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/geom)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_INTEGRATOR_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/integrator)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_LIGHT_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/light)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_OSL_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/osl)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_SAMPLE_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/sample)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_SVM_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/svm)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_TYPES_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_UTIL_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/util)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_UTIL_HEADERS}" ${CYCLES_INSTALL_PATH}/source/util)

# -----------------------------------------------------------------------------
# OSL module.

if(WITH_CYCLES_OSL)
  list(APPEND LIB
    cycles_kernel_osl
  )
  add_subdirectory(osl)
  add_subdirectory(osl/shaders)
endif()

# -----------------------------------------------------------------------------
# Backend-specific rules.

add_subdirectory(device/cpu)
add_subdirectory(device/cuda)
add_subdirectory(device/hip)
add_subdirectory(device/hiprt)
add_subdirectory(device/metal)
add_subdirectory(device/oneapi)
add_subdirectory(device/optix)

target_link_libraries(cycles_kernel
  INTERFACE cycles_kernel_cpu
)

if(NOT WITH_CYCLES_PARALLEL_DEVICE_KERNEL_BUILD)
  set(_cycles_prev_gpu_kernel_target "")
  # Modifies in parent scope:
  # - `_cycles_prev_gpu_kernel_target`: updated to current target for chaining.
  function(cycles_add_gpu_kernel_dependencies GPU_KERNEL_TARGET)
    if(TARGET ${GPU_KERNEL_TARGET})
      if(_cycles_prev_gpu_kernel_target)
        add_dependencies(${GPU_KERNEL_TARGET} ${_cycles_prev_gpu_kernel_target})
      endif()
      set(_cycles_prev_gpu_kernel_target ${GPU_KERNEL_TARGET} PARENT_SCOPE)
    endif()
  endfunction()

  cycles_add_gpu_kernel_dependencies(cycles_kernel_cuda)
  cycles_add_gpu_kernel_dependencies(cycles_kernel_optix)
  cycles_add_gpu_kernel_dependencies(cycles_kernel_hip)
  cycles_add_gpu_kernel_dependencies(cycles_kernel_hiprt)
  cycles_add_gpu_kernel_dependencies(cycles_kernel_oneapi)
  cycles_add_gpu_kernel_dependencies(cycles_kernel_cpu)
endif()
