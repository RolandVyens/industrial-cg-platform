# Deep EXR Surface Coverage Worktree Archive

- Archived on: `2026-04-02`
- Source worktree: `E:\blender_modify\blender_deep_surface_coverage`
- Source branch: `feature/deep-exr-surface-coverage`
- Purpose: preserve the historical `.agent` investigation set and Deep EXR plan docs before
  removing the merged surface-coverage worktree and its paired build directory

## Contents

- `agent/`: copied from the historical worktree `.agent/` folder, including scratch probes, logs,
  and local branch docs that were never migrated into the mainline `.agent` layout
- `plans/`: copied from the historical worktree `docs/plans/` folder

## Notes

- The source worktree contained many untracked diagnostic files. This archive preserves them before
  the worktree cleanup.
- Detached Deep EXR helper worktrees such as `blender_deep_exr_fix` and
  `blender_deep_surface_coverage_e720_clean` were not removed as part of this archive step.
