# Deep EXR Context

## Status

- Status: merged to mainline and included in the 2026-03-26 release
- Canonical source: `E:\blender_modify\blender` on `vfx-rendering-branch-github`
- Locked validation scene: `D:\blender_projects\light-passes-test-v001.blend`
- Locked visual test: `E:\blender_modify\deep_merge_test.nk`
- Archived historical surface-coverage worktree snapshot:
  `E:\blender_modify\blender\.agent\archive\deep-exr\surface-coverage-worktree-2026-04-02`
- Archived detached helper worktrees:
  `E:\blender_modify\blender\.agent\archive\deep-exr\helper-worktrees-2026-04-02`

## Current Accepted Behavior

- Direct scene-output RGBA Deep EXR is working on the shipped path.
- Compositor RGBA Deep EXR is working on the shipped path.
- Compositor alpha-only Deep EXR is working on the shipped path.
- Hard-surface seam regression is resolved to the currently accepted quality level.
- Current shipped volume deep behavior is accepted and should remain unchanged unless a future task
  explicitly reopens it.

## Where To Start

- Current gate and required checks:
  [TEST_MATRIX.md](/E:/blender_modify/blender/.agent/features/deep-exr/TEST_MATRIX.md)
- Detailed running log:
  [STATE.md](/E:/blender_modify/blender/.agent/features/deep-exr/STATE.md)
- Memory direction and MoonRay comparison:
  [MEMORY_REVIEW.md](/E:/blender_modify/blender/.agent/features/deep-exr/MEMORY_REVIEW.md)
- Latest local review:
  [CODE_REVIEW.md](/E:/blender_modify/blender/.agent/features/deep-exr/CODE_REVIEW.md)
- Archived historical worktree snapshot:
  [archive/deep-exr/surface-coverage-worktree-2026-04-02/README.md](/E:/blender_modify/blender/.agent/archive/deep-exr/surface-coverage-worktree-2026-04-02/README.md)
- Archived detached helper worktrees:
  [archive/deep-exr/helper-worktrees-2026-04-02/README.md](/E:/blender_modify/blender/.agent/archive/deep-exr/helper-worktrees-2026-04-02/README.md)
- Validation and helper scripts:
  [scripts](/E:/blender_modify/blender/.agent/features/deep-exr/scripts)

## Future Work

- Metadata reconstruction is future work, not part of the shipped baseline.
- Sparse/compressed storage ideas inspired by MoonRay remain a later optimization direction.
- If a future change reopens seam or volume regressions, re-run the full locked matrix before
  keeping the change.
