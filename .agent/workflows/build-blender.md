# Build Blender

## Primary Mainline Build

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release -- /m:28
```

Primary runtime:

- `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`

## Install Mainline Build

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --install 'E:\blender_modify\build_windows_x64_vc17_Release' --config Release `
  --prefix 'E:\blender_modify\release\blender-vfx-5.2-YYYY-MM-DD'
```

## Feature Worktree Build Pattern

```powershell
$build = 'E:\blender_modify\build_<feature>'

& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build $build --target blender --config Release -- /m:28
```

## Kernel Management

- A successful MSBuild is not the whole runtime story for Cycles GPU validation.
- Blender can still load stale precompiled `kernel_*.zst` payloads if the active runtime kernel
  folder was not refreshed after rebuilding `cycles_kernel_cuda` or `cycles_kernel_optix`.
- If a GPU regression appears inconsistent with the latest source, always verify the active
  runtime kernel path before debugging the shader/integrator code.

## Active Runtime Rule

- The active precompiled kernel folder is determined by the runtime layout Blender is launched
  from, not by the intermediate build output folders under `intern\cycles\kernel\device\...`.
- For the fog feature build, CLI validation currently loads precompiled kernels from:
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
- This was confirmed with `--debug-cycles`, which logs the exact precompiled kernel path Blender
  is using.

## When To Rebuild Host Code

- Rebuild `blender` when you changed code that affects the executable or host-side Cycles data
  layout.
- In practice, rebuild `blender` after changes under areas like:
  - `intern/cycles/scene/...`
  - `intern/cycles/blender/...`
  - `intern/cycles/kernel/data_template.h`
  - node registration / RNA / shader sync code
- If the change is clearly kernel-only, you can often rebuild `cycles_kernel_cuda` and/or
  `cycles_kernel_optix` directly first.
- If you are unsure whether host-side structs or feature flags changed, prefer rebuilding
  `blender` to avoid host/kernel mismatch.

## When To Sync Runtime Kernels

- Manually sync runtime kernel payloads whenever you rebuild only:
  - `cycles_kernel_cuda.vcxproj`
  - `cycles_kernel_optix.vcxproj`
- Also sync them after a full `blender` rebuild if the render results still look like old kernel
  behavior.
- Source payload folders:
  - main build CUDA:
    `E:\blender_modify\build_windows_x64_vc17_Release\intern\cycles\kernel\device\cuda`
  - main build OptiX:
    `E:\blender_modify\build_windows_x64_vc17_Release\intern\cycles\kernel\device\optix`
  - `E:\blender_modify\build_env_fog\intern\cycles\kernel\device\cuda`
  - `E:\blender_modify\build_env_fog\intern\cycles\kernel\device\optix`
- Main build active runtime destination:
  - `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\scripts\addons_core\cycles\lib`
- Active runtime destination for the fog feature build:
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`

## Mainline Runtime Sync Pattern

- On `2026-04-11`, mainline OptiX validation only became trustworthy after syncing both:
  - `kernel_optix*.zst`
  - `kernel_sm_*.cubin.zst`
- Syncing only the OptiX PTX payloads was not enough, because Blender still loaded the stale
  runtime `kernel_sm_89.cubin.zst` and the factory-startup OptiX smoke test failed until the CUDA
  cubin runtime payloads were refreshed too.

```powershell
$cudaSrc = 'E:\blender_modify\build_windows_x64_vc17_Release\intern\cycles\kernel\device\cuda'
$optixSrc = 'E:\blender_modify\build_windows_x64_vc17_Release\intern\cycles\kernel\device\optix'
$dst = 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\scripts\addons_core\cycles\lib'

Get-ChildItem -LiteralPath $cudaSrc -Filter 'kernel_sm_*.cubin.zst' |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Force }
Get-ChildItem -LiteralPath $optixSrc -Filter 'kernel_optix*.zst' |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Force }
```

## Fog Feature Sync Pattern

```powershell
$cudaSrc = 'E:\blender_modify\build_env_fog\intern\cycles\kernel\device\cuda'
$optixSrc = 'E:\blender_modify\build_env_fog\intern\cycles\kernel\device\optix'
$dst = 'E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib'

Get-ChildItem -LiteralPath $cudaSrc -Filter 'kernel_*.zst' |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Force }
Get-ChildItem -LiteralPath $optixSrc -Filter 'kernel_*.zst' |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Force }
```

