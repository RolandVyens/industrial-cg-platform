# Deep EXR Development State





> **Worktree:** `E:\blender_modify\blender_deep_surface_coverage`


> **Branch:** `feature/deep-exr-surface-coverage`


> **Build:** `E:\blender_modify\build_deep_surface_coverage`


> **Blender:** `E:\blender_modify\build_deep_surface_coverage\bin\Release\blender.exe`


> **Last Updated:** 2026-03-25





---





## Direct Scene-Output Merge Parity Fix (2026-03-25)

- Confirmed root cause: the direct Deep EXR callback path and the RenderResult handoff path had diverged. `DeepOutputDriver::finalize_deep_output()` could write a merged deep file, but `DeepOutputDriver::get_processed_deep_data()` still returned the raw `processed_cache_`, so the direct scene-output / RenderResult save path kept writing the unmerged payload.
- Implemented fix:
  - added a local `DeepSampleTraits<blender::DeepSample>` specialization in `intern/cycles/session/deep_output_driver.cpp` so Cycles can reuse the shared `IMB_deep_sample_merge.hh` merge helper;
  - added a shared `deep_copy_merged_samples()` helper and now use the same merged deep data for both `finalize_deep_output()` and `get_processed_deep_data()`;
  - kept the earlier `BlenderSession::render()` change that stores processed deep data in `RenderResult` for direct `DEEP_EXR` renders as well as compositor-driven deep output.
- Important scope note: this does **not** re-enable the older early generic Cycles `merge_nearby_samples()` path. It only restores parity with the compositor-style **final** deep merge on the direct scene-output/export path.
- Build status:
  - worktree: `E:\blender_modify\blender_deep_surface_coverage`
  - build: `E:\blender_modify\build_deep_surface_coverage`
  - current `blender.exe` rebuilt successfully on 2026-03-25 after removing temporary `DEEP_DIAG` instrumentation.
- CPU validation (`D:\blender_projects\light-passes-test-v001.blend`, frame 2):
  - direct deep file: `C:\tmp\scene_output_rgba_deep_cpu_retry_####.exr`
  - merged direct sample stats: `total_samples=40688109`, `nonempty_pixels=1807695`, `max=136`
  - Nuke preview: `C:\tmp\deep_merge_cpu_preview_20260325.png`
  - visual result: no obvious white seam comeback; no visible volume hole comeback in the checked DeepMerge RGB preview.
- OptiX validation (`D:\blender_projects\light-passes-test-v001.blend`, frame 2):
  - direct deep file: `C:\tmp\scene_output_rgba_deep_optix_retry_####.exr`
  - merged direct sample stats: `total_samples=40576684`, `nonempty_pixels=1807695`, `max=136`
  - Nuke preview: `C:\tmp\deep_merge_optix_preview_20260325.png`
  - visual result: matches the CPU preview qualitatively; no obvious white seam or volume-hole comeback in the checked DeepMerge RGB preview.
- Script checks now passing on both CPU and OptiX merged direct files:
  - `.agent/check_deep_single_surface_alpha.py`
  - `.agent/check_deep_mixed_surface_volume_case1.py`
  - `.agent/check_deep_flatten_matches_flat.py`
  - `.agent/check_deep_surface_front_alpha.py` (diagnostic-only, but currently clean on its alpha criteria)
- Remaining separate issue:
  - `.agent/check_deep_surface_compaction.py` still reports ~3.7k over-fragmented fractional hard-surface pixels on both the direct merged file and the compositor deep file. That means this checker is tracking a broader hard-surface compaction problem, not the direct scene-output merge-parity regression fixed here.

---

## Locked Test Matrix (2026-03-25)

The active Deep EXR gate is now intentionally narrowed:

- scene: `D:\blender_projects\light-passes-test-v001.blend`
- devices: **CPU** and **OptiX** only
- known machine-sticking runs are excluded from the mandatory gate
- Nuke visual judgment is the primary pass/fail authority

Reference:
- `.agent/DEEP_EXR_TEST_MATRIX.md`

Every Deep EXR change must pass that matrix before it is kept.

---

## 1. Overall Status





The earlier “done / acceptable” story no longer holds. Even though the current Nuke mask/seam


