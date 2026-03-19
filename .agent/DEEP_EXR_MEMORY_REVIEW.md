# Deep EXR Memory Branch - Code Review Report

> **Branch:** `task/deep-exr-memory`
> **Reviewed:** 2026-02-22
> **Reviewer:** Antigravity Code Review Agent
> **Status:** Uncommitted working-tree changes  -  12 files, 366 insertions / 142 deletions

---

## Summary

The `task/deep-exr-memory` branch adds three optimizations on top of the merged deep EXR output
feature:

1. **User-controlled tile budget** (`deep_tile_budget_mb`, default 1024 MB per device).  When
   greater than zero, `Session::get_effective_tile_size()` clamps the auto tile size so that the
   worst-case per-tile deep buffer fits within the budget.  Zero disables clamping.
2. **Skip `RenderResult.deep_data` when compositor does not need it**.  A new
   `RE_scene_has_deep_exr_file_output()` scan determines whether any compositor File Output node
   uses the DEEP_EXR format; the expensive `RenderResult` population is skipped when it returns
   false.
3. **Tiled deep accumulation**.  Per-tile Combined pass pixels are fed into a new
   `DeepOutputDriver::accumulate_tile()` call so all tiles are collected rather than only the
   last one.

## Follow-up Direction (2026-03-17)

- Current Cycles deep storage is still a **fixed-capacity per-tile allocation model** with a
  user-controlled tile budget. This makes deep memory use predictable and clampable, but it is not
  as storage-efficient as a sparse/compressed deep architecture.
- Local MoonRay code review indicates MoonRay's deep path is likely more memory-efficient overall
  because it uses sparser/compressed deep structures and per-pixel/per-thread volume working data
  rather than a full worst-case fixed-capacity deep buffer for every pixel.
- Current branch priority is **solid-surface deep alpha correctness**, not a deep volume or deep
  storage rewrite. Future memory optimization may borrow MoonRay-style sparse/compressed storage
  ideas, but any such work must preserve the current accepted volume deep behavior.

### Changed files

| File | Change |
|------|--------|
| `intern/cycles/session/deep_buffers.h` | Add `deep_compute_buffer_bytes()` + `deep_effective_max_samples()` declarations |
| `intern/cycles/session/deep_buffers.cpp` | Implement both helpers; fix member init order |
| `intern/cycles/session/deep_output_driver.h` | Add `accumulate_tile()` + `pixel_written_` member |
| `intern/cycles/session/deep_output_driver.cpp` | Tile accumulation; extend `merge_slice_into_cache()` |
| `intern/cycles/session/session.h` | Add `deep_tile_budget_mb` to `SessionParams` |
| `intern/cycles/session/session.cpp` | Budget-based tile-size clamp in `get_effective_tile_size()` |
| `intern/cycles/blender/session.cpp` | Guard `RenderResult.deep_data` with `compositor_needs_deep`; use shared helper |
| `intern/cycles/blender/sync.cpp` | Sync `deep_tile_budget_mb` from Python scene settings |
| `intern/cycles/blender/addon/properties.py` | `deep_tile_budget_mb` IntProperty |
| `intern/cycles/blender/addon/ui.py` | Display budget in Performance > Memory |
| `intern/cycles/blender/addon/presets.py` | Include budget in performance presets |
| `intern/cycles/integrator/path_trace.cpp` | Call `accumulate_tile()` per written tile |

---

## Resolution Update (2026-02-22)

All H/M/L findings in this report have been addressed in the working tree. The only remaining
pre-commit task is to pick a final commit message if/when the changes are committed.

---

## Blender Code Standards Reference

Standards applied in this review (from AGENT.md):

- **Cycles C++:** `CCL_NAMESPACE_BEGIN/END`, K&R braces, `snake_case`, 2-space indent
- **Line limit:** 100 characters
- **Include order:** system (`<...>`) -> blank -> Blender project (`"..."`) headers;  within project:
  `DNA_` / `BKE_` / `IMB_` first, then sub-module and `util/` headers
- **Comments:** Full sentences ending with a period inside `/* */`; Doxygen `/** \name ... \{ */`
  sections for class regions
