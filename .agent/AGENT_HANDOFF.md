# VFX Rendering Branch - Agent Handoff

> **Branch:** `vfx-rendering-branch-github`
> **Base:** Blender `main` (5.2 dev)
<<<<<<< HEAD
> **Last Updated:** 2026-03-19
=======
> **Last Updated:** 2026-03-19
>>>>>>> c0889eaf738 (Cycles: fix GPU lightgroup split-pass indexing)

---

## Current Status

- **Deep EXR:** Merged and complete. Code reviewed, all critical/high issues fixed.
- **Deep EXR memory:** Implemented user-controlled budget (default 1024 MB) + tile clamp; RenderResult deep storage skipped unless compositor needs it. Added tiled deep accumulation to avoid last-tile-only deep outputs. Merged into `vfx-rendering-branch` on 2026-02-22; code review checklist resolved.
- **Validation:** Rendered `D:\blender_projects\deep-branch-test.blend` on 2026-02-22; deep compositor outputs saved and tile rendering confirmed.
  Compositor deep EXRs now large (e.g., `test_compoutput_deep0001.exr` ~584 MB).
<<<<<<< HEAD
- **Deep EXR edge alpha fix (2026-03-16):** Root cause traced to hard-surface deep samples being
  stored with opaque alpha and later normalized only against flattened beauty alpha, which cannot
  recover per-depth edge coverage at foreground/background intersections. The fix preserves opaque
  surface duplicates through deep merge and reconstructs front-to-back conditional alpha from the
  per-pixel deep hit distribution before Deep Recolor/output.
- **Deep EXR edge alpha validation (2026-03-16):**
  - CPU render re-run with `--factory-startup` on `D:\blender_projects\deep-branch-test.blend`.
  - `.agent/check_deep_surface_front_alpha.py C:\tmp\surface_compoutput_deep0001.exr` now reports
    `violating_front_alpha_pixels=0` (was `317109` before the fix).
  - Example fixed pixel `(574, 150)` now starts with front alpha `0.0625` instead of `1.0`.
- **Deep EXR single-surface AA fix (2026-03-16):**
  - Remaining seam/AA failures were traced to pixels that still collapse to a single visible deep
    surface after grouping, but arrive as multiple same-depth opaque duplicates before export.
  - Deep output now uses the flattened beauty alpha directly for that single-group case, while
    multi-depth hard-surface pixels still use internal `PASS_SAMPLE_COUNT` capture for
    front-to-back conditional alpha reconstruction.
  - Validation on `D:\blender_projects\deep-branch-test.blend` frame 1:
    - `.agent/check_deep_single_surface_alpha.py C:\tmp\surface_compoutput_flat0001.exr C:\tmp\surface_compoutput_deep0001.exr`
      reports `checked_single_surface_fractional_pixels=6657`,
      `mismatching_single_surface_pixels=0`.
    - `.agent/check_deep_surface_front_alpha.py C:\tmp\surface_compoutput_deep0001.exr` remains
      clean with `violating_front_alpha_pixels=0`.
