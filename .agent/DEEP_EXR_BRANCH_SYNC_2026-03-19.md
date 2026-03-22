# Deep EXR branch sync note (2026-03-19)

## Scope
Synced the proven Feature 4 GPU split-pass indexing fix from the validated Deep EXR sync worktree into
`feature/deep-exr-surface-coverage` so the Deep EXR development branch line also carries the flat-beauty
hole fix.

## Source commits
- Code sync source: `447de54b190` ? `Cycles: fix GPU lightgroup split-pass indexing`
- Validation/doc source: `527731e93d9` ? `Docs: record deep worktree sync verification`

## Synced files
- `.agent/AGENT.md`
- `.agent/check_feature4_gpu_flat_alpha_hole.py`
- `.agent/check_feature4_lightgroup_subimages.py`
- `intern/cycles/kernel/data_template.h`
- `intern/cycles/kernel/film/light_passes.h`
- `intern/cycles/scene/devicescene.cpp`
- `intern/cycles/scene/devicescene.h`
- `intern/cycles/scene/film.cpp`

## Validation reference
Validated in `E:\blender_modify\blender_deep_exr_fix` / `E:\blender_modify\build_deep_exr_fix` before sync:
- GPU flat-hole check: `hole_pixel_count=0`, `alpha_diff_pixels_gt_0.01=7`, `alpha_max_diff=0.03125`
- Deep EXR checks:
  - `checked_single_surface_fractional_pixels=6657`
  - `mismatching_single_surface_pixels=0`
  - `multi_sample_pixels=39349`
  - `violating_front_alpha_pixels=0`

## Test harness note
Under `--factory-startup`, `prefs.get_devices()` exposed both CUDA and OPTIX entries for the same GPU.
For the flat-hole regression test, only the CUDA device should be enabled; enabling every returned
device caused a CUDA illegal-address crash during CLI validation.
