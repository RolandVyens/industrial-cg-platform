# World Environment Fog Context

## Status

- Branch: `feature/world-environment-fog`
- Worktree: `E:\blender_modify\blender_env_fog`
- Build: `E:\blender_modify\build_env_fog`
- State: synced to `vfx-rendering-branch-github` at `4b99a0f4725` on `2026-04-03`; after the mainline upstream merge to `08fcb983633` on `2026-04-11`, this worktree is now behind current VFX mainline and should be re-synced before new feature work resumes; the current direct-light scatter slice compiles and now uses Cycles light evaluation plus shared homogeneous scatter/transmittance terms as of `2026-04-05`
- Current docs:
  - research: `RESEARCH_2026-04-02.md`
  - realtime follow-up: `RESEARCH_REALTIME_FOG_2026-04-03.md`
  - consolidated design: `DESIGN_2026-04-03.md`
  - implementation plan: `IMPLEMENTATION_PLAN_2026-04-03.md`
  - remaining tasks: `REMAINING_TASKS_2026-04-08.md`

## Current Progress

- The world-only `ShaderNodeEnvironmentFog` registration slice is implemented in the fog worktree.
- Cycles background state now carries fog parameters into `KernelBackground`.
- `sync_world()` now scans the active world node graph for a connected Environment Fog node and
  populates the background fog state.
- The Cycles kernel now evaluates the fog on primary camera segments for:
  - camera to surface hit
  - camera to direct lamp hit
  - camera to background
- The current kernel slice:
  - keeps the effect additive on the primary camera segment instead of dimming the whole view with
    strong extinction
  - adds fixed-step fog contribution from explicit scene lights only
  - removes the earlier ambient haze term so the look is driven by actual scene lights
  - evaluates the scatter phase using the camera-path direction, matching Cycles volume-scatter
    conventions
  - evaluates light visibility as `PATH_RAY_VOLUME_SCATTER`, so the default
    `visible_camera=False` light setup still contributes to the fog
  - derives lamp radiance from Cycles light evaluation instead of per-light tuned gain constants
  - restores finite-light sampled normalization with `eval_fac / pdf`, matching Cycles direct-light
    energy falloff for point / spot / area lights after a brief 2026-04-05 regression
  - uses a shared homogeneous single-scatter step term:
    - transmittance from camera to sample
    - transmittance from sample to light
    - Henyey-Greenstein phase from the fog anisotropy input
  - excludes `LIGHT_BACKGROUND` / HDRI lighting
  - now has an experimental opaque-only light-side shadow test on `2026-04-05`, using
    `scene_intersect_shadow(PATH_RAY_SHADOW_OPAQUE)` from each fog sample toward the light
  - requests `KERNEL_FEATURE_NODE_RAYTRACE` whenever environment fog is enabled so OptiX raytrace
    kernels are available even without material-node raytrace features
  - routes camera-hit surfaces through the raytrace surface kernel when environment fog is enabled,
    even if the surface shader itself does not advertise `SD_HAS_RAYTRACE`
  - accepted V2 milestone scope now limits the explicit opaque shadow query to surface camera
    segments only
    - `shade_surface`: shadow query enabled
    - `shade_background`: shadow query disabled
    - `shade_light_forward`: shadow query disabled
  - this fixes obvious "light through wall" fog leakage while keeping broader background/light-hit
    shadowing explicitly deferred
  - does not yet include emissive-geometry fog lighting
- On `2026-04-07`, the source-side helper `integrator_environment_fog_apply()` was restored from a
  temporary no-op diagnostic stub to a minimal additive implementation:
  - evaluate fog only on the current camera segment
  - multiply by the current path throughput
  - write the result as additive volume emission without mutating path state
