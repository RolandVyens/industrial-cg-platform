# Deep EXR Merged Surface Color Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Re-enable Deep EXR hard-surface merging without falling back to flattened antialiased beauty RGB for grouped near/far deep samples.

**Architecture:** The fix removes the hard-surface duplicate-preservation bypass in the pre-save merge path and makes grouped hard-surface export use grouped true surface RGB instead of flat beauty RGB for multi-surface edge pixels. Direct scene-output and compositor RGBA deep are both validated, while compositor alpha-only deep is checked only for structural merge behavior.

**Tech Stack:** Blender/Cycles C++, Deep EXR host export path, compositor deep output path, local Blender/Nuke validation scripts.

---

### Task 1: Lock the current failing expectations in helper checks

**Files:**
- Modify: `.agent/run_nuke_direct_scene_output_test.py`
- Create: `.agent/check_deep_merge_matrix.py`

**Step 1: Write the failing test**

Write a helper that records, for the traced seam pixel `(302, 150)`, the sample count and first-sample RGB/alpha for:
- direct deep
- compositor RGBA deep
- compositor alpha-only deep

Expected initial failure:
- direct deep still has excessive hard-surface sample count
- compositor RGBA deep still depends on currently emitted grouped color behavior

**Step 2: Run test to verify it fails**

Run:

```powershell
& 'E:\blender_modify\build_deep_surface_coverage\bin\Release\5.2\python\bin\python.exe' `
  'E:\blender_modify\blender_deep_surface_coverage\.agent\check_deep_merge_matrix.py'
```

Expected: FAIL with current sample-count / grouped-color assertions.

**Step 3: Write minimal implementation**

Add the matrix helper only. Do not change production code yet.

**Step 4: Run test to verify it passes/fails correctly**

Expected: the helper runs and fails for the current behavior rather than crashing.

**Step 5: Commit**

```powershell
git -C 'E:\blender_modify\blender_deep_surface_coverage' add `
  '.agent/run_nuke_direct_scene_output_test.py' `
  '.agent/check_deep_merge_matrix.py'
git -C 'E:\blender_modify\blender_deep_surface_coverage' commit -m "test: add deep merge matrix helper"
```

### Task 2: Remove hard-surface duplicate preservation from pre-save deep merge

**Files:**
- Modify: `intern/cycles/session/deep_buffers.cpp`
- Modify: `source/blender/imbuf/IMB_deep_sample_merge.hh`
- Test: `.agent/check_deep_merge_matrix.py`

**Step 1: Write the failing test**

Use the matrix helper to assert that direct deep sample count at the traced seam pixel drops when merge tolerance is active.

**Step 2: Run test to verify it fails**

Expected: FAIL because direct deep still keeps 22 samples.

**Step 3: Write minimal implementation**

Remove the `preserve_opaque_surface_duplicates = true` bypass from the direct pre-save merge path so opaque hard-surface duplicates can merge again under tolerance.

**Step 4: Run test to verify it passes**

Re-render the direct scene-output case and rerun the matrix helper.

Expected: direct deep sample count is reduced from the old preserved-duplicate state.

**Step 5: Commit**

```powershell
git -C 'E:\blender_modify\blender_deep_surface_coverage' add `
  'intern/cycles/session/deep_buffers.cpp' `
  'source/blender/imbuf/IMB_deep_sample_merge.hh'
git -C 'E:\blender_modify\blender_deep_surface_coverage' commit -m "fix: re-enable hard-surface deep merging"
```

### Task 3: Preserve grouped true surface RGB during hard-surface export

**Files:**
- Modify: `intern/cycles/session/deep_output_driver.cpp`
- Modify: `intern/cycles/session/deep_output_driver.h`
- Test: `.agent/check_deep_merge_matrix.py`

**Step 1: Write the failing test**

Add assertions that grouped front/far surface samples do not reuse the same flat antialiased beauty RGB on multi-surface seam pixels.

**Step 2: Run test to verify it fails**

Expected: FAIL because grouped multi-surface export can still fall back to beauty RGB in the wrong cases.

**Step 3: Write minimal implementation**

Refactor grouped hard-surface export so multi-surface grouped samples source premultiplied RGB from grouped true sample color, not flattened beauty RGB. Keep volume/suffix behavior unchanged.

**Step 4: Run test to verify it passes**

Expected: traced near/far grouped sample colors stay distinct and structurally correct.

**Step 5: Commit**

```powershell
git -C 'E:\blender_modify\blender_deep_surface_coverage' add `
  'intern/cycles/session/deep_output_driver.cpp' `
  'intern/cycles/session/deep_output_driver.h'
git -C 'E:\blender_modify\blender_deep_surface_coverage' commit -m "fix: preserve grouped surface color in deep export"
```

### Task 4: Re-render the full test matrix

**Files:**
- Use: `D:\blender_projects\light-passes-test-v001.blend`
- Use: `E:\blender_modify\deep_merge_test.nk`
- Use: `.agent/run_nuke_direct_scene_output_test.py`

**Step 1: Write the failing test**

Define the acceptance matrix:
- direct deep: visually acceptable edge, merged sample reduction
- compositor RGBA deep: visually acceptable edge, merged sample reduction
- compositor alpha-only deep: merge/sample-count reduction only

**Step 2: Run test to verify current state**

Render and export all three cases.

**Step 3: Write minimal implementation**

No new production code here. Only verify the previous tasks on all paths.

**Step 4: Run test to verify it passes**

Required outputs:
- preview PNG and mask PNG for direct deep
- preview PNG and mask PNG for compositor RGBA deep
- sample-count / merge report for compositor alpha-only deep

**Step 5: Commit**

```powershell
git -C 'E:\blender_modify\blender_deep_surface_coverage' add '.agent/'
git -C 'E:\blender_modify\blender_deep_surface_coverage' commit -m "test: validate deep merge matrix"
```

### Task 5: Update project docs

**Files:**
- Modify: `.agent/AGENT_HANDOFF.md`
- Modify: `.agent/VFX_RENDERING_PLAN.md`

**Step 1: Write the failing test**

List the doc facts that must change:
- hard-surface duplicate-preservation bypass removed
- grouped true surface RGB preserved
- direct/compositor RGBA/alpha-only matrix results

**Step 2: Run test to verify it fails**

Manual check: docs still describe the older unresolved state.

**Step 3: Write minimal implementation**

Update docs with the final behavior and matrix results.

**Step 4: Run test to verify it passes**

Manual check: docs match the actual implementation and latest validation artifacts.

**Step 5: Commit**

```powershell
git -C 'E:\blender_modify\blender_deep_surface_coverage' add `
  '.agent/AGENT_HANDOFF.md' `
  '.agent/VFX_RENDERING_PLAN.md'
git -C 'E:\blender_modify\blender_deep_surface_coverage' commit -m "docs: update deep merge color fix status"
```
