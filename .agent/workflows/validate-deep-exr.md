# Validate Deep EXR

## Canonical Inputs

- Blend: `D:\blender_projects\light-passes-test-v001.blend`
- Nuke script: `E:\blender_modify\deep_merge_test.nk`
- Blender build: `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`
- Blender Python: `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe`
- Preview output folder: `C:\tmp\`

## Current Rule

- CPU and OptiX are the locked validation devices.
- Nuke visual judgment comes before script checks.
- Do not change the blend or `.nk` file for the standard validation round.

## CPU Direct Scene-Output Render

```powershell
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' `
  -b 'D:\blender_projects\light-passes-test-v001.blend' `
  --python 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\render_scene_output_rgba_deep_probe.py' `
  -f 2
```

## CPU Flat Reference Render

```powershell
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' `
  -b 'D:\blender_projects\light-passes-test-v001.blend' `
  --python 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\render_scene_output_rgba_flat_probe.py' `
  -f 2
```

## Runtime Compositor RGBA Deep Render

```powershell
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' `
  -b 'D:\blender_projects\light-passes-test-v001.blend' `
  --python 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\render_temp_compositor_rgba_deep.py'
```

## Nuke Visual Check

Run the unchanged Nuke script against the direct scene-output deep file:

```powershell
& 'D:\nuke\Nuke15.1.exe' --nukex `
  -t 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\run_nuke_direct_scene_output_test.py' `
  --deep-input 'C:\tmp\scene_output_rgba_deep_probe_0002.exr' `
  --output-png 'C:\tmp\direct_scene_output_saved_write1.png' `
  --mask-png 'C:\tmp\direct_scene_output_saved_mask.png'
```

Primary judgment:

- inspect the DeepMerge RGB result
- inspect the teapot / gray card seam
- inspect volume regions

## Script Checks

```powershell
$py = 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe'
$deep = 'C:\tmp\scene_output_rgba_deep_probe_0002.exr'
$flat = 'C:\tmp\scene_output_rgba_flat_probe_0002.exr'

& $py 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_single_surface_alpha.py' $flat $deep
& $py 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_mixed_surface_volume_case1.py' $deep
& $py 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_flatten_matches_flat.py' $flat $deep
& $py 'E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_surface_front_alpha.py' $deep
```

## References

- Current gate: [features/deep-exr/TEST_MATRIX.md](/E:/blender_modify/blender/.agent/features/deep-exr/TEST_MATRIX.md)
- Current summary: [features/deep-exr/CONTEXT.md](/E:/blender_modify/blender/.agent/features/deep-exr/CONTEXT.md)
