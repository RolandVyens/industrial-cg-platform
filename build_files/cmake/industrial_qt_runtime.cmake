# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Keep binary wheels and the selected manifest together. The source tree retains
# the original Windows payload, but each install contains only its platform set.
set(_icp_qt_source "${CMAKE_SOURCE_DIR}/release/extensions/system/blender_vfx_qt_runtime")
set(_icp_qt_destination "${TARGETDIR_VER}/extensions/system/blender_vfx_qt_runtime")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  if(WITH_INSTALL_PORTABLE)
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/release/bin/industrial-cg-platform" DESTINATION ".")
  endif()
  set(_icp_qt_manifest "${_icp_qt_source}/blender_manifest.linux.toml")
  install(
    DIRECTORY "${_icp_qt_source}/"
    DESTINATION "${_icp_qt_destination}"
    PATTERN "wheels" EXCLUDE
    PATTERN "blender_manifest*.toml" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
  )
  install(FILES "${_icp_qt_manifest}"
    DESTINATION "${_icp_qt_destination}" RENAME blender_manifest.toml)
  install(FILES "${_icp_qt_source}/wheels/linux/bqt-2.2.0+icp.linux.1-source.tar.gz"
    DESTINATION "${_icp_qt_destination}/wheels/linux")
  file(STRINGS "${_icp_qt_manifest}" _icp_qt_wheel_lines REGEX "^  \"\\./wheels/")
  foreach(_icp_qt_line IN LISTS _icp_qt_wheel_lines)
    string(REGEX REPLACE "^  \"\\./([^\"]+)\",?$" "\\1" _icp_qt_wheel "${_icp_qt_line}")
    if(NOT EXISTS "${_icp_qt_source}/${_icp_qt_wheel}")
      message(FATAL_ERROR "Missing Industrial Qt runtime wheel: ${_icp_qt_wheel}")
    endif()
    get_filename_component(_icp_qt_wheel_dir "${_icp_qt_wheel}" DIRECTORY)
    install(FILES "${_icp_qt_source}/${_icp_qt_wheel}"
      DESTINATION "${_icp_qt_destination}/${_icp_qt_wheel_dir}")
  endforeach()
else()
  install(DIRECTORY "${_icp_qt_source}/" DESTINATION "${_icp_qt_destination}"
    PATTERN "linux" EXCLUDE
    PATTERN "blender_manifest.linux.toml" EXCLUDE
    PATTERN "__pycache__" EXCLUDE)
endif()
unset(_icp_qt_source)
unset(_icp_qt_destination)
unset(_icp_qt_manifest)
unset(_icp_qt_wheel_lines)
unset(_icp_qt_line)
unset(_icp_qt_wheel)
unset(_icp_qt_wheel_dir)
