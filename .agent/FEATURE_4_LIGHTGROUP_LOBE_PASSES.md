# Feature 4: Per-Light-Group Lobe AOVs (Combined + Direct/Indirect)

Branch: `feature/per-lightgroup-lobe-passes`  
Difficulty: Medium-High  
Goal: Render per-lightgroup lobe passes with combined, direct, and indirect components.
Foundation for future LPE support; combined lobe passes are raw sums (no albedo divide).

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

## Architecture Design

Current state: Light groups only get a combined pass (`pass_lightgroup` in KernelFilm). Lobe passes
exist but only as global (not per-lightgroup).

Approach: Use existing PassType entries and tag them with `lightgroup`. Add KernelFilm offsets for
each lightgroup lobe pass (combined + direct/indirect). Avoid new PassType enum values.

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
- Per-lightgroup lobe AOVs (combined + direct + indirect) render correctly on CPU and GPU.
- Labels exist in data structures (light/material/lobe), even if not user-editable.
- Event hooks exist in integrator code paths (even if they are no-ops for now).
- No regressions in existing passes when lightgroup lobe passes are disabled.

Phase 2 can start when:
- Phase 1 is stable and validated in production-like scenes.
- Label plumbing is validated (no collisions, stable indices, predictable naming).
- AOV accumulation entry points are centralized and testable.

Phase 2 is complete when:
- Arbitrary LPE expressions parse and validate with clear errors.
- LPE AOVs render correctly on CPU and GPU for surfaces, volumes, emission, and background.
- LPE AOVs coexist with lightgroup lobe passes without breaking older files.

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

Step 2: PassInfo override for lightgroup combined
- File: `intern/cycles/scene/pass.cpp`
- For lightgroup passes of type `PASS_DIFFUSE`, `PASS_GLOSSY`, `PASS_TRANSMISSION`, `PASS_VOLUME`:
  - Force `is_written = true`, `divide_type = PASS_NONE`, `direct_type = PASS_NONE`,
    `indirect_type = PASS_NONE`, `use_compositing = false`.
- Ensures combined lightgroup lobes are raw sums, not derived via divide or D/I aggregation.

Step 3: Film allocation
- File: `intern/cycles/scene/film.cpp`
- Initialize all new offsets to `PASS_UNUSED`.
- When `pass->get_lightgroup()` is non-empty:
  - Route by `pass->get_type()` to the correct lightgroup offset (combined or direct/indirect).
  - Increment `pass_stride` by `pass->get_info().num_components`.

Step 4: Kernel writes
- File: `intern/cycles/kernel/film/light_passes.h`
- `film_write_direct_light()`:
  - When lightgroup valid, write to combined lobe offsets and to direct/indirect offsets.
- `film_write_emission_or_background_pass()`:
  - Include emission/background contributions in combined + direct/indirect lobe passes.

Step 5: Cycles add-on properties
- File: `intern/cycles/blender/addon/properties.py`
- Add a global enable + per-lobe combined/direct/indirect toggles.
- Add per-lobe "all" convenience properties (computed, no storage).

Step 6: UI
- File: `intern/cycles/blender/addon/ui.py`
- Add a Light Groups sub-panel for lobe passes:
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
| `intern/cycles/kernel/data_template.h` | KernelFilm lobe pass offsets |
| `intern/cycles/scene/pass.cpp` | Lightgroup combined PassInfo override |
| `intern/cycles/scene/film.cpp` | Buffer allocation |
| `intern/cycles/kernel/film/light_passes.h` | Per-LG lobe writes |
| `intern/cycles/blender/addon/properties.py` | Toggles |
| `intern/cycles/blender/addon/ui.py` | UI |
| `intern/cycles/blender/addon/engine.py` | Pass registration |
| `intern/cycles/blender/sync.cpp` | Pass registration + expected_passes |

