# VFX Rendering Branch - Agent Handoff

> **Branch:** `vfx-rendering-branch-github`
> **Base:** Blender `main` (5.2 dev)
> **Last Updated:** 2026-03-05

---

## Current Status

- **Deep EXR:** Merged and complete. Code reviewed, all critical/high issues fixed.
- **Deep EXR memory:** Implemented user-controlled budget (default 1024 MB) + tile clamp; RenderResult deep storage skipped unless compositor needs it. Added tiled deep accumulation to avoid last-tile-only deep outputs. Merged into `vfx-rendering-branch` on 2026-02-22; code review checklist resolved.
- **Validation:** Rendered `D:\blender_projects\deep-branch-test.blend` on 2026-02-22; deep compositor outputs saved and tile rendering confirmed.
  Compositor deep EXRs now large (e.g., `test_compoutput_deep0001.exr` ~584 MB).
- **VFX features:** Feature 1 complete (Per-Light Shadow Color) and merged.
- **Working branch:** `vfx-rendering-branch-github`
- **Next up:** Feature 4 (per-lightgroup lobe AOVs) when ready; dedicated LPE plan added to `VFX_RENDERING_PLAN.md`.
- **Docs:** MoonRay LPE/AOV code report added at `.agent/MOONRAY_LPE_REPORT.md` (Cycles LPE reference).
- **Docs:** Added Phase 1/Phase 2 LPE-ready strategy notes to Feature 4 in `VFX_RENDERING_PLAN.md`.
- **Docs:** Added Phase 1 LPE-ready checklist under Feature 4 in `VFX_RENDERING_PLAN.md`.
- **Docs:** Added Phase 1/Phase 2 acceptance criteria under Feature 4 in `VFX_RENDERING_PLAN.md`.
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

### Feature 4: Per-Light-Group Lobe AOVs (LPE Foundation)
- **Branch:** `feature/per-lightgroup-lobe-passes`
- **Difficulty:** Medium-High
- **Status:** Not started
- Per-lightgroup diffuse/glossy/transmission/volume passes with combined + direct/indirect variants.
  Naming planned as `diffuse_<lg>`, `diffuse_direct_<lg>`, `diffuse_indirect_<lg>` (same pattern for other lobes).
  Dedicated LPE plan documented in `VFX_RENDERING_PLAN.md`.

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
