# Deep EXR Development State

> **Worktree:** `E:\blender_modify\blender_deep_surface_coverage`
> **Branch:** `feature/deep-exr-surface-coverage`
> **Build:** `E:\blender_modify\build_deep_surface_coverage`
> **Blender:** `E:\blender_modify\build_deep_surface_coverage\bin\Release\blender.exe`
> **Last Updated:** 2026-03-23

---

## 1. Overall Status

Deep EXR is currently in an **acceptable done state** for the active hard-surface follow-up scope.

Current conclusion:
- the major hard-surface DeepMerge seam bug is fixed;
- hard-surface compaction is fixed;
- hard-surface edge alpha / opaque coverage is fixed on the validated scene;
- the remaining tiny Nuke mask specks are treated as mostly noise and are not considered blocking.

Volume deep was intentionally left unchanged in this phase.

Important implementation-state note:
- the **currently active visible fix** comes from the low-level deep-buffer merge change that now
  preserves opaque hard-surface duplicates before export;
- the metadata-aware hard-surface path is now partially active on the kernel side for bounce-0
  surfaces: primary/bounce-0 surface hits write hard-surface metadata, bounce-0 surface/shadow
  contributions accumulate RGB into that exact deep sample, and the older export fallback remains
  in place when metadata is absent;
- volume deep remains unchanged.

---

## 2. Current Scope Decision

Locked current Deep EXR direction:
- **fix hard-surface deep behavior;**
- **keep volume deep unchanged;**
- **do not redesign compositor alpha-only Deep EXR policy;**
- **do not do a MoonRay-style sparse/compressed storage rewrite in this phase.**

This phase is a visible-behavior correction pass, not a full architecture rewrite.

---

## 3. Main Validation Inputs

Primary scene:
- `D:\blender_projects\light-passes-test-v001.blend`

Controlled direct scene-output Deep EXR:
- `C:\tmp\scene_output_rgba_deep_probe_####.exr`

Nuke validation:
- script: `E:\blender_modify\deep_merge_test.nk`
- preview: `C:\tmp\nuke_scene_output_rgba_deep_probe.png`
- mask: `C:\tmp\nuke_scene_output_rgba_deep_probe_mask.png`

Primary visual judgment:
- the large white DeepMerge seam between teapot/gray-card region is gone;
- remaining residual mask is small sparse noise.

---

## 4. Current Deep EXR Behavior by Aspect

### 4.1 Direct scene-output Deep EXR

Current state:
- works as the main validation path;
- carries deep `RGBA/Z/ZBack`;
- hard-surface edge behavior is now acceptable on the tested scene;
- direct scene-output deep is the authoritative path for the current hard-surface fix.

### 4.2 Compositor Deep EXR

Current compositor policy remains unchanged:
- if the File Output Deep EXR input is linked from `Image`, it is RGBA deep;
- if linked from `Alpha`, it remains **alpha-only deep** by design.

This policy was explicitly preserved.

### 4.3 Hard-surface compaction

Current state:
- fixed on the validated scene.

The earlier issue where one visible hard surface exported too many thin samples is no longer active
on the current controlled test path.

### 4.4 Hard-surface edge alpha / coverage

Current state:
- fixed.

Foreground hard-surface edges no longer stay incorrectly solid because of lost duplicate coverage on
the tested seam cases.

### 4.5 Front-sample color

Current state:
- acceptable on the validated seam case.

The previously bad front hard-surface sample is back near the actual object-side color instead of
behaving like the flat antialiased edge color.

### 4.6 Volume deep

Current state:
- unchanged by this phase;
- accepted for current branch goals.

Volume deep is not being reworked here. The current branch priority is hard-surface correctness.

---

## 5. Root Causes and Fixes Landed in This Follow-up

### 5.1 Hard-surface over-fragmentation root cause

Root cause:
- export-side hard-surface prefix grouping was too narrow and still split adjacent visible-surface
  hits more than desired.

Current code state:
- export-side grouping helpers for visible-surface identity and normal continuity are active
  together with the newly wired kernel-side hard-surface metadata write / RGB accumulation path;
- the accepted visible coverage fix still depends first on preserved opaque duplicate coverage in
  `DeepRenderBuffers::merge_nearby_samples()`.

Result:
- the validated scene no longer shows the earlier over-fragmented output, but the accepted current
  visible fix should be attributed first to preserved opaque duplicate coverage in
  `DeepRenderBuffers::merge_nearby_samples()`.

### 5.2 Hard-surface opaque seam / coverage root cause

Final root cause:
- `intern/cycles/session/deep_buffers.cpp`
- `DeepRenderBuffers::merge_nearby_samples()`

The generic deep merge was still collapsing opaque hard-surface duplicate hits too early before
export-side coverage reconstruction. That destroyed the real camera-hit coverage distribution, so
later export saw too few representatives and reconstructed front alphas that were far too small on
some seam pixels.

Fix:
- preserve opaque hard-surface duplicates in the low-level merge path by keeping
  `preserve_opaque_surface_duplicates=true`;
- keep volume merging behavior unchanged;
- keep the older export fallback path working correctly when metadata is absent.

Result:
- previously bad opaque seam pixels now flatten back to the correct opaque alpha.

---

## 6. Current Verified Pixel Examples

### Pixel `(655, 403)`

This is the key repaired opaque seam case.

Current deep result:
- sample 0: front teapot, `A=0.5625`
- sample 1: farther hard surface, `A~0.2142857`
- sample 2: far background/ground, `A=1.0`

Flattened deep alpha:
- `1.0`

Flat alpha:
- `1.0`

