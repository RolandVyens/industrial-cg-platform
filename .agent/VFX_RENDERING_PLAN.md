# VFX Rendering Branch - Development Plan

## Context

The vfx-rendering-branch already has deep EXR support merged. We're planning the next set of VFX rendering features, each implemented as a separate feature branch based off vfx-rendering-branch.

## Active Optimization: Deep EXR Memory Efficiency (task/deep-exr-memory)

Consolidated current-state reference:
- `.agent/DEEP_EXR_DEVELOPMENT_STATE.md`

- **Deep EXR surface follow-up (2026-03-20, validated/current):** The current worktree source is back on the older e720-like hard-surface path (the broad surface-coverage experiment is not the active code path). The accepted hard-surface fix reconstructs front opaque-surface prefixes and keeps volume suffix handling on the existing path.
**Goal:** Reduce RAM/VRAM usage for Deep EXR renders by clamping tile size based on a user-controlled
deep buffer budget (default 1024 MB) and skipping RenderResult deep storage when the compositor does
not need it.

### Implementation Notes
- Add shared deep buffer byte estimation helper (used by deep output driver + session tiling).
- Clamp auto tile size when `film.use_deep_output` is on and deep buffers exceed budget.
- Budget: user setting `deep_tile_budget_mb` (default 1024 MB), per device, 0 disables clamp.
- Skip `RenderResult.deep_data` population unless compositor has a Deep EXR File Output node.
- Deep EXR surface edge follow-up (2026-03-16): preserve opaque surface duplicates through the deep
  merge stage and reconstruct conditional edge alpha from sorted hard-surface hits before export, so
  foreground edge samples no longer stay solid `1.0` in Deep EXR outputs.
- Deep EXR single-surface follow-up (2026-03-16): use internal tiled `PASS_SAMPLE_COUNT` capture
  for multi-depth hard-surface reconstruction, but when preserved opaque duplicates still collapse
  to one depth group, assign the final deep alpha directly from flattened beauty alpha to keep
  single-surface AA edges consistent with the flat EXR.
- Deep EXR cleanup sweep (2026-03-17): follow-up code review items were applied in
  `feature/deep-exr-edge-alpha-fix`, including `size_t` sample offsets, cache pixel-count overflow
  cleanup, unified pixel-population helper flow, mutable deep-buffer accessor rename cleanup,
  `deep_compute_buffer_bytes()` declaration/definition matching, and normalization of mixed EOLs
  across all modified tracked files in the fix worktree.

- Deep EXR narrow front-prefix follow-up (2026-03-20): on the current e720-like Deep EXR path,
  collapse single-active-sample pixels before export and reconstruct only the front opaque-surface
  prefix when any remaining suffix is volume-only. Leave the suffix on the existing volume/scaled
  path instead of reintroducing the broader surface-coverage experiment.
- Deep EXR hard-surface compaction debug (2026-03-21): traced the remaining Nuke DeepMerge seam on
  `light-passes-test-v001.blend` frame 2 to an over-strict hard-surface prefix normal threshold.
  Saved EXR checker pixel `(302, 150)` was splitting one teapot foreground segment into two groups
  because one valid hit had normal dot `~0.9679` against the main foreground cluster while the
  grouping threshold was `0.98`. The current narrow follow-up test lowers
  `deep_surface_normal_dot_threshold` to `0.95`; refreshed frame-2 validation now reports
  `.agent/check_deep_surface_compaction.py ... overfragmented_pixels=0`.
- Deep EXR direct scene-output follow-up (2026-03-21): direct scene-output Deep EXR finalization on
  `light_passes_test_deep_saved.blend` was dropping all accumulated samples whenever finalize-time
  full-frame Combined / Debug Sample Count buffers were missing. The current fix keeps the
  accumulated `processed_cache_` alive by skipping null beauty/sample-count resets in
  `intern/cycles/blender/session.cpp`; verification now reports
  `nonempty_pixels=1807695`, `total_samples=69538624`, `max_samples=235` for
  `C:\tmp\scene_output_rgba_deep_saved_####.exr`.
