# VFX Rendering Branch - Agent Reference

> **Workspace:** `E:\blender_modify\blender`
> **Build Output:** `E:\blender_modify\build_windows_x64_vc17_Release`
> **Branch:** `vfx-rendering-branch-github`
> **Last Updated:** 2026-03-16

---

## Data Safety Rules

1. **NEVER delete files** without explicit user confirmation
2. **NEVER force git operations** without user approval
3. **ALWAYS preserve** existing `.agent/` documentation
4. **DO NOT modify** any file outside `E:\blender_modify\`
5. **DO NOT delete** test files in `C:\tmp\` or `D:\blender_projects\`

---

## How To Build

```powershell
# Incremental build
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release

# Feature/per-lightgroup-lobe-passes worktree build + runtime sync
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_lobe_passes' --target blender --config Release -- /m:28
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_lobe_passes' --target install --config Release -- /m:28
```

> [!IMPORTANT]
> **Close Blender before building!** The linker will fail if `blender.exe` is locked.

**Build Output:** `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`

**Multi-thread build** (faster):
```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release -- /m:28
```

---

## How To Test

```powershell
# Compositor render (deep EXR + flat outputs)
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' -b "D:\blender_projects\deep-branch-test.blend" -f 1

# Safer Deep EXR validation render (CPU, avoids local GPU/add-on state)
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' --factory-startup -b "D:\blender_projects\deep-branch-test.blend" --python-expr "import bpy; bpy.context.scene.cycles.device='CPU'; prefs=bpy.context.preferences.addons['cycles'].preferences; prefs.compute_device_type='NONE'" -f 1

# Deep hard-surface front-alpha regression check
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe' 'E:\blender_modify\blender\.agent\check_deep_surface_front_alpha.py' 'C:\tmp\surface_compoutput_deep0001.exr'

# Deep single-surface AA alpha regression check
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\5.2\python\bin\python.exe' 'E:\blender_modify\blender\.agent\check_deep_single_surface_alpha.py' 'C:\tmp\surface_compoutput_flat0001.exr' 'C:\tmp\surface_compoutput_deep0001.exr'

# Light pass AOV validation scene (environment/lightgroup channels)
& 'E:\blender_modify\build_lobe_passes\bin\Release\blender.exe' -b "D:\blender_projects\light-passes-test-v001.blend" -f 3
```

---

## Branch Strategy

Current working branch: `vfx-rendering-branch-github` (GitHub release/publishing branch).
Local parity branch: `vfx-rendering-branch` (non-GitHub integration branch).

```
vfx-rendering-branch-github / vfx-rendering-branch
|-- merged: feature/shadow-color
|-- pending re-sync: feature/no-direct-lighting
|-- pending re-sync: feature/collection-material-override
|-- merged: feature/per-lightgroup-lobe-passes
`-- next candidate: feature/world-environment-fog
```

Feature branches should be re-synced to the latest VFX base before active development if they predate
the 2026-02-22 history rewrite or the 2026-03-16 Feature 4 merge integration.

---

## Release Artifact Scope

- **Compiled Blender binary (local):** `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe`
- **Test project (local):** `D:\blender_projects\deep-branch-test.blend`
- **GitHub release asset:** only the packaged install zip from `E:\blender_modify\release\<tag>.zip`

Test `.blend` files are kept local and are not bundled in release zips by default.

---

## Parallel Worktrees

Parallel development uses git worktrees + separate build directories:

```
E:\blender_modify\blender               (feature/shadow-color)
E:\blender_modify\blender_no_direct     (feature/no-direct-lighting)
E:\blender_modify\blender_mat_override  (feature/collection-material-override)
E:\blender_modify\blender_lobe_passes   (feature/per-lightgroup-lobe-passes)
E:\blender_modify\blender_env_fog       (feature/world-environment-fog)
E:\blender_modify\blender_deep_exr_fix  (feature/deep-exr-edge-alpha-fix)

E:\blender_modify\build_no_direct
E:\blender_modify\build_mat_override
E:\blender_modify\build_lobe_passes
E:\blender_modify\build_env_fog
E:\blender_modify\build_deep_exr_fix
```
As of 2026-03-16, `feature/per-lightgroup-lobe-passes` is merged back to both VFX branches, and
`feature/world-environment-fog` has been fast-forwarded to the latest
`vfx-rendering-branch-github` tip for next-step development. The other two feature worktrees are
still on pre-rewrite history and should be re-synced before development. The Deep EXR follow-up
cleanup and regression verification worktree lives at
`E:\blender_modify\blender_deep_exr_fix` / `E:\blender_modify\build_deep_exr_fix`.

---

## Key Documents

| File | Purpose |
|------|---------|
| `AGENT.md` | This file - agent reference and build instructions |
| `AGENT_HANDOFF.md` | Project state, completed features, current status |
| `VFX_RENDERING_PLAN.md` | Detailed implementation plan for all VFX features |
| `GITHUB_MANAGEMENT.md` | Snapshot mirroring + release publishing workflow |
| `archive/deep-exr/` | Archived deep EXR development history and utilities |
| `workflows/build-blender.md` | Full build guide |

---

## GPU Kernel Dependencies

- CUDA 12.8.0 installed at default path with junction at `C:\tools\cuda\12.8.0`
- OptiX headers via `E:\blender_modify\optix-dev` (commit `f1f6dd8`)
- CMake flags: `-DWITH_CYCLES_DEVICE_CUDA=ON -DWITH_CYCLES_CUDA_BINARIES=ON -DCYCLES_CUDA_BINARIES_ARCH=sm_89 -DOPTIX_ROOT_DIR=E:/blender_modify/optix-dev -DWITH_CYCLES_DEVICE_OPTIX=ON`
- Kernel `.zst` files auto-copied to add-on runtime folder by `intern/cycles/kernel/CMakeLists.txt`

---

## Blender Code Standards (Quick Reference)

- **Blender source:** `namespace blender`, Allman braces, `snake_case`
- **Cycles kernel:** `CCL_NAMESPACE_BEGIN/END`, K&R braces, `ccl_` prefix, `__FEATURE__` defines
- **DNA:** All new fields need versioning in `versioning_510.cc`
- **RNA:** Tooltips use infinitive form, no trailing period
- **Line limit:** 100 chars
- **Comments:** Full sentences with periods
- **Includes:** System before project, blank line between groups
