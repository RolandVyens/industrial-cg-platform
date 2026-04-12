# Validate Lightgroup Lobe Passes

## Canonical Inputs

- Blend: `D:\blender_projects\light-passes-test-v001.blend`
- Blender build: `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`
- Blender Python: `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe`

## Required Checks

1. Render one CPU flat RGBA output and one GPU flat RGBA output from the same frame.
2. Compare them with the GPU flat-hole checker.
3. Render or reuse the saved multilayer EXR output for the same frame.
4. Check that combined-only lightgroups still do not create forbidden split channels.
5. Check that expected env/key split channels still exist and contain data.

## Example Render

```powershell
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' `
  -b 'D:\blender_projects\light-passes-test-v001.blend' -f 3
```

## Script Checks

```powershell
$py = 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe'
$cpuFlat = 'C:\tmp\feature4_cpu_flat_0003.exr'
$gpuFlat = 'C:\tmp\feature4_gpu_flat_0003.exr'
$multilayer = 'D:\blender_projects\rendered\test\light-passes-test-v001\light-passes-test-v001_0003.exr'

& $py 'E:\blender_modify\blender\.agent\features\lightgroup-lobe-passes\scripts\check_feature4_gpu_flat_alpha_hole.py' $cpuFlat $gpuFlat
& $py 'E:\blender_modify\blender\.agent\features\lightgroup-lobe-passes\scripts\check_feature4_lightgroup_subimages.py' $multilayer
```

The CPU/GPU flat files should be rendered from the same saved scene with matching output settings.

## Reference

- Feature summary: [features/lightgroup-lobe-passes/CONTEXT.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/CONTEXT.md)