case now looks acceptable in that narrow probe, the **direct Deep EXR outputs deviate from the


known-good baselines** and, more importantly, **volume regions are visibly wrong** after the


recent changes.





Current conclusion:


- the narrow seam pixel `(655, 403)` now reports acceptable alpha/color on the current probe,


  so the earlier white mask is absent in that specific test;


- the historical baselines (`C:\tmp\scene_output_rgba_deep_0002.exr`,


  `C:\tmp\scene_output_rgba_deep_saved_0001.exr`, `C:\tmp\direct_scene_output_write1.png`)


  still look correct, whereas the current direct deep path (`D:\blender_projects\rendered\test\trash_output\.exr`)


  shows volume-region hollowing;


- the old-good files contain ~69.5M / 74.7M samples while the current broken run only has ~38.2M,


  proving a massive early collapse in sample counts;


- `check_deep_volume_passthrough.py` between the old-good vs current deep files reports ~444,038


  mismatching volume pixels, demonstrating that broad volume segments are being merged away.


- The early generic deep merge/compaction strategy is now the wrong direction; correctness requires


  keeping those volume/segment boundaries intact even if deep counts grow.





Volume deep was intentionally left untouched in the earlier phase, but that is no longer


acceptable when the direct outputs visibly differ from the known-good deep baselines.





Important implementation-state note:


- the previously validated visual fix stemmed from preserving opaque hard-surface duplicates during


  the Cycles deep-buffer merge phase;


- with the new evidence we instead now plan to **disable early generic deep merging entirely** for


  all Deep EXR output paths, rely on later, more specific processing, and accept larger deep files


  / sample counts to keep volume data faithful.





---





## 2. Current Scope Decision





Locked current Deep EXR direction:


- **fix hard-surface deep behavior while acknowledging the broader visible regressions;**


- **disable the early generic deep merge/compaction step for all Deep EXR output paths;**


- **accept larger deep files / sample counts if that preserves volume-region correctness;**


- **keep volume deep and existing compositor alpha-only policy untouched by this rollback;**


- **do not attempt a MoonRay-style sparse/compressed storage rewrite in this phase.**





This phase is a visibility-first rollback, not a full architecture rewrite.





---





## 3. Main Validation Inputs





Primary scene:


- `D:\blender_projects\light-passes-test-v001.blend`





Controlled direct scene-output Deep EXR:


- `C:\tmp\scene_output_rgba_deep_probe_####.exr`





Nuke validation:


- script: `E:\blender_modify\deep_merge_test.nk`


- preview: `C:\tmp\direct_scene_output_saved_write1.png`


- mask: `C:\tmp\direct_scene_output_saved_mask.png`





Primary visual judgment:


- the large white DeepMerge seam between teapot/gray-card region is gone;


- remaining residual mask is small sparse noise.





Latest rerun note (2026-03-23):


- the current accepted validation state comes from a fresh rerender of the probe file with the


  current local build, followed by the unchanged Nuke script above;


- several `.agent` deep-check scripts were corrected during this rerun to use native deep data


  channels correctly, because earlier versions could falsely report failures.





---





## 4. Current Deep EXR Behavior by Aspect





### 4.1 Direct scene-output Deep EXR





Current state:


- works as the main validation path;


- carries deep `RGBA/Z/ZBack`;


- hard-surface edge behavior is now acceptable on the tested scene;


- direct scene-output deep is the authoritative path for the current hard-surface fix.





### 4.2 Compositor Deep EXR





Current compositor policy remains unchanged:


- if the File Output Deep EXR input is linked from `Image`, it is RGBA deep;


- if linked from `Alpha`, it remains **alpha-only deep** by design.





This policy was explicitly preserved.





### 4.3 Hard-surface compaction





Current state:


- fixed on the validated scene.





The earlier issue where one visible hard surface exported too many thin samples is no longer active


on the current controlled test path.





### 4.4 Hard-surface edge alpha / coverage





Current state:


- fixed.





Foreground hard-surface edges no longer stay incorrectly solid because of lost duplicate coverage on


the tested seam cases.





### 4.5 Front-sample color





Current state:


- acceptable on the validated seam case.





The previously bad front hard-surface sample is back near the actual object-side color instead of


behaving like the flat antialiased edge color.





