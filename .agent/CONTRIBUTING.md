# `.agent` Maintenance Rules

## Purpose

This folder is the long-run operating manual for the local VFX Blender branch. Keep it small at the
root and detailed only where the detail belongs.

## Placement Rules

- Put root operational docs in `.agent/` only when they apply to the whole branch.
- Put feature-specific docs under `.agent/features/<feature>/`.
- Put validation scripts beside the feature they validate in `scripts/`.
- Put temporary probes, scratch scripts, and transient logs in `.agent/tmp/`.
- Put preserved snapshots, backups, and superseded long logs in `.agent/archive/`.

## Update Rules

- Update [STATUS.md](/E:/blender_modify/blender/.agent/STATUS.md) when branch truth, release truth,
  build paths, or active worktree truth changes.
- Update the relevant feature `CONTEXT.md` when a feature ships, regresses, or changes scope.
- Update workflow docs when command lines, file paths, or mandatory validation steps change.
- If a root doc grows into a historical journal, archive the old version and shrink the root doc.

## Style Rules

- Prefer ASCII and plain Markdown.
- Prefer PowerShell examples unless a command must be run elsewhere.
- Use absolute local paths when a workflow depends on a specific local checkout.
- Keep workflow docs executable. If a command cannot be run as written, the doc is stale.

## Preservation Rules

- Do not delete `.agent` history unless the user explicitly asks.
- Prefer move + archive over overwrite when replacing large docs.
- Keep compatibility redirects like `AGENT_HANDOFF.md` stable.
