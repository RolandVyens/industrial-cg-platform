# VFX Rendering Branch - Agent Reference

> **Workspace:** `E:\blender_modify\blender`
> **Build Output:** `E:\blender_modify\build_windows_x64_vc17_Release`
> **Branch:** `task/deep-exr-memory`
> **Last Updated:** 2026-02-22

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
```

---

## Branch Strategy

Current working branch: `task/deep-exr-memory` (Deep EXR memory efficiency).

```
vfx-rendering-branch (has deep EXR)
|-- feature/shadow-color          (Per-light shadow color)
|-- feature/no-direct-lighting    (Indirect-only object toggle)
|-- feature/collection-material-override  (Per-collection/object mat override)
`-- feature/per-lightgroup-lobe-passes    (Per-LG lobe AOVs / LPE foundation)
```

Each feature branch is created from and merged back to `vfx-rendering-branch`.

---

## Parallel Worktrees

Parallel development uses git worktrees + separate build directories:

```
E:\blender_modify\blender               (feature/shadow-color)
E:\blender_modify\blender_no_direct     (feature/no-direct-lighting)
E:\blender_modify\blender_mat_override  (feature/collection-material-override)
E:\blender_modify\blender_lobe_passes   (feature/per-lightgroup-lobe-passes)

E:\blender_modify\build_no_direct
E:\blender_modify\build_mat_override
E:\blender_modify\build_lobe_passes
```

Configure each build dir with the same CMake flags as the main build, but swap `-S`/`-B` paths.

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