- Deep EXR hard-surface grouping follow-up (2026-03-21): the traced checker pixel
  `(302, 150)` still showed a single overly strong front teapot sample because prefix compaction was
  merging by `object+shader` only. The current hypothesis test switches prefix grouping to exact
  `surface_key` matching, preserving primitive identity for hard-surface prefix export. On the
  traced pixel the front teapot sample no longer collapses to `A=0.75`; it becomes a stack of
  smaller front samples (`0.03125`, `0.032258`, `0.033333`, ...) while preserving total resolved
  alpha. Volume handling remains unchanged.
- Deep EXR scene-output validation redirect (2026-03-22): the active visual
  validation path is now the **straight scene-output Deep EXR** case, not the compositor
  DeepRecolor path. The temporary compositor `alpha_only=false` hypothesis was rejected and backed
  out. Current direct scene-output Nuke validation still shows the white seam in
  `C:\tmp\direct_scene_output_saved_write1.png`, while the traced seam pixel `(302, 150)` in
  `C:\tmp\scene_output_rgba_deep_saved_####.exr` contains 22 deep samples with real RGB. This keeps
  the current investigation focused on hard-surface prefix compaction / coverage, not compositor
  deep channel stripping.
- Deep merge matrix refresh (2026-03-22): the untouched `light-passes-test-v001.blend` currently
  contains only an alpha-only compositor Deep EXR node (`ViewLayer--Deep` linked from
  `ViewLayer.Alpha`). For current regression checks, compositor RGBA deep validation is therefore
  done with the runtime-only helper
  `.agent/render_temp_compositor_rgba_deep.py`, which rewires that node to `ViewLayer.Image` and
  writes `D:\blender_projects\rendered\test\TempDeepRGBA\ViewLayer_Deep_v001_0002.exr` without
  saving the blend. Latest seam-pixel matrix check at `(302, 150)` passes with matching
  `sample_count=4` across direct scene-output, runtime compositor RGBA deep, and untouched
  compositor alpha-only deep; direct and runtime compositor RGBA also match the same nonzero deep
  RGB values.
- Deep EXR review cleanup follow-up (2026-03-22): current merge-prep cleanup now uses a shared
  `deep_file_debug_enabled()` helper in `intern/cycles/blender/session.cpp`, lowers the env-var
  deep file tracing to `LOG_DEBUG`, and adds cross-reference comments that keep the duplicated deep
  metadata constants/helpers synchronized between `deep_write.h` and `deep_buffers.h`.
- Deep EXR hard-surface compaction follow-up (2026-03-22): resumed from a fresh controlled
  scene-output Deep EXR render path (`C:\tmp\scene_output_rgba_deep_probe_####.exr`) to avoid
  stale-file confusion from `trash_output\.exr`. Root cause of the remaining over-fragmentation was
  that hard-surface prefix grouping still compared full `object+shader+prim` identity, so adjacent
  triangles on the same visible surface never compacted. The current follow-up groups by
  `object+shader` plus normal continuity instead. Fresh verification now reports
  `.agent/check_deep_surface_compaction.py ... overfragmented_pixels=0`, while the fresh controlled
  render still keeps `mismatching_single_surface_pixels=0` and
  `violating_front_surface_alpha_pixels=0`.
- Deep EXR hard-surface opaque-coverage follow-up (2026-03-23): the remaining DeepMerge seam after
  compaction was traced to **pre-export opaque duplicate collapse**, not to the export-side
  grouping logic. `DeepRenderBuffers::merge_nearby_samples()` was still running the shared deep
  merge helper with `preserve_opaque_surface_duplicates=false`, collapsing many identical opaque
  hard-surface camera hits into only a few representatives before export. That left pixels like
  EXR `(655, 403)` with only `4` raw opaque hits but `sample_count=32`, so export reconstructed
  tiny front alphas and the flattened deep alpha dropped to `0.125` instead of `1.0`. The current
  fix restores `preserve_opaque_surface_duplicates=true` in the Cycles deep-buffer merge path while
  leaving volume merging unchanged. Fresh controlled-render verification now keeps:
  `.agent/check_deep_surface_opaque_coverage.py ... mismatching_opaque_pixels=0`,
  `.agent/check_deep_surface_compaction.py ... overfragmented_pixels=0`,
  `.agent/check_deep_single_surface_alpha.py ... mismatching_single_surface_pixels=0`, and
  `.agent/check_deep_surface_front_alpha.py ... violating_front_surface_alpha_pixels=0`. Updated
  Nuke artifacts in `C:\tmp\nuke_scene_output_rgba_deep_probe*.png` show the large white seam
  removed, with only a tiny residual mask cluster still visible for later polish/debug.
