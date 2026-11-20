# Building Linux Beta 2

This tag contains the Linux integration corresponding to the Blender 5.2.4 LTS
Industrial CG Platform Linux Beta 2 binary. The binary's original build commit is
416ec9ba37f36dae794ba3d22fc8495c3ac50430 in the local full-history repository.
This curated continuation tag preserves the same production code; documentation
provenance is portable. The stable default branch is not promoted by this Beta.

Use the Rocky Linux 8 setup script under build_files/build_environment/linux/.
The recorded build used glibc 2.28, GCC toolset 14.2.1 and CUDA 12.8.93, with the
Blender precompiled Linux libraries pinned to754e07f9c992d03370bf944e66d9fb03177b0fc0.
Blender source/data dependencies and precompiled libraries must be hydrated from
Blender's official repositories or a trusted matching local copy. This continuation
repository intentionally does not mirror historical LFS payloads.

The runtime wheels are dependency payloads, supplied inside the Beta binary archive,
not duplicated as new LFS objects in this public tag. Extract the release archive;
copy 5.2/extensions/system/blender_vfx_qt_runtime/wheels into the matching
release/extensions/system/blender_vfx_qt_runtime/wheels source directory.
The Linux bQt wheel and its complete MPL-2.0 fork source archive are included there.
PySide6, Essentials, Addons and shiboken6 are unmodified official PyPI6.9.3
manylinux_2_28_x86_64 wheels, pinned together in blender_manifest.linux.toml.
Keep the notices in third_party_sources.md and THIRD_PARTY_LICENSES.md.

Configure from build_files/cmake/config/blender_release.cmake with:

- CMAKE_BUILD_TYPE=Release and a fresh out-of-source build directory.
- LIBDIR pointing to the matching precompiled Linux library tree.
- OPTIX_ROOT_DIR pointing to OptiX SDK9.1; CUDA_TOOLKIT_ROOT_DIR to CUDA12.8.
- WITH_CYCLES_DEVICE_CUDA=ON, WITH_CYCLES_DEVICE_OPTIX=ON.
- WITH_CYCLES_CUDA_BINARIES=ON; CYCLES_CUDA_BINARIES_ARCH=
  sm_50;sm_52;sm_60;sm_70;sm_75;sm_86;sm_120;compute_75.
- WITH_CYCLES_DEVICE_HIP=ON; WITH_CYCLES_HIP_BINARIES=OFF;
  WITH_CYCLES_DEVICE_HIPRT=OFF.
- WITH_CYCLES_DEVICE_ONEAPI=OFF; WITH_CYCLES_ONEAPI_BINARIES=OFF.
- WITH_CYCLES_DEEP_REFERENCE_GPU=OFF.
- WITH_GHOST_X11=ON, WITH_GHOST_WAYLAND=ON.

Build the complete default target and install to an empty staging directory.
The selected Linux manifest installs only its own platform wheels. Run the installed
industrial-cg-platform launcher to select X11/xcb for both Blender and Qt.
Minimal Rocky desktop runtime dependencies include xcb-util-cursor, xcb-util-wm,
xcb-util-keysyms, libxkbcommon-x11 and working Mesa/OpenGL drivers.

See the Beta 2 release notes for the runtime checks and their scope.