- On `2026-04-07`, broad local GPU crashes were traced to stale runtime kernel payloads under each
  build tree's `bin\Release\5.2\scripts\addons_core\cycles\lib` folder rather than to source
  divergence from the March 26 release.
  - After refreshing those runtime kernels from the actual build outputs, both local builds again
    passed plain factory-startup GPU renders on `CUDA` and `OPTIX`.
  - The fog validation scene now renders successfully on the fog build with `CUDA`:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cuda32_after_apply_fix_0001.png`
- On `2026-04-08`, the active runtime kernel path for CLI validation was confirmed with
  `--debug-cycles`:
  - `E:\blender_modify\build_env_fog\bin\Release\5.2\scripts\addons_core\cycles\lib`
  - targeted kernel rebuilds do not automatically refresh that runtime folder
  - syncing the rebuilt `kernel_*.zst` payloads there was required before OptiX validation matched
    current source
- On `2026-04-08`, after rebuilding both the host runtime and the OptiX/CUDA kernel payloads, the
  current V2 fog path now renders successfully on the fog build with `OPTIX`:
  - isolated sun-only fog sanity render:
    `E:\blender_modify\blender\.agent\tmp\optix_background_sun_only_real_0001.png`
  - full validation scene, 1 spp:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples1_0001.png`
  - full validation scene, 8 spp:
    `E:\blender_modify\blender\.agent\tmp\optix_full_scene_samples8_0001.png`
- On `2026-04-08`, the V2 milestone shadow scope was explicitly locked:
  - in scope:
    - opaque light-side fog shadowing on camera-to-surface segments
  - deferred:
    - opaque light-side fog shadowing on camera-to-background segments
    - opaque light-side fog shadowing on camera-to-direct-light-hit segments
  - rationale:
    - this covers the most visible wall-leak case that motivated V2
    - it matches the currently validated code path
    - it keeps broader shadow expansion out of the feature-complete blocker list
- On `2026-04-08`, the final alley validation matrix was completed under:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-08`
  - successful outputs now include:
    - `CUDA`, 1 spp
    - `CUDA`, 32 spp
    - `OPTIX`, 1 spp
    - `OPTIX`, 32 spp
- On `2026-04-08`, a non-`--factory-startup` default-profile render was re-run with
  `--debug-cycles`, and the loaded user profile selected:
  - `NVIDIA GeForce RTX 4080 SUPER (OptiX)`
- On `2026-04-08`, explicit close-out proofs were added:
  - HDRI-ignore proof:
    `foggy_street_hdri_ignore_dark_0001.png`
    `foggy_street_hdri_ignore_bright_0001.png`
  - camera-hit clipping proof:
    `foggy_street_camera_blocker_full_0001.png`
  - supporting reports:
    `foggy_street_hdri_ignore_report.txt`
    `foggy_street_camera_blocker_clip_report.txt`
- Same-day holdout validation note:
  - direct PNG combined-output holdout proof did not produce a reliable alpha/image delta in either
    the fog build or the installed March 26 release control
  - current close-out therefore treats camera-hit clipping as the explicit proof path, while
    compositor-focused holdout validation is documented as deferred
- Current milestone scope is now treated as feature complete for this pass.
  - shipped / validated:
    - node + world sync
    - point / spot / area / sun lighting
    - no HDRI contribution
    - accepted V2 surface-only opaque light-side shadowing
    - stable `CUDA` and `OPTIX` alley-scene validation
  - explicitly deferred:
    - emissive-geometry fog lighting
    - fog light-AOV / lightgroup split behavior
    - broader shadowing beyond the accepted V2 slice
- On `2026-04-09`, the alley validation scene palette was retuned so the combined all-lights beauty
  no longer reads predominantly cold.
  - the saved scene now mixes warm amber/orange, magenta/violet, cyan/teal, and green accents
    across the active fixtures
  - the validation blend was regenerated from:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_test_scene.py`
  - a fresh all-lights `OPTIX` beauty render was then recorded using the scene's own saved render
    settings at:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_optix_default_scene_0001.png`
  - this is now the primary beauty render for the current alley validation scene
- Later on `2026-04-09`, we confirmed that the generic alley retune above was still not the same
  thing as the older cyberpunk reference image the user was judging against.
  - a dedicated preserved cyberpunk scene was therefore added:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - source builder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_cyberpunk_scene.py`
  - the current dedicated cyberpunk OptiX beauty render is:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_cyberpunk_test_optix_default_v2_0001.png`
  - this scene keeps all lights active together, but biases the result toward warm wall lighting,
    a cool blue beam, and the darker cube treatment requested during earlier look-dev
- Later on `2026-04-09`, the dedicated cyberpunk builder was rebalanced again toward a lower-key
  neon look while preserving the same all-lights validation setup.
  - the world background was darkened further to reduce background wash
  - fog density was reduced slightly and anisotropy was raised to favor beam readability
  - ground, wall, trim, and pole materials were darkened for stronger volume contrast
  - broad fill lights were reduced, while the two spot fixtures were strengthened and separated
    chromatically:
    - `BeamSpot`: cyan
    - `SideSpot`: magenta
  - the street/area lights were also spread across orange, magenta, teal, and blue accents so the
    beauty reads more clearly as a cyberpunk low-key theme
  - the saved validation blend was regenerated from the builder on the same day:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - a fresh all-lights `OPTIX` beauty render from that rebalanced scene was recorded at:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_cyberpunk_test_optix_lowkey_0001.png`