## Faster OptiX Kernel Refresh

- On `2026-04-10`, we confirmed that forcing `cycles_kernel_optix.vcxproj` through a full
  rebuild can be much slower than expected for local fog-kernel iteration.
- Practical runtime behavior on this machine:
  - CPU usage may stay deceptively low
  - `/m` parallelism helps little because the slow step is mainly the custom `nvcc` PTX build
    rather than a broad C++ project compile
  - an interrupted `MSBuild` / `nvcc` run can leave background processes alive, so check and stop
    them before retrying
- For kernel-only OptiX changes that affect the current fog path, a faster validated pattern is:
  1. Skip the full `cycles_kernel_optix` rebuild
  2. Directly compile only the affected PTX payloads with `nvcc`
  3. Compress them with `zstd_compress.exe`
  4. Write them straight into the active runtime kernel folder

### Current Fast Path

- This was used successfully on `2026-04-10` for the environment-fog point/spot sampling change.
- The targeted direct compile finished in about `90s`, which was materially faster than waiting on
  the full project rebuild path.
- Current active runtime destination:
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
- Current targeted OptiX payloads for this fog slice:
  - `kernel_optix_shader_raytrace.ptx.zst`
  - `kernel_optix.ptx.zst`

```powershell
$msvcBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64'
$env:Path = "$msvcBin;$env:Path"

$nvcc = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe'
$srcRoot = 'E:/blender_modify/blender_env_fog/intern/cycles'
$optixSrcDir = 'E:/blender_modify/blender_env_fog/intern/cycles/kernel/device/optix'
$optixInclude = 'E:/blender_modify/optix-dev/include'
$dstDir = 'E:/blender_modify/build_env_fog/bin/Release/5.2/scripts/addons_core/cycles/lib'
$zstd = 'E:\blender_modify\build_env_fog\bin\Release\zstd_compress.exe'

& $nvcc --ptx -arch=compute_50 --keep-device-functions `
  -I $optixInclude -I $srcRoot `
  -o "$dstDir/kernel_optix_shader_raytrace.ptx" `
  -D OSL_LIBRARY_VERSION_CODE=11407 `
  -ccbin="$msvcBin" -std=c++17 --use_fast_math -Wno-deprecated-gpu-targets `
  -D WITH_NANOVDB `
  "$optixSrcDir/kernel_shader_raytrace.cu"

& $zstd `
  "$dstDir/kernel_optix_shader_raytrace.ptx" `
  "$dstDir/kernel_optix_shader_raytrace.ptx.zst"

& $nvcc --ptx -arch=compute_50 `
  -I $optixInclude -I $srcRoot `
  -o "$dstDir/kernel_optix.ptx" `
  -D OSL_LIBRARY_VERSION_CODE=11407 `
  -ccbin="$msvcBin" -std=c++17 --use_fast_math -Wno-deprecated-gpu-targets `
  -D WITH_NANOVDB `
  "$optixSrcDir/kernel.cu"

& $zstd `
  "$dstDir/kernel_optix.ptx" `
  "$dstDir/kernel_optix.ptx.zst"
```

### Stale Background Build Cleanup

- If a prior forced rebuild was interrupted, check for leftover processes before retrying:

```powershell
Get-Process msbuild,nvcc -ErrorAction SilentlyContinue |
  Stop-Process -Force
```

## Verification

- Use `--debug-cycles` on a quick render to confirm which precompiled kernel path Blender is using.
- Look for lines like:
  - `Testing for pre-compiled kernel ...`
  - `Using precompiled kernel.`
- If those paths do not point at the folder you just refreshed, do not trust the validation result
  yet.

## Notes

- Close any running `blender.exe` before building.
- Do not build `blender` and `install` concurrently against the same MSBuild directory.
- If Windows file locking starts failing, rerun sequentially and drop to `-- /m:1 /nr:false`.
- If a render behavior still looks stale after syncing runtime kernels, verify both:
  - the executable build stamp reported by Blender
  - the active precompiled kernel path reported by `--debug-cycles`