### Pixel `(302, 150)`

This is a validated mixed foreground/background hard-surface case.

Current deep result:
- sample 0: front surface, `A=0.75`
- sample 1: far background, `A=0.375`

Flattened deep alpha:
- `0.84375`

Flat alpha:
- `0.84375`

---

## 7. Fresh Verification Results

All results below are from the current build/worktree state on 2026-03-23.

### 7.1 Opaque coverage regression

Command:
- `.agent/check_deep_surface_opaque_coverage.py`

Result:
- `pixel=(655,403) flat_alpha=1.000000000 deep_alpha=1.000000000 diff=0.000000000`
- `mismatching_opaque_pixels=0`

### 7.2 Metadata wiring regression

Command:
- `.agent/check_deep_surface_metadata_wiring.py`

Result:
- `PASS: Deep EXR surface metadata wiring is active.`
- `metadata_wiring_failures=0`

### 7.3 Hard-surface compaction regression

Command:
- `.agent/check_deep_surface_compaction.py`

Result:
- `checked_fractional_pixels=4704`
- `all_surface_fractional_pixels=4704`
- `overfragmented_pixels=0`

### 7.4 Single-surface AA regression

Command:
- `.agent/check_deep_single_surface_alpha.py`

Result:
- `checked_single_surface_fractional_pixels=4696`
- `mismatching_single_surface_pixels=0`

### 7.5 Front-surface alpha regression

Command:
- `.agent/check_deep_surface_front_alpha.py`

Result:
- `active_sample_pixels=1807695`
- `multi_active_sample_pixels=0`
- `multi_surface_pixels=0`
- `violating_front_surface_alpha_pixels=0`

### 7.6 Surface sample color regression

Command:
- `.agent/check_deep_surface_sample_color.py`

Result:
- passes on the previously bad seam case `(655, 403)`;
- current measured ratio is `interior_to_edge_ratio=0.091213669`.

### 7.7 Nuke visual validation

Command path:
- `.agent/run_nuke_direct_scene_output_test.py`

Outputs:
- `C:\tmp\nuke_scene_output_rgba_deep_probe.png`
- `C:\tmp\nuke_scene_output_rgba_deep_probe_mask.png`

Visual result:
- the large white seam is gone;
- remaining mask is small sparse residual noise.

---

## 8. Current Technical Design Summary

### Low-level deep merge

Shared helper:
- `source/blender/imbuf/IMB_deep_sample_merge.hh`

Current Cycles deep-buffer use:
- preserve opaque hard-surface duplicates;
- still merge compatible volume samples normally.

### Export-side hard-surface compaction

Main file:
- `intern/cycles/session/deep_output_driver.cpp`

Current responsibility:
- keep correct resolved edge coverage behavior on the accepted current path;
- leave volume suffix handling on the current existing path.

Important note:
- bounce-0 hard-surface metadata writes and RGB accumulation are now active;
- preserved opaque duplicate coverage is still required so export-side reconstruction sees the real
  camera-hit coverage distribution;
- the older export fallback remains important for no-metadata cases;
- volume deep stays on the existing path unchanged.

---

## 9. Memory / Architecture State

Current Cycles Deep EXR storage remains:
- fixed-capacity per-tile deep buffers;
- tile-budget / clamp based;
- predictable and controllable;
- less memory-efficient than MoonRay-style sparse/compressed deep storage.

Future direction noted but not in scope here:
- consider MoonRay-style sparse/compressed storage ideas later.

Current phase did **not** attempt a storage redesign.

---

## 10. GPU / Backend State

Deep EXR feature code is intended to support:
- CPU
- CUDA
- OptiX

Important note:
- the previously investigated CUDA illegal-address issue was traced to stale runtime cubin copying
  and handled separately;
- the current hard-surface coverage fix is mainly host/export-side deep processing logic, not a new
  backend-specific feature path.

---

## 11. Remaining Non-Blocking Items

Accepted residuals:
- tiny sparse residual mask specks in Nuke preview/mask;
- no volume deep redesign in this phase;
- no sparse/compressed storage optimization in this phase.

These are currently not considered blockers for marking the hard-surface Deep EXR follow-up done.

---

## 12. Key Files to Remember

Main implementation files:
- `intern/cycles/kernel/film/deep_write.h`
- `intern/cycles/session/deep_buffers.h`
- `intern/cycles/session/deep_buffers.cpp`
- `intern/cycles/session/deep_output_driver.h`
- `intern/cycles/session/deep_output_driver.cpp`
- `intern/cycles/blender/session.cpp`
- `source/blender/nodes/composite/nodes/node_composite_file_output.cc`
- `source/blender/imbuf/IMB_deep_sample_merge.hh`

Main helper / validation files:
- `.agent/render_scene_output_rgba_deep_probe.py`
- `.agent/run_nuke_direct_scene_output_test.py`
- `.agent/check_deep_surface_opaque_coverage.py`
- `.agent/check_deep_surface_compaction.py`
- `.agent/check_deep_single_surface_alpha.py`
- `.agent/check_deep_surface_front_alpha.py`
- `.agent/check_deep_surface_sample_color.py`

---

## 13. Bottom Line

Current Deep EXR state is:
- **hard-surface compaction:** good
- **hard-surface edge alpha / coverage:** good
- **opaque seam bug:** fixed
- **front-surface color on validated seam case:** good
- **volume deep:** unchanged and accepted
- **direct scene-output deep:** good
- **compositor alpha-only policy:** intentionally unchanged
- **remaining residual:** tiny noise-level mask specks only

Therefore this Deep EXR follow-up is currently considered **done / acceptable** for the branch.
