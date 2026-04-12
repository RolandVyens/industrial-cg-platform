# World Environment Fog Close-Out State

> Date: 2026-04-08

## Goal

Close the current world-environment-fog feature to a clear "feature complete" state for the
accepted current V2 direction.

## Current Done State

- World-only `ShaderNodeEnvironmentFog` exists and syncs into Cycles background/kernel data.
- Primary camera-segment fog integration works for:
  - camera to surface hit
  - camera to direct light hit
  - camera to background
- Direct scene lights are supported:
  - point
  - spot
  - area
  - sun / distant
- HDRI / `LIGHT_BACKGROUND` contribution is excluded.
- Current V2 shadowing exists in limited form:
  - opaque light-side shadow query is enabled for surface camera segments
  - background and direct-light-hit camera segments currently keep shadow query disabled
- That limited V2 shadow scope is now accepted as the milestone target for this feature pass.
- Current fog validation scene renders successfully on:
  - `CUDA`
  - `OPTIX`

## Close-Out Result

- The final validation matrix is complete:
  - `CUDA`, 1 spp
  - `CUDA`, 32 spp
  - `OPTIX`, 1 spp
  - `OPTIX`, 32 spp
- A default-profile non-`--factory-startup` render was re-run and the loaded user profile selected
  `OptiX`.
- An explicit HDRI-ignore proof was recorded.
- A camera-hit clipping proof was recorded.
- The accepted deferred-vs-shipped scope is now locked in docs.

## Shipped / Validated

- current world-only fog node and world sync
- camera-segment fog on:
  - surface hits
  - direct-light hits
  - background hits
- supported explicit light types:
  - point
  - spot
  - area
  - sun
- HDRI / `LIGHT_BACKGROUND` excluded
- accepted V2 opaque light-side shadowing on surface camera segments
- stable `CUDA` and `OPTIX` alley-scene validation

## Deferred Beyond This Milestone

- emissive-geometry fog lighting
- fog light-AOV / lightgroup split behavior
- broader shadowing beyond the accepted V2 surface-only slice
- compositor-focused holdout validation beyond the current CLI PNG proof path

## 2026-04-09 Addendum

- The dedicated fog pass / fog AOV is no longer deferred.
- It is now implemented and validated on the cyberpunk OptiX scene with standalone artifacts under:
  `E:\blender_modify\blender\.agent\features\world-environment-fog\validation\fog_pass_2026-04-09`
- Current saved outputs:
  - raw fog EXR:
    `foggy_street_cyberpunk_test_optix_fog_pass_0001.exr`
  - PNG preview derived from that EXR:
    `foggy_street_cyberpunk_test_optix_fog_pass_0001.png`
- Current compositor/export note:
  - this local 5.2 compositor path still writes the file-output fog artifact as EXR reliably
  - the PNG preview is generated as a follow-up conversion from the saved EXR rather than directly
    from the compositor node

## Holdout Validation Note

- A direct holdout PNG proof was attempted in both the fog build and the installed March 26 release
  control.
- The saved PNG combined output did not produce a meaningful alpha/image delta in either case.
- Current close-out therefore treats camera-hit clipping as explicitly validated, while
  holdout-specific compositing validation is documented as deferred rather than silently claimed.

## Definition Of Done Result

- current V2 shadow behavior is explicitly accepted
- `CUDA` and `OPTIX` validation both pass on the final test scene
- deferred items are explicitly documented instead of left ambiguous
- final validation evidence is written into the feature docs
- no remaining crash or kernel-sync ambiguity exists for the active build workflow
