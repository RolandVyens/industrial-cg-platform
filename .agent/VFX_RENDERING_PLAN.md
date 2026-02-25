# VFX Rendering Branch - Development Plan

## Context

The vfx-rendering-branch already has deep EXR support merged. We're planning the next set of VFX rendering features, each implemented as a separate feature branch based off vfx-rendering-branch.

## Active Optimization: Deep EXR Memory Efficiency (task/deep-exr-memory)

**Goal:** Reduce RAM/VRAM usage for Deep EXR renders by clamping tile size based on a user-controlled
deep buffer budget (default 1024 MB) and skipping RenderResult deep storage when the compositor does
not need it.

### Implementation Notes
- Add shared deep buffer byte estimation helper (used by deep output driver + session tiling).
- Clamp auto tile size when `film.use_deep_output` is on and deep buffers exceed budget.
- Budget: user setting `deep_tile_budget_mb` (default 1024 MB), per device, 0 disables clamp.
- Skip `RenderResult.deep_data` population unless compositor has a Deep EXR File Output node.

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

## Feature 4: Per-Light-Group Lobe AOVs (Combined + Direct/Indirect)

**Branch:** `feature/per-lightgroup-lobe-passes`
**Difficulty:** Medium-High
**Goal:** Render per-lightgroup lobe passes with **combined**, **direct**, and **indirect** components.
Foundation for future LPE support; combined lobe passes are **raw sums** (no albedo divide).

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

Each feature branch is independently developed, tested, and merged back to vfx-rendering-branch.

---

## Feature 5: World Environment Fog (Arnold aiFog-like)

**Branch:** `feature/world-environment-fog`
**Difficulty:** Medium
**Goal:** Add a world-shader fog/atmosphere control that behaves like Arnold’s `aiFog` shader
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
- Preserve existing lightgroup and lobe pass behavior.
- LPE AOVs are additive, not breaking older files.