- Later on `2026-04-09`, geometry-holdout validation exposed a transparency mismatch in the
  additive fog path:
  - the holdout beauty PNG still contained fog RGB, but alpha stayed zero everywhere, so the image
    appeared blank in normal viewers
  - root cause:
    - the additive fog helper wrote RGB through `film_write_volume_emission()`
    - but transparent-background alpha for holdout/background paths was still using the untouched
      path throughput, because this V2 fog slice intentionally did not mutate path state
  - same-day fix:
    - transparent-background holdout and background writes now multiply their transparency by the
      environment-fog segment transmittance for that camera segment
    - this keeps the additive RGB path intact while making geometry-holdout renders carry visible
      fog alpha
  - updated OptiX holdout validation render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-09\foggy_street_cyberpunk_test_optix_holdout_geo_fix_0001.png`
- Later on `2026-04-09`, a dedicated built-in Cycles fog pass / AOV was implemented end-to-end and
  validated on the OptiX cyberpunk scene.
  - code path:
    - `PASS_FOG` is registered in Cycles pass metadata and Blender pass sync/UI
    - the environment-fog integrator now writes the fog contribution into that pass while keeping
      the existing additive beauty/emission behavior intact
  - validation script:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\render_fog_pass_validation.py`
  - validation folder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09`
  - primary artifacts:
    - raw fog pass EXR:
      `foggy_street_cyberpunk_test_optix_fog_pass_0001.exr`
    - PNG preview converted from that EXR:
      `foggy_street_cyberpunk_test_optix_fog_pass_0001.png`
  - runtime validation truth:
    - `--debug-cycles` reported `type: fog, name: "Fog", mode: NOISY, is_written: True`
    - the same render used `NVIDIA GeForce RTX 4080 SUPER (OptiX)`
  - current compositor quirk on this 5.2 build:
    - the file-output node reliably writes the fog pass as EXR
    - the human-readable PNG preview is therefore generated from that saved EXR as a follow-up
      validation step instead of directly from the compositor node
- Later on `2026-04-09`, the dedicated cyberpunk validation scene was promoted to the default
  baseline for current fog testing rather than treating the older box-street scene as the primary
  active reference.
  - current default validation blend:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - current default scene builder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_cyberpunk_scene.py`
  - current active test helpers were updated to use cyberpunk-specific naming/output for:
    - A/B fog render outputs
    - light-type scatter outputs
    - dedicated fog-pass exports
- Same-day point-light fog follow-up on the cyberpunk scene:
  - user review correctly noted that the wall-adjacent point lights were still half-buried enough to
    read weakly in the dedicated fog pass
  - the three `StreetGlow_*` point lights were moved farther into the alley volume while preserving
    their wall-adjacent look, and `BackFill` was moved/boosted slightly as well
  - focused point-only fog-pass validation was then re-run on `OPTIX` with:
    - `FOG_ENABLED_LIGHT_TYPES=POINT`
    - `FOG_PASS_TAG=point_only`
  - resulting artifacts:
    - EXR:
      `foggy_street_cyberpunk_test_optix_fog_pass_point_only_0001.exr`
    - PNG preview:
      `foggy_street_cyberpunk_test_optix_fog_pass_point_only_0001.png`
  - measured EXR stats now confirm a clearly non-empty point-light fog pass:
    - mean: about `0.156`
    - 99th percentile: about `1.238`
    - max: about `248.017`
