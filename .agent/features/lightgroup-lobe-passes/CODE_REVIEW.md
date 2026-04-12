# Code Review: `feature/per-lightgroup-lobe-passes` (Re-review)

**Date:** 2026-03-13
**Commits:**
1. `6004a70` — *Cycles: optimize light pass AOV detection/sync and stale split pruning*
2. `ec56ced` — *Cycles: apply code review fixes for lightgroup light pass AOVs*

**Base:** `vfx-rendering-branch-github` (`1cf4166`)
**Files changed:** 14 (+1078 / −81)

---

## Merge Conflict Status

✅ **Clean merge** — no conflicts with `vfx-rendering-branch-github`.

---

## Previous Review Issues → Resolution Status

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | Unconditional lightgroup lobe writes in `film_write_direct_light` | P0 | ✅ **Fixed** — `has_lightgroup_surface_passes` and volume guards added |
| 2 | `is_lightgroup_split_output_identifier` false positives (naming collisions) | P0 | ✅ **Fixed** — now validates suffix against actual lightgroup names |
| 3 | Massive code duplication in `sync.cpp` | P1 | ✅ **Fixed** — data-driven `LightgroupSplitPassDesc` table |
| 4 | 13 boolean property reads in `sync.cpp` | P1 | ✅ **Fixed** — single loop over descriptor table |
| 5 | `compositing_node_group` fallback | P2 | ✅ **Fixed** — `getattr` fallback to `scene.node_tree` |
| 6 | `view_layer.world_override` fallback | P2 | ✅ **Fixed** — `getattr(view_layer, "world_override", None)` |
| 7 | `_LIGHTGROUP_SPLIT_PASS_PREFIXES` ordering | P2 | ✅ **Fixed** — longest-first (`diffuse_direct_` before `diffuse_`) |
| 8 | Commit message scope (single monolithic commit) | P2 | ⚠️ **Partially addressed** — now 2 commits, still broad |
| 9 | Duplicated Python/C++ lightgroup inheritance logic | P2 | ℹ️ **Acknowledged** — acceptable for custom branch |
| 10 | `.agent/` doc files in commit | P2 | ℹ️ **By design** — custom branch documentation |

---

## New Review: Fix Commit `ec56ced`

### ✅ What's Good

**1. `sync.cpp` — Data-driven descriptor table**

The 12 copy-pasted blocks are replaced with a clean `LightgroupSplitPassDesc` struct array + single loop. This is exactly what was recommended:

```cpp
struct LightgroupSplitPassDesc {
  const char *property_name;
  const char *pass_name_format;
  PassType pass_type;
};

static const LightgroupSplitPassDesc lightgroup_split_passes[] = {
    {"use_lightgroup_light_pass_aov_diffuse_combined", "diffuse_%s", PASS_DIFFUSE},
    // ... 11 more entries
};
```

The `enabled_split_passes` vector collects only the active entries, avoiding repeated boolean checks. Net reduction: ~200 lines.

**2. `light_passes.h` — Availability guards in `film_write_direct_light`**

Surface and volume paths are now guarded by compound `PASS_UNUSED` checks:

```c
const bool has_lightgroup_surface_passes =
    kernel_data.film.pass_lightgroup_diffuse != PASS_UNUSED || ...;
if (has_lightgroup_surface_passes) { /* 6 write calls */ }
```

This avoids 6–9 unnecessary function calls per sample when the feature is disabled.

**3. `node_composite_render_layers.cc` — Real lightgroup validation**

The identifier check now extracts the suffix after the prefix and validates it against actual `ViewLayerLightgroup` entries. The prefix order is also longest-first to prevent `"diffuse_"` from matching `"diffuse_direct_foo"` before `"diffuse_direct_"` gets checked.

**4. `engine.py` — Python robustness**

Both `world_override` and `compositing_node_group`/`node_tree` access now use `getattr` with fallbacks.

---

### ⚠️ Remaining Items (Minor)

#### 1. `has_lightgroup_surface_passes` — 9-way OR on every shadow ray

The compound boolean reads 9 kernel data members per sample. These are uniform (constant across the frame), so the compiler/driver should hoist and fold them. On CPU this is trivial; on GPU, the constant data is typically in constant memory. This is acceptable as-is — just noting it's an explicit trade-off vs. a dedicated `kernel_features` flag.

#### 2. `engine.py` still has 12 repeated `yield` blocks in `list_render_passes`

The data-driven cleanup in `sync.cpp` wasn't mirrored to the Python side. The `list_render_passes` function still has 12 `if crl.use_...: yield(...)` blocks. This can be a follow-up cleanup but is not blocking.

#### 3. Commit granularity for upstream

The two commits are better than one, but for an upstream Blender PR this would typically be split further (kernel/scene, sync, addon, compositor). Fine for the custom branch.

#### 4. `emission_or_background` path still writes lobe passes for background light

The `split_lightgroup_lobes` guard:
```c
const bool split_lightgroup_lobes = (pass != kernel_data.film.pass_emission);
```
This means background contributions (HDRI environment) **are** split into lobe channels, but emission objects are not. Verify this is the intended policy — an environment light contributing via glossy bounce will show up in `glossy_<lg>` but a mesh emitter won't.

---

## Performance Considerations (Unchanged)

| Concern | Impact |
|---------|--------|
| 12 extra `KERNEL_STRUCT_MEMBER(film, int, ...)` | +48 bytes to `KernelFilm` — always allocated |
| Availability guards in `film_write_direct_light` | New: eliminates function-call overhead when feature off ✅ |
| `get_splittable_lightgroups` iterates view layer objects | O(n) per sync; fine for typical scenes |

---

## Verdict

The fix commit cleanly addresses all P0 and P1 issues from the original review. The code is now significantly more maintainable (`sync.cpp` cut by ~200 LOC), safer against naming collisions (compositor validates against real lightgroup names), and more efficient (kernel guards prevent dead work).

**Remaining work is all P2/minor** — Python-side yield-block dedup is a nice-to-have follow-up. The branch is in good shape for merge into `vfx-rendering-branch-github`.
