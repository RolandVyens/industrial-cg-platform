# Environment Fog Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a world-only, camera-ray-only, direct-light-only environment fog feature for Cycles that behaves like a fake `aiFog`-style atmosphere, excludes HDRI, supports sun and local lights, and remains holdout-aware without full volumetric shadowing.

**Architecture:** Add a world-level fog definition, sync its parameters into Cycles background/kernel data, and evaluate fog over the camera ray segment from the camera to the first hit or background. Reuse existing light-object sampling infrastructure where possible, keep emissive geometry constrained to existing Cycles emission-sampling eligibility, and route the result as an additive atmosphere contribution rather than true volume transport.

**Tech Stack:** Blender shader node system, Cycles scene sync, Cycles kernel integrator, Cycles film/pass plumbing, Windows CMake build, Cycles test scenes.

---

### Task 1: Re-sync The Fog Worktree Before Any Feature Code

**Files:**
- Modify: `E:\blender_modify\blender\.agent\STATUS.md`
- Modify: `E:\blender_modify\blender\.agent\features\world-environment-fog\CONTEXT.md`
- Reference: `E:\blender_modify\blender\.agent\features\world-environment-fog\DESIGN_2026-04-03.md`

**Step 1: Verify branch divergence**

Run:
```powershell
git -C E:\blender_modify\blender rev-list --left-right --count feature/world-environment-fog...vfx-rendering-branch-github
```

Expected: feature branch is behind mainline and needs re-sync.

**Step 2: Recreate or fast-forward the feature worktree from current mainline**

Run one safe branch-sync flow against `E:\blender_modify\blender_env_fog` without force operations.

Expected: `feature/world-environment-fog` is based on current `vfx-rendering-branch-github`.

**Step 3: Update fog docs to reflect the new branch truth**

Record:
- new HEAD
- worktree path
- sync date

**Step 4: Commit the sync-only doc updates**

Run:
```powershell
git -C E:\blender_modify\blender add .agent\STATUS.md .agent\features\world-environment-fog\CONTEXT.md
git -C E:\blender_modify\blender commit -m "Docs: refresh environment fog branch status"
```

### Task 2: Define The World-Only Fog Node API

**Files:**
- Create: `E:\blender_modify\blender\source\blender\nodes\shader\nodes\node_shader_environment_fog.cc`
- Modify: `E:\blender_modify\blender\source\blender\nodes\shader\nodes\CMakeLists.txt`
- Modify: `E:\blender_modify\blender\source\blender\blenkernel\intern\node.cc`
- Modify: `E:\blender_modify\blender\source\blender\makesdna\DNA_node_types.h`
- Modify: `E:\blender_modify\blender\source\blender\makesrna\intern\rna_nodetree.cc`

**Step 1: Write a failing smoke test target or minimal compile expectation**

Use the node registration pattern from `[node_shader_background.cc](/E:/blender_modify/blender/source/blender/nodes/shader/nodes/node_shader_background.cc)` as the baseline and define the intended API in comments or a local checklist before coding:
- World-only node
- Inputs: `Color`, `Density`, `Start Distance`, `Max Distance`, `Anisotropy`, `Samples`
- Output: `Fog`

Expected: build will fail until the node is fully registered.

**Step 2: Add the node implementation and registration**

Implement:
- declaration
- UI description
- world-only poll
- node type registration

**Step 3: Add DNA/RNA enum and registration plumbing**

Register the new node type so Blender can serialize and display it.

**Step 4: Build Blender to verify registration compiles**

Run:
```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release -- /m:28
```

Expected: successful build with the new node type.

**Step 5: Commit the node-API slice**

Run:
```powershell
git -C E:\blender_modify\blender add source\blender\nodes\shader\nodes\node_shader_environment_fog.cc source\blender\nodes\shader\nodes\CMakeLists.txt source\blender\blenkernel\intern\node.cc source\blender\makesdna\DNA_node_types.h source\blender\makesrna\intern\rna_nodetree.cc
git -C E:\blender_modify\blender commit -m "Cycles: add world-only environment fog node"
```