- Deep EXR metadata reconstruction activation (2026-03-23): the newer hard-surface metadata path is
  now active on the kernel side. Bounce-0 hard-surface hits write metadata through
  `film_write_deep_surface_sample_transparent(...)`, path/shadow state carries
  `deep_surface_sample_idx`, and `light_passes.h` accumulates bounce-0 surface/shadow RGB into the
  exact deep sample via `film_accumulate_deep_surface_rgb(...)`. Volume deep remains unchanged.
- Current interpretation after the activation follow-up (2026-03-23): the accepted visible
  coverage fix should still be attributed first to preserved opaque duplicate coverage in
  `DeepRenderBuffers::merge_nearby_samples()`, while the metadata-aware hard-surface
  reconstruction code in `deep_write.h` / `deep_buffers.h` / `deep_output_driver.cpp` is now
  partially live rather than pure groundwork.
- Validation (2026-03-20): `deep-branch-test.blend` CPU/factory-startup rerun keeps
  `checked_single_surface_fractional_pixels=6657`, `mismatching_single_surface_pixels=0`,
  `multi_sample_pixels=39349`, and `violating_front_alpha_pixels=0`.
- Mixed-case safety (2026-03-20): `light-passes-test-v001.blend` still passes
  `.agent/check_deep_mixed_surface_volume_case1.py` with `mismatching_pixels=0`.
**Features sorted by development difficulty (easiest first):**
1. Per-Light Shadow Color
2. Indirect-Only Object Toggle (No Direct Lighting)
3. Per-Collection/Object ViewLayer Material Override
4. Per-Light-Group Direct/Indirect Material Lobe AOVs (LPE foundation)
5. World Environment Fog (Arnold aiFog-like)

---

## Feature 1: Per-Light Shadow Color

**Branch:** `feature/shadow-color`
**Difficulty:** Low
**Goal:** Add a shadow color property to lights so artists can tint shadows per light.

### Implementation Steps

**Step 1: DNA - Add shadow_color to Light struct**
- File: `source/blender/makesdna/DNA_light_types.h`
- Add `float shadow_color[3] = {0.0f, 0.0f, 0.0f};` to `Light` struct (black = no tinting, default behavior)
- Semantics: shadow_color blends with the shadow. `{0,0,0}` = default untinted shadows. `{0.2, 0, 0}` = reddish shadows.

**Step 2: RNA - Expose shadow_color property**
- File: `source/blender/makesrna/intern/rna_light.cc`
- Add `RNA_def_property(srna, "shadow_color", PROP_FLOAT, PROP_COLOR)` with subtype, min=0, max=1, description
- Reference existing color properties in the file for pattern

**Step 3: Versioning - Default initialization for old files**
- File: `source/blender/blenloader/intern/versioning_510.cc`
- Iterate all lights, set `shadow_color = {0,0,0}` for files below new subversion

**Step 4: Cycles Light class - Add shadow_color socket**
- File: `intern/cycles/scene/light.h` - Add `NODE_SOCKET_API(float3, shadow_color)`
- File: `intern/cycles/scene/light.cpp` - Register socket in node type

**Step 5: KernelLight - Add shadow_color to kernel struct**
- File: `intern/cycles/kernel/types.h` - Add `float shadow_color[3]` to `KernelLight`, adjust padding for 16-byte alignment

**Step 6: Light sync - Map Blender shadow_color to Cycles**
- File: `intern/cycles/blender/light.cpp` - In `sync_light()`, read `b_light.shadow_color()` and set on Cycles light
- File: `intern/cycles/scene/light.cpp` - In device_update, write shadow_color to KernelLight

**Step 7: Kernel shadow evaluation - Apply shadow_color**
- File: `intern/cycles/kernel/film/light_passes.h` - In `film_write_direct_light()`, after computing shadow contribution, multiply by `(1 - shadow_strength) * shadow_color + shadow_strength * white` where shadow_strength comes from the shadow ray
- Alternative (simpler): In `intern/cycles/kernel/integrator/shade_shadow.h`, after `integrate_transparent_shadow()` computes the final shadow throughput, blend shadow_color into the throughput for the specific light. Access light's shadow_color via `kernel_data_fetch(lights, light_index).shadow_color`
- The shadow throughput is stored in `shadow_path.throughput`. The tinting should apply when `throughput < 1.0` (partial occlusion): `throughput = max(throughput, shadow_color)`

