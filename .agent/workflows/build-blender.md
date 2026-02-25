---
description: How to build Blender from source on Windows
---
# How to Build Blender

> Based on the [Official Blender Build Guide](https://developer.blender.org/docs/handbook/building_blender/windows/)

## Prerequisites

### Required Software
1. **Visual Studio 2022** (Community or Build Tools)
   - Select **"Desktop Development with C++"** workload
2. **Git for Windows** - https://gitforwindows.org/
3. **CMake** - https://cmake.org (or use VS bundled CMake)

---

## Incremental Build (Development)

```powershell
# PowerShell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release
```

```batch
:: cmd.exe
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build E:\blender_modify\build_windows_x64_vc17_Release --target blender --config Release
```

**Multi-thread** (faster): append `-- /m:28`

> [!IMPORTANT]
> **Close Blender before building!** The linker will fail if `blender.exe` is locked.

**Build Output:** `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`

---

## Full Build (First Time / After CMake Changes)

```cmd
cd E:\blender_modify\blender
make update
make 2022b
```

First build takes 30-60 minutes.

---

## GPU Kernels (CUDA / OptiX)

CMake flags for GPU kernel compilation:
```
-DWITH_CYCLES_DEVICE_CUDA=ON
-DWITH_CYCLES_CUDA_BINARIES=ON
-DCYCLES_CUDA_BINARIES_ARCH=sm_89
-DCUDA_TOOLKIT_ROOT_DIR="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8"
-DOPTIX_ROOT_DIR=E:/blender_modify/optix-dev
-DWITH_CYCLES_DEVICE_OPTIX=ON
```

---

## Reference

| Command | Purpose |
|---------|---------|
| `make update` | Download/update libraries and submodules |
| `make 2022b` | Build with VS 2022 Build Tools |
| `make clean` | Clean build files |
