# GitHub Management - vfx-rendering-branch (Agent Guide)

## Goals
1. **Primary repo:** `https://github.com/RolandVyens/blender-vfx` (main source of truth).
2. Publish release zips built from the local workspace.

## Constraints (Important)
- **Blender Projects is not maintained.** Do not push or mirror to `projects.blender.org`.
- **No force git ops without approval.** Always ask before running `--force`.
- **Do not modify files outside** `E:\blender_modify\`.
- **Never delete files** unless the user explicitly approves.

## Primary Repo Workflow (GitHub)
Use normal pushes to GitHub for day-to-day work.

```powershell
# From repo root: E:\blender_modify\blender
git status -sb
git push github <branch>
```

**Notes**
- The GitHub default branch should be `vfx-rendering-branch`.
- If README on GitHub appears cached, update **both** `README.md` and `.github/README.md`.

## Release Build + Zip (Windows)
Release packaging should use install + zip (no PDBs):

```powershell
# Build
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release

# Install to release folder
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --install 'E:\blender_modify\build_windows_x64_vc17_Release' --config Release `
  --prefix 'E:\blender_modify\release\blender-vfx-5.2-2026-02-21'

# Remove PDBs (do not delete anything else)
cmd /c del /f /q "E:\blender_modify\release\blender-vfx-5.2-2026-02-21\blender.pdb" `
  "E:\blender_modify\release\blender-vfx-5.2-2026-02-21\5.2\python\lib\venv\scripts\nt\venvlauncher.pdb" `
  "E:\blender_modify\release\blender-vfx-5.2-2026-02-21\5.2\python\lib\venv\scripts\nt\venvwlauncher.pdb"

# Zip
tar -a -c -f E:\blender_modify\release\blender-vfx-5.2-2026-02-21.zip -C E:\blender_modify\release blender-vfx-5.2-2026-02-21
```

## Release Publish (Manual UI)
The Codex environment may block opening URLs, so use a browser manually:

1. Create a new release on GitHub:
   - Tag name: **use zip name** (e.g., `blender-vfx-5.2-2026-02-21`)
   - Release title: **same as tag**
2. Upload the zip:
   - `E:\blender_modify\release\blender-vfx-5.2-2026-02-21.zip`

Example release URL:
```
https://github.com/RolandVyens/blender-vfx/releases/new?tag=blender-vfx-5.2-2026-02-21&title=blender-vfx-5.2-2026-02-21
```

## Release Publish (CLI via gh)
Preferred when available. Ensure `gh` is installed and authenticated.

```powershell
# If gh is not on PATH, use full path
$gh = "C:\Program Files\GitHub CLI\gh.exe"

# Login (opens browser)
& $gh auth login -p https -w

# Verify auth
& $gh auth status

# Create release + upload zip
& $gh release create blender-vfx-5.2-2026-02-21 `
  "E:\blender_modify\release\blender-vfx-5.2-2026-02-21.zip" `
  --repo RolandVyens/blender-vfx `
  --title "blender-vfx-5.2-2026-02-21" `
  --notes "Release build: multi-arch CUDA (sm_75/86/89)."
```