- On `2026-04-10`, the combined all-lights cyberpunk beauty was rendered again to a fresh dated
  validation folder after the prior file location was easy to miss among older outputs.
  - command path:
    `E:\blender_modify\build_env_fog\bin\Release\blender.exe`
  - input scene:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - saved beauty output:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_rerender_0001.png`
  - saved render log:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_rerender.log`
  - runtime truth from the log:
    - the render completed successfully and saved the file above
    - the log shows OptiX acceleration-structure builds during scene/device update, consistent with
      the user's default-profile GPU path being active for this run
- Later on `2026-04-10`, point/spot fog striping on the near wall-adjacent point light was traced
  further after the earlier center-ray shadow fix.
  - confirmed runtime truth:
    - the cyberpunk scene's point lights currently use `use_soft_falloff=True`
    - in Cycles this means the finite point lights are treated as disk-like rather than spherical
      emitters
  - code-side interpretation:
    - the remaining artifact is no longer primarily the old single center-ray wall cutoff
    - it now reads more like residual structured sampling from:
      - disk-like finite point-light sampling
      - fixed fog segment marching positions
  - same-day low-cost follow-up:
    - the fog camera-segment integration was changed from fixed per-step midpoints to jittered
      stratified step positions keyed from the path RNG
    - this preserves the same nominal fog step count while allowing render spp to average out the
      previous stable "24 slice" look over time
  - implementation file:
    `E:\blender_modify\blender_env_fog\intern\cycles\kernel\integrator\environment_fog.h`
  - fast validation build path:
    - instead of waiting on a full `cycles_kernel_optix` rebuild, the affected OptiX PTX payloads
      were regenerated directly with `nvcc` and written into the active runtime folder
    - the faster workflow is now documented in:
      `E:\blender_modify\blender\.agent\workflows\build-blender.md`
  - validation outputs:
    - EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_step_jitter_0001.exr`
    - PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_step_jitter_0001.png`
  - current read:
    - the stable slice pattern is reduced, but a visible residual near-light stripe remains
    - next likely improvement, if needed, is a small multi-sample average for point/spot fog light
      shape or shadow evaluation rather than a broad increase in fog step count
- Later on `2026-04-10`, that next point/spot follow-up was implemented as a small targeted
  multisample average rather than a broad fog-step increase.
  - scope:
    - only `LIGHT_POINT` / `LIGHT_SPOT`
    - only when the Cycles finite light has nonzero radius and is not spherical
    - current sample count:
      - `2` light-shape samples per fog step
  - rationale:
    - this specifically targets the remaining striping on the near wall-adjacent point light
    - it avoids globally increasing fog step count for every light/path
  - implementation file:
    `E:\blender_modify\blender_env_fog\intern\cycles\kernel\integrator\environment_fog.h`
  - validation outputs:
    - EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_step_jitter_multisample2_0001.exr`
    - PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_step_jitter_multisample2_0001.png`
  - current read:
    - the left-near point-light stripe is softened further compared with the step-jitter-only
      result
    - the result is closer to the intended fog mass, but not yet perfectly smooth
    - this now looks like a reasonable low-cost stopping point unless stricter quality is required
- Later on `2026-04-10`, an exploratory global fog-step increase was measured on the active
  cyberpunk scene to estimate the cost of a brute-force quality push.
  - test change:
    - `ShaderNodeEnvironmentFog` scene sample count in the cyberpunk builder was raised from `24`
      to `48`
  - regenerated scene:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - OptiX all-lights beauty timing result:
    - output:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_fog48_0001.png`
    - log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_fog48.log`
    - reported render time:
      - `00:24.99`
  - practical read:
    - a global fog-step increase is materially more expensive than the targeted jitter /
      point-light multisample adjustments
    - this confirms that broad fog-step scaling should be treated as a heavier fallback, not the
      default next move for iteration