### Task 3: Add Scene And Kernel Data Plumbing For Fog Settings

**Files:**
- Modify: `E:\blender_modify\blender\intern\cycles\scene\background.h`
- Modify: `E:\blender_modify\blender\intern\cycles\scene\background.cpp`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\data_template.h`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\types.h`

**Step 1: Define the kernel-facing fog parameter struct additions**

Add fields needed for v1:
- enable flag
- fog color
- density
- start distance
- max distance
- anisotropy
- sample count

Do not add height fog fields yet unless they are zero-cost placeholders.

**Step 2: Detect the fog node from the world graph**

In `background.cpp`, detect whether the active world graph contains the environment fog node and extract its values into the background state.

**Step 3: Copy the fog state into kernel background data**

Keep this separate from true world-volume flags.

**Step 4: Rebuild and confirm no regressions in background sync**

Run the Blender build command again.

Expected: clean build.

**Step 5: Commit the sync/kernel-data slice**

Run:
```powershell
git -C E:\blender_modify\blender add intern\cycles\scene\background.h intern\cycles\scene\background.cpp intern\cycles\kernel\data_template.h intern\cycles\kernel\types.h
git -C E:\blender_modify\blender commit -m "Cycles: sync environment fog world parameters"
```

### Task 4: Implement Camera-Segment Fog Integration

**Files:**
- Create: `E:\blender_modify\blender\intern\cycles\kernel\integrator\environment_fog.h`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\shade_background.h`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\shade_surface.h`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\CMakeLists.txt`

**Step 1: Write the integration helper interface**

Define a helper that accepts:
- ray origin
- ray direction
- segment start
- segment end
- current state

and returns a fog contribution spectrum for camera rays only.

**Step 2: Implement the fixed-step integration**

V1 rules:
- clamp segment by fog start and max distance
- zero contribution for empty segments
- use low fixed sample count
- apply transmittance along the camera path
- apply HG phase function
- do not cast volumetric shadow rays

**Step 3: Hook the helper into background rays**

Call the helper from `shade_background.h` for camera rays that reach the background.

**Step 4: Hook the helper into surface-hit camera rays**

Call the helper from `shade_surface.h` for the camera segment between camera and first surface hit.

**Step 5: Keep the feature out of non-camera paths**

Guard on `PATH_RAY_CAMERA` and ensure no reflection/refraction/indirect participation.

**Step 6: Build to verify kernel compilation**

Run the Blender build command again.

Expected: CPU and GPU kernels compile successfully.

**Step 7: Commit the fog-integration slice**

Run:
```powershell
git -C E:\blender_modify\blender add intern\cycles\kernel\integrator\environment_fog.h intern\cycles\kernel\integrator\shade_background.h intern\cycles\kernel\integrator\shade_surface.h intern\cycles\kernel\integrator\CMakeLists.txt
git -C E:\blender_modify\blender commit -m "Cycles: integrate camera-ray environment fog"
```

### Task 5: Reuse Eligible Scene Lights Without HDRI

**Files:**
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\environment_fog.h`
- Reference: `E:\blender_modify\blender\intern\cycles\kernel\light\sample.h`
- Reference: `E:\blender_modify\blender\intern\cycles\scene\light.cpp`

**Step 1: Implement light-object contribution first**

Support:
- point
- spot
- area
- distant / sun

Explicitly skip:
- `LIGHT_BACKGROUND`

**Step 2: Validate that HDRI/background is excluded**

Use a world with bright HDRI and no scene lights.

Expected: fog contribution remains zero.

**Step 3: Add emissive-geometry contribution only through existing emitter eligibility**

Do not invent a second emitter list.

Implementation must follow one of these safe paths:
- reuse existing light sampling helper behavior that can pick emissive emitters
- or defer emissive geometry from v1 if the code path proves too invasive

**Step 4: Measure the cost and decide whether emissive geometry stays in v1**

If the emitter reuse is too invasive or too slow, leave a documented v1 limitation instead of
landing a brittle implementation.

