# VFX Rendering Branch Status

> Date: 2026-04-11

## Mainline

- Repo: `E:\blender_modify\blender`
- Branch: `vfx-rendering-branch-github`
- HEAD: `08fcb983633`
- Primary build: `E:\blender_modify\build_windows_x64_vc17_Release`
- Primary runtime: `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`
- Latest packaged release: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26`
- Installed GitHub release used for runtime comparison:
  `C:\Program Files\Blender Foundation\blender-vfx-5.2-2026-03-26`
- Latest release zip: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26.zip`
- Release notes draft: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26-notes.md`

## Upstream Sync

- Upstream Blender main fetched from `origin/main` at `8deaa11ab02` on `2026-04-11`.
- `vfx-rendering-branch-github` merged that upstream mainline at `08fcb983633`.
- `vfx-rendering-branch` then merged the updated GitHub-facing branch at `a9cf151dbb5`.
- Primary Windows build was re-run after the sync and successfully rebuilt `bf_intern_cycles`
  plus `blender.exe` from the merged source tree.

## Housekeeping Snapshot

- Pass 1 cleanup removed stub build folders:
  - `E:\blender_modify\build_mat_override`
  - `E:\blender_modify\build_no_direct`
  - `E:\blender_modify\build_deep_surface_cov_ca742`
- Older unpacked release folders were pruned.
- Older release zip files were kept.
- Historical Deep EXR surface-coverage assets were archived to
  `E:\blender_modify\blender\.agent\archive\deep-exr\surface-coverage-worktree-2026-04-02`.
- Detached Deep EXR helper worktrees were archived to
  `E:\blender_modify\blender\.agent\archive\deep-exr\helper-worktrees-2026-04-02`.
- AF66 reference cleanup note:
  `E:\blender_modify\blender\.agent\archive\backups\af66-reference-2026-04-02.md`
- Removed merged branch/worktree/build:
  - `feature/deep-exr-surface-coverage`
  - `E:\blender_modify\blender_deep_surface_coverage`
  - `E:\blender_modify\build_deep_surface_coverage`
- Removed detached Deep EXR helper worktrees/builds:
  - `E:\blender_modify\blender_deep_exr_fix`
  - `E:\blender_modify\blender_deep_surface_coverage_e720_clean`
  - `E:\blender_modify\blender_deep_surface_cov_ca742`
  - `E:\blender_modify\build_deep_exr_fix`
  - `E:\blender_modify\build_deep_surface_coverage_e720_clean`
- Removed AF66 reference worktree/build:
  - `E:\blender_modify\blender_af66_origin`
  - `E:\blender_modify\build_af66_origin`
- Removed historical non-primary build folders:
  - `E:\blender_modify\build_lobe_passes`
  - `E:\blender_modify\build_vfx_branch_sync`
- Removed stale placeholder feature worktrees while keeping their branches:
  - `E:\blender_modify\blender_no_direct`
  - `E:\blender_modify\blender_mat_override`

## Merged Feature State

- Deep EXR is merged to mainline and validated on the locked `light-passes-test-v001.blend` scene.
- Shadow Color is merged to mainline and included in the latest release.
- Lightgroup Lobe Passes / Light AOV Phase 1 is merged to mainline and included in the latest release.

## Deep EXR Current Truth

- Current canonical Deep EXR source is the mainline branch, not a detached feature worktree.
- Locked validation scene: `D:\blender_projects\light-passes-test-v001.blend`
- Locked Nuke visual test: `E:\blender_modify\deep_merge_test.nk`
- Locked preview output folder: `C:\tmp\`
- Historical surface-coverage worktree snapshot:
  `E:\blender_modify\blender\.agent\archive\deep-exr\surface-coverage-worktree-2026-04-02`
- Historical helper worktree archive:
  `E:\blender_modify\blender\.agent\archive\deep-exr\helper-worktrees-2026-04-02`
- Current accepted scope:
  - direct scene-output RGBA Deep EXR works
  - compositor RGBA Deep EXR works
  - compositor alpha-only Deep EXR works
  - hard-surface seam is resolved to accepted quality
  - volume behavior is preserved on the current shipped path
- Deferred future work:
  - metadata reconstruction follow-up
  - sparse/compressed memory ideas inspired by MoonRay
  - broader performance tuning after correctness

## Feature Branch / Worktree Map

| Worktree | Branch / State | Note |
| --- | --- | --- |
| `E:\blender_modify\blender` | `vfx-rendering-branch-github` | Canonical GitHub-facing mainline at `08fcb983633`, merged with `origin/main` commit `8deaa11ab02` on `2026-04-11`. |
| `E:\blender_modify\blender_vfx_branch_sync` | `vfx-rendering-branch` | Non-GitHub parity branch at `a9cf151dbb5`, merged from `vfx-rendering-branch-github` on `2026-04-11`. |
| `E:\blender_modify\blender_lobe_passes` | `feature/per-lightgroup-lobe-passes` | Historical feature worktree kept for reference. |
| `E:\blender_modify\blender_env_fog` | `feature/world-environment-fog` | Synced to mainline on 2026-04-03 and still validated at `4b99a0f4725`; after the 2026-04-11 upstream-main merge to `08fcb983633`, this worktree is now behind current VFX mainline and should be re-synced before new fog feature work resumes. Dedicated build `E:\blender_modify\build_env_fog` compiles the node/API, world-volume output sync, and the current V2 camera-segment single-scatter slice at `4b99a0f4725`. On 2026-04-05 the fog kernel moved off per-light tuned gains and now derives scatter from Cycles light evaluation plus shared homogeneous transmittance/phase terms, with finite lights normalized by sampled-light `eval_fac / pdf` again after a same-day regression briefly removed that factor and caused whiteout. On 2026-04-07, `build_env_fog` was reconfigured against `E:\blender_modify\optix-dev` with `WITH_CYCLES_CUDA_BINARIES=ON`, rebuilt successfully, and runtime probing confirmed OptiX exposure again: `_cycles.get_device_types()` now returns `(True, True, True, False, False, False)` and `_cycles.available_devices('OPTIX')` lists the RTX 4080 SUPER. On 2026-04-08, a stale-runtime mismatch was confirmed as the reason recent OptiX crash conclusions kept reproducing even after source changes: `--debug-cycles` showed the active precompiled kernels are loaded from `build_env_fog\\bin\\Release\\5.2\\scripts\\addons_core\\cycles\\lib`, so rebuilding `cycles_kernel_optix` / `cycles_kernel_cuda` is not enough by itself. After rebuilding the host runtime (`blender`, `bf_intern_cycles`, `cycles_scene`) and syncing the freshly generated `kernel_*.zst` payloads into that active runtime folder, OptiX V2 validation now succeeds again on the fog feature build. Same-day close-out completed the final alley-scene validation matrix on `CUDA` and `OPTIX`, confirmed that a plain default-profile render selected `OptiX`, and recorded explicit HDRI-ignore plus camera-hit clipping proofs under `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-08`. On 2026-04-09, transparent-background holdout validation on the dedicated cyberpunk scene exposed that the additive fog path wrote RGB but left holdout alpha at zero; the current worktree now multiplies transparent-background holdout/background writes by the fog segment transmittance so OptiX geometry-holdout renders carry visible fog alpha again, validated at `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-09\\foggy_street_cyberpunk_test_optix_holdout_geo_fix_0001.png`. Later the same day, a dedicated built-in Cycles `Fog` pass / AOV was added, wired through Blender pass registration/UI, and validated on the cyberpunk scene with a standalone OptiX fog-pass export at `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_0001.exr`; a viewable PNG preview was also generated at `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_0001.png`. Later on 2026-04-09, the cyberpunk blend was explicitly promoted to the default active validation scene, and the wall-adjacent point lights were pulled farther out into the alley volume so the dedicated fog pass would no longer lose them to wall occlusion; focused point-only OptiX fog-pass validation now saves `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_point_only_0001.exr` plus a PNG preview, confirming visible point-light fog contribution on the dedicated pass. On 2026-04-10, the combined all-lights cyberpunk beauty was re-rendered again with the saved scene defaults and the current default user profile, producing `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-10\\foggy_street_cyberpunk_all_lights_optix_rerender_0001.png`; the render log in the same folder shows OptiX acceleration-structure builds and a successful save. Later the same day, point/spot fog striping was reduced by jittering fog segment steps and then by a small targeted two-subsample finite-light average for soft-falloff point/spot emitters. The current finer follow-up has now rolled the temporary cyberpunk fog-48 scene test back to `Samples=24` and refines the finite point/spot fog branch further by averaging full per-subsample fog contributions instead of averaging radiance/direction first; current validation outputs are `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample_full_0001.png` and `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-10\\foggy_street_cyberpunk_all_lights_optix_subsample_full_0001.png`. A same-day follow-up probe temporarily raised the targeted soft-falloff point/spot subsample count from `2` to `4`, but the image delta was too small for the added cost, so the default source/runtime behavior was restored to `2` after recording the validation outputs `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample4_0001.png` and `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-10\\foggy_street_cyberpunk_all_lights_optix_subsample4_0001.png`. A further same-day opacity-warp experiment redistributed point/spot camera-segment steps using cumulative homogeneous fog opacity and preserved per-step weights, but it still pushed the all-lights beauty from about `00:06.43` to about `00:14.06` while only making a modest fog-pass difference, so the source and active runtime were restored again to the accepted uniform-step + targeted point/spot subsample path after recording `.agent\\features\\world-environment-fog\\validation\\fog_pass_2026-04-09\\foggy_street_cyberpunk_test_optix_fog_pass_point_only_opacity_warp_0001.png` and `.agent\\features\\world-environment-fog\\validation\\final_matrix_2026-04-10\\foggy_street_cyberpunk_all_lights_optix_opacity_warp_0001.png`. On 2026-04-11, a temporary inward-clearance scene probe was completed and compared against the accepted point-only `subsample_full` baseline; moving the wall-adjacent soft-falloff point lights farther off the walls reduced the normalized left-core fog-pass gradient by about `25%` and the broader left-artifact ROI by about `12%`, which is strong evidence that the remaining striping is primarily scene-side high variance from finite disk lights grazing wall geometry rather than an unresolved global fog integrator defect. A same-day adaptive kernel follow-up then tested a local disagreement-triggered `2 -> 4` refinement for soft point/spot fog samples, but the measured gradient reduction stayed only around `4%` to `4.5%` while the point-only fog-pass render rose to about `04:54.516`, so that probe was rejected and the source plus active OptiX runtime were restored again to the accepted fixed-`2` point/spot subsample path. Later the same day, default-profile cyberpunk beauty performance was measured with fog temporarily disabled and restored in-process; the helper recorded `scene.cycles.device: GPU`, `preferences.compute_device_type: OPTIX`, a fog-off render time of about `3.814s`, and a fog-on render time of about `13.424s`, so the current fog path adds about `9.610s` or roughly `+251.973%` on this saved validation scene. A second same-day OptiX stress test then built a new high-angle cyberpunk city overview scene with `250` real lights and measured it with the same fog-off / fog-on method; the rerun recorded about `4.348s` fog-off versus about `106.003s` fog-on, meaning the current fog path added about `101.655s` or roughly `+2338.012%` on that hundreds-of-lights scene. Current milestone truth: this feature now includes dedicated fog/AOV output, while emissive-geometry fog lighting, fog lightgroup split, broader background/light-hit shadowing, and compositor-focused holdout validation remain deferred. |

## Current GPU Truth

- Root cause of the broad local GPU crash was not source parity against the March 26 release.
- The actual issue was stale build-tree Cycles kernel payloads under:
  - `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\scripts\addons_core\cycles\lib`
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
- The host executables matched newer source/build state, but those runtime kernel files still
  carried older March 19 artifacts in both local build trees.
- After syncing runtime kernels from the build outputs:
  - main build plain factory-startup `CUDA` render succeeds:
    `E:\blender_modify\blender\.agent\tmp\main_build_cuda8_fixed_0001.png`
  - main build plain factory-startup `OPTIX` render succeeds:
    `E:\blender_modify\blender\.agent\tmp\main_build_optix8_fixed_0001.png`
  - fog build plain factory-startup `CUDA` render succeeds:
    `E:\blender_modify\blender\.agent\tmp\fog_build_cuda8_fixed_0001.png`
  - fog build plain factory-startup `OPTIX` render succeeds:
    `E:\blender_modify\blender\.agent\tmp\fog_build_optix8_fixed_0001.png`
- Additional 2026-04-08 runtime truth:
  - `--debug-cycles` on the fog build confirms the active precompiled kernel path is:
    `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
  - the stale `bin\Release\lib` kernel folder is not the path used by the CLI validation renders