- Later on `2026-04-10`, that temporary brute-force fog-48 scene tweak was rolled back and the
  finer point/spot fog fix path was continued instead.
  - scene baseline reset:
    - the dedicated cyberpunk builder now sets the Environment Fog node `Samples` back to `24`
    - regenerated baseline blend:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_cyberpunk_test.blend`
  - kernel refinement:
    - the finite point/spot fog branch no longer averages sampled radiance, direction, and light
      distance first and then evaluates one shared phase / light-transmittance term
    - instead, each finite-light subsample now evaluates its own:
      - sampled radiance
      - phase
      - transmittance to light
      - optional opaque shadow term
    - those full per-subsample fog contributions are then averaged back into the current fog step
  - rationale:
    - this keeps direction- and distance-dependent fog terms aligned with the actual sampled light
      shape, which is more faithful for near-wall / near-light cases than using an averaged proxy
      direction
  - implementation file:
    `E:\blender_modify\blender_env_fog\intern\cycles\kernel\integrator\environment_fog.h`
  - validation outputs:
    - focused point-only fog pass EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample_full_0001.exr`
    - focused point-only fog pass PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample_full_0001.png`
    - full all-lights beauty rerender:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_subsample_full_0001.png`
  - current read:
    - the result is subtly smoother around the near point-light artifact without paying the large
      cost of doubling the global fog step count
    - the remaining structure is now in diminishing-returns territory rather than the earlier
      obvious fixed-slice look
- Later on `2026-04-10`, a stricter local quality probe raised the soft-falloff point/spot finite
  light fog subsample count from `2` to `4` for measurement only.
  - scope:
    - same targeted branch as before:
      - only `LIGHT_POINT` / `LIGHT_SPOT`
      - only finite soft-falloff lights with nonzero radius and `!is_sphere`
  - validation outputs:
    - focused point-only fog pass EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample4_0001.exr`
    - focused point-only fog pass PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_subsample4_0001.png`
    - full all-lights beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_subsample4_0001.png`
    - beauty timing log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_subsample4.log`
  - measured cost:
    - point-only fog pass render:
      - about `04:11.875`
    - full all-lights beauty wall-clock time:
      - about `22.51s`
  - measured image delta versus the prior per-subsample-averaged `2`-sample result:
    - point-only ROI mean difference stayed around the low `1e-3` range
    - all-lights ROI mean difference stayed around the low `1e-4` range
  - decision:
    - the quality gain was too small for the added cost
    - the default source/runtime behavior was therefore restored to the earlier `2`-sample
      targeted point/spot setting after recording the result
- Later on `2026-04-10`, a more physically-motivated nonuniform camera-step experiment was tested
  for point/spot fog integration and also rejected as the new default.
  - implementation idea:
    - keep the same nominal point/spot fog step count
    - redistribute those steps across the camera segment using the cumulative homogeneous fog
      opacity, so denser fog automatically allocates more strata nearer the camera
    - preserve per-step weighting by recomputing each warped step's true segment length before
      evaluating `sample_scatter`
  - scope:
    - tested only on the point/spot fog camera-segment stepping path
    - finite-light subsample count remained at the accepted targeted default of `2`
  - validation outputs:
    - focused point-only fog pass EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_opacity_warp_0001.exr`
    - focused point-only fog pass PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_opacity_warp_0001.png`
    - full all-lights beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_opacity_warp_0001.png`
    - beauty timing log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\final_matrix_2026-04-10\foggy_street_cyberpunk_all_lights_optix_opacity_warp.log`
  - measured cost:
    - point-only fog pass render:
      - about `04:03.000`
    - full all-lights beauty render time:
      - about `00:14.06`
    - prior accepted all-lights beauty reference for comparison:
      - `foggy_street_cyberpunk_all_lights_optix_rerender_0001.png`
      - about `00:06.43`
  - measured image delta versus the accepted `subsample_full` baseline:
    - point-only ROI mean difference rose into the low `1e-3` range, so the change was visible
      but still modest
    - all-lights ROI mean difference stayed in the low `1e-4` range, so the beauty impact
      remained subtle
  - decision:
    - the opacity-warp path did not deliver enough improvement for the more-than-2x beauty time
      increase versus the accepted all-lights baseline
    - the source and active runtime were restored to the prior accepted uniform-step +
      per-subsample-averaged point/spot path after recording the result