**Step 5: Commit the emitter-selection slice**

Run:
```powershell
git -C E:\blender_modify\blender add intern\cycles\kernel\integrator\environment_fog.h
git -C E:\blender_modify\blender commit -m "Cycles: add direct-light environment fog emitters"
```

### Task 6: Implement Holdout-Aware Camera Blocking

**Files:**
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\shade_surface.h`
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\integrator\environment_fog.h`
- Reference: `E:\blender_modify\blender\intern\cycles\kernel\integrator\shade_surface.h`
- Reference: `E:\blender_modify\blender\intern\cycles\kernel\film\light_passes.h`

**Step 1: Define the first-hit segment behavior**

Fog should integrate only from the camera to the first visible hit distance.

**Step 2: Ensure holdout hits terminate further fog behind the hit**

Use the existing holdout path as the correctness reference.

Expected: holdout objects clip fog behind them on the camera path.

**Step 3: Confirm this does not require light-side volumetric shadowing**

This task is only about camera-side clipping.

**Step 4: Commit the holdout-aware blocking slice**

Run:
```powershell
git -C E:\blender_modify\blender add intern\cycles\kernel\integrator\shade_surface.h intern\cycles\kernel\integrator\environment_fog.h
git -C E:\blender_modify\blender commit -m "Cycles: make environment fog holdout-aware"
```

### Task 7: Route Fog Output Into Initial Passes

**Files:**
- Modify: `E:\blender_modify\blender\intern\cycles\kernel\film\light_passes.h`
- Modify: `E:\blender_modify\blender\intern\cycles\scene\film.cpp`
- Modify: `E:\blender_modify\blender\intern\cycles\scene\pass.cpp`

**Step 1: Land the smallest safe v1 pass behavior**

Preferred order:
1. combined contribution only
2. combined plus dedicated fog pass
3. full emission / light-AOV reuse only if obviously correct

**Step 2: Avoid breaking existing emission and volume pass semantics**

Do not force the fog through ordinary volume transport pass rules.

**Step 3: If a dedicated fog pass is added, wire pass registration and film offsets**

Keep it isolated from lightgroup split logic in v1 unless implementation is trivial.

**Step 4: Build again**

Run the Blender build command again.

Expected: successful build and pass registration.

**Step 5: Commit the pass-routing slice**

Run:
```powershell
git -C E:\blender_modify\blender add intern\cycles\kernel\film\light_passes.h intern\cycles\scene\film.cpp intern\cycles\scene\pass.cpp
git -C E:\blender_modify\blender commit -m "Cycles: add initial environment fog pass routing"
```

### Task 8: Validate Functionality On Minimal Test Scenes

**Files:**
- Modify: `E:\blender_modify\blender\.agent\features\world-environment-fog\CONTEXT.md`
- Modify: `E:\blender_modify\blender\.agent\STATUS.md`
- Create: `E:\blender_modify\blender\.agent\features\world-environment-fog\VALIDATION_2026-04-03.md`
- Optional: `E:\blender_modify\blender\.agent\features\world-environment-fog\scripts\`

**Step 1: Build the final binary**

Run:
```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release -- /m:28
```

Expected: successful build.

**Step 2: Create or identify a minimal fog validation scene**

Need scenarios for:
- point / spot / area / sun light response
- HDRI ignored
- camera-to-surface fog accumulation
- camera-to-background fog accumulation
- holdout clipping
- emissive geometry behavior if included

**Step 3: Render validation frames**

Use the main runtime:
```powershell
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' -b <scene> -f 1
```

**Step 4: Record results in feature docs**

Document:
- what passed
- what is intentionally deferred
- emissive geometry cost read
- whether light AOV split remains deferred

**Step 5: Commit validation docs**

Run:
```powershell
git -C E:\blender_modify\blender add .agent\features\world-environment-fog\CONTEXT.md .agent\STATUS.md .agent\features\world-environment-fog\VALIDATION_2026-04-03.md
git -C E:\blender_modify\blender commit -m "Docs: record environment fog validation status"
```