- **Commit message:** `Module: short imperative summary` on line 1 (<=72 chars); blank line; body
  explaining user-visible change and technical approach

---

## Critical Issues

_None found._

---

## High Priority Issues

### H1. Mixed line endings in modified files

**Severity:** High  -  will generate noisy diffs in upstream patch review.

Git reports LF-only lines inside otherwise CRLF files for 8 of the 12 changed files:

| File | CRLF | LF-only |
|------|------|---------|
| `deep_output_driver.h` | 183 | 42 |
| `deep_output_driver.cpp` | 650 | 202 |
| `session.cpp` | 816 | 22 |
| `blender/sync.cpp` | 1162 | 14 |
| `integrator/path_trace.cpp` | 1548 | 30 |
| `addon/properties.py` | 2026 | 14 |
| `addon/ui.py` | 2648 | 5 |
| `addon/presets.py` | 131 | 6 |

`deep_buffers.h` and `deep_buffers.cpp` are pure CRLF (correct on Windows with
`core.autocrlf=true`).  The above files have residual LF-only lines  -  likely from the editor that
wrote the new blocks.  These should be normalized before committing.

**Fix:** `git add` then inspect with `git diff --check`; or run
`dos2unix` / `unix2dos` on each file after confirming `core.autocrlf=true`.

---

### H2. `deep_output_driver.cpp` include group ordering

**Severity:** High  -  violates Blender include ordering convention.

In `deep_buffers.cpp` the `IMB_deep_sample_merge.hh` header is grouped with project-local Cycles
headers without a blank-line separator:

```cpp
// deep_buffers.cpp lines 9-14
#include "device/device.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "IMB_deep_sample_merge.hh"   // <-- IMB_ header mixed into Cycles group
#include "util/algorithm.h"
#include "util/log.h"
```

The convention used elsewhere in Cycles (e.g. `blender/image.cpp`) separates Blender API headers
(`IMB_`, `BKE_`, `DNA_`) from Cycles-internal headers with a blank line:

```cpp
#include "IMB_deep_sample_merge.hh"

#include "device/device.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "util/algorithm.h"
#include "util/log.h"
```

**Fix:** Add a blank line before `#include "IMB_deep_sample_merge.hh"` to separate it from the
Cycles-internal group.

---

### H3. `deep_output_driver.cpp`: private method `compute_deep_bytes()` is a dead wrapper

**Severity:** High  -  unnecessary indirection; dead code in public API surface.

`DeepOutputDriver::compute_deep_bytes()` (line 91) is a one-line wrapper that simply calls the
free function `deep_compute_buffer_bytes()`:

```cpp
bool DeepOutputDriver::compute_deep_bytes(int width, int height, int max_samples, size_t &bytes) const
{
  return deep_compute_buffer_bytes(width, height, max_samples, bytes);
}
```

The wrapper adds a private method declaration in the header, a `const` qualifier that carries no
meaning (it does not access `this`), and a one-level indirection.  `build_device_estimates()` is
the only caller and should call `deep_compute_buffer_bytes()` directly.

**Fix:** Remove `compute_deep_bytes()` from the header and `.cpp`; call
`deep_compute_buffer_bytes()` directly from `build_device_estimates()`.

---

### H4. `deep_output_driver.cpp`: `check_device_memory()` still queries device VRAM

**Severity:** High  -  contradicts the stated goal of replacing dynamic VRAM queries with a
user budget, and is not guarded by the new budget path.

The handoff notes "no device memory queries remain," but `check_device_memory()` (line 139) still
calls `estimate.device->get_device_memory_info(total, free)` and can block allocation when VRAM is
insufficient.  This check occurs inside `sync_device_buffers()` which runs on every tile layout
change.  For CPU devices `get_device_memory_info()` returns zeros and is skipped, but for GPU
devices it remains active.

The user budget in `session.cpp` reduces tile size so the budget is respected _before_ allocation,
making the per-allocation VRAM check partially redundant.  The two mechanisms need either clearer
documentation of their relationship or the VRAM check should be removed/documented as a hard-stop
safety net distinct from the user budget.

