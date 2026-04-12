# World Environment Fog Validation Notes

## Foggy Street Box Scene

- Builder script: `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_test_scene.py`
- Generated blend: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend`
- Earlier factory-startup render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test0001.png`
- Default-profile fog-on render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_default_profile0001.png`
- Default-profile fog-off render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_default_profile_off0001.png`
- Intermediate scatter-visible render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_visibility_fix0001.png`
- Intermediate scatter-visible fog-off render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_visibility_fix_off0001.png`
- Tuned scatter fog-on render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_tuned0001.png`
- Tuned scatter fog-off render: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_tuned_off0001.png`

## Default-Profile Render Command

```powershell
E:\blender_modify\build_env_fog\bin\Release\blender.exe -b E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend --python-expr "import bpy; bpy.context.scene.cycles.device='GPU'" -o E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_default_profile -F PNG -f 1
```

## 2026-04-06 Scene / Device Follow-Up

- The test scene was expanded with dedicated area-light fixtures:
  - `WallWash_L`
  - `WallWash_R`
  - `BillboardGlow`
- The builder script no longer forces `scene.cycles.device='CPU'`.
- The regenerated validation blend now reports:
  - `scene.cycles.device = GPU`
- Plain default-profile render with no explicit device override:

```powershell
E:\blender_modify\build_env_fog\bin\Release\blender.exe -b E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend --render-output E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_profile_device_#### --render-frame 1
```