- **Deep EXR cleanup sweep (2026-03-17):**
  - Follow-up implementation worktree: `E:\blender_modify\blender_deep_exr_fix`
    on branch `feature/deep-exr-edge-alpha-fix`.
  - Accepted review items: mixed-EOL normalization across all modified tracked files, deep sample
    offset widening to `size_t`, cache pixel-count overflow cleanup, duplicate pixel-population
    helper removal, early fast-path allocation cleanup, mutable accessor rename cleanup, and
    declaration/definition signature match for `deep_compute_buffer_bytes()`.
  - Rejected/non-actioned review items: the `kg` “unused parameter” note and the self-corrected
    bounds-check note.
  - Fresh verification on 2026-03-17:
    - `cmake --build E:\blender_modify\build_deep_exr_fix --target blender --config Release -- /m:28`
      succeeded.
    - CPU/factory-startup render of `D:\blender_projects\deep-branch-test.blend` frame 1 completed
      and wrote updated deep/flat EXRs in `C:\tmp\` (PowerShell surfaced OpenColorIO warnings on
      stderr, but Blender completed and saved outputs).
    - `.agent/check_deep_single_surface_alpha.py` reports
      `checked_single_surface_fractional_pixels=6657`,
      `mismatching_single_surface_pixels=0`.
    - `.agent/check_deep_surface_front_alpha.py` reports `multi_sample_pixels=39349`,
      `violating_front_alpha_pixels=0`.
- **VFX features:** Feature 1 complete (Per-Light Shadow Color) and merged. Feature 4 Phase 1 is
  now merged into both VFX branches.
=======
- **VFX features:** Feature 1 complete (Per-Light Shadow Color) and merged. Feature 4 Phase 1 is
  complete on this branch, with the 2026-03-19 GPU split-pass corruption fix now ported here.
>>>>>>> c0889eaf738 (Cycles: fix GPU lightgroup split-pass indexing)
- **Working branch:** `vfx-rendering-branch-github`
- **Feature 4:** Phase 1 implementation completed on `feature/per-lightgroup-lobe-passes`.
- **Branch sync:** `feature/per-lightgroup-lobe-passes` re-synced to `vfx-rendering-branch-github` on 2026-03-09 after history rewrite. Backup branch: `backup/feature-per-lightgroup-lobe-passes-pre-resync-20260309`.
- **Validation (Feature 4 WIP):** `python -m py_compile` passed for Cycles add-on files; full `blender` target build completed in `E:\blender_modify\build_lobe_passes` on 2026-03-09 (`blender.exe` timestamp 14:05:21).
- **Runtime package:** `install` target run for `build_lobe_passes` on 2026-03-09 to populate `bin\Release\5.2` scripts/runtime files. Verified `ViewLayer.cycles.use_lightgroup_light_pass_aovs` is present in background Python check.
- **Build:** Followed `.agent/workflows/build-blender.md` incremental build command on 2026-03-09; output `E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe` updated (timestamp 14:12:01).
- **Branch setup:** Created `feature/world-environment-fog` worktree at `E:\blender_modify\blender_env_fog` on 2026-03-09 from `vfx-rendering-branch-github`.
- **Naming update:** Feature 4 UI/props wording updated from "Lobe Passes" to "Light Pass AOVs" on 2026-03-09.
- **Feature 4 fix (2026-03-11):** Light-group direct/indirect diffuse/glossy/transmission/volume
  pass metadata now forces raw-sum behavior (`divide_type = PASS_NONE`,
  `use_compositing = false`) to fix over-bright direct/indirect channels.
- **Feature 4 validation (2026-03-11):** Using
  `D:\blender_projects\light-passes-test-v001.blend` frame 3 output EXRs,
  `RGBA_env.rgb ~= diffuse+glossy+transmission+volume` and
  `RGBA_env.rgb ~= sum(direct+indirect)` within small floating-point tolerance
  (RGB max diff ~0.0029-0.0070 across tested view layers). Alpha channels are not additive.
- **Feature 4 full-res validation (2026-03-11):** Re-ran at 1920x1080, 32 samples (frame 5).
  RGB equations remain consistent; worst absolute diff ~0.0588 and worst relative diff <0.001.
- **Feature 4 policy update (2026-03-12):** Emission lightgroup contributions are kept in
  `Combined_<lg>` only and are no longer split into diffuse/glossy/transmission/volume
  light pass AOV channels.
- **Validation (2026-03-12):** Rebuilt `build_lobe_passes` (`bf_intern_cycles` + `blender`) and
  rendered `D:\blender_projects\light-passes-test-v001.blend` frame 6 successfully.
- **Feature 4 medium optimization rollout (2026-03-13):**
  - `engine.py`: single-pass lightgroup classification + global early-outs for full Light Pass AOV
    detection path.
  - Split eligibility rule: only lightgroups used by LIGHT objects or active world are splittable.
    All groups always keep `Combined_<lg>`.
  - `sync.cpp`: precomputed `available_passes`, set-based pass gating, and split-pass sync aligned
    strictly to registered pass names.
  - `node_composite_render_layers.cc`: removed legacy preservation for deleted lightgroup split
    sockets.
  - File Output stale split cleanup now removes stale `file_output_items` (and fallback sockets),
    triggered during pass registration and render update.
- **Feature 4 validation (2026-03-13):**
  - Strict build + install completed in `E:\blender_modify\build_lobe_passes`.
  - `check_registered_passes.py`: emissive group only registers `Combined_emissive`; splittable
    env/key groups keep full split channels.
  - Single-render EXR check (frame 9): emissive subimages contain only `RGBA_emissive`
    (no emissive split subimages remain).
  - `update_render_passes` micro-benchmark confirms early-outs:
    `split_off` ~0.061 ms, `split_on_lobes_off` ~0.067 ms, `split_on_lobes_on` ~0.137 ms.
- **Feature 4 review follow-up (2026-03-13):**
  - `sync.cpp`: replaced 12 duplicated split-pass blocks with a descriptor table loop and enabled
    pass list, reducing maintenance risk and avoiding unnecessary RNA reads when global toggle is off.
  - `node_composite_render_layers.cc`: tightened split-socket identification to require a known
    lightgroup suffix, reducing false positives for similarly named non-lightgroup outputs.
  - `engine.py`: added compatibility fallbacks for `scene.node_tree` and missing
    `view_layer.world_override`.
  - `light_passes.h`: added pass-availability guards before lightgroup split writes in direct-light
    paths to reduce redundant per-sample checks when splits are disabled.
  - Validation re-run: `blender` + `install` builds succeed; pass registration and EXR emissive
    channel checks remain correct.
- **Feature 4 branch integration (2026-03-16):**
  - Merged into `vfx-rendering-branch-github` at `da0b36c3c14`.
  - Cherry-picked into `vfx-rendering-branch` at `af66efa870f` to keep the non-GitHub branch in
    parity without unrelated-history merge noise.
- **Branch prep (2026-03-16):**
  - `feature/world-environment-fog` fast-forwarded from `1cf4166c5f3` to `da0b36c3c14` so future
    work starts from the current Light Pass AOV base.
  - `feature/no-direct-lighting` and `feature/collection-material-override` still need re-sync
    before development resumes.
- **Feature 4 GPU hole investigation (2026-03-19):**
  - `D:\blender_projects\light-passes-test-v001.blend` reproduces a large flat-beauty alpha hole on `af66efa870f` when rendered on GPU/CUDA, even with compositor Deep EXR output disabled.
  - Controlled background render evidence on `E:\blender_modify\build_af66_origin\bin\Release\blender.exe`:
    - CPU flat (`C:\tmp\af66_cpu_flat_0002.exr`) vs GPU flat (`C:\tmp\af66_gpu_cuda_flat_0002.exr`): `alpha_max_diff=1.0`, `alpha_diff_pixels_gt_0.01=216555`; worst sampled pixel `(851, 344)` is `alpha 1.0 -> 0.0`.
    - GPU with `ViewLayer.cycles.use_lightgroup_light_pass_aovs=False` (`C:\tmp\af66_gpu_cuda_flat_aovs_off_0002.exr`) collapses the regression to only 7 small alpha diffs (`alpha_max_diff=0.03125`).
    - GPU with the emissive mesh lightgroup cleared (`Cone.lightgroup=''`) while keeping split light pass AOVs enabled (`C:\tmp\af66_gpu_cuda_flat_cone_nolg_0002.exr`) also collapses the regression to the same small residual diffs (`alpha_max_diff=0.03125`).
  - Root cause was later confirmed: Feature 4 split lightgroup pass allocation skips combined-only emissive mesh lightgroups, but GPU split-lobe write sites were still indexing dense split storage with the logical global lightgroup index, corrupting adjacent film-pass data.
- **Feature 4 GPU split-pass corruption fix (2026-03-19):**
  - Fix implemented on `vfx-rendering-branch-github` with an internal dense remap `logical_lightgroup_index -> split_lightgroup_index` stored in `DeviceScene` / `KernelFilm`.
  - Combined lightgroup writes still use the original logical lightgroup index, so `Combined_<lg>` behavior is unchanged.
  - Split lobe writes now go through a dedicated remapped helper and return early for combined-only lightgroups (`-1` remap), preventing out-of-bounds/dense-pack corruption on GPU.
  - Files touched for the fix:
    - `intern/cycles/scene/devicescene.h`
    - `intern/cycles/scene/devicescene.cpp`
    - `intern/cycles/kernel/data_template.h`
    - `intern/cycles/kernel/film/light_passes.h`
    - `intern/cycles/scene/film.cpp`
  - Verification:
    - Incremental build succeeded in `E:\blender_modify\build_windows_x64_vc17_Release`.
    - `install` target re-run was required so updated GPU kernel/runtime files were copied into `bin\Release\5.2`; the earlier CUDA illegal-address symptom was stale runtime state, not a new logic regression.
    - Fresh CPU/GPU flat renders on `D:\blender_projects\light-passes-test-v001.blend` frame 2:
      - `C:\tmp\feature4_fix_cpu_flat_0002.exr`
      - `C:\tmp\feature4_fix_gpu_flat_0002.exr`
      - `.agent/check_feature4_gpu_flat_alpha_hole.py` now reports `hole_pixel_count=0`, `alpha_diff_pixels_gt_0.01=7`, `alpha_max_diff=0.03125`.
    - Fresh GPU compositor multilayer output:
      - `C:\tmp\feature4_gpu_comp\rgba\feature4_gpu_rgba_0002.exr`
      - `.agent/check_feature4_lightgroup_subimages.py` reports 28 required subimages present, `RGBA_emissive` preserved, no emissive split subimages, and all env/key split subimages remain non-zero.
  - Deep EXR was ruled out as the primary cause of the flat-beauty hole; this was a film-pass indexing bug in Feature 4 GPU split writes.
- **Docs:** MoonRay LPE/AOV code report added at `.agent/MOONRAY_LPE_REPORT.md` (Cycles LPE reference).
- **Docs:** Added Phase 1/Phase 2 LPE-ready strategy notes to Feature 4 in `VFX_RENDERING_PLAN.md`.
- **Docs:** Added Phase 1 LPE-ready checklist under Feature 4 in `VFX_RENDERING_PLAN.md`.
- **Docs:** Added Phase 1/Phase 2 acceptance criteria under Feature 4 in `VFX_RENDERING_PLAN.md`.
- **Docs:** Added explicit Light Pass AOV formula/equation list in
  `.agent/FEATURE_4_LIGHTGROUP_LOBE_PASSES.md`.
- **Docs:** Added ray-event-level accumulation formulas and edge-case notes
  (surface/volume, direct/indirect, camera-visible background/emission) in
  `.agent/FEATURE_4_LIGHTGROUP_LOBE_PASSES.md`.
- **Docs:** Added Feature 5 (World Environment Fog, aiFog-like) to `VFX_RENDERING_PLAN.md`.
- **Docs:** Moved Feature 4 details into `.agent/FEATURE_4_LIGHTGROUP_LOBE_PASSES.md` and left link in `VFX_RENDERING_PLAN.md`.
- **Docs:** Feature 5 scope clarified: environment fog is direct-light only (no indirect/shadows), and optional fog AOV split should reuse volume AOV.
- **Docs:** Updated README and README.zh-CN roadmap to include world environment fog.
- **Docs:** Feature 3 note added: must handle linked/library data and library overrides.
- **Docs:** GitHub is now the primary repo; Blender Projects is not maintained (see `GITHUB_MANAGEMENT.md`).
- **GitHub mirror:** `vfx-rendering-branch` snapshot updated on 2026-02-22 (force-push, single snapshot).
- **Docs:** Root README trimmed to features + roadmap (EN/CN). Removed `README_VFX.md` and `README_VFX.zh-CN.md`.
- **Release package:** Installed Release to `E:\blender_modify\release\blender-vfx-5.2-2026-02-22` (no PDBs) and zipped to `E:\blender_modify\release\blender-vfx-5.2-2026-02-22.zip` on 2026-02-22; includes multi-arch CUDA kernels (`sm_75`, `sm_86`, `sm_89`).
- **Release published:** GitHub release created on 2026-02-22 with tag/title `blender-vfx-5.2-2026-02-22` and asset `blender-vfx-5.2-2026-02-22.zip`.
- **Release package:** Installed Release to `E:\blender_modify\release\blender-vfx-5.2-2026-03-04` and zipped to `E:\blender_modify\release\blender-vfx-5.2-2026-03-04.zip` on 2026-03-04.
- **Release published:** GitHub release updated on 2026-03-04 with tag/title `blender-vfx-5.2-2026-03-04` and asset `blender-vfx-5.2-2026-03-04.zip`.
- **Release checksum:** `SHA256 9CD99213DD1E1FA459A4981E6F20BFAE5BC569C3252AD416A95017270E5920F3`.
- **Release scope docs:** `AGENT.md` + `GITHUB_MANAGEMENT.md` now explicitly document where compiled Blender and test project live locally, and why they are not included in GitHub release assets.
- **Release notes policy:** Release notes should be written in Markdown.
- **History:** Doc commits on feature/shadow-color squashed; vfx-rendering-branch history rewritten on 2026-02-22. Force-pushed to git.blender.org and GitHub snapshot.
- **GitHub management:** Workflow documented in `.agent/GITHUB_MANAGEMENT.md` (snapshot mirroring + release via UI/gh).

---

## Completed: Deep EXR Output (Merged)

Deep EXR per-pixel depth samples for VFX compositing (Nuke workflow). Merged into vfx-rendering-branch on 2026-02-18.

### What It Does
- New `DEEP_EXR` file format in Blender (RGBA/Z/ZBack per sample)
- Direct output (render format) and compositor File Output node
- Per-pixel deep sample merging with configurable depth/alpha tolerances
- Deep Recolor: premultiplied beauty RGB associated per-sample
- Multi-device support (CPU + OptiX with rebalancing)
- Multi-view guard (deep EXR disabled with multi-view)
- Per-view-layer deep data passthrough in compositor

### Key Files (Deep EXR)
| File | Purpose |
|------|---------|
| `intern/cycles/kernel/film/deep_write.h` | Kernel deep sample write |
| `intern/cycles/session/deep_output_driver.h/cpp` | Deep export processing |
| `intern/cycles/session/deep_buffers.h/cpp` | Per-device deep buffer management |
| `intern/cycles/blender/session.cpp` | Deep driver setup + RenderResult storage |
| `source/blender/render/intern/pipeline.cc` | Deep data to compositor |
| `source/blender/nodes/composite/nodes/node_composite_file_output.cc` | Compositor deep EXR write |
| `source/blender/imbuf/intern/openexr/openexr_api.cpp` | `IMB_exr_save_deep()` |
| `source/blender/imbuf/IMB_deep_sample.hh` | `DeepSample` struct |
| `source/blender/imbuf/IMB_deep_sample_merge.hh` | Shared merge logic |

### Data Flow
```
Cycles Kernel -> DeepRenderBuffers -> DeepOutputDriver -> RenderResult
                                                     -> pipeline.cc
                                                     -> RenderContext
                                                     -> FileOutputOperation
                                                     -> IMB_exr_save_deep()