### 4.6 Volume deep





Current state:


- unchanged by this phase;


- still mismatching the known-good baseline volume coverage (old-good deep files have


  ~69.5M / 74.7M samples while the current direct run only records ~38.2M);


- preserving volume correctness is now the rollback goal, so we will accept larger deep files /


  sample counts instead of collapsing these regions.





Volume deep is not being reworked beyond disabling the early generic merge/compaction pass.


The current branch priority is restoring the historical volume fidelity by keeping the raw


structure intact.





### 4.7 Validation tooling status





Current state:


- the active `.agent` deep validation tooling is now more trustworthy than the earlier morning


  state;


- `check_deep_single_surface_alpha.py`,


  `check_deep_surface_front_alpha.py`,


  `check_deep_mixed_surface_volume_case1.py`, and


  `check_deep_volume_passthrough.py`


  were corrected to read native deep data/channel indices consistently.





Important consequence:


- the earlier apparent mixed surface/volume failure was traced to a checker bug, not to a newly


  reproduced renderer regression on the current probe file.





---





## 5. Root Causes and Fixes Landed in This Follow-up





### 5.1 Hard-surface over-fragmentation root cause





Root cause:


- export-side hard-surface prefix grouping was too narrow and still split adjacent visible-surface


  hits more than desired.





Current code state:


- export-side grouping helpers for visible-surface identity and normal continuity are present in the


  branch as metadata-aware groundwork;


- however, the kernel-side hard-surface metadata write path is not fully activated yet, so this is


  not the primary active reason for the validated current result.





Result:


- the validated scene no longer shows the earlier over-fragmented output, but the accepted current


  visible fix should be attributed first to preserved opaque duplicate coverage in


  `DeepRenderBuffers::merge_nearby_samples()`.





### 5.2 Hard-surface opaque seam / coverage root cause





Final root cause:


- `intern/cycles/session/deep_buffers.cpp`


- `DeepRenderBuffers::merge_nearby_samples()`





The generic deep merge was still collapsing opaque hard-surface duplicate hits too early before


export-side coverage reconstruction. That destroyed the real camera-hit coverage distribution, so


later export saw too few representatives and reconstructed front alphas that were far too small on


some seam pixels.





Fix:


- preserve opaque hard-surface duplicates in the low-level merge path by keeping


  `preserve_opaque_surface_duplicates=true`;


- keep volume merging behavior unchanged;


- keep the older export fallback path working correctly when metadata is absent.





Result:


- previously bad opaque seam pixels now flatten back to the correct opaque alpha.





---





## 6. Current Verified Pixel Examples





### Pixel `(655, 403)`





This is the key repaired opaque seam case.





Current deep result:


- sample 0: front teapot, `A=0.5625`


- sample 1: farther hard surface, `A~0.2142857`


- sample 2: far background/ground, `A=1.0`





Flattened deep alpha:


- `1.0`





Flat alpha:


- `1.0`





### Pixel `(302, 150)`





This is a validated mixed foreground/background hard-surface case.





Current deep result:


- sample 0: front surface, `A=0.75`


- sample 1: far background, `A=0.375`





Flattened deep alpha:


- `0.84375`





Flat alpha:


- `0.84375`





---





## 7. Fresh Verification Results





All results below are from the current build/worktree state on 2026-03-23.





### 7.1 Opaque coverage regression





Command:


- `.agent/check_deep_surface_opaque_coverage.py`





Result:


- `pixel=(655,403) flat_alpha=1.000000000 deep_alpha=1.000000000 diff=0.000000000`


- `mismatching_opaque_pixels=0`





### 7.2 Hard-surface compaction regression





Command:


- `.agent/check_deep_surface_compaction.py`





Result:


- `checked_fractional_pixels=4704`


- `all_surface_fractional_pixels=4704`


- `overfragmented_pixels=0`





### 7.3 Single-surface AA regression





Command:


- `.agent/check_deep_single_surface_alpha.py`





Result:


- `checked_single_surface_fractional_pixels=4696`


- `mismatching_single_surface_pixels=0`





### 7.4 Front-surface alpha regression





Command:


- `.agent/check_deep_surface_front_alpha.py`





Result:


- `active_sample_pixels=1807695`