- Current fog-build GPU validation after rebuilding and syncing runtime kernels:
  - fog build full validation scene on `OPTIX`, 1 spp, succeeds:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples1_0001.png`
  - fog build full validation scene on `OPTIX`, 8 spp, succeeds:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples8_0001.png`
  - isolated sun-only fog sanity render on `OPTIX` succeeds:
    `E:\blender_modify\blender\.agent\tmp\optix_background_sun_only_real_0001.png`
- Additional 2026-04-11 mainline truth:
  - after merging `origin/main` at `08fcb983633`, the primary build successfully rebuilt
    `bf_intern_cycles` and `blender.exe`
  - on the same day, the main build's active runtime kernel payloads under
    `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\scripts\addons_core\cycles\lib`
    were confirmed to still be older March 26 copies even though fresh 2026-04-11 payloads had
    been generated under both `intern\cycles\kernel\device\optix` and
    `intern\cycles\kernel\device\cuda`
  - syncing only the fresh `kernel_optix*.zst` files was not enough for the main build, because a
    factory-startup `OPTIX` render still loaded stale runtime `kernel_sm_89.cubin.zst` and then
    failed later in `cuMemcpyHtoD_v2(...)` from `intern/cycles/device/optix/device_impl.cpp:1935`
  - after syncing both sets of fresh runtime payloads:
    - `kernel_optix*.zst`
    - `kernel_sm_*.cubin.zst`
    into that active runtime folder, the main build again passes a factory-startup OptiX render:
    `E:\blender_modify\blender\.agent\tmp\main_build_optix_postsync2_0001.png`
  - corrected current rule:
    - for main-build OptiX validation after kernel rebuilds, refresh both the OptiX PTX payloads
      and the CUDA cubin payloads in the active runtime `cycles\lib` folder before trusting the
      result

## Immediate Documentation Goal

- Keep `.agent/` maintainable for long-run work.
- Keep root docs small and operational.
- Keep feature-specific history and validation under `features/`.
- Move scratch artifacts out of the root and into `tmp/` or `archive/`.