**Fix (option A):** Add a block comment above `check_device_memory()` and its call site explaining
that the user budget in `session.cpp` is the primary mechanism and this is a secondary allocation
guard.

**Fix (option B):** Remove `check_device_memory()` and its associated `DeviceEstimate` struct and
`build_device_estimates()` usage, relying entirely on the user budget.

---

## Medium Priority Issues

### M1. `session.h`: `deep_tile_budget_mb` lacks an inline comment

**Severity:** Medium  -  other fields in `SessionParams` are grouped and commented (e.g.
`use_auto_tile` / `tile_size` are documented together).  The new field sits between `tile_size`
and `use_resolution_divider` without explanation:

```cpp
bool use_auto_tile;
int tile_size;
int deep_tile_budget_mb;    // <-- no comment

bool use_resolution_divider;
```

**Fix:** Add a brief inline comment, e.g.:
```cpp
/* Deep EXR buffer budget per device (MB); 0 disables tile-size clamping. */
int deep_tile_budget_mb;
```

---

### M2. `session.cpp`: tile clamp logic has two redundant `max(..., 8)` calls

**Severity:** Medium  -  minor clarity issue.

```cpp
const int max_tile_unaligned = max(8, static_cast<int>(std::floor(...)));
int max_tile = max_tile_unaligned;
if (max_tile >= TileManager::IMAGE_TILE_SIZE) {
    max_tile = (max_tile / TileManager::IMAGE_TILE_SIZE) * TileManager::IMAGE_TILE_SIZE;
}
max_tile = max(max_tile, 8);   // <-- second redundant clamp
```

The first `max(8, ...)` ensures `max_tile_unaligned >= 8`.  The alignment step can only reduce
`max_tile` to a multiple of `IMAGE_TILE_SIZE`; if `IMAGE_TILE_SIZE > 8`, the aligned value could
in theory fall below 8 if `max_tile` was less than `IMAGE_TILE_SIZE`.  But then the first `max`
would already produce a value >= 8 which is below `IMAGE_TILE_SIZE`, so the alignment block is
skipped.  The second `max(max_tile, 8)` is therefore unreachable.

**Fix:** Remove the second `max_tile = max(max_tile, 8)` call and add a brief comment that the
alignment step is only applied when `max_tile >= IMAGE_TILE_SIZE`.

---

### M3. `deep_output_driver.cpp`: `accumulate_tile()` calls `process_device_buffers()` on every tile

**Severity:** Medium  -  performance concern.

`accumulate_tile()` (line 440) calls `process_device_buffers()` at the start of each tile.
`process_device_buffers()` sorts and merges samples for all slices  -  an O(pixels x samples)
operation.  For N tiles this is O(N x pixels x samples) total sort work, which grows
quadratically with tile count.

The `deep_buffers_processed_` flag prevents re-processing, but `accumulate_tile()` itself resets
it to `false` at the end (line 476), so the next tile will re-sort the same (now larger cache)
data again.

A correct approach would process buffers once per tile (before copying from device) but not
re-process the accumulated `processed_cache_`.  The current structure confounds device-buffer
processing with cache population.

**Note:** This may be intentional for correctness (merge thresholds may need to be re-applied
across tiles), but the performance implication should be documented.

**Fix:** Add a comment explaining why `deep_buffers_processed_` is reset after accumulation, or
restructure to process once per tile before merging into the cache.

---

### M4. `deep_output_driver.h`: `pixel_written_` member is public-side-visible state coupling

**Severity:** Medium  -  design concern.

`pixel_written_` (line 174) tracks which global pixels have been written and is checked in both
`accumulate_tile()` and `ensure_processed_cache()`.  The dual use of this flag creates a subtle
coupling: if `accumulate_tile()` is not called (single-tile render), `ensure_processed_cache()`
falls back to the original path with its own local `pixel_written` vector.  If
`accumulate_tile()` is called for some tiles but not others (partially tiled render), the
`pixel_written_` member could have stale entries.