- `multi_active_sample_pixels=464917`


- `multi_surface_pixels=14479`


- `fractional_front_checked_pixels=8`


- `violating_front_surface_alpha_pixels=0`


- `flat_alpha_mismatching_pixels=0`





### 7.5 Surface sample color regression





Command:


- `.agent/check_deep_surface_sample_color.py`





Result:


- passes on the previously bad seam case `(655, 403)`.





### 7.6 Nuke visual validation





Command path:


- `.agent/run_nuke_direct_scene_output_test.py`





Outputs:


- `C:\tmp\direct_scene_output_saved_write1.png`


- `C:\tmp\direct_scene_output_saved_mask.png`





Visual result:


- the large white seam is gone;


- remaining mask is small sparse residual noise.





---





## 8. Current Technical Design Summary





### Low-level deep merge





Shared helper:


- `source/blender/imbuf/IMB_deep_sample_merge.hh`





Current Cycles deep-buffer use:


- preserve opaque hard-surface duplicates;


- still merge compatible volume samples normally.





### Export-side hard-surface compaction





Main file:


- `intern/cycles/session/deep_output_driver.cpp`





Current responsibility:


- keep correct resolved edge coverage behavior on the accepted current path;


- leave volume suffix handling on the current existing path.





Important note:


- metadata-aware hard-surface grouping/reconstruction helpers are currently present but are not yet


  fully exercised because the kernel-side metadata write/accumulate flow is not fully wired.





---





## 9. Memory / Architecture State





Current Cycles Deep EXR storage remains:


- fixed-capacity per-tile deep buffers;


- tile-budget / clamp based;


- predictable and controllable;


- less memory-efficient than MoonRay-style sparse/compressed deep storage.





Future direction noted but not in scope here:


- consider MoonRay-style sparse/compressed storage ideas later.





Current phase did **not** attempt a storage redesign.





---





## 10. GPU / Backend State





Deep EXR feature code is intended to support:


- CPU


- CUDA


- OptiX





Important note:


- the previously investigated CUDA illegal-address issue was traced to stale runtime cubin copying


  and handled separately;


- the current hard-surface coverage fix is mainly host/export-side deep processing logic, not a new


  backend-specific feature path.





---





## 11. Remaining Non-Blocking Items





Accepted residuals:


- tiny sparse residual mask specks in Nuke preview/mask;


- no volume deep redesign in this phase;


- no sparse/compressed storage optimization in this phase.





These residuals remain acceptable for now, but the branch still treats the Deep EXR follow-up as


incomplete until the volume mismatch / early merge problem is resolved.





---





## 12. Key Files to Remember





Main implementation files:


- `intern/cycles/kernel/film/deep_write.h`


- `intern/cycles/session/deep_buffers.h`


- `intern/cycles/session/deep_buffers.cpp`


- `intern/cycles/session/deep_output_driver.h`


- `intern/cycles/session/deep_output_driver.cpp`


- `intern/cycles/blender/session.cpp`


- `source/blender/nodes/composite/nodes/node_composite_file_output.cc`


- `source/blender/imbuf/IMB_deep_sample_merge.hh`





Main helper / validation files:


- `.agent/render_scene_output_rgba_deep_probe.py`


- `.agent/run_nuke_direct_scene_output_test.py`


- `.agent/check_deep_surface_opaque_coverage.py`


- `.agent/check_deep_surface_compaction.py`


- `.agent/check_deep_single_surface_alpha.py`


- `.agent/check_deep_surface_front_alpha.py`


- `.agent/check_deep_surface_sample_color.py`





---





## 13. Bottom Line





Current Deep EXR state is:


- **hard-surface compaction:** good


- **hard-surface edge alpha / coverage:** good


- **opaque seam bug:** fixed


- **front-surface color on validated seam case:** good


- **volume deep:** still mismatching the known-good baselines (old-good deep files have ~69.5M /


  74.7M samples while the current direct output only records ~38.2M, and the volume comparison


  reports ~444,038 mismatching pixels)


- **direct scene-output deep:** still suffers the early generic merge collapse and visual


  divergence from `C:\tmp\direct_scene_output_write1.png`


- **compositor alpha-only policy:** intentionally unchanged


- **remaining residual:** tiny noise-level mask specks only





