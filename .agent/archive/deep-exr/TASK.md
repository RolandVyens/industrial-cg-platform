# Deep EXR Implementation - Task Checklist

> **Workspace:** `E:\blender_modify\blender`  
> **Last Updated:** 2026-02-11

---

## Active Bug: Volume Alpha Black Holes (Resolved 2026-01-28)

> [!NOTE]
> Volume alpha in Deep EXR had black holes. Fixes applied and verified against flat alpha.

**Root Cause Identified (2026-01-23):**
- Deep Recolor was **DISABLED** in `deep_output_driver.cpp`
- Kernel writes `RGB=0, alpha=sample_alpha` (alpha-only approach for Deep Recolor)
- Without Deep Recolor, output has RGB=0 everywhere -> black holes

**Fixes Applied:**
- [x] Re-enable Deep Recolor (restore beauty buffer reads)
- [x] Tile accumulation for Combined pass (full-frame beauty buffer)
- [x] Cache processed deep data to avoid double merge
- [x] Deep recolor premultiplies beauty RGB by per-sample alpha
- [x] Remove per-segment alpha normalization for volume deep samples
- [x] Ray-marched volume deep alpha uses throughput ratio (match Combined alpha)
- [x] Unbiased volume deep alpha uses ratio-tracking transmittance
- [x] Surface deep samples use shader alpha
- [x] Per-pixel deep alpha scaling to match Combined alpha
- [x] Use render settings for deep merge tolerance + compression/half-float
- [x] Expose Deep Merge Tolerance in DEEP_EXR image format UI
- [x] Type-safe deep data stored in RenderResult
- [x] Debug prints removed
- [x] Build and run compositor test (deep EXR generated)
- [x] Build and run compositor test (2026-01-28, deep EXR regenerated)
- [x] Verify direct output
- [x] Verify compositor output
- [x] Deep merge tolerance sweep (0.01, 0.1) direct + compositor outputs saved (2026-02-04)
- [x] Compositor DEEP_EXR compression set to ZIPS and re-rendered (2026-02-04)
- [x] Compositor DEEP_EXR auto alpha-only export (A/Z/ZBack only for single Alpha link) (2026-02-04)
- [x] Verify compositor deep alpha-only vs RGBA channel sets (2026-02-04)
- [x] Filter DEEP_EXR compression UI to supported codecs (2026-02-04)
- [x] Clamp unsupported codecs when switching to DEEP_EXR (2026-02-04)

---

## Completed Milestones

### Milestone A: MVP on CPU
- [x] Core implementation (buffers, kernel, driver)
- [x] Volume deep samples
- [x] Alpha merging

### Milestone B: Deep EXR File Format
- [x] DEEP_EXR as file format option
- [x] Auto-enable, flat suppression
- [x] UI settings

### B7: Compositor Passthrough (Verified)
- [x] RenderResult deep data fields
- [x] Pipeline deep data passing
- [x] FileOutputOperation execute_deep_exr()
- [x] Auto-enable when compositor has DEEP_EXR
- [x] Property cleanup (deprecated use_deep_output)
- [x] Fix volume alpha black holes (verified)

---

## Debug Cleanup

- [x] Remove debug printf after bug fix:
  - `session.cpp`
  - `pipeline.cc`
  - `node_composite_file_output.cc`
  - `openexr_api.cpp`

## Utility
- [x] Add `E:\blender_modify\blender\.agent\deep_alpha_diff.py` for deep vs flat alpha comparison

---

## Milestone C (OptiX GPU) - Complete
- [x] Per-device DeepRenderBuffers (slice-aligned)
- [x] Per-device kernel data sync for deep pointers/dimensions
- [x] Slice-aware merge into full output
- [x] Deep buffer memory estimation (early OOM fail)
- [x] Rebalance-aware deep buffer preservation
- [x] GPU validation (OptiX single-device)
- [x] CPU+OptiX rebalance crash fix (2026-02-07)

---

## Update (2026-02-09)
- [ ] Ray-marched volume deep alpha: throughput-ratio fallback (dropped; code reverted)
  - Test results from dropped experiment:
    - Alpha diff: `mean_diff=-0.000180 mean_abs_diff=0.000185 min_diff=-0.091029 max_diff=0.007973 | diff>0.05=0 diff<-0.05=272`
    - RGB diff (flat vs deep_flat): `mean_abs_diff_rgba [1.40518343e-04 1.02342754e-04 6.26806723e-05 1.84842342e-04]`

### Crash Fix (2026-02-07)
- **Root Cause:** CPU `kernel_thread_globals_` cached BEFORE `sync_deep_output_buffers()` updated deep pointers
- **Fix:** Moved `render_init_kernel_execution()` to AFTER `sync_deep_output_buffers()` in `path_trace.cpp`
- **Result:** CPU+OptiX render with rebalancing works (5 requested, 3 performed, no crash)

---

## Update (2026-02-10)
- [x] Ray-marched volume deep alpha uses **transmittance-only** per segment.
- [x] Deep-only stepping after scatter to capture full camera-ray extinction.
- [x] Deep recolor: unpremultiply beauty RGB + low-alpha linear fallback.
- [x] Skip merging volumetric segments (Cycles + compositor deep merge).
- [x] Auto-bump deep max samples for ray-marched volumes.
- [x] Build (retry succeeded).
- [x] Tests (deep vs flat validation).
- [ ] Investigate remaining deep alpha opacity mismatch in ray-marched volume test.