**Step 8: UI - Add shadow color picker to light properties**
- File: `scripts/startup/bl_ui/properties_data_light.py` - Add `layout.prop(light, "shadow_color")` in the shadow section

### Key Files
| File | Change |
|------|--------|
| `source/blender/makesdna/DNA_light_types.h` | Add `shadow_color[3]` |
| `source/blender/makesrna/intern/rna_light.cc` | RNA property |
| `source/blender/blenloader/intern/versioning_510.cc` | Versioning |
| `intern/cycles/scene/light.h` | Node socket |
| `intern/cycles/scene/light.cpp` | Socket registration + device update |
| `intern/cycles/kernel/types.h` | KernelLight field |
| `intern/cycles/blender/light.cpp` | Sync from Blender |
| `intern/cycles/kernel/integrator/shade_shadow.h` | Shadow tinting |
| `scripts/startup/bl_ui/properties_data_light.py` | UI |

---

## Feature 2: Indirect-Only Object Toggle (No Direct Lighting)

**Branch:** `feature/no-direct-lighting`
**Difficulty:** Low-Medium
**Goal:** Object appears in camera, receives indirect illumination (GI, environment reflections), but does NOT receive direct light from any explicit light source.

This is different from the existing `BASE_INDIRECT_ONLY` (which removes camera visibility). Here the object IS camera-visible but direct light sampling is suppressed.

### Implementation Steps

**Step 1: DNA - Add object flag**
- File: `source/blender/makesdna/DNA_object_types.h` - Add `OB_NO_DIRECT_LIGHT = (1 << 11)` to `Object::visibility_flag` enum (check available bits)

**Step 2: DNA - Add collection flag**
- File: `source/blender/makesdna/DNA_layer_types.h` - Add `LAYER_COLLECTION_NO_DIRECT_LIGHT = (1 << 9)` and `BASE_NO_DIRECT_LIGHT = (1 << 12)` (check available bits in both enums)

**Step 3: Layer flag propagation**
- File: `source/blender/blenkernel/intern/layer.cc` - Add `BASE_NO_DIRECT_LIGHT` to `g_base_collection_flags`, propagate `LAYER_COLLECTION_NO_DIRECT_LIGHT` to `base->flag_from_collection`

**Step 4: RNA - Expose properties**
- File: `source/blender/makesrna/intern/rna_object.cc` - Add `no_direct_light` boolean property to `Object.visibility` (or similar group)
- File: `source/blender/makesrna/intern/rna_layer.cc` - Add property to LayerCollection

**Step 5: Cycles Object - Add kernel flag**
- File: `intern/cycles/kernel/types.h` - Add `SD_OBJECT_NO_DIRECT_LIGHT = (1u << 13)` to SD_OBJECT flags, add to `SD_OBJECT_FLAGS`
- File: `intern/cycles/scene/object.h` - Add `NODE_SOCKET_API(bool, no_direct_light)`

**Step 6: Cycles sync - Map flag from Blender**
- File: `intern/cycles/blender/object.cpp` - Read `OB_NO_DIRECT_LIGHT` from `b_ob.visibility_flag` and `BASE_NO_DIRECT_LIGHT` from `base_parent->flag`. Set `object->set_no_direct_light(true)` if either is set.

**Step 7: Kernel - Skip direct light for flagged objects**
- File: `intern/cycles/kernel/integrator/shade_surface.h` - In `integrate_surface_direct_light()`, early return if the hit object has `SD_OBJECT_NO_DIRECT_LIGHT` flag:
  ```c
  if (sd->object_flag & SD_OBJECT_NO_DIRECT_LIGHT) {
    return;
  }
  ```
- This skips all direct light sampling for the surface but still allows:
  - Indirect GI (bounced light from other surfaces)
  - Environment/HDRI illumination through indirect paths
  - Emission from the object itself
  - The object being visible in camera

**Step 8: Versioning**
- File: `source/blender/blenloader/intern/versioning_510.cc` - No special versioning needed (new flag defaults to 0 = off)

