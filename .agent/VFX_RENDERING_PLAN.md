# VFX Rendering Branch Plan

## Goal

Keep the long-running Blender VFX branch organized around isolated feature worktrees, stable local
validation workflows, and releasable mainline integrations.

## Current Mainline Scope

| Feature | Status | Primary doc |
| --- | --- | --- |
| Deep EXR | Merged and released | [features/deep-exr/CONTEXT.md](/E:/blender_modify/blender/.agent/features/deep-exr/CONTEXT.md) |
| Shadow Color | Merged and released | [features/shadow-color/CONTEXT.md](/E:/blender_modify/blender/.agent/features/shadow-color/CONTEXT.md) |
| Lightgroup Lobe Passes | Merged and released | [features/lightgroup-lobe-passes/CONTEXT.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/CONTEXT.md) |
| No Direct Lighting | Pending re-sync | [features/no-direct-lighting/CONTEXT.md](/E:/blender_modify/blender/.agent/features/no-direct-lighting/CONTEXT.md) |
| Collection Material Override | Pending re-sync | [features/collection-material-override/CONTEXT.md](/E:/blender_modify/blender/.agent/features/collection-material-override/CONTEXT.md) |
| World Environment Fog | Pending development | [features/world-environment-fog/CONTEXT.md](/E:/blender_modify/blender/.agent/features/world-environment-fog/CONTEXT.md) |

## Operating Model

- Keep feature development in separate worktrees.
- Merge validated features back to both VFX mainline branches.
- Document current truth in root docs and feature detail in feature folders.
- Preserve old investigations in `archive/` instead of leaving them in the root.

## Current Priorities

1. Keep the mainline docs, workflows, and release notes aligned with reality.
2. Maintain shipped Deep EXR validation coverage and future follow-up notes without reopening solved regressions.
3. Re-sync stale feature branches before new implementation starts on them.
4. Use the latest released branch state as the reference point for future VFX additions.

## Deep EXR Direction

- Deep EXR is currently considered shipped on the mainline branch.
- Current correctness focus is hard-surface and volume behavior on the locked validation scene.
- Current volume deep behavior stays as shipped.
- Future memory/performance work can borrow MoonRay-style sparse/compressed ideas later, but that is
  explicitly not part of the shipped correctness baseline.

## Feature Order

1. Maintain merged features and release quality.
2. Re-sync `feature/no-direct-lighting`.
3. Re-sync `feature/collection-material-override`.
4. Continue `feature/world-environment-fog` when the user selects it as the next active feature.
