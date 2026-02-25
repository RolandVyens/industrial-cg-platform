# Deep EXR — Pull Request Submission Guide

> Based on [Blender Contributing Code](https://developer.blender.org/docs/handbook/contributing/) and [Pull Requests](https://developer.blender.org/docs/handbook/contributing/pull_requests/) documentation.  
> **Current state:** Single squashed commit `27ec98a9e` on `feature/deep-exr-output`, 46 files, +2864/−20 lines.

---

## Phase 1: One-Time Setup

### 1.1 — Fork the Blender repository

Go to [projects.blender.org/blender/blender](https://projects.blender.org/blender/blender) → click **Fork** → confirm with default settings.

### 1.2 — Add your fork as a git remote

```powershell
# Replace <USERNAME> with your projects.blender.org username
git remote add me git@projects.blender.org:<USERNAME>/blender.git
git submodule sync
```

### 1.3 — Set up SSH key (if not done already)

```powershell
# Generate key (Git Bash or PowerShell)
ssh-keygen

# Copy public key content
cat ~/.ssh/id_rsa.pub
```

Paste the public key into [projects.blender.org → Settings → SSH Keys](https://projects.blender.org/user/settings/keys).

### 1.4 — Verify

```powershell
git fetch me   # Should complete without errors
```

### 1.5 — Email

Disable "Hide Email address" in your projects.blender.org profile settings so commits are properly attributed.

---

## Phase 2: Pre-Submission Cleanup

### 2.1 — Fix remaining cosmetic issues

```powershell
# Remove extra blank lines in shade_volume.h (lines 2418-2419)
# Strip trailing whitespace from added lines in shade_volume.h
```

### 2.2 — Run clang-format

Blender uses clang-format. Run it on all modified files:

```powershell
# From blender root
make format
```

Or manually on changed files:
```powershell
git diff --name-only dc35b31f9..HEAD | ForEach-Object { clang-format -i $_ }
```

### 2.3 — Normalize line endings (optional)

If you want to silence the LF→CRLF warnings:

```powershell
# Normalize to LF (matching Blender upstream)
git ls-files --eol | Select-String "w/crlf"
# Then convert affected files, or let git autocrlf handle it
```

### 2.4 — Build and test

```powershell
# Build
cmake --build build --config Release

# Run Blender tests
cd build
ctest --output-on-failure
```

### 2.5 — Amend the squashed commit (if cleanup was needed)

```powershell
git add -A
git commit --amend --no-edit
```

---

## Phase 3: Prepare the Commit Message

Blender's [commit message guidelines](https://developer.blender.org/docs/handbook/guidelines/commit_messages/) require:

- **Subject line** starts with category prefix (e.g., `Cycles:`)
- **Body** separated from subject by blank line
- Lines ≤ 72 characters
- User-level explanation, not just code-level
- No images in commit messages

### Recommended commit message:

```
Cycles: Add deep EXR output support

Add deep EXR rendering to Cycles, storing per-pixel depth
samples that enable compositing transparent and volumetric
layers with correct depth ordering.

Surfaces write alpha-only deep samples at primary ray
intersections. Volumes write per-segment opacity samples
using physical transmittance. A "Deep Recolor" post-process
distributes beauty pass RGB to samples based on log-domain
alpha scaling.

Deep data flows through two paths:
- Direct: Render Properties > Output > Deep EXR format
- Compositor: File Output node set to Deep EXR format

New scene properties control depth and alpha merge
tolerances. Deep EXR files support NONE, RLE, and ZIPS
compression. Multi-view rendering reports an error when
deep output is requested.

Technical notes:
- Kernel writes use atomic sample counting with per-pixel
  fixed-size buffers (DeepRenderBuffers)
- Multi-device rebalancing preserves deep data via
  snapshot/restore in DeepOutputDriver
- OpenEXR deep scanline output uses per-scanline buffering
  to minimize peak memory
- DNA versioning in versioning_510.cc initializes defaults
```

### Apply this message:

```powershell
git commit --amend
# Paste the message above in the editor, save and close
```

---

## Phase 4: Push and Create the PR

### 4.1 — Rebase onto latest `main`

```powershell
git fetch origin
git rebase origin/main
# Resolve any conflicts, then:
# git rebase --continue
```

### 4.2 — Push to your fork

```powershell
git push me feature/deep-exr-output
```

Git will print a URL like:

```
remote: Create a new pull request for 'feature/deep-exr-output':
remote: https://projects.blender.org/blender/blender/compare/main...<USERNAME>:feature/deep-exr-output
```

### 4.3 — Create the PR on projects.blender.org

1. Click the link from git output, or go to your fork page and click the PR icon next to the branch
2. Set **target branch** to `blender:main`
3. Fill in the PR description (see template below)

---

## Phase 5: PR Description Template

> [!IMPORTANT]
> The PR description doubles as the commit message when squash-merged. Write it for both audiences.

```markdown
Cycles: Add deep EXR output support

## Problem

Cycles currently has no deep EXR output capability. Deep compositing
requires per-pixel variable-depth samples with alpha and Z data to
correctly composite transparent and volumetric layers. This is an
industry-standard feature in production renderers (Arnold, RenderMan,
V-Ray).

## Proposed Solution

Full deep EXR pipeline from kernel to file I/O:

### Kernel (GPU/CPU)
- `deep_write.h`: Atomic deep sample writes for surfaces, transparent
  surfaces, and volumes
- `shade_surface.h`: Primary ray surface deep samples
- `shade_volume.h`: Per-segment volume deep samples via transmittance
  tracking (both unbiased and ray-marched modes)

### Session
- `DeepRenderBuffers`: Fixed-max per-pixel sample storage with device
  memory management
- `DeepOutputDriver`: 14-function driver handling sync, rebalance
  snapshot/restore, deep recolor, and processed cache
- `session.cpp`: Multi-view guard, deep output lifecycle

### Blender Integration
- `DNA_scene_types.h`: R_IMF_IMTYPE_DEEP_EXR enum, merge tolerances
- `rna_scene.cc`: UI properties, codec filtering (NONE/RLE/ZIPS only)
- `openexr_api.cpp`: IMB_exr_save_deep() with per-scanline output
- `node_composite_file_output.cc`: Compositor File Output node support
- `versioning_510.cc`: Defaults initialization

### Deep Recolor
Beauty pass RGB is distributed to alpha-only deep samples using
log-domain alpha scaling with 4 fallback strategies, producing
correctly premultiplied deep data matching the Combined pass.

## Alternatives Considered

1. **Full RGB in kernel**: Would multiply memory by 3x and require
   complex atomic operations for color accumulation. Alpha-only +
   Deep Recolor is the industry standard approach (matches Pixar
   RenderMan's strategy).

2. **Variable-size per-pixel buffers**: Would require GPU-side
   dynamic allocation. Fixed-max with configurable limit is simpler
   and matches Arnold's approach.

## Limitations

- Multi-view rendering is blocked (reported via RE_engine_report)
- Deep data is CPU-only for post-processing (device→host copy
  required)
- Maximum samples per pixel is user-configurable but bounded
- Half-float precision not yet implemented for deep channels

## User Interface

Output Properties → Format → "Deep EXR (.exr)"
- Depth Merge Tolerance (default: 0.01)
- Alpha Merge Tolerance (default: 0.01)
- Compression: None, RLE, ZIPS only

Compositor → File Output Node → Format → "Deep EXR (.exr)"
- Same merge tolerance and compression controls

---

Additional context: 46 files, ~2900 lines added. Tested on CPU and
mixed CPU+GPU configurations. Deep rebalance preservation handles
work redistribution without data loss.
```

---

## Phase 6: After Submission

### 6.1 — Assign reviewers

Find the right reviewers from the [modules list](https://projects.blender.org/blender/blender/wiki):
- **Cycles module**: Brecht Van Lommel, Sergey Sharybin
- **Render module**: For `RE_pipeline.h` / `render_result.cc` changes
- **Compositor module**: For `node_composite_file_output.cc` changes

Assign as **individual reviewers** (not module labels) to avoid noise.

### 6.2 — Add tags

Add relevant module tags: `Module: Cycles`, `Module: Render`, `Module: Compositor`

### 6.3 — Respond to review

- Push fixes as **new commits** (avoid force-push during review so reviewers can see deltas)
- Mark resolved conversations as resolved
- Click **"Re-request Review"** when fixes are ready — don't ping by username

### 6.4 — Buildbot testing

A reviewer will trigger CI with:
```
@blender-bot build
```

---

## Quick Reference

| Step | Command |
|------|---------|
| Add fork remote | `git remote add me git@projects.blender.org:<USER>/blender.git` |
| Rebase on latest | `git fetch origin && git rebase origin/main` |
| Push to fork | `git push me feature/deep-exr-output` |
| Force push (after amend) | `git push -f me feature/deep-exr-output` |
| Run format | `make format` |
| Run tests | `cd build && ctest --output-on-failure` |

---

## References

- [Contributing Code](https://developer.blender.org/docs/handbook/contributing/)
- [Pull Requests](https://developer.blender.org/docs/handbook/contributing/pull_requests/)
- [Commit Message Guidelines](https://developer.blender.org/docs/handbook/guidelines/commit_messages/)
- [Code Style Guidelines](https://developer.blender.org/docs/handbook/guidelines/)
- [Quality Checklist](https://developer.blender.org/docs/handbook/contributing/pull_requests/#quality-checklist)
