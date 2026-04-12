# VFX Rendering Branch Agent Guide

> Workspace: `E:\blender_modify\blender`
>
> Canonical branch: `vfx-rendering-branch-github`
>
> Non-GitHub parity branch: `vfx-rendering-branch`
>
> Primary build: `E:\blender_modify\build_windows_x64_vc17_Release`
>
> Latest packaged release: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26`

## Data Safety Rules

1. Never delete files without explicit user confirmation.
2. Never use force git operations without explicit user approval.
3. Preserve `.agent/` history by moving or archiving, not by dropping files.
4. Do not modify files outside `E:\blender_modify\`.
5. Do not delete test assets in `C:\tmp\` or `D:\blender_projects\`.

## Startup Order

1. Read [STATUS.md](/E:/blender_modify/blender/.agent/STATUS.md) for current branch truth.
2. Use [AGENT_HANDOFF.md](/E:/blender_modify/blender/.agent/AGENT_HANDOFF.md) only as a compatibility redirect.
3. Open the relevant workflow doc under [workflows](/E:/blender_modify/blender/.agent/workflows).
4. Open the active feature context under [features](/E:/blender_modify/blender/.agent/features).

## Root Document Map

| File | Purpose |
| --- | --- |
| [STATUS.md](/E:/blender_modify/blender/.agent/STATUS.md) | Current branch, build, release, and worktree truth. |
| [CONTRIBUTING.md](/E:/blender_modify/blender/.agent/CONTRIBUTING.md) | Rules for maintaining `.agent` docs and scripts. |
| [VFX_RENDERING_PLAN.md](/E:/blender_modify/blender/.agent/VFX_RENDERING_PLAN.md) | Long-run roadmap across VFX features. |
| [GITHUB_MANAGEMENT.md](/E:/blender_modify/blender/.agent/GITHUB_MANAGEMENT.md) | GitHub push, release, and notes workflow. |
| [workflows/build-blender.md](/E:/blender_modify/blender/.agent/workflows/build-blender.md) | Current Windows build commands. |
| [workflows/validate-deep-exr.md](/E:/blender_modify/blender/.agent/workflows/validate-deep-exr.md) | Deep EXR validation workflow. |
| [workflows/validate-feature4.md](/E:/blender_modify/blender/.agent/workflows/validate-feature4.md) | Lightgroup lobe pass validation workflow. |
| [workflows/release-build.md](/E:/blender_modify/blender/.agent/workflows/release-build.md) | Release build, zip, and notes workflow. |
| [workflows/new-feature-branch.md](/E:/blender_modify/blender/.agent/workflows/new-feature-branch.md) | Worktree-based feature branch setup. |

## `.agent` Layout

- Root `.agent/` is for active operational entry docs only.
- Feature-specific docs live under `.agent/features/<feature>/`.
- Scratch scripts, ad hoc probes, and temporary logs live under `.agent/tmp/`.
- Snapshots, backups, and superseded long-form logs live under `.agent/archive/`.
- Validation scripts that belong to a feature live in that feature's `scripts/` subfolder.

## Current Feature Entry Points

| Feature | Status | Entry doc |
| --- | --- | --- |
| Deep EXR | Merged and released | [features/deep-exr/CONTEXT.md](/E:/blender_modify/blender/.agent/features/deep-exr/CONTEXT.md) |
| Shadow Color | Merged and released | [features/shadow-color/CONTEXT.md](/E:/blender_modify/blender/.agent/features/shadow-color/CONTEXT.md) |
| Lightgroup Lobe Passes | Merged and released | [features/lightgroup-lobe-passes/CONTEXT.md](/E:/blender_modify/blender/.agent/features/lightgroup-lobe-passes/CONTEXT.md) |
| No Direct Lighting | Pending re-sync | [features/no-direct-lighting/CONTEXT.md](/E:/blender_modify/blender/.agent/features/no-direct-lighting/CONTEXT.md) |
| Collection Material Override | Pending re-sync | [features/collection-material-override/CONTEXT.md](/E:/blender_modify/blender/.agent/features/collection-material-override/CONTEXT.md) |
| World Environment Fog | Pending development | [features/world-environment-fog/CONTEXT.md](/E:/blender_modify/blender/.agent/features/world-environment-fog/CONTEXT.md) |

## Common Validation Assets

- Deep EXR edge and volume validation scene: `D:\blender_projects\light-passes-test-v001.blend`
- Older Deep EXR scene: `D:\blender_projects\deep-branch-test.blend`
- Nuke visual test: `E:\blender_modify\deep_merge_test.nk`
- Preview output staging: `C:\tmp\`

## Notes

- Keep `.agent` docs ASCII unless the source material already requires non-ASCII.
- Prefer PowerShell commands in docs because the local environment is Windows-first.
- When branch truth changes, update [STATUS.md](/E:/blender_modify/blender/.agent/STATUS.md) first.
