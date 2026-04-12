# New Feature Branch

## Naming Pattern

- Branch: `feature/<name>`
- Worktree: `E:\blender_modify\blender_<name>`
- Build dir: `E:\blender_modify\build_<name>`

## Create The Worktree

```powershell
git -C 'E:\blender_modify\blender' worktree add 'E:\blender_modify\blender_<name>' -b 'feature/<name>' vfx-rendering-branch-github
```

## Build Directory

Create or reuse a matching build directory:

```powershell
New-Item -ItemType Directory -Force -Path 'E:\blender_modify\build_<name>' | Out-Null
```

## Rules

- Start feature work from `vfx-rendering-branch-github` unless the user explicitly chooses another base.
- Keep feature-specific docs under `.agent/features/<feature>/`.
- Re-sync stale worktrees before using them for new active development.