**Step 9: UI**
- File: `scripts/startup/bl_ui/properties_object.py` - Add checkbox in the visibility section
- File: `scripts/startup/bl_ui/properties_collection.py` or view layer outliner - Add collection toggle

### Key Files
| File | Change |
|------|--------|
| `source/blender/makesdna/DNA_object_types.h` | `OB_NO_DIRECT_LIGHT` flag |
| `source/blender/makesdna/DNA_layer_types.h` | Collection + base flags |
| `source/blender/blenkernel/intern/layer.cc` | Flag propagation |
| `source/blender/makesrna/intern/rna_object.cc` | RNA property |
| `source/blender/makesrna/intern/rna_layer.cc` | RNA property |
| `intern/cycles/kernel/types.h` | `SD_OBJECT_NO_DIRECT_LIGHT` |
| `intern/cycles/scene/object.h` | Node socket |
| `intern/cycles/blender/object.cpp` | Sync |
| `intern/cycles/kernel/integrator/shade_surface.h` | Skip direct light |
| UI python scripts | Checkboxes |

---

## Feature 3: Per-Collection/Object ViewLayer Material Override

**Branch:** `feature/collection-material-override`
**Difficulty:** Medium-High
**Goal:** Per-collection and per-object material overrides with priority: Object > Collection > ViewLayer.

Currently Blender only has `ViewLayer.mat_override` (one material for entire layer).

**Requirement:** Must support linked/library data and library overrides (no breaking on linked collections or
overridden objects/materials). Ensure overrides work in both local and linked data blocks.

### Implementation Steps

**Step 1: DNA - Add mat_override to LayerCollection**
- File: `source/blender/makesdna/DNA_layer_types.h`
- Add `struct Material *mat_override = nullptr;` to `LayerCollection` struct
- Add `struct Material *mat_override = nullptr;` to `Base` struct

**Step 2: RNA - Expose override properties**
- File: `source/blender/makesrna/intern/rna_layer.cc` - Add `mat_override` pointer property to LayerCollection and Base RNA types with `PROP_EDITABLE` flag

**Step 3: Layer flag propagation - Resolve override hierarchy**
- File: `source/blender/blenkernel/intern/layer.cc`
- During layer collection sync, propagate `mat_override` from parent collections to child collections (child override wins if set)
- Store resolved material override on each `Base` (from closest collection with an override, or null)
- New function: `BKE_layer_collection_material_override_get(LayerCollection *lc)` that walks up the hierarchy
- Ensure collection traversal includes linked collections and honors library overrides

**Step 4: Depsgraph - Tag objects when override changes**
- File: `source/blender/depsgraph/intern/builder/deg_builder_relations.cc` - Ensure material override changes on collection trigger object re-evaluation
- May need to add `DEG_id_tag_update` calls when override material changes
- Confirm depsgraph update paths handle library overrides and linked data updates

**Step 5: Cycles sync - Per-object material override**
- File: `intern/cycles/blender/sync.cpp` - Instead of single `view_layer.material_override`, store override per-object or pass it during object sync
- File: `intern/cycles/blender/geometry.cpp` - In `find_used_shaders()`, check resolution order:
  1. `base->mat_override` (per-object override from Base)
  2. Collection hierarchy override (resolved during layer sync)
  3. `view_layer.material_override` (existing ViewLayer override)
  4. Original object materials
- Ensure overrides resolve correctly for linked collections and library overrides

**Step 6: Versioning**
- File: `source/blender/blenloader/intern/versioning_510.cc` - New fields default to nullptr, no explicit versioning needed for pointer fields

**Step 7: UI**
- File: `scripts/startup/bl_ui/properties_collection.py` - Material override picker in collection properties
- Outliner: add override indicator icon for collections with material overrides
- File: `scripts/startup/bl_ui/properties_render_layer.py` - Keep existing ViewLayer override, add note about hierarchy

### Key Files
| File | Change |
|------|--------|
| `source/blender/makesdna/DNA_layer_types.h` | `mat_override` on LayerCollection and Base |
| `source/blender/makesrna/intern/rna_layer.cc` | RNA properties |
| `source/blender/blenkernel/intern/layer.cc` | Override resolution |
| `source/blender/depsgraph/intern/builder/` | Dependency tracking |
| `intern/cycles/blender/sync.cpp` | Per-object override passing |
| `intern/cycles/blender/geometry.cpp` | Override resolution in `find_used_shaders()` |
| UI python scripts | Override pickers |