**Fix:** Add a comment documenting the invariant: `pixel_written_` is populated only when
`accumulate_tile()` is called, and `ensure_processed_cache()` checks whether it is non-empty to
choose the code path.

---

### M5. `deep_buffers.cpp`: `compute_sample_offsets()` computes a trivial always-fixed layout

**Severity:** Medium  -  dead computation and misleading API.

`compute_sample_offsets()` fills `sample_offsets_[i] = i * max_samples_per_pixel_`  -  a value
that is deterministic from `i` and `max_samples_per_pixel_` at any time.  The comment even
acknowledges this:

```cpp
/* Note: For deep buffers we use a fixed layout where each pixel has
 * max_samples_per_pixel slots, so offset = pixel_index * max_samples_per_pixel. */
```

The `sample_offsets_` vector adds memory overhead proportional to image size, and
`get_sample_offsets_host()` is only called from `merge_slice_into_cache()`  -  which could compute
the offset inline with the same formula.

**Fix:** Remove `sample_offsets_`, `compute_sample_offsets()`, and `get_sample_offsets_host()`.
Replace the `sample_offsets[local_idx]` lookup in `merge_slice_into_cache()` with
`local_idx * max_samples_per_pixel_`.  This reduces memory usage and removes an unnecessary
pass.

---

### M6. `deep_output_driver.cpp`: large `merge_slice_into_cache()` parameter list

**Severity:** Medium  -  readability and Blender style.

`merge_slice_into_cache()` takes 9 parameters (line 565):

```cpp
void merge_slice_into_cache(const DeepBufferSlice &slice,
                             bool track_overlap,
                             vector<uint8_t> &pixel_written,
                             bool &overlap_logged,
                             const float *beauty_pixels,
                             int beauty_width,
                             int beauty_height,
                             int beauty_offset_x,
                             int beauty_offset_y);
```

Blender convention favours grouping related parameters into a struct rather than using long
parameter lists.  A `TileBeautyParams` or similar POD struct would improve readability at call
sites.

**Fix (low priority):** Group the last 5 beauty parameters into an anonymous struct or a small
named struct, or at minimum add a blank comment line between the overlap tracking params and the
beauty params.

---

### M7. `blender/session.cpp`: indentation of `RE_GetRenderLayer` call site

**Severity:** Medium  -  style.

At line ~675 of the diff:

```cpp
blender::RenderLayer *deep_layer = RE_GetRenderLayer(render_result,
                                                    b_rlay_name.c_str());
```

The continuation is misaligned: it aligns to one space past the opening paren of `RE_GetRenderLayer`
rather than matching the argument indent convention used in the surrounding code (2-space indent
from the start of the `RE_GetRenderLayer` token).  Compare with the old code in the same file
which aligned to the first argument column.

**Fix:** Align the second line to the first argument:
```cpp
blender::RenderLayer *deep_layer = RE_GetRenderLayer(render_result, b_rlay_name.c_str());
```
(fits in 100 chars) or use the standard 4-space continuation indent if the call is intentionally
wrapped.

---

## Low Priority / Style Issues

### L1. `deep_buffers.cpp`: `w > 0 && h > 0` guard is always true

In `deep_compute_buffer_bytes()`:

```cpp
if (w > 0 && h > 0 && w > max_size / h) {
```

`w` and `h` are `static_cast<size_t>(width/height)` where the early return already guarantees
`width > 0` and `height > 0`, so both casts produce values >= 1.  The `w > 0 && h > 0` test
is never false.

**Fix:** Remove the redundant guard: `if (w > max_size / h)`.

---

### L2. `deep_output_driver.h`: `DeepBufferSnapshot` struct is private to `.cpp`, not `.h`

The `DeepBufferSnapshot` struct (line 147) is only used as the return type of
`snapshot_device_buffers()` (private method) and as a parameter of `restore_snapshots()`.  Both
methods are private and the struct is never visible outside the class.  Placing it in the header
exposes it unnecessarily.

**Fix:** Move `DeepBufferSnapshot` inside `deep_output_driver.cpp` as a class-local or anonymous
namespace type.

---

### L3. `deep_output_driver.cpp`: `get_deep_buffers()` only works for single-device and returns `nullptr` for multi-device

