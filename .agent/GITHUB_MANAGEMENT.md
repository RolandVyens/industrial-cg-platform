# GitHub Management

## Current Release Truth

- Latest verified tag: `blender-vfx-5.2-2026-03-26`
- Local release folder: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26`
- Local release zip: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26.zip`
- Notes draft: `E:\blender_modify\release\blender-vfx-5.2-2026-03-26-notes.md`
- Primary repo: `https://github.com/RolandVyens/blender-vfx`

## Branch Roles

- `vfx-rendering-branch-github` is the GitHub-facing mainline in the local main worktree.
- `vfx-rendering-branch` is the non-GitHub parity branch kept in `E:\blender_modify\blender_vfx_branch_sync`.

## Release Contents Policy

- Include only the install-packaged Blender folder zipped under `E:\blender_modify\release\`.
- Exclude build-tree binaries, `.pdb` files, temporary scripts, and local test scenes.
- Keep release notes in Markdown.

## Constraints

- Do not force-push without explicit user approval.
- Do not modify files outside `E:\blender_modify\`.
- Do not delete files unless the user explicitly approves it.
- `projects.blender.org` is out of scope for this branch workflow.

## Standard Push Flow

```powershell
git -C 'E:\blender_modify\blender' status -sb
git -C 'E:\blender_modify\blender' push github vfx-rendering-branch-github
```

## Release Build Flow

Use [workflows/release-build.md](/E:/blender_modify/blender/.agent/workflows/release-build.md) for
the exact build, install, zip, and checksum commands.

## GitHub Release Publish

### Browser

1. Create a new GitHub release with the tag name equal to the zip name.
2. Upload the matching zip from `E:\blender_modify\release\`.
3. Paste release notes from the local notes draft or the final Markdown summary.

### `gh` CLI

```powershell
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
$tag = 'blender-vfx-5.2-YYYY-MM-DD'
$zip = "E:\blender_modify\release\$tag.zip"

& $gh auth status
& $gh release create $tag $zip --repo RolandVyens/blender-vfx --title $tag --notes-file "E:\blender_modify\release\$tag-notes.md"
& $gh release view $tag --repo RolandVyens/blender-vfx --json name,tagName,url,assets
```