- Later on `2026-04-11`, the temporary scene-side clearance probe was completed to test whether the
  remaining left-near point-light striping is primarily caused by the soft-falloff finite light
  overlapping the nearby wall.
  - probe script:
    `E:\blender_modify\blender\.agent\tmp\render_temp_inward_point_fog.py`
  - temporary scene-side change during render only:
    - `StreetGlow_A.x -> -2.0`
    - `StreetGlow_B.x -> 2.0`
    - `StreetGlow_C.x -> -2.0`
    - the saved blend itself was not modified
  - validation outputs:
    - focused point-only fog pass EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_inward_clearance_0001.exr`
    - converted PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_inward_clearance_0001.png`
  - measured comparison versus the accepted point-only `subsample_full` baseline:
    - global mean luminance difference:
      - about `+0.01555`
    - left artifact ROI mean luminance difference:
      - about `+0.00808`
    - center-left ROI mean luminance difference:
      - about `+0.02963`
    - normalized local gradient in the left artifact ROI:
      - baseline:
        - about `0.004829`
      - inward-clearance probe:
        - about `0.004251`
      - reduction:
        - about `12%`
    - normalized local gradient in the tighter left-core ROI:
      - baseline:
        - about `0.005609`
      - inward-clearance probe:
        - about `0.004232`
      - reduction:
        - about `25%`
  - current interpretation:
    - this is strong evidence that the remaining structured artifact is primarily scene-side high
      variance from wall-adjacent soft-falloff point lights whose sampled finite disk overlaps or
      grazes nearby wall geometry
    - the accepted code path is therefore likely already solving the main integrator-side issue
    - any stricter cleanup should prefer either:
      - cleaner validation-scene light clearance for reference renders
      - or a more targeted finite-light sampling strategy near occluders
    - broad fog-step increases or opacity-warp redistribution remain unjustified for the current
      quality/cost tradeoff
- Later on `2026-04-11`, a kernel-side adaptive finite-light follow-up was tested to see whether a
  very local `2 -> 4` point/spot fog subsample refinement could clean up only the disagreeing
  wall-adjacent steps without globally raising cost.
  - implementation trial:
    - keep the current default soft-falloff point/spot fog sample count at `2`
    - after the first two samples, detect strong per-step disagreement from:
      - shadow-blocked versus unblocked sample mixes
      - or a large relative span in per-sample fog contribution
    - only then extend that single fog step to `4` samples
  - validation outputs:
    - focused point-only fog pass EXR:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_adaptive4_0001.exr`
    - converted PNG preview:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_adaptive4_0001.png`
    - render log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09\foggy_street_cyberpunk_test_optix_fog_pass_point_only_adaptive4.log`
  - measured result versus the accepted point-only `subsample_full` baseline:
    - point-only fog pass render:
      - about `04:54.516`
    - left artifact ROI normalized gradient:
      - baseline:
        - about `0.004829`
      - adaptive probe:
        - about `0.004629`
      - reduction:
        - about `4%`
    - tighter left-core ROI normalized gradient:
      - baseline:
        - about `0.005609`
      - adaptive probe:
        - about `0.005358`
      - reduction:
        - about `4.5%`
  - decision:
    - the quality improvement was real but too small for the added point-only render cost
    - this kernel-side adaptive refinement is therefore rejected for now
    - the source file and active OptiX runtime payloads were restored to the accepted uniform-step
      + targeted fixed-`2` point/spot subsample path after the test
- Later on `2026-04-11`, OptiX default-profile beauty performance was measured directly on the
  active cyberpunk validation scene with fog temporarily toggled off and back on without modifying
  the saved blend.
  - temporary helper:
    `E:\blender_modify\blender\.agent\tmp\render_fog_perf_compare.py`
  - validation folder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_2026-04-11`
  - outputs:
    - report:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_2026-04-11\fog_perf_compare_report.txt`
    - blender log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_2026-04-11\fog_perf_compare_blender.log`
    - fog off beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_2026-04-11\foggy_street_cyberpunk_perf_fog_off_0001.png`
    - fog on beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_2026-04-11\foggy_street_cyberpunk_perf_fog_on_0001.png`
  - runtime settings captured by the helper:
    - `scene.cycles.device: GPU`
    - `preferences.compute_device_type: OPTIX`
  - measured wall-clock render times:
    - fog off:
      - about `3.814s`
    - fog on:
      - about `13.424s`
    - added fog cost:
      - about `+9.610s`
      - about `+251.973%`
  - current read:
    - on this saved cyberpunk beauty scene, the current environment-fog path is about `3.52x` the
      fog-off render time
    - this gives a practical performance reference for feature close-out without reopening the
      wall-adjacent striping investigation
  - cleanup truth:
    - no additional experimental fog-kernel sampling path remains active after this measurement
    - accepted source/runtime behavior stays on the restored fixed-`2` point/spot subsample path
