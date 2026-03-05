# GitHub Management - vfx-rendering-branch (Agent Guide)

## Latest Verified Release
- **Tag:** `blender-vfx-5.2-2026-03-04`
- **URL:** `https://github.com/RolandVyens/blender-vfx/releases/tag/blender-vfx-5.2-2026-03-04`
- **Asset:** `blender-vfx-5.2-2026-03-04.zip`
- **SHA256:** `9CD99213DD1E1FA459A4981E6F20BFAE5BC569C3252AD416A95017270E5920F3`

## Goals
1. **Primary repo:** `https://github.com/RolandVyens/blender-vfx` (main source of truth).
2. Publish release zips built from the local workspace.

## Release Contents Policy
- **Include:** install-packaged Blender folder zipped as `E:\blender_modify\release\<tag>.zip`.
- **Exclude:** build tree binaries, `.pdb`, local test scenes (for example `D:\blender_projects\deep-branch-test.blend`), and temporary debug scripts.
- **Reason:** keep release artifacts reproducible and small; test projects stay local/internal unless explicitly requested.

## Release Notes Format (Required)
- Always write release notes in Markdown.
- Keep notes short and explicit.
- Default template:

```markdown
## Release Build

Release build: multi-arch CUDA (`sm_75` / `sm_86` / `sm_89`).
```

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
# Release tag (keep zip/folder/release tag exactly the same)
$tag = "blender-vfx-5.2-YYYY-MM-DD"

# Build
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release

# Install to release folder
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --install 'E:\blender_modify\build_windows_x64_vc17_Release' --config Release `
  --prefix "E:\blender_modify\release\$tag"

# Remove PDBs (do not delete anything else)
cmd /c del /f /q "E:\blender_modify\release\$tag\blender.pdb" `
  "E:\blender_modify\release\$tag\5.2\python\lib\venv\scripts\nt\venvlauncher.pdb" `
  "E:\blender_modify\release\$tag\5.2\python\lib\venv\scripts\nt\venvwlauncher.pdb"

# Zip
tar -a -c -f "E:\blender_modify\release\$tag.zip" -C E:\blender_modify\release $tag

# Optional: print checksum for release notes
Get-FileHash -Algorithm SHA256 "E:\blender_modify\release\$tag.zip"
```

## Release Publish (Manual UI)
The Codex environment may block opening URLs, so use a browser manually:

1. Create a new release on GitHub:
   - Tag name: **use zip name** (e.g., `blender-vfx-5.2-2026-03-04`)
   - Release title: **same as tag**
2. Upload the zip:
   - `E:\blender_modify\release\<tag>.zip`

Example release URL:
```
https://github.com/RolandVyens/blender-vfx/releases/new?tag=<tag>&title=<tag>
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

# Markdown release notes
$notes = @"
## Release Build

Release build: multi-arch CUDA (`sm_75` / `sm_86` / `sm_89`).
"@

# Create release + upload zip
& $gh release create $tag `
  "E:\blender_modify\release\$tag.zip" `
  --repo RolandVyens/blender-vfx `
  --title $tag `
  --notes $notes

# Update existing release notes in Markdown
& $gh release edit $tag --repo RolandVyens/blender-vfx --notes $notes

# Post-release verification (required)
& $gh release view $tag --repo RolandVyens/blender-vfx --json name,tagName,url,assets,body
```