---

## Feature 4: Per-Light-Group Light Pass AOVs (Combined + Direct/Indirect)

**Branch:** `feature/per-lightgroup-lobe-passes`
**Difficulty:** Medium-High
**Status:** Phase 1 complete. Merged into `vfx-rendering-branch-github` and cherry-picked into
`vfx-rendering-branch` on 2026-03-16.
**Goal:** Render per-lightgroup light pass AOVs with **combined**, **direct**, and **indirect** components.
Foundation for future LPE support; combined light pass AOVs are **raw sums** (no albedo divide).

Progress snapshot (2026-03-09):
- Added KernelFilm per-lightgroup light pass AOV offsets and film allocation plumbing.
- Added kernel writes for per-lightgroup combined + direct/indirect light pass AOV accumulation.
- Added Cycles add-on properties/UI and pass registration/sync for per-lightgroup light pass AOVs.
- Updated UI and property naming from "Lobe Passes" to "Light Pass AOVs".
- Validation: full `blender` target build completed in `E:\blender_modify\build_lobe_passes`
  on 2026-03-09; next is functional render validation in Blender UI.

Progress snapshot (2026-03-11):
- Root-cause fix for incorrect direct/indirect channels: light-group direct/indirect
  diffuse/glossy/transmission/volume passes now disable divide/compositing and stay raw sums.
- Validation on `D:\blender_projects\light-passes-test-v001.blend` frame 3:
  `RGBA_env.rgb ~= diffuse+glossy+transmission+volume` and
  `RGBA_env.rgb ~= sum(direct+indirect)` within small floating-point tolerance.
- Full-resolution recheck (1920x1080, 32 samples, frame 5) confirms consistency with
  worst relative RGB diff < 0.001.

Progress snapshot (2026-03-12):
- Policy update: emission lightgroup contribution stays in `Combined_<lg>` and is not split into
  diffuse/glossy/transmission/volume light pass AOV channels.

Progress snapshot (2026-03-13):
- Medium optimization rollout for full Light Pass AOV detection/sync (not emission-only).
- `engine.py` now performs one classification pass with early-outs:
  - no lightgroups,
  - global split toggle off,
  - all lobe toggles off.
- Classification rule: split only for lightgroups used by LIGHT objects or active world; always
  keep `Combined_<lg>` for all groups.
- `sync.cpp` now precomputes `available_passes` once and only creates custom/split lightgroup passes
  when names exist in `RenderLayer.passes`, using set membership checks.
- Compositor/File Output stale data handling:
  - removed old Render Layers split-output retention,
  - stale split entries are pruned from `CompositorNodeOutputFile.file_output_items` (with socket
    fallback cleanup), preventing stale emissive split subimages in EXR.
- Validation:
  - strict `blender` + `install` build done in `E:\blender_modify\build_lobe_passes`,
  - `light-passes-test-v001.blend` emits `Combined_emissive` only for emissive group,
  - frame 9 EXR confirms no emissive split subimages remain.
  - follow-up code-review fixes validated on 2026-03-13:
    - sync split-pass logic converted to descriptor-table loop,
    - compositor split-output filter tightened with lightgroup-suffix validation,
    - Python world/node-tree fallback hardening,
    - direct-light split-write availability guards added.


Progress snapshot (2026-03-16):
- Integrated into both long-lived VFX branches:
  - `vfx-rendering-branch-github` at `da0b36c3c14`
  - `vfx-rendering-branch` via cherry-pick `af66efa870f`
- `feature/world-environment-fog` was fast-forwarded to the current VFX base so follow-up
  development can start from the Light Pass AOV-ready branch state.

Detailed specification and implementation plan:
- `.agent/FEATURE_4_LIGHTGROUP_LOBE_PASSES.md`

---

## Branch Strategy