```cpp
DeepRenderBuffers *DeepOutputDriver::get_deep_buffers()
{
  if (device_buffers_.size() == 1) {
    return device_buffers_[0].buffers.get();
  }
  return nullptr;
}
```

This method is declared in the header but has no callers outside the class in the current code
(confirmed by grep).  If the method is intended for future use it should be documented.  If it
is dead, it should be removed.

**Fix:** Add a comment explaining the single-device restriction, or remove the method if unused.

---

### L4. `session.cpp` TODO comment: ownership attribution

The modified TODO at line 486:

```cpp
/* TODO(sergey): Take available memory into account for non-deep renders so we can
 * avoid tiling when there is enough memory. Deep EXR now applies a user budget. */
```

The appended sentence ("Deep EXR now applies a user budget.") is new content added by this
branch.  Blender convention is that `TODO(name):` attributions refer to the original author who
filed the TODO.  Adding new prose to someone else's TODO without a second attribution may be
confusing in future diffs.

**Fix:** Split into two comments: keep the original `TODO(sergey):` unchanged and add a separate
sentence comment below it explaining the deep EXR approach.

---

### L5. `deep_output_driver.cpp`: `deep_memory_headroom_bytes` constant name vs. purpose after refactor

After the budget refactor, `deep_memory_headroom_bytes` (32 MB) is only used by
`check_device_memory()` as a safety headroom.  Its name was meaningful when it was used in the
tile budget calculation, but now the constant is scoped inside an anonymous namespace in a 850-line
file and its sole use is in `check_device_memory()`.

**Fix:** Move the constant's definition immediately above `check_device_memory()` or inline it,
and update its comment to read: `/* Safety headroom: refuse allocation when less than 32 MB
 * free to avoid OOM from other driver allocations. */`

---

### L6. `properties.py`: tooltip wording

RNA tooltip for `deep_tile_budget_mb` (line 1066):

```
"Maximum Deep EXR buffer budget per device (MB). Set to 0 to disable deep tile clamp"
```

Two issues:
- Blender RNA tooltips should be in infinitive form and end with a period (AGENT.md).
- "deep tile clamp" is an internal implementation term, not a user-facing concept.

**Fix:**
```
"Limit Deep EXR tile buffer memory per device in megabytes. Set to 0 to disable the limit."
```

---

### L7. `ui.py`: no visibility condition on `deep_tile_budget_mb` prop

The budget widget (line 884) is always visible regardless of whether `use_auto_tile` is enabled.
The existing `tile_size` prop has the same unconditional display.  For consistency with Blender UI
patterns the budget should only be meaningful (and ideally enabled/greyed out) when
`use_auto_tile` is on, since the clamp only applies to auto tile sizing.

**Fix (low priority):** Wrap the property display in:
```python
sub = layout.column()
sub.enabled = cscene.use_auto_tile
sub.prop(cscene, "deep_tile_budget_mb")
```
or add a note in the tooltip that the budget applies only when auto-tile is on.

---

## Commit Message Assessment

The branch contains only one pending commit (uncommitted working tree).  The suggested commit
message from the previous review entry is:

```
Cycles: user budget for Deep EXR tile sizing

Expose a Deep EXR tile budget in the UI (default 1024 MB). When set,
auto-tiling clamps deep buffers to the budget per device, and 0 disables
the clamp. Keep RenderResult deep data only when compositor needs it.
```

**Assessment:**

- Line 1: `Cycles: user budget for Deep EXR tile sizing`  -  follows the `Module: summary`
  convention, <=72 chars, imperative form. **Pass.**
- Body: explains both the tile budget feature and the `compositor_needs_deep` optimization.
  **Pass.**
- Missing: no mention of the tiled accumulation fix (Feature 3 in the summary).  The body should
  add a sentence: "Also accumulate per-tile deep samples progressively to avoid last-tile-only
  output when rendering multiple tiles."
- Missing: no mention of the `deep_effective_max_samples()` helper extraction.

**Revised suggested commit message:**

