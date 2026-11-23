# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

set(INC
  ..
)

set(INC_SYS
)

set(SRC
  adaptive_sampling.cpp
  deep_bounded_transport.cpp
  deep_bounded_work.cpp
  deep_bounded_cpu.cpp
  deep_frame_capture.cpp
  deep_gpu_frame_work.cpp
  deep_gpu_frame_archive.cpp
  deep_gpu_frame_witness.cpp
  deep_optical_work.cpp
  denoiser.cpp
  denoiser_dlss.cpp
  denoiser_gpu.cpp
  denoiser_oidn.cpp
  denoiser_oidn_base.cpp
  denoiser_oidn_gpu.cpp
  denoiser_optix.cpp
  path_trace.cpp
  tile.cpp
  pass_accessor.cpp
  pass_accessor_cpu.cpp
  pass_accessor_gpu.cpp
  path_trace_display.cpp
  path_trace_tile.cpp
  path_trace_work.cpp
  path_trace_work_cpu.cpp
  path_trace_work_gpu.cpp
  render_scheduler.cpp
  shader_eval.cpp
  work_balancer.cpp
  work_tile_scheduler.cpp
)

set(SRC_HEADERS
  adaptive_sampling.h
  deep_bounded_publication.h
  deep_bounded_plan.h
  deep_bounded_layout.h
  deep_bounded_work_internal.h
  deep_bounded_work.h
  deep_bounded_cpu.h
  deep_frame_capture.h
  deep_gpu_frame_work.h
  deep_control_journal.h
  deep_expression_journal.h
  deep_native_capacity.h
  deep_camera_receipt_writer.h
  deep_input_receipt_reader.h
  deep_optical_work.h
  deep_state_binding.h
  denoiser.h
  denoiser_dlss.h
  denoiser_gpu.h
  denoiser_oidn.h
  denoiser_oidn_base.h
  denoiser_oidn_gpu.h
  denoiser_optix.h
  guiding.h
  path_trace.h
  tile.h
  pass_accessor.h
  pass_accessor_cpu.h
  pass_accessor_gpu.h
  path_trace_display.h
  path_trace_tile.h
  path_trace_work.h
  path_trace_work_cpu.h
  path_trace_work_gpu.h
  render_scheduler.h
  shader_eval.h
  work_balancer.h
  work_tile_scheduler.h
)

set(LIB
  PUBLIC cycles_device

  # NOTE: Is required for RenderBuffers access. Might consider moving files around a bit to
  # avoid such cyclic dependency.
  PUBLIC cycles_session

  PUBLIC cycles_util
  PRIVATE bf::dependencies::optional::openimagedenoise
  PRIVATE bf::dependencies::optional::openpgl
)

if(WITH_DLSS)
  list(APPEND INC_SYS
    ${DLSS_INCLUDE_DIR}
  )
endif()

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

cycles_add_library(cycles_integrator "${LIB}" ${SRC} ${SRC_HEADERS})
