# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

set(INC
  ..
)

set(INC_SYS
)

set(SRC
  buffers.cpp
  cache_eviction.cpp
  deep_buffers.cpp
  deep_canonical_encode.cpp
  deep_camera_depth.cpp
  deep_depth_support.cpp
  deep_homogeneous_curve.cpp
  deep_native_curve.cpp
  deep_emission_expression.cpp
  deep_frame_reference.cpp
  deep_encoded_view.cpp
  deep_frame_production.cpp
  deep_frame_pipeline.cpp
  deep_page_store.cpp
  deep_volume_profile.cpp
  denoising.cpp
  display_driver.cpp
  merge.cpp
  session.cpp
  tile.cpp
)

# Retain the former standalone driver for explicit reference and oracle tests only.
if(WITH_GTESTS OR WITH_CYCLES_DEEP_REFERENCE_GPU)
  list(APPEND SRC deep_output_driver.cpp)
endif()

set(SRC_HEADERS
  buffers.h
  cache_eviction.h
  deep_buffers.h
  deep_canonical_encode.h
  deep_camera_depth.h
  deep_depth_support.h
  deep_homogeneous_curve.h
  deep_native_curve.h
  deep_emission_expression.h
  deep_bounded_format.h
  deep_frame_production_internal.h
  deep_frame_production.h
  deep_capture_types.h
  deep_frame_pipeline.h
  deep_memory_budget.h
  deep_view_registry.h
  deep_page_store.h
  deep_camera_sample_set.h
  deep_output_driver.h
  deep_volume_profile.h
  denoising.h
  display_driver.h
  merge.h
  output_driver.h
  session.h
  tile.h
)

set(LIB
  PUBLIC cycles_device
  PUBLIC cycles_integrator
  PUBLIC cycles_util
)

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

if(MSVC)
  set_source_files_properties(deep_camera_depth.cpp deep_native_curve.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
  set_source_files_properties(deep_camera_depth.cpp deep_native_curve.cpp PROPERTIES COMPILE_OPTIONS "-fno-fast-math")
endif()

cycles_add_library(cycles_session "${LIB}" ${SRC} ${SRC_HEADERS})
