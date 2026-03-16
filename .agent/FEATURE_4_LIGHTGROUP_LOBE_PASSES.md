# Feature 4: Per-Light-Group Light Pass AOVs (Combined + Direct/Indirect)

Branch: `feature/per-lightgroup-lobe-passes`  
Difficulty: Medium-High  
Goal: Render per-lightgroup light pass AOVs with combined, direct, and indirect components.
Foundation for future LPE support; combined light pass AOVs are raw sums (no albedo divide).

## Medium Optimization Rollout (2026-03-13)

- Whole-path optimization now covers both detection and sync, not just emissive-group filtering.
- Source-based split eligibility rule:
  - **Splittable** when a lightgroup is referenced by at least one LIGHT object or active world.
  - **Combined-only** otherwise.
- Registration behavior:
  - Always register `Combined_<lg>`.
  - Register split light pass AOVs only for splittable groups.
- Early-outs in pass listing:
  - no lightgroups,
  - global split toggle off,
  - all lobe toggles off.
- Sync alignment:
  - precompute `available_pass_names` from `RenderLayer.passes`,
  - create custom/split passes only when names are available, via set membership checks.
- Kernel policy remains stable in this phase:
  - emission contribution stays in `Combined_<lg>` only (no emission split channels).
- Compositor/file-output compatibility:
  - stale lightgroup split outputs are not retained on Render Layers node,
  - stale split entries are pruned from `CompositorNodeOutputFile.file_output_items`
    (with socket fallback cleanup) so EXR outputs do not keep removed split channels.

### Code review follow-up updates (2026-03-13)

- `sync.cpp` split-pass creation is now data-driven via a descriptor table
  (`property_name` + `pass_name_format` + `PassType`) instead of 12 copy-pasted blocks.
- Split-output preservation filter in compositor now validates `{prefix}{lightgroup}` against
  actual view-layer lightgroup names, reducing false positives for unrelated similarly named outputs.
- Python compatibility hardening:
  - use `getattr(view_layer, "world_override", None)` fallback,
  - fallback to `scene.node_tree` when `scene.compositing_node_group` is unavailable.
- Direct-light kernel path now checks whether any related lightgroup split offsets are available
  before issuing split writes, reducing redundant PASS_UNUSED checks when split passes are disabled.

## Phased Strategy (LPE-Ready)

Phase 1: Fixed lobe + lightgroup passes (this feature)
- Implement combined/direct/indirect per-lobe lightgroup AOVs with the naming scheme below.
- Add LPE-ready label plumbing now (light labels, material/lobe labels) so Phase 2 reuses them.
- Add LPE-ready event hooks in the integrator (camera, scatter, light hit, emission, volume),
  even if Phase 1 only consumes a subset.

Phase 2: Full arbitrary LPE syntax
- Add parser + state machine + validation.
- Reuse Phase 1 labels + event hooks, swap hardcoded lobe/lightgroup matching for LPE evaluation.
- See the "Dedicated LPE Plan (Phase 2)" section in `VFX_RENDERING_PLAN.md`.

## New passes per lightgroup (current naming convention to use)
- Combined: `diffuse_<lg>`, `glossy_<lg>`, `transmission_<lg>`, `volume_<lg>`
- Direct: `diffuse_direct_<lg>`, `glossy_direct_<lg>`, `transmission_direct_<lg>`,
  `volume_direct_<lg>`
- Indirect: `diffuse_indirect_<lg>`, `glossy_indirect_<lg>`, `transmission_indirect_<lg>`,
  `volume_indirect_<lg>`

## Pass formulas (RGB, per pixel, per lightgroup `<lg>`)

- `diffuse_<lg> = diffuse_direct_<lg> + diffuse_indirect_<lg>`
- `glossy_<lg> = glossy_direct_<lg> + glossy_indirect_<lg>`
- `transmission_<lg> = transmission_direct_<lg> + transmission_indirect_<lg>`
- `volume_<lg> = volume_direct_<lg> + volume_indirect_<lg>`

- Base lobe identity:
  - `diffuse_<lg> + glossy_<lg> + transmission_<lg> + volume_<lg>`
  - `= sum(all 8 direct/indirect lobe channels)`

- For non-emission contributions, `Combined_<lg>.rgb` matches the lobe sums above.
- Emission contribution policy (current decision):
  - emission lightgroup energy is accumulated in `Combined_<lg>.rgb`,
  - and is **not** split into diffuse/glossy/transmission/volume light pass AOV channels.

Notes:
- These equalities are for **RGB**. Alpha channels are not additive light-energy channels.
- Values are raw sums (no albedo divide/compositing).

## Ray-level accumulation formulas (Phase 1 implementation)