```
Cycles: user budget and progressive accumulation for Deep EXR

Expose a Deep EXR tile budget in the UI (default 1024 MB, 0 disables).
Auto-tiling clamps deep buffer size to this budget per device.

Accumulate deep samples per tile rather than collecting only the last
tile, so compositor deep outputs are complete for multi-tile renders.

Skip RenderResult deep_data population when no compositor File Output
node requires it, avoiding unnecessary memory allocation.

Extract deep_effective_max_samples() helper shared between the tile
clamp path and the deep driver setup.
```

---

## Summary Table

| ID | Severity | File | Issue |
|----|----------|------|-------|
| H1 | High | 8 files | Mixed CRLF / LF-only line endings |
| H2 | High | `deep_buffers.cpp` | `IMB_deep_sample_merge.hh` not separated from Cycles includes |
| H3 | High | `deep_output_driver.cpp` | `compute_deep_bytes()` is dead wrapper for free function |
| H4 | High | `deep_output_driver.cpp` | `check_device_memory()` still queries device VRAM; unclear relation to user budget |
| M1 | Medium | `session.h` | `deep_tile_budget_mb` field missing inline comment |
| M2 | Medium | `session.cpp` | Second `max(max_tile, 8)` is unreachable |
| M3 | Medium | `deep_output_driver.cpp` | `accumulate_tile()` re-processes all device buffers each tile (quadratic sort) |
| M4 | Medium | `deep_output_driver.h/.cpp` | `pixel_written_` dual-use coupling without invariant documentation |
| M5 | Medium | `deep_buffers.cpp` | `compute_sample_offsets()` and `sample_offsets_` are dead data |
| M6 | Medium | `deep_output_driver.cpp/.h` | `merge_slice_into_cache()` 9-param signature |
| M7 | Medium | `blender/session.cpp` | Misaligned continuation indent on `RE_GetRenderLayer` call |
| L1 | Low | `deep_buffers.cpp` | Redundant `w > 0 && h > 0` guard in `deep_compute_buffer_bytes()` |
| L2 | Low | `deep_output_driver.h` | `DeepBufferSnapshot` struct exposed in header unnecessarily |
| L3 | Low | `deep_output_driver.cpp` | `get_deep_buffers()` unreachable for multi-device; no callers found |
| L4 | Low | `session.cpp` | New text appended to `TODO(sergey):` without attribution |
| L5 | Low | `deep_output_driver.cpp` | `deep_memory_headroom_bytes` constant far from its only use site |
| L6 | Low | `addon/properties.py` | Tooltip not infinitive form, uses internal term "deep tile clamp" |
| L7 | Low | `addon/ui.py` | Budget prop always visible even when auto-tile is off |

---

## Recommended Pre-Commit Checklist

- [x] Normalize line endings (H1)
- [x] Separate `IMB_` include into its own group in `deep_buffers.cpp` (H2)
- [x] Remove `compute_deep_bytes()` wrapper (H3)
- [x] Document or resolve VRAM check vs. user budget (H4)
- [x] Add inline comment on `deep_tile_budget_mb` in `session.h` (M1)
- [x] Remove second `max(max_tile, 8)` (M2)
- [x] Add comment explaining `deep_buffers_processed_` reset in `accumulate_tile()` (M3)
- [x] Add invariant comment for `pixel_written_` (M4)
- [x] Remove `compute_sample_offsets()` / `sample_offsets_` (M5)
- [x] Clarify `merge_slice_into_cache()` parameter grouping (M6)
- [x] Fix `RE_GetRenderLayer` continuation alignment (M7)
- [x] Remove redundant `w > 0 && h > 0` guard in `deep_compute_buffer_bytes()` (L1)
- [x] Move `DeepBufferSnapshot` definition into the `.cpp` (L2)
- [x] Document `get_deep_buffers()` single-device limitation (L3)
- [x] Split the `TODO(sergey)` comment for deep budget context (L4)
- [x] Move the deep memory headroom constant near its use (L5)
- [x] Fix tooltip wording (L6)
- [x] Disable deep budget UI when auto-tile is off (L7)
- [ ] Update commit message body to cover all three changes (Commit)