- Later on `2026-04-11`, a second performance probe was added specifically to stress the current
  fog path with a much larger light count than the alley benchmark.
  - new validation scene builder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_cyberpunk_city_overview_scene.py`
  - generated blend:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\cyberpunk_city_overview_test.blend`
  - scene design:
    - high-angle cyberpunk city overview
    - `14 x 14` area-light street grid
    - `7 x 7` point-light accent grid
    - `4` spot beams
    - `1` sun
    - total real lights:
      - `250`
  - dedicated OptiX perf helper:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\render_city_overview_fog_perf_compare.py`
  - validation folder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11`
  - outputs:
    - report:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11\city_overview_fog_perf_compare_report.txt`
    - log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11\city_overview_fog_perf_compare_blender.log`
    - rerun log:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11\city_overview_fog_perf_compare_blender_rerun.log`
    - fog off beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11\cyberpunk_city_overview_fog_off_0001.png`
    - fog on beauty:
      `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\perf_city_overview_2026-04-11\cyberpunk_city_overview_fog_on_0001.png`
  - runtime constraint:
    - the helper explicitly requires `preferences.compute_device_type == OPTIX`
  - measured result from the rerun:
    - fog off:
      - about `4.348s`
    - fog on:
      - about `106.003s`
    - added fog cost:
      - about `+101.655s`
      - about `+2338.012%`
  - current interpretation:
    - unlike the smaller alley benchmark, a hundreds-of-lights overview scene drives the current fog
      implementation into a clearly dominant cost regime
    - the current primary scaling risk is therefore not just scene complexity in general, but
      especially light count across a long visible camera segment
- Verified builds on `2026-04-04`:
  - `bf_nodes_shader`
  - `bf_intern_cycles`
  - `blender`
- Validation asset created on `2026-04-04`:
  - script: `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\build_foggy_street_test_scene.py`
  - blend: `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_boxes_test.blend`
  - on `2026-04-06`, the builder was updated to stop forcing `scene.cycles.device='CPU'` so the
    saved validation blend inherits the normal default-profile device choice instead
  - on `2026-04-06`, the scene lighting was expanded with dedicated area-light validation fixtures:
    `WallWash_L`, `WallWash_R`, and `BillboardGlow`
  - intermediate visibility-fix render that proved local-light scatter was finally live:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_visibility_fix0001.png`
  - latest tuned fog-on render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_tuned0001.png`
  - matching tuned fog-off render:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\foggy_street_scatter_tuned_off0001.png`
  - latest fog-on vs fog-off sampled delta:
    - avg luma delta: about `15.40`
    - max luma delta: about `85.51`
- Additional isolated physical-gain validation on `2026-04-05`:
  - output folder:
    `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\light_type_scatter_cli_2026-04-05`
  - point fog on/off:
    - `point_fog_on_cli30001.png`
    - `point_fog_off_cli30001.png`
  - area fog on/off:
    - `area_fog_on_cli30001.png`
    - `area_fog_off_cli30001.png`
  - practical read:
    - `Point` now gives visible localized fog in front of the geometry instead of disappearing into
      ordinary surface lighting.
    - `Area` now gives a strong readable fog mass in the isolated lamp-only test, where the older
      tuned path had looked effectively absent.
  - same-day normalization correction:
    - `point_fog_on_pdf_fix0001.png` proved that removing sampled-light `pdf` normalization blows
      the point-light case out to near white
    - `point_fog_on_pdf_restore0001.png` returns to the readable pre-regression result
    - `foggy_street_physical_full_pdf_restore0001.png` also resolves the full-scene whiteout enough
      to keep the alley readable again
- Remaining close-out tasks are tracked in:
  - `E:\blender_modify\blender\.agent\features\world-environment-fog\REMAINING_TASKS_2026-04-08.md`

## Rule

- Start from the current worktree only after checking it still matches the latest VFX mainline.
- Current branch sync status: `feature/world-environment-fog` fast-forwarded to `vfx-rendering-branch-github` on `2026-04-03`, but it is behind the current `08fcb983633` mainline after the `2026-04-11` upstream merge.