Notation (per sample, per pixel, per lightgroup `<lg>`):
- `C`: spectral light contribution carried by the current ray event (after throughput/MIS/shadowing).
- `w_d`: diffuse split weight (`pass_diffuse_weight`).
- `w_g`: glossy split weight (`pass_glossy_weight`).
- `w_t = 1 - w_d - w_g`: transmission split weight.
- `C_d = w_d * C`, `C_g = w_g * C`, `C_t = w_t * C`.
- Surface direct/indirect classifier:
  - Shadow-ray path (`film_write_direct_light`): direct iff `bounce == 0`.
  - Path-state emission/background (`film_write_emission_or_background_pass`): direct iff `bounce == 1`.

### 1) Direct light event on a surface path

When path flag contains `PATH_RAY_SURFACE_PASS`:
- `Combined_<lg>.rgb += C`
- `diffuse_<lg> += C_d`
- `glossy_<lg> += C_g`
- `transmission_<lg> += C_t`

Direct case:
- `diffuse_direct_<lg> += C_d`
- `glossy_direct_<lg> += C_g`
- `transmission_direct_<lg> += C_t`

Indirect case:
- `diffuse_indirect_<lg> += C_d`
- `glossy_indirect_<lg> += C_g`
- `transmission_indirect_<lg> += C_t`

### 2) Direct light event on a volume path

When path flag contains `PATH_RAY_VOLUME_PASS`:
- `Combined_<lg>.rgb += C`
- `volume_<lg> += C`

Direct case:
- `volume_direct_<lg> += C`

Indirect case:
- `volume_indirect_<lg> += C`

### 3) Background event seen through prior scattering

In `film_write_emission_or_background_pass` for background pass (`pass_background`),
when `PATH_RAY_ANY_PASS` is set:
- Surface case (`PATH_RAY_SURFACE_PASS`): uses the same split as section (1).
- Volume case (`PATH_RAY_VOLUME_PASS`): uses the same split as section (2).

### 4) Emission event (current policy: no lobe split)

For emission pass (`pass_emission`) in `film_write_emission_or_background_pass`:
- `Combined_<lg>.rgb += C`
- No diffuse/glossy/transmission/volume light pass AOV write for this event.

### 5) Directly visible background (camera-visible, no prior pass path)

In `film_write_emission_or_background_pass` for background, when `PATH_RAY_ANY_PASS` is not set:
- `Combined_<lg>.rgb += C`
- No lobe split (`diffuse/glossy/transmission/volume`) is written for this event in Phase 1.

### 6) Exclusions / guard conditions

- Shadow catcher paths (`PATH_RAY_SHADOW_CATCHER_HIT`) skip light pass writes.
- Invalid lightgroup (`LIGHTGROUP_NONE`) writes nothing for lightgroup passes.
- All equations above are RGB energy equations; alpha channels are separate and non-additive.

## Architecture Design

Current state: Light groups only get a combined pass (`pass_lightgroup` in KernelFilm). Light pass AOVs
exist but only as global (not per-lightgroup).

Approach: Use existing PassType entries and tag them with `lightgroup`. Add KernelFilm offsets for
each lightgroup light pass AOV (combined + direct/indirect). Avoid new PassType enum values.

## LPE-Ready Plumbing (Phase 1)
- Reserve label ID plumbing for lights/materials/lobes (even if not yet exposed in UI).
- Ensure integrator path state can carry lightweight LPE state IDs (placeholders OK in Phase 1).
- Keep the accumulation API flexible so Phase 2 can swap matching logic without rewiring AOV storage.

## Phase 1 LPE-Ready Checklist
- Define light label IDs (per light) and store them in Cycles light data.
- Define material label IDs + lobe label IDs (global, stable indices).
- Encode material/lobe labels onto lobes during BSDF construction or shader eval.
- Add event transition hooks: camera start, surface scatter, light hit, emission, volume scatter.
- Extend path state to carry LPE state ID (even if Phase 1 keeps it at default/unused).
- Keep AOV accumulation entry points centralized so Phase 2 can plug in LPE matching.

## Acceptance Criteria (Phase 1 vs Phase 2)

Phase 1 is complete when:
- Per-lightgroup light pass AOVs (combined + direct + indirect) render correctly on CPU and GPU.
- For a given lightgroup, `rgba_<lg>.rgb` matches both
  `diffuse_<lg>+glossy_<lg>+transmission_<lg>+volume_<lg>` and the sum of all
  direct+indirect light pass AOV RGB channels within normal floating-point tolerance.
- Labels exist in data structures (light/material/lobe), even if not user-editable.
- Event hooks exist in integrator code paths (even if they are no-ops for now).
- No regressions in existing passes when per-lightgroup light pass AOVs are disabled.