### Investigation Notes (2026-02-10)
- Deep alpha mismatch traced to **low-alpha linear fallback** in Deep Recolor scaling.
- Combined alpha from the deep render matches the flat render; scaling should use log
  correction, not linear fallback, for target_alpha ~0.1.
- Next step: lower/remove fallback threshold or make it conditional on deep_alpha ~0.

### Update (2026-02-10)
- [x] Lowered low-alpha fallback threshold to **1e-3**.
- [x] Deep recolor now prefers output-driver Combined buffer (RenderResult fallback).
- [x] Added log-scaling minimum transparency fallback (`kLogScaleMinTransp`) to avoid precision loss.
- [x] Deep alpha scaling now matches Combined alpha for ray-marched volumes.

### Validation (2026-02-10)
- [x] Deep vs flat alpha diff (ray-marched volume test):
  - Deep: `C:\tmp\test_volume_alpha_deep_20260210m.exr`
  - Flat: `C:\tmp\test_volume_alpha_flat_20260210f.exr`
  - Result: `mean_diff=0.000000 mean_abs_diff=0.000003 min_diff=-0.000164 max_diff=0.000164`
- [x] CPU-only validation (ray-marched volume test):
  - Deep: `C:\tmp\test_volume_alpha_deep_20260210cpu.exr`
  - Flat: `C:\tmp\test_volume_alpha_flat_20260210cpu.exr`
  - Result: `mean_diff=0.000000 mean_abs_diff=0.000003 min_diff=-0.000164 max_diff=0.000164`
- [x] Deep-branch-test direct output validation (compositor off):
  - Deep: `C:\tmp\deep_branch_direct_deep_20260210m.exr`
  - Flat: `C:\tmp\deep_branch_direct_flat_20260210m.exr`
  - Result: `mean_diff=-0.000000 mean_abs_diff=0.000000 min_diff=-0.001586 max_diff=0.000815`

> [!NOTE]
> Compositor deep output in `deep-branch-test.blend` is alpha-only (A/Z/ZBack) when linked
> from the Alpha socket, so `deep_alpha_diff.py` cannot be used on those files.

### Update (2026-02-10)
- [x] DeepOutputDriver now refreshes merge thresholds/compression/half-float each render and
  resets buffers when dimensions or max samples change (ensures UI updates take effect).
- [x] Build succeeded after the update.

### Update (2026-02-10)
- [x] Deep merge tolerance fallback: if `deep_merge_tolerance <= 0` but
  `deep_alpha_merge_tolerance > 0`, use **0.01** at render time (direct + compositor).
- [x] Build succeeded after the update.

### Test (2026-02-10)
- [x] Direct DEEP_EXR merge tolerance validation (compositor off):
  - Fallback (depth=0.0, alpha=0.01): `C:\tmp\deep_branch_direct_deep_mt_fallback.exr`
    - total deep samples: **144,961,967**
  - Explicit off (depth=0.0, alpha=0.0): `C:\tmp\deep_branch_direct_deep_mt_off.exr`
    - total deep samples: **154,286,441**
  - High tolerance (depth=0.1, alpha=0.1): `C:\tmp\deep_branch_direct_deep_mt010c.exr`
    - total deep samples: **144,935,895**

### Update (2026-02-10)
- [x] Removed deep-only stepping after scatter in ray-marched volumes (reduces deep sample count).
- [x] Default file render test:
  - `D:\blender_projects\deep-branch-test.blend` -> `C:\tmp\test_compoutput_deep0001.exr`
  - Size: **659,580,926 bytes (~629 MB)**

### Update (2026-02-11)
- [x] Re-enabled volume sample merging (requires z/zBack/alpha within tolerance; no surface/volume mixing).
- [x] Direct DEEP_EXR default file comparison:
  - Default merge (fallback 0.01): `C:\tmp\deep_branch_direct_deep_default_volmerge.exr`
    - Size: **1,621,234,000 bytes (~1.51 GB)**
    - Total samples: **88,950,205**
  - No-merge (depth=0.0, alpha=0.0): `C:\tmp\deep_branch_direct_deep_default_nomerge.exr`
    - Size: **2,387,031,829 bytes (~2.22 GB)**
    - Total samples: **138,992,080**

### Surface Merge Validation (Sampled)
- [x] 32px grid sampling on default file:
  - Merge on: surface_samples ≈ 270, surface_multi_pixels ≈ 40 (max 8)
  - No merge: surface_samples ≈ 18,620, surface_multi_pixels ≈ 645 (max 32)

### Update (2026-02-11)
- [x] Keep merged **surface** samples as surfaces (no thickness) so subsequent surface merges
  are not blocked when z differs slightly.
- [x] Applied to Cycles deep buffer merge + compositor deep merge.
- [x] Build succeeded.
- [x] Direct DEEP_EXR test (default file, compositor off):
  - Output: `C:\tmp\deep_branch_surface_merge_fix.exr`
  - Size: **1,523,413,192 bytes (~1.42 GB)**
  - Deep sample stats (OIIO): max_samples **228**, avg_samples **37.62**
