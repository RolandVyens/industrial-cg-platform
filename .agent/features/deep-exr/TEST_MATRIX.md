# Deep EXR Test Matrix

> Canonical source: `E:\blender_modify\blender`
>
> Canonical build: `E:\blender_modify\build_windows_x64_vc17_Release`
>
> Scene: `D:\blender_projects\light-passes-test-v001.blend`
>
> Last refreshed: 2026-04-01

## Locked Scope

- Test only `D:\blender_projects\light-passes-test-v001.blend`.
- Test only CPU and OptiX.
- Use Nuke visual judgment first.
- Keep the shipped volume path unchanged unless the task explicitly reopens it.

## Required Round

1. Build the target code.
2. Render the unchanged scene.
3. Run the unchanged Nuke script and inspect the DeepMerge RGB result.
4. Export preview PNGs to `C:\tmp\`.
5. Run the required script checks.

## Hard Fail Conditions

- The teapot / gray-card seam comes back visibly.
- Volume regions show the old hole / eaten-through regression.
- CPU or OptiX introduces a backend failure.
- Required check scripts fail on the accepted test output.

## Required Scripts

- `E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_single_surface_alpha.py`
- `E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_mixed_surface_volume_case1.py`
- `E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_flatten_matches_flat.py`

Diagnostic only:

- `E:\blender_modify\blender\.agent\features\deep-exr\scripts\check_deep_surface_front_alpha.py`

## Reference Workflow

- [workflows/validate-deep-exr.md](/E:/blender_modify/blender/.agent/workflows/validate-deep-exr.md)