Therefore this Deep EXR follow-up is currently **not done/acceptable** for the branch. The approved


rollback direction remains to disable the early generic deep merge for all Deep EXR paths, preserve


volume correctness, and accept larger sample counts / file sizes while matching the historical


baseline.

## 14. 2026-03-25 Runtime RGBA Deep Recovery Update

The above “not done/acceptable” summary is now outdated for the validated runtime RGBA deep test on
`D:\blender_projects\light-passes-test-v001.blend`.

New local change:

- `intern/cycles/session/deep_output_driver.cpp` now classifies each deep pixel using sample/ray
  metadata before deciding whether to use the new hard-surface correction path.
- Only the metadata-proven safe **surface-front-prefix + volume-behind** case is allowed to use the
  hard-surface correction logic.
- Pure volume, volume-front mixed, and other unsupported/ambiguous mixed pixels stay on the legacy
  fallback path.

Verification result from the runtime compositor RGBA deep override:

- Deep EXR:
  `D:\blender_projects\rendered\test\TempDeepRGBA\ViewLayer_Deep_v001_0002.exr`
- Nuke preview:
  `C:\tmp\deep_test_direct_write1_case1_gate.png`
- Flatten-vs-flat:
  - `mean_abs_rgb=(8.509848703397438e-05, 7.660665141884238e-05, 9.596627205610275e-05)`
  - `pixels_gt_0.05=13`
  - `flatten_matches_flat=1`

Residual difference is down to 13 tiny highlight/noise pixels near `(1282-1287, 345-347)` and
`(1271-1274, 488-490)`, rather than a large volume-region divergence.

Working interpretation:

- the broad volume regression is no longer reproducing on the validated runtime RGBA deep test
- the metadata-gated safe-case correction is the currently validated direction
- remaining work is follow-up visual confirmation / cleanup, not a known large blocker on this test

## 15. 2026-03-25 Matrix Recheck / Current Blocker

The runtime RGBA deep recovery result above is still useful, but it is **not sufficient** to claim
the branch passes the locked matrix in `.agent/DEEP_EXR_TEST_MATRIX.md`.

Fresh matched re-renders were made on `D:\blender_projects\light-passes-test-v001.blend` for:

- CPU direct deep / compositor RGBA deep / compositor alpha-only deep
- OptiX direct deep / compositor RGBA deep / compositor alpha-only deep

Current matrix read:

- **Build:** pass
- **Nuke visual previews:** acceptable on the refreshed CPU/OptiX direct and compositor-RGBA
  outputs; the obvious white seam / large volume-hole regression is not what is failing now
- **OptiX required scripts:** current refreshed outputs pass
- **CPU required scripts:** fail on flatten-vs-flat for both refreshed RGBA-deep variants

Fresh CPU flatten failures:

- direct:
  - flat: `C:\tmp\matrix_cpu_direct_flat_0002.exr`
  - deep: `C:\tmp\matrix_cpu_direct_deep_0002.exr`
  - `mean_abs_rgb=(0.011504311114549637, 0.009117967449128628, 0.013690846040844917)`
  - `pixels_gt_0.05=173970`
  - `flatten_matches_flat=0`
- compositor RGBA:
  - flat: `C:\tmp\matrix_cpu_comp_rgba_flat_0002.exr`
  - deep: `C:\tmp\matrix_cpu_comp_rgba_deep_0002.exr`
  - `mean_abs_rgb=(0.011510949581861496, 0.009124506264925003, 0.013697427697479725)`
  - `pixels_gt_0.05=174058`
  - `flatten_matches_flat=0`

Observed failure character:

- this is **not** primarily the old edge seam bug
- the mismatch is broad across the lit image
- strongest peaks show up in bright/specular regions rather than only at the teapot edge
- preview/debug artifacts exported to `C:\tmp\cpu_flatten_debug\`
- during inspection the current worst sampled mismatch pixel was `(1466, 381)`, with flattened deep
  much brighter than the flat image in a hot highlight

Therefore the current accepted blocker is:

- **CPU-wide flatten-vs-flat RGB mismatch on refreshed RGBA deep outputs**

The next debugging task should target that systemic CPU RGB/energy mismatch before any new claim of
matrix completion.