- Output:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_profile_device_0001.png`
- Practical read:
  - the scene now respects the saved/default-profile scene device instead of us forcing CPU
  - the loaded profile still reports an invalid/empty `CyclesPreferences.compute_device_type`, so
    the blend can say `GPU` while the backend selection remains suspect for true GPU execution in
    this feature-build runtime

## 2026-04-06 OptiX Build Reality Check

- We compared the fog feature runtime against the primary mainline runtime on the same machine.
- Primary mainline runtime:
  - `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`
  - `_cycles.get_device_types()` returned `(True, True, True, False, False, False)`
  - `_cycles.available_devices('OPTIX')` listed the RTX 4080 SUPER as an OptiX device
- Fog feature runtime:
  - `E:\blender_modify\build_env_fog\bin\Release\blender.exe`
  - `_cycles.get_device_types()` returned `(True, False, True, False, False, False)`
  - `_cycles.available_devices('OPTIX')` returned CPU only
- Build-config comparison explains the mismatch:
  - mainline `CMakeCache.txt` has:
    - `OPTIX_INCLUDE_DIR:PATH=E:/blender_modify/optix-dev/include`
    - `OPTIX_ROOT_DIR:PATH=E:/blender_modify/optix-dev`
    - `WITH_CYCLES_CUDA_BINARIES:BOOL=ON`
  - fog build `CMakeCache.txt` has:
    - `OPTIX_INCLUDE_DIR:PATH=OPTIX_INCLUDE_DIR-NOTFOUND`
    - `OPTIX_ROOT_DIR:PATH=`
    - `WITH_CYCLES_CUDA_BINARIES:BOOL=OFF`
- Generated project files confirm the active compile state:
  - mainline `intern/cycles/*.vcxproj` includes `WITH_OPTIX` in preprocessor definitions
  - fog build `intern/cycles/*.vcxproj` includes `WITH_CUDA` but not `WITH_OPTIX`
- Practical conclusion:
  - the fog build folder still contains stale `kernel_optix*.ptx.zst` files from an older or
    differently configured state
  - those files do not mean the current `build_env_fog` binaries actually expose OptiX
  - for now this feature build should be treated as CUDA-capable but not OptiX-capable until the
    build is reconfigured against the OptiX SDK and rebuilt

## 2026-04-07 OptiX Recovery

- `build_env_fog` was reconfigured with:
  - `OPTIX_ROOT_DIR=E:/blender_modify/optix-dev`
  - `OPTIX_INCLUDE_DIR=E:/blender_modify/optix-dev/include`
  - `WITH_CYCLES_CUDA_BINARIES=ON`
- Rebuild command used the documented workflow in:
  - `E:\blender_modify\blender\.agent\workflows\build-blender.md`
- Rebuild completed successfully and generated a new fog feature runtime:
  - `E:\blender_modify\build_env_fog\bin\Release\blender.exe`
  - build stamp reported by Blender:
    - `Blender 5.2.0 Alpha (hash 4b99a0f4725d built 2026-04-06 07:29:07)`
- Runtime verification after rebuild:
  - `_cycles.get_device_types()` returned:
    - `(True, True, True, False, False, False)`
  - `_cycles.available_devices('OPTIX')` returned:
    - `NVIDIA GeForce RTX 4080 SUPER` as an `OPTIX` device
- Practical conclusion:
  - the fog feature build is now OptiX-capable again
  - future default-profile renders on this build can rely on the user's normal GPU device choice
    instead of us treating the runtime as CUDA-only

## Intent

- Provide a simple scene made from a ground plane and box primitives.
- Exercise the current environment fog kernel with explicit scene lights:
  - sun
  - area street lamps
  - point back fill
  - spot accent
- Keep HDRI out of the test so the current v1 fog-lighting rules remain valid.
- The current framing emphasizes warm street-side glow, a cool side spotlight, and primary-ray fog
  accumulation along the lane.

## Latest Result

- The stable renderer fix on `2026-04-04` was not just gain tuning.
- Two correctness issues were fixed in the fog kernel:
  - the scatter phase now uses the camera-path direction, matching Cycles volume-scatter
    conventions
  - explicit lights are now filtered with `PATH_RAY_VOLUME_SCATTER` visibility instead of
    `PATH_RAY_CAMERA`
- That visibility fix matters because the validation lights use the default Blender setup:
  `visible_camera=False` and `visible_volume_scatter=True`.
- After the visibility fix, the first scatter render (`foggy_street_scatter_visibility_fix0001.png`)
  proved local-light fog was active, but it was heavily over-bright and too close to a white-out.
- The tuned render (`foggy_street_scatter_tuned0001.png`) keeps a readable overhead shaft and
  side-light atmosphere without the earlier full-frame wash.
- Tuned fog-on vs fog-off image comparison:
  - average full-frame luma delta: about `15.40`
  - max luma delta: about `85.51`
  - different pixels: `1424360 / 1440000`

## Notes

- Background renders with the normal user profile still complete, but they emit many addon errors
  from incompatible or partially missing Python modules in the feature build runtime.
- Those addon/profile warnings did not block the validation renders on `2026-04-04`.
- Attempting to force `CyclesPreferences.compute_device_type='OPTIX'` in this runtime hit an enum
  mismatch even though OptiX-class devices were listed; the stable validation commands therefore
  only set `scene.cycles.device='GPU'` and otherwise used the loaded default profile.

## Light-Type Scatter Check

- Quick per-type fog-on renders were written to:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\light_type_scatter`
- Outputs:
  - `point_fog_on0001.png`
  - `spot_fog_on0001.png`
  - `area_fog_on0001.png`
  - `sun_fog_on0001.png`
- Practical read from the current alley scene:
  - `Spot` gives the clearest visible shaft and is the strongest beam-style case.
  - `Point` contributes visible localized haze and wall/ground lift, but reads more as glow than a
    hard shaft.
  - `Sun` / distant light contributes broad atmospheric fill, not a narrow beam.
  - `Area` is technically routed through the fog kernel, but in this isolated test it reads very
    weakly and the image is close to black, so area-light scatter still needs look tuning if it is
    expected to be a strong artist-facing case.

## Physical-Gain Follow-Up

- On `2026-04-05` the fog kernel was changed to stop using per-light tuned gain constants.
- The current path instead:
  - evaluates lamps through Cycles light evaluation
  - applies shared homogeneous transmittance from camera-to-sample and sample-to-light
  - applies a single Henyey-Greenstein phase term from the fog anisotropy input
- Fresh isolated renders were written to:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\light_type_scatter_cli_2026-04-05`
- Key files:
  - `point_fog_on_cli30001.png`
  - `point_fog_off_cli30001.png`
  - `area_fog_on_cli30001.png`
  - `area_fog_off_cli30001.png`
- Practical read from these new outputs:
  - `Point` now clearly shows fog in front of the boxes and along the lane, instead of reading as
    only ordinary surface illumination.
  - `Area` now produces a readable fog volume in the isolated lamp test, fixing the earlier
    "effectively absent" result.
  - `Area` in the isolated fog-off baseline is still nearly black, so this setup remains a
    fog-read test more than a balanced beauty-lighting setup.
- Full-scene beauty follow-up:
  - `foggy_street_physical_full0001.png`
  - Path:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_physical_full0001.png`
  - Practical read:
    - the physical scatter path now makes the isolated point and area cases read correctly
    - the full alley beauty with all lights enabled is currently too hot / washed out, so the next
      look-dev step is rebalancing density, exposure, or light transport shaping for the combined
      scene rather than going back to per-light gain constants

## PDF Normalization Correction

- Same-day follow-up on `2026-04-05`:
  - A temporary kernel edit removed sampled-light `pdf` normalization from the fog path.
  - That change was incorrect for finite lights because Cycles direct-light energy for point /
    spot / area lamps depends on `eval_fac / pdf`, which carries the distance / solid-angle
    falloff.
- Regression proof renders:
  - point over-bright regression:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\light_type_scatter_cli_2026-04-05\point_fog_on_pdf_fix0001.png`
  - corrected point render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\light_type_scatter_cli_2026-04-05\point_fog_on_pdf_restore0001.png`
  - corrected full-scene render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_physical_full_pdf_restore0001.png`
- Quantitative read from sampled image stats:
  - `point_fog_on_cli30001.png`: mean RGB about `153.64 / 136.88 / 117.98`
  - `point_fog_on_pdf_fix0001.png`: mean RGB about `245.66 / 238.09 / 230.07`
  - `point_fog_on_pdf_restore0001.png`: mean RGB about `153.64 / 136.88 / 117.98`
  - `foggy_street_physical_full0001.png`: mean RGB about `254.71 / 254.92 / 254.92`
  - `foggy_street_physical_full_pdf_restore0001.png`: mean RGB about `166.82 / 155.88 / 147.53`
  - `foggy_street_physical_full_off0001.png`: mean RGB about `88.79 / 83.97 / 81.30`
- Practical conclusion:
  - the immediate whiteout cause in the current physical-gain branch was the removed `pdf`
    normalization, not the move away from per-light gain constants itself
  - after restoring `eval_fac / pdf`, the point-light probe matches the earlier sane result and the
    full street scene becomes readable again
  - remaining brightness pressure still comes from the intended v1 design:
    - additive-only camera-segment accumulation
    - no fog-side object shadowing
    - a deliberately harsh validation setup with near-white fog, density `0.24`, max distance
      `36.0`, and many strong local lights

## Density Reduction Check

- On `2026-04-05`, the validation scene density was reduced from `0.24` to `0.08` while keeping the
  rest of the lighting/layout unchanged.
- Updated sources:
  - builder script:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_test_scene.py`
  - saved validation blend:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend`
- New render:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_density_0080001.png`
- Sampled image stats:
  - `foggy_street_physical_full_pdf_restore0001.png`: mean RGB about `166.82 / 155.88 / 147.53`
  - `foggy_street_density_0080001.png`: mean RGB about `191.04 / 178.18 / 165.34`
- Practical read:
  - lowering density in this additive v1 fog path does not simply darken the beauty
  - the frame became brighter overall because the reduced medium extinction let more base beauty
    survive while the explicit-light additive fog term still contributed strongly
  - this makes density alone a weak control for fixing washout in the current additive/no-shadow
    design

## Opaque Shadow Test

- On `2026-04-05`, after identifying visible spot-light leakage through the right-side wall, the
  fog kernel was updated to cast an opaque shadow query from each fog sample toward the light using
  `scene_intersect_shadow(PATH_RAY_SHADOW_OPAQUE)`.
- New render:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_density_008_shadowed0001.png`
- Comparison against the non-shadowed density-`0.08` render:
  - `foggy_street_density_0080001.png`: mean RGB about `191.04 / 178.18 / 165.34`
  - `foggy_street_density_008_shadowed0001.png`: mean RGB about `113.33 / 112.57 / 116.68`
- Practical read:
  - this confirms the user's diagnosis that missing light-side occlusion was a real source of
    physically implausible fog, especially for the visible spot leak through walls
  - the first opaque-only implementation is expensive in the current fixed-step/per-light loop and
    roughly tripled the render time for the alley test
  - it is also a very strong visual lever, so the next step is likely tuning / mode-gating rather
    than treating this exact behavior as the final default

## 2026-04-07 GPU Regression Repair

- Installed release used for comparison:
  `C:\Program Files\Blender Foundation\blender-vfx-5.2-2026-03-26`
- Key finding:
  - the broad local GPU crash on both local builds was caused by stale Cycles runtime kernels in
    each build tree's `bin\Release\5.2\scripts\addons_core\cycles\lib` folder
  - this was not a source-parity problem against the March 26 release
  - the local executables were newer, but the runtime GPU payloads were still carrying older
    March 19 artifacts
- Main build repair validation:
  - plain factory-startup `CUDA` render now succeeds:
    `E:\blender_modify\blender\.agent\tmp\main_build_cuda8_fixed_0001.png`
  - plain factory-startup `OPTIX` render now succeeds:
    `E:\blender_modify\blender\.agent\tmp\main_build_optix8_fixed_0001.png`
- Fog build repair validation:
  - plain factory-startup `CUDA` render now succeeds:
    `E:\blender_modify\blender\.agent\tmp\fog_build_cuda8_fixed_0001.png`
  - plain factory-startup `OPTIX` render now succeeds:
    `E:\blender_modify\blender\.agent\tmp\fog_build_optix8_fixed_0001.png`
- Same-day fog helper repair:
  - `intern/cycles/kernel/integrator/environment_fog.h` was restored from a temporary empty
    diagnostic stub to a minimal additive camera-segment implementation
  - the restored path evaluates fog, multiplies by current path throughput, and writes it as
    additive volume emission without mutating integrator path state
- Fog scene results after rebuilding the fog kernels and refreshing the runtime payload:
  - `CUDA` fog-scene render succeeds:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cuda32_after_apply_fix_0001.png`
  - `OPTIX` fog-scene render still fails with:
    `Illegal address in CUDA queue copy_from_device (integrator_shade_surface integrator_sorted_paths_array prefix_sum)`
- Practical conclusion:
  - the old broad GPU regression is fixed
  - the remaining crash is now isolated to the environment-fog scene path on `OPTIX`
  - the most suspicious remaining interaction is the experimental opaque-only fog shadow query,
    because the current branch docs still describe that path as experimental and outside the
    intended v1 baseline

## 2026-04-08 OptiX Runtime Path Correction

- The previous OptiX crash investigation was partly polluted by stale runtime payloads again.
- `--debug-cycles` on the fog build showed the active precompiled kernel path is:
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
- Practical consequence:
  - rebuilding `cycles_kernel_optix.vcxproj` and `cycles_kernel_cuda.vcxproj` is not enough by
    itself
  - the generated `kernel_*.zst` files must be copied from:
    - `E:\blender_modify\build_env_fog\intern\cycles\kernel\device\optix`
    - `E:\blender_modify\build_env_fog\intern\cycles\kernel\device\cuda`
  - into:
    - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
- Same-day rebuild/sync:
  - `blender` was rebuilt so the host runtime stamp moved to:
    - `Blender 5.2.0 Alpha (hash 4b99a0f4725d built 2026-04-08 05:27:38)`
  - `cycles_kernel_optix.vcxproj` was rebuilt after the latest V2 edits
  - fresh `kernel_*.zst` payloads were synced into the active runtime folder above
- Control proof:
  - with a temporary OptiX-only fog-eval no-op patch, the isolated sun-only fog scene rendered
    successfully once the runtime folder was refreshed
  - after reverting that diagnostic patch and rebuilding/syncing again, the real V2 path also
    rendered successfully
- Current successful OptiX outputs:
  - isolated sun-only fog sanity render:
    `E:\blender_modify\blender\.agent\tmp\optix_background_sun_only_real_0001.png`
  - full validation scene, 1 spp:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples1_0001.png`
  - full validation scene, 8 spp:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples8_0001.png`
- Practical conclusion:
  - the earlier 2026-04-07 OptiX crash conclusion is no longer current truth
  - after refreshing the actual active runtime kernels, the present V2 fog scene is stable on
    OptiX for the validated alley scene
  - the recurring addon `tomllib` exception still appears on CLI shutdown, but it did not block
    any of the successful renders above

## 2026-04-08 Locked V2 Shadow Scope

- The current V2 shadow behavior is now accepted as the milestone scope for this feature pass.
- Accepted in-scope behavior:
  - opaque light-side fog shadowing on camera-to-surface segments
- Current code-path truth:
  - `shade_surface` passes `use_light_shadows = true`
  - `shade_background` passes `use_light_shadows = false`
  - `shade_light_forward` passes `use_light_shadows = false`
- Practical meaning:
  - the main wall-leak case is addressed for geometry-facing camera views
  - open-background fog and direct-light-hit camera segments remain unshadowed in the current
    milestone
- Deferred beyond the current feature-complete target:
  - background-segment fog shadowing
  - direct-light-hit segment fog shadowing
  - broader volumetric-style shadow expansion beyond the current camera-surface V2 slice

## 2026-04-08 Final Validation Matrix

- Final output folder:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08`
- Successful controlled renders on the current alley validation scene:
  - `CUDA`, 1 spp:
    `foggy_street_cuda_s1_0001.png`
  - `CUDA`, 32 spp:
    `foggy_street_cuda_s32_0001.png`
  - `OPTIX`, 1 spp:
    `foggy_street_optix_s1_0001.png`
  - `OPTIX`, 32 spp:
    `foggy_street_optix_s32_0001.png`
- Practical timing summary from the render logs:
  - `CUDA`, 1 spp: about `00:02.14`
  - `CUDA`, 32 spp: about `00:02.88`
  - `OPTIX`, 1 spp: about `00:02.38`
  - `OPTIX`, 32 spp: about `00:03.33`
- All four runs exited successfully on `2026-04-08`.
- Same-run runtime note:
  - CLI shutdown still prints the known non-blocking addon exception:
    `ModuleNotFoundError: No module named 'tomllib'`
  - this did not block any of the saved renders above

## 2026-04-08 Default-Profile Device Confirmation

- A plain non-`--factory-startup` render was re-run with `--debug-cycles` and no explicit device
  override.
- Output:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_default_profile_debug_0001.png`
  - debug log:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_default_profile_debug.log`
- Important runtime truth from the debug log:
  - the loaded normal user profile selected:
    `Path tracing on: NVIDIA GeForce RTX 4080 SUPER (OptiX)`
- Practical conclusion:
  - the saved alley validation scene now respects the user's default-profile OptiX choice
  - we no longer need to treat normal-profile validation on this build as CPU-suspect
  - unrelated addon/profile import errors still appear on shutdown, but the render completed and the
    backend selection itself was correct

## 2026-04-08 Explicit HDRI Ignore Proof

- Controlled proof outputs:
  - dark background variant:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_hdri_ignore_dark_0001.png`
  - bright background variant:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_hdri_ignore_bright_0001.png`
  - report:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_hdri_ignore_report.txt`
- Controlled setup:
  - `OPTIX`, factory-startup, 8 spp
  - all explicit scene lights hidden
  - film transparent enabled
  - view-layer material override forced all visible geometry to pure black
  - only the world `Background` surface color / strength changed between the two renders
- Measured result:
  - dark avg RGBA:
    `0.022032, 0.022032, 0.022035, 0.908389`
  - bright avg RGBA:
    `0.022032, 0.022032, 0.022035, 0.908389`
  - average absolute RGB diff:
    `0.000000`
  - max absolute RGB diff:
    `0.000000`
- Practical conclusion:
  - changing the world surface background alone does not change the fog result in this controlled
    scene
  - this is explicit validation that the current v1/v2 fog path ignores `LIGHT_BACKGROUND` / HDRI

## 2026-04-08 Camera-Hit Clipping Proof

- Baseline / blocker outputs:
  - baseline:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_holdout_baseline_0001.png`
  - camera blocker:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_camera_blocker_full_0001.png`
  - report:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\foggy_street_camera_blocker_clip_report.txt`
- Controlled setup:
  - baseline is the normal alley scene with transparent film
  - blocker render adds a camera-parented full-frame opaque black plane at the first visible camera
    segment
- Measured result:
  - baseline avg luma:
    `0.499992`
  - blocker avg luma:
    `0.447611`
- Practical conclusion:
  - a foreground camera hit measurably truncates the visible alley / fog contribution behind it
  - this is the final explicit clipping proof captured on the current CLI render path

## 2026-04-08 Holdout PNG Proof Limitation

- Additional same-day tests attempted a direct holdout-image proof with:
  - holdout shader on a camera-facing plane in the fog build
  - `Object.is_holdout` on a camera-facing plane in the fog build
  - `Object.is_holdout` on the same setup in the installed March 26 release build
- Output note:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08\holdout_png_proof_note_2026-04-08.txt`
- Observed result:
  - the saved PNG combined output and average alpha remained effectively unchanged in all three
    cases
- Practical conclusion:
  - this does not currently read as a world-environment-fog-only regression
  - the present CLI PNG combined-output path is not a reliable standalone proof vehicle for holdout
    compositing behavior
  - for current feature close-out, camera-hit clipping is treated as explicitly validated

## 2026-04-08 Scope Lock For Feature Complete

- Shipped / validated in the current feature-complete milestone:
  - world-only environment fog node and world sync
  - primary camera-segment fog on:
    - surface hits
    - direct-light hits
    - background hits
  - explicit light support for:
    - point
    - spot
    - area
    - sun
  - `HDRI` / `LIGHT_BACKGROUND` excluded
  - accepted V2 opaque light-side shadowing on surface camera segments
  - stable `CUDA` and `OPTIX` validation on the alley test scene
- Explicitly deferred beyond this milestone:
  - emissive-geometry fog lighting
  - dedicated fog pass / fog AOV
  - fog light-AOV / lightgroup split behavior
  - broader shadow expansion beyond the accepted current V2 surface-only slice
  - compositor-focused holdout proof beyond the current CLI PNG validation path

## 2026-04-09 OptiX Beauty Retune

- The alley validation scene lighting was retuned to avoid the previous "mostly cold" read in the
  combined beauty render.
- Updated palette direction in the builder scene:
  - warm amber / orange street and beam accents
  - magenta / violet billboard and lamp accents
  - cyan / teal side and wall-wash accents
  - one green point accent for clearer separation across fixtures
- Updated source:
  - builder script:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_test_scene.py`
  - regenerated blend:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend`
- New requested validation render:
  - `OPTIX` only
  - all lights active together
  - no separate light-type breakdown
  - scene's own saved render settings, with no low-sample override
- Output folder:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09`
- Main beauty output:
  - `foggy_street_optix_default_scene_0001.png`
- Practical render settings used from the saved scene:
  - samples: `192`
  - adaptive sampling: `True`
  - resolution: `1600 x 900`
- Timing from the CLI render log:
  - about `00:07.27`
- Practical conclusion:
  - the current primary approval image for this validation scene is now the full all-lights OptiX
    beauty above, using the saved scene defaults rather than the earlier 1 spp / matrix probes

## 2026-04-09 Dedicated Cyberpunk Test Scene

- The older reference image
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk0001.png`
  did not actually have a preserved dedicated `.blend` alongside it.
- To stop drifting between the generic alley scene and the intended look target, a dedicated
  cyberpunk validation scene was added:
  - builder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_cyberpunk_scene.py`
  - blend:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
- Validation outputs on `OPTIX` with the saved scene defaults:
  - first dedicated scene render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_cyberpunk_test_optix_default_0001.png`
  - tuned follow-up after removing the purple cast:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_cyberpunk_test_optix_default_v2_0001.png`
- Practical scene direction:
  - all lights remain active together
  - warm / neutral wall lighting
  - cool blue beam and distance haze
  - dark cube / trim materials retained
- Practical conclusion:
  - `foggy_street_cyberpunk_test_optix_default_v2_0001.png` is the current dedicated cyberpunk
    validation render and is the right reference for future OptiX beauty checks
