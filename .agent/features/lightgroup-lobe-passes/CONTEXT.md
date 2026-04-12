# Lightgroup Lobe Passes Context

## Status

- Status: merged to mainline and included in the 2026-03-26 release
- Canonical source: `E:\blender_modify\blender` on `vfx-rendering-branch-github`
- Historical feature worktree: `E:\blender_modify\blender_lobe_passes`

## What Shipped

- Per-lightgroup combined and split lobe light passes for the Phase 1 design.
- Combined-only lightgroups remain combined-only by design.
- The GPU flat-beauty hole regression was traced to split-pass addressing and fixed with a
  split-lightgroup remap path.
- Emissive mesh lightgroups still keep `Combined_<lg>` behavior without incorrectly indexing dense
  split-pass storage.

## Where To Start

- Current validation workflow:
  [workflows/validate-feature4.md](/E:/blender_modify/blender/.agent/workflows/validate-feature4.md)
- Research/background note:
  [MOONRAY_LPE_REPORT.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/MOONRAY_LPE_REPORT.md)
- Latest review docs:
  [CODE_REVIEW.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/CODE_REVIEW.md)
  and
  [CODE_REVIEW_FOLLOWUP.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/CODE_REVIEW_FOLLOWUP.md)
- Archived long design snapshot:
  [archive/backups/FEATURE_4_LIGHTGROUP_LOBE_PASSES_2026-04-01.md](/E:/blender_modify/blender/.agent/archive/backups/FEATURE_4_LIGHTGROUP_LOBE_PASSES_2026-04-01.md)

## Future Direction

- Full arbitrary LPE syntax is still future work.
- Keep Phase 1 stable on CPU and GPU before reopening broader LPE architecture changes.