```

### Code Review Status
- 3 CRITICAL: All fixed
- 23 HIGH: 21 fixed, 2 partially fixed (trailing whitespace in 2 files)
- 34 MEDIUM: 28 fixed, remaining are pre-existing code or API constraints
- Full report archived at `.agent/archive/deep-exr/CODE_REVIEW_REPORT.md`

### Known Remaining Minor Items
1. Trailing whitespace in `properties_output.py` (2 lines) and `openexr_api.cpp` (~34 lines)
2. `TODO:` in `openexr_api.cpp` missing author attribution (`TODO(name):`)
3. Raw `delete` for `RenderLayer::deep_data` (C struct API constraint)

---

## Planned: VFX Rendering Features

See `VFX_RENDERING_PLAN.md` for detailed implementation plans.

### Feature 1: Per-Light Shadow Color
- **Branch:** `feature/shadow-color` -> merged into `vfx-rendering-branch`
- **Difficulty:** Low
- **Status:** Complete (CPU + GPU verified), merged to `vfx-rendering-branch`
- DNA/RNA/UI/Cycles kernel/light sync updated; shadow tint applied in `integrator_shade_shadow()` and
  opaque handling in `integrator_intersect_shadow()`
- Fix for transparent-shadow baked throughput: store `shadow_path.unshadowed_throughput` for
  all shadow rays, use it to tint shadows even when BVH returns baked throughput
- Build fix: added Light DNA padding + removed RNA array default (kept DNA defaults)
- Build: completed after shadow throughput fix; CPU test passed; GPU/OptiX test pending
- Install target run to populate `bin/Release/5.2` (bundled Python/data) for launching Blender
- **Test:** CPU path verified by user on 2026-02-19 (shadow color works on solids)
- **Update:** Add world background shadow color setting (Cycles World Settings) and sync to
  background light; rebuild done; copied updated Cycles add-on scripts to build output
  (`bin/Release/5.2/scripts/addons_core/cycles`) so UI shows the new setting; GPU cache clear pending
- **Fix:** Avoid shadow-color clamping of bright NEE lights by storing full evaluated
  `unshadowed_throughput` in `shade_light_nee()`. Opaque-hit path now sets shadow throughput to zero
  and lets `shade_shadow()` apply shadow color once (prevents double-apply).
- **Fix:** Keep `shadow_ray.self_light_object/prim` intact on opaque hit so `shade_shadow()` can
  still identify the light and apply shadow color (HDRI/world shadows were unaffected before).
- **Maintenance:** Rebuilt and recompressed Cycles GPU kernels; updated `kernel_*.zst` in
  `bin/Release/5.2/scripts/addons_core/cycles/lib` on 2026-02-19.
- **Test:** GPU/OptiX path verified by user on 2026-02-19 (world + light shadow color).
- **Polish:** Addressed review items: removed EEVEE UI exposure, added kernel comments,
  added RNA range, and documented DNA field.
- **Parallel setup:** Worktrees created for `feature/no-direct-lighting`,
  `feature/collection-material-override`, `feature/per-lightgroup-lobe-passes` with build dirs
  `build_no_direct`, `build_mat_override`, `build_lobe_passes`.
- **Parallel note:** Each worktree should be configured with the same CMake flags as the main
  build; swap `-S`/`-B` to point at the worktree + its build dir.
- **Code Review:** Full Blender standards + PR review done 2026-02-20. Report: `.agent/SHADOW_COLOR_REVIEW.md`
  - **M1.** Commit message needs body text (user-level + technical explanation)
  - **M2.** Missing algorithm comments in `shade_shadow.h` and `intersect_shadow.h`
  - **M3.** Remove `shadow_color` from EEVEE panel (non-functional there)
  - **L1-L3.** Minor: DNA inline comment, kernel code duplication, explicit RNA range

### Feature 2: Indirect-Only Object (No Direct Lighting)
- **Branch:** `feature/no-direct-lighting`
- **Difficulty:** Low-Medium
- **Status:** Not started
- Object visible in camera but receives no direct light. `SD_OBJECT_NO_DIRECT_LIGHT` flag skips `integrate_surface_direct_light()`.

### Feature 3: Per-Collection/Object Material Override
- **Branch:** `feature/collection-material-override`
- **Difficulty:** Medium-High
- **Status:** Not started
- `mat_override` on LayerCollection and Base. Priority: Object > Collection > ViewLayer.

### Feature 4: Per-Light-Group Light Pass AOVs (LPE Foundation)
- **Branch:** `feature/per-lightgroup-lobe-passes`
- **Difficulty:** Medium-High
- **Status:** Complete for Phase 1. Merged to `vfx-rendering-branch-github` and cherry-picked to
  `vfx-rendering-branch` on 2026-03-16.
- Per-lightgroup diffuse/glossy/transmission/volume light pass AOVs with combined + direct/indirect variants.
  Naming planned as `diffuse_<lg>`, `diffuse_direct_<lg>`, `diffuse_indirect_<lg>` (same pattern for other lobes).
  Dedicated LPE plan documented in `VFX_RENDERING_PLAN.md`.
- **Implemented (2026-03-09):**
  - KernelFilm offsets for per-lightgroup combined + direct/indirect light pass AOVs.
  - Film pass allocation for per-lightgroup light pass AOV types.
  - Kernel accumulation for direct-light and emission/background light pass AOV writes.
  - Cycles add-on toggles + Light Groups UI sub-panel for light pass AOVs.
  - Pass registration in add-on + Cycles sync with per-lightgroup pass tagging.
- **Fixed (2026-03-11):**
  - Light-group direct/indirect pass metadata now uses raw sums (no albedo divide/compositing),
    so `RGBA_env.rgb`, lobe-combined sum, and direct+indirect sum are consistent.

### Feature 5: World Environment Fog (aiFog-like, Direct-Light Only)
- **Branch:** `feature/world-environment-fog`
- **Difficulty:** Medium
- **Status:** Branch created on 2026-03-09 and fast-forwarded to the latest
  `vfx-rendering-branch-github` base on 2026-03-16; implementation not started
- Environment fog in world shader with aiFog-like controls, direct-light only (no indirect/shadowing).

---

## Quick Start

```powershell
# Build
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release

# Test
& 'E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe' -b "D:\blender_projects\deep-branch-test.blend" -f 1
```

---

## Data Safety Rules

> [!CAUTION]
> **DO NOT DELETE** test files in `C:\tmp\` or `D:\blender_projects\`

---

## Archive

Old deep EXR development history, utility scripts, code review reports, and implementation plans are preserved in `.agent/archive/deep-exr/`.