```
vfx-rendering-branch (has deep EXR)
|-- feature/shadow-color
|-- feature/no-direct-lighting
|-- feature/collection-material-override
`-- feature/per-lightgroup-lobe-passes
```

Each feature branch is independently developed, tested, and merged back to the current VFX base
branch in use. Re-sync older branches before development if they predate the latest VFX integration
point.

---

## Feature 5: World Environment Fog (Arnold aiFog-like)

**Branch:** `feature/world-environment-fog`
**Difficulty:** Medium
**Status:** Branch created on 2026-03-09 and fast-forwarded to the latest
`vfx-rendering-branch-github` base on 2026-03-16. Implementation not started.
**Goal:** Add a world-shader fog/atmosphere control that behaves like Arnold鈥檚 `aiFog` shader
(environment fog driven by distance and optional height).

### Scope
- Fog applies in the world shader (environment), affecting rays that travel through empty space.
- Intended to be a single, artist-friendly control similar to `aiFog`.
- **Direct-light only**: no indirect contribution, no shadowing/occlusion evaluation (real-time goal).
- If possible, **split fog into the existing volume AOV**; `aiFog` outputs as combined emission.

### Parameters (TBD: confirm exact aiFog attribute list)
Minimum recommended set:
- Fog color.
- Density / intensity.
- Height / height falloff (optional, for ground fog).
- Start distance (optional, to keep near camera clear).
- Maximum distance (optional clamp).

### Implementation Steps

**Step 1: Node + UI**
- Add a new world-shader node (e.g., `Environment Fog`) in
  `source/blender/nodes/shader/nodes`.
- Expose parameters above with sensible defaults.
- Add tooltips matching Blender RNA style (infinitive verb, no trailing period).

**Step 2: RNA + Node Registration**
- Define sockets and RNA properties for the new node.
- Ensure it appears only in World shader context (not material by default).

**Step 3: Cycles Shader Translation**
- Map the node to a Cycles volume closure or dedicated fog closure.
- Files likely touched:
  - `intern/cycles/scene/shader_nodes.cpp` (or specific node file)
  - `intern/cycles/blender/shader.cpp` (node translation)
  - `intern/cycles/kernel/svm/svm_*` (SVM or OSL path)

**Step 4: Kernel Evaluation**
- Implement fog evaluation along world ray segments using **direct lighting only**.
- Do not evaluate indirect fog or fog shadows/occlusion (explicitly skip).
- If feasible, route fog into the existing volume AOV while still contributing to combined emission
  output. (Match Arnold `aiFog` behavior: combined emission output is acceptable if split is not
  viable.)
  - Any split should be optional and keep default behavior unchanged.

**Step 5: AOV Compatibility**
- Confirm fog contributes to existing volume passes.
- If needed, add dedicated fog pass in the future (not required for v1).

**Step 6: Versioning**
- New node does not require versioning (node is additive).

### Acceptance Criteria
- Fog visible in world-only scenes with no geometry.
- Fog affects reflections/indirect rays consistently.
- No regression in scenes without the node.
- Works on CPU and GPU paths.

## Verification Plan

For each feature:
1. **Build:** `cmake --build E:\blender_modify\build_windows_x64_vc17_Release --target blender --config Release`
2. **Functional test:** Render test scene with feature enabled, verify output
3. **Regression test:** Render without feature enabled, verify output matches baseline
4. **GPU test:** Verify OptiX/CUDA kernel compilation (kernel struct changes require recompile)
5. **Versioning test:** Open old .blend file, verify defaults are correct
6. **UI test:** Open Blender GUI, verify properties appear and are editable

## Dedicated LPE Plan (Phase 2)

Feature 4 establishes per-lightgroup lobe infrastructure, but a **full LPE system** is required
for finer material-component AOVs (Arnold-style workflows). Planned scope:

### LPE Grammar + Parsing
- Add an LPE parser (regex-like state machine) compatible with common renderers.
- Allow tokens for camera, light, emission, scatter, reflection/transmission, lobe labels.

### Path State Tracking
- Extend path state to carry LPE state ID(s).
- Integrate transitions on scatter/emission/light events (CPU + GPU).

### AOV Registration
- Define LPE-based AOVs as custom passes with user expressions.
- Support lightgroup + LPE intersections where applicable.

### UI
- UI panel for managing LPE AOVs and expressions.
- Validation feedback (parse errors, unsupported tokens).

### Performance + Memory
- Cache compiled LPE automata.
- Limit maximum number of LPE AOVs; expose memory cost.

### Compatibility
- Preserve existing lightgroup and light pass AOV behavior.
- LPE AOVs are additive, not breaking older files.