Phase 2 can start when:
- Phase 1 is stable and validated in production-like scenes.
- Label plumbing is validated (no collisions, stable indices, predictable naming).
- AOV accumulation entry points are centralized and testable.

Phase 2 is complete when:
- Arbitrary LPE expressions parse and validate with clear errors.
- LPE AOVs render correctly on CPU and GPU for surfaces, volumes, emission, and background.
- LPE AOVs coexist with per-lightgroup light pass AOVs without breaking older files.

## Implementation Steps

Step 1: KernelFilm offsets
- File: `intern/cycles/kernel/data_template.h` (and `kernel/types.h` via template)
- Add offsets:
  - Combined: `pass_lightgroup_diffuse`, `pass_lightgroup_glossy`,
    `pass_lightgroup_transmission`, `pass_lightgroup_volume`
  - Direct/Indirect: `pass_lightgroup_diffuse_direct`, `pass_lightgroup_diffuse_indirect`,
    `pass_lightgroup_glossy_direct`, `pass_lightgroup_glossy_indirect`,
    `pass_lightgroup_transmission_direct`, `pass_lightgroup_transmission_indirect`,
    `pass_lightgroup_volume_direct`, `pass_lightgroup_volume_indirect`
- Each stores `PASS_UNUSED` or an offset. Each lightgroup occupies 3 floats (RGB) per pass.

Step 2: PassInfo override for lightgroup raw-sum passes
- File: `intern/cycles/scene/pass.cpp`
- For lightgroup passes of type:
  - Combined: `PASS_DIFFUSE`, `PASS_GLOSSY`, `PASS_TRANSMISSION`, `PASS_VOLUME`
  - Direct/Indirect: `PASS_DIFFUSE_DIRECT`, `PASS_DIFFUSE_INDIRECT`,
    `PASS_GLOSSY_DIRECT`, `PASS_GLOSSY_INDIRECT`, `PASS_TRANSMISSION_DIRECT`,
    `PASS_TRANSMISSION_INDIRECT`, `PASS_VOLUME_DIRECT`, `PASS_VOLUME_INDIRECT`
  force:
  - Force `is_written = true`, `divide_type = PASS_NONE`, `direct_type = PASS_NONE`,
    `indirect_type = PASS_NONE`, `use_compositing = false`.
- Ensures per-lightgroup light pass AOVs remain raw sums, not derived via albedo divide/compositing.

Step 3: Film allocation
- File: `intern/cycles/scene/film.cpp`
- Initialize all new offsets to `PASS_UNUSED`.
- When `pass->get_lightgroup()` is non-empty:
  - Route by `pass->get_type()` to the correct lightgroup offset (combined or direct/indirect).
  - Increment `pass_stride` by `pass->get_info().num_components`.

Step 4: Kernel writes
- File: `intern/cycles/kernel/film/light_passes.h`
- `film_write_direct_light()`:
  - When lightgroup valid, write to combined offsets and to direct/indirect offsets.
- `film_write_emission_or_background_pass()`:
  - Include emission/background contributions in combined + direct/indirect light pass AOVs.

Step 5: Cycles add-on properties
- File: `intern/cycles/blender/addon/properties.py`
- Add a global enable + per-lobe combined/direct/indirect light pass AOV toggles.
- Add per-lobe "all" convenience properties (computed, no storage).

Step 6: UI
- File: `intern/cycles/blender/addon/ui.py`
- Add a Light Groups sub-panel for light pass AOVs:
  - Rows per lobe with `All`, `Combined`, `Direct`, `Indirect` toggles.
  - Disable when global toggle is off.
  - Add memory cost note.

Step 7: Pass registration
- File: `intern/cycles/blender/addon/engine.py`
- When enabled, register per-lightgroup passes with the naming scheme above.

Step 8: Blender sync
- File: `intern/cycles/blender/sync.cpp`
- Create lightgroup passes directly with `pass->set_lightgroup(...)` and add names to
  `expected_passes` so they are not treated as unknown.

## Memory Considerations
With N lightgroups x 12 passes x 3 channels = 36N floats per pixel.
Document in UI tooltips.

## Key Files
| File | Change |
|------|--------|
| `intern/cycles/kernel/data_template.h` | KernelFilm light pass AOV offsets |
| `intern/cycles/scene/pass.cpp` | Lightgroup combined PassInfo override |
| `intern/cycles/scene/film.cpp` | Buffer allocation |
| `intern/cycles/kernel/film/light_passes.h` | Per-LG light pass AOV writes |
| `intern/cycles/blender/addon/properties.py` | Toggles |
| `intern/cycles/blender/addon/ui.py` | UI |
| `intern/cycles/blender/addon/engine.py` | Pass registration |
| `intern/cycles/blender/sync.cpp` | Pass registration + expected_passes |

