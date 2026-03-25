# Deep EXR Test Matrix

> **Worktree:** `E:\blender_modify\blender_deep_surface_coverage`
>
> **Scene:** `D:\blender_projects\light-passes-test-v001.blend`
>
> **Last Updated:** 2026-03-25

---

## Locked Validation Scope

Every Deep EXR code change must pass this matrix before it is kept.

Locked scope:
- test only `D:\blender_projects\light-passes-test-v001.blend`
- test only **CPU** and **OptiX**
- exclude known machine-sticking runs
- use **Nuke visual judgment first**, then supporting scripts

---

## 1. Build Gate

Build the current worktree:
- source: `E:\blender_modify\blender_deep_surface_coverage`
- build: `E:\blender_modify\build_deep_surface_coverage`

Pass:
- build succeeds

---

## 2. Required Render Variants Per Device

Run the same scene on both:
- **CPU**
- **OptiX**

For each device, validate all of:
- flat RGBA output
- direct scene-output RGBA Deep EXR
- compositor RGBA Deep EXR
- compositor alpha-only Deep EXR

Pass:
- render completes
- no white seam regression
- no volume-region hole regression
- no backend/device failure

Known bad runs that previously stuck the machine remain out of scope and must not be used as the
mandatory gate.

---

## 3. Nuke-First Visual Gate

Use:
- script: `E:\blender_modify\deep_merge_test.nk`

Primary judgment:
- inspect the DeepMerge RGB result first
- inspect the teapot / gray-card edge
- inspect volume regions

Hard fail:
- visible white seam comes back
- visible volume hole / eaten region comes back
- result is clearly worse than the accepted baseline look

Artifacts:
- export preview PNGs to `C:\tmp\`
- keep one CPU preview and one OptiX preview for each accepted round

---

## 4. Required Script Checks

Run these on the tested outputs.

### Required pass/fail scripts
- `.agent/check_deep_flatten_matches_flat.py`
- `.agent/check_deep_single_surface_alpha.py`
- `.agent/check_deep_mixed_surface_volume_case1.py`

### Diagnostic-only script
- `.agent/check_deep_surface_front_alpha.py`

The front-alpha script is currently diagnostic only. Record its output, but do not fail a change
from that script alone unless the visual/Nuke result also regresses.

---

## 5. Manual DeepSample Spot Check

In Nuke, inspect at least:
- one corrected edge pixel
- one volume-region pixel
- one opaque interior pixel

Pass:
- edge front sample is not wrongly forced to solid `1`
- volume samples still exist in volume areas
- no obvious sample collapse that recreates the visible regressions

If a pixel has more than 5 deep samples, record it for debugging, but sample count alone is not a
failure unless it causes visible merge/output problems.

---

## 6. Acceptance Rule

A Deep EXR code change is only kept if all of the following are true:
- build passes
- CPU passes
- OptiX passes
- Nuke visual gate passes
- required scripts pass

If any of the above fail, the change is not accepted.
