# VFX Rendering Branch - Agent Handoff





> **Branch:** `vfx-rendering-branch-github`


> **Base:** Blender `main` (5.2 dev)


> **Last Updated:** 2026-03-25





---





## Deep EXR Direct Scene-Output Merge Parity Fix (2026-03-25)

- The current `feature/deep-exr-surface-coverage` worktree fix is now focused on a **direct scene-output merge parity** bug, not on re-enabling the older early generic deep merge.
- Confirmed root cause: `DeepOutputDriver::finalize_deep_output()` could already write a merged deep file, but `DeepOutputDriver::get_processed_deep_data()` still returned the raw `processed_cache_`, so the RenderResult / direct scene-output handoff path kept exporting the unmerged payload.
- Implemented fix in `intern/cycles/session/deep_output_driver.cpp`:
  - local `DeepSampleTraits<blender::DeepSample>` specialization for `IMB_deep_sample_merge.hh`;
  - shared `deep_copy_merged_samples()` helper;
  - merged deep data now feeds both the callback write path and `get_processed_deep_data()`.
- Kept the existing `intern/cycles/blender/session.cpp` behavior that populates `RenderResult.deep_data` for direct `DEEP_EXR` renders as well as compositor-driven deep output.
- Validation on the locked matrix scene `D:\blender_projects\light-passes-test-v001.blend`:
  - CPU direct deep: `C:\tmp\scene_output_rgba_deep_cpu_retry_####.exr` -> `total_samples=40688109`, `max=136`; Nuke preview `C:\tmp\deep_merge_cpu_preview_20260325.png` looks visually clean.
  - OptiX direct deep: `C:\tmp\scene_output_rgba_deep_optix_retry_####.exr` -> `total_samples=40576684`, `max=136`; Nuke preview `C:\tmp\deep_merge_optix_preview_20260325.png` also looks visually clean.
  - Passing script checks on both devices: `check_deep_single_surface_alpha.py`, `check_deep_mixed_surface_volume_case1.py`, `check_deep_flatten_matches_flat.py`, and the diagnostic `check_deep_surface_front_alpha.py` remains clean on its current alpha criteria.
- Important remaining note: `check_deep_surface_compaction.py` still fails on both the merged direct file and the compositor deep file (~3.7k over-fragmented fractional hard-surface pixels), so that broader compaction issue remains open and is separate from the direct scene-output merge-parity bug fixed here.

---

## Deep EXR Direction Update (2026-03-24)





- Known-good reference artifacts: the Nuke baseline `C:\tmp\direct_scene_output_write1.png`


  plus deep EXRs `C:\tmp\scene_output_rgba_deep_0002.exr` and


  `C:\tmp\scene_output_rgba_deep_saved_0001.exr`.


- Current broken direct Deep EXR path: `D:\blender_projects\rendered\test\trash_output\.exr`.


- Sample-count regression: the historical baselines contain ~69.5M / 74.7M deep samples,


  whereas the current direct render only has ~38.2M—an alarming collapse consistent with


  the observed visual hollowing.


- Volume-region mismatch: `check_deep_volume_passthrough.py` comparing the old-good deep files


  against the current output reports ~444,038 mismatching volume pixels, showing that broad


  volume segments are being merged away.


- Approved next step: abandon the early **generic deep merge/compaction** for all Deep EXR output


  paths in this branch, disable that pass, and accept larger deep sample counts/files in order to


  preserve correct volume structure and match the known-good baselines.





This supersedes the earlier “Deep EXR is done/acceptable” narrative. Deep EXR is currently NOT


done/acceptable overall because the direct outputs still diverge from the known-good volume baseline,


so the new direction keeps rolling back toward correctness by disabling the early generic merge.





## Current Status





- Deep EXR validation scope was narrowed on 2026-03-25 to a locked mandatory matrix:
  `D:\blender_projects\light-passes-test-v001.blend` only, tested on **CPU** and **OptiX** only,
  with known machine-sticking runs excluded. Nuke visual validation remains the primary authority.
  See `.agent/DEEP_EXR_TEST_MATRIX.md`.

Reference summary document:


- `.agent/DEEP_EXR_DEVELOPMENT_STATE.md` now holds the consolidated current Deep EXR development


  state, behavior summary, root causes, fixes, and latest validation results.





- **Deep EXR validation rerun / checker correction (2026-03-23, current):**


  - Re-rendered the controlled direct scene-output deep probe with the current local build:


    `C:\tmp\scene_output_rgba_deep_probe_####.exr`


    from `D:\blender_projects\light-passes-test-v001.blend` frame 2.


  - Re-ran the authored Nuke test script unchanged except for input/output paths:


    `E:\blender_modify\deep_merge_test.nk`


    now writing:


    - preview: `C:\tmp\direct_scene_output_saved_write1.png`


    - mask: `C:\tmp\direct_scene_output_saved_mask.png`


  - Current visual state from that rerun: the major teapot/ground DeepMerge white seam is again in


    the acceptable fixed state; the remaining mask content is limited to tiny sparse specks.


  - Corrected `.agent` verification scripts that were still using the wrong deep access pattern:


    - `check_deep_single_surface_alpha.py`


    - `check_deep_surface_front_alpha.py`


    - `check_deep_mixed_surface_volume_case1.py`


    - `check_deep_volume_passthrough.py`


  - Important correction: the previous apparent mixed surface/volume regression was a **bad


    checker**, not a confirmed renderer failure. `check_deep_mixed_surface_volume_case1.py` had


    been reading native deep channel `0` as if it were alpha; after fixing it to use the real `A`


    channel, the same target pixels now pass with `mismatching_pixels=0`.


  - Latest rerun results on the current probe:


    - `.agent/check_deep_single_surface_alpha.py`:


      `checked_single_surface_fractional_pixels=4696`,


      `mismatching_single_surface_pixels=0`


    - `.agent/check_deep_surface_compaction.py`:


      `checked_fractional_pixels=4704`,


      `overfragmented_pixels=0`


    - `.agent/check_deep_surface_front_alpha.py`:


      `active_sample_pixels=1807695`,


      `multi_active_sample_pixels=464917`,


      `multi_surface_pixels=14479`,


      `fractional_front_checked_pixels=8`,


      `violating_front_surface_alpha_pixels=0`,


      `flat_alpha_mismatching_pixels=0`


    - `.agent/check_deep_surface_sample_color.py` still passes on seam pixel `(655, 403)`, with


      front-sample unpremultiplied RGB matching the teapot interior instead of the flat edge color.


    - `.agent/check_deep_flatten_matches_flat.py` still does **not** exactly match flat RGBA


      (`pixels_gt_0.05=173079`); this is currently treated as the accepted tradeoff of restoring


      truer per-sample surface color instead of forcing flat edge color into every deep sample.


- **Deep EXR hard-surface opaque-coverage follow-up (2026-03-23, validated/current):**


  - Confirmed the remaining direct scene-output DeepMerge seam was **not** the earlier


    object/shader compaction bug anymore. The fresh controlled render still had opaque seam pixels


    like EXR `(655, 403)` flattening to only `deep_alpha=0.125` while the flat RGBA pixel stayed


    fully opaque `1.0`.


  - Root cause: `intern/cycles/session/deep_buffers.cpp` was still calling the shared


    `merge_sorted_deep_samples(..., preserve_opaque_surface_duplicates=false)` path before export.


    That generic pre-export merge collapsed many identical opaque hard-surface camera hits into only


    a few representatives, so later export-side coverage reconstruction saw only `4` raw opaque


    hits with `sample_count=32` on the traced seam pixel and assigned tiny front alphas like


    `1/32`, `1/31`, etc.


  - Fix: restored opaque duplicate preservation in `DeepRenderBuffers::merge_nearby_samples()` by


    passing `true` for `preserve_opaque_surface_duplicates`, while leaving volume merging untouched.


    Export-side prefix grouping/compaction still collapses the preserved hard-surface duplicates


    into compact final deep samples using the real camera-hit counts.


  - Fresh verification on the controlled scene-output render


    `C:\tmp\scene_output_rgba_deep_probe_####.exr` from


    `D:\blender_projects\light-passes-test-v001.blend` frame 2:


    - new red/green checker


      `.agent/check_deep_surface_opaque_coverage.py D:\blender_projects\rendered\test\ViewLayer\RGBA\ViewLayer_RGBA_v001_0002.exr C:\tmp\scene_output_rgba_deep_probe_####.exr`


      now reports `pixel=(655,403) flat_alpha=1.0 deep_alpha=1.0 diff=0.0 samples=3`


      (was `deep_alpha=0.125` before the fix).


    - `.agent/check_deep_surface_compaction.py` still reports `overfragmented_pixels=0`.


    - `.agent/check_deep_single_surface_alpha.py` still reports


      `checked_single_surface_fractional_pixels=4696`,


      `mismatching_single_surface_pixels=0`.


    - `.agent/check_deep_surface_front_alpha.py` still reports


      `active_sample_pixels=1807695`, `multi_active_sample_pixels=464917`,


      `multi_surface_pixels=14479`, `fractional_front_checked_pixels=8`,


      `violating_front_surface_alpha_pixels=0`, `flat_alpha_mismatching_pixels=0`.


    - `check_deep_surface_sample_color.py` now passes on the previously failing opaque seam pixel


      `(655, 403)`; the front sample unpremultiplied RGB is back near the teapot interior color


      instead of the flat edge color.


    - Fresh Nuke validation via


      `.agent/run_nuke_direct_scene_output_test.py --deep-input C:\tmp\scene_output_rgba_deep_probe_####.exr`


      rewrote:


      - preview: `C:\tmp\direct_scene_output_saved_write1.png`


      - mask: `C:\tmp\direct_scene_output_saved_mask.png`


      The large white seam is visually gone; only a tiny sparse residual mask cluster remains for


      follow-up classification.


  - Expanded review note (2026-03-23): the **currently active** validated fix is the low-level


    opaque-duplicate preservation in `DeepRenderBuffers::merge_nearby_samples()`. The newer


    metadata-aware hard-surface reconstruction helpers are present in the branch as groundwork, but


    the kernel-side metadata write path is not fully wired yet, so that metadata-aware path is not


    currently the primary reason the validated renders look correct.





- **Deep EXR hard-surface compaction follow-up (2026-03-22, validated/current):**


  - Resumed debugging from a **fresh controlled scene-output Deep EXR** render instead of the


    stale `D:\blender_projects\rendered\test\trash_output\.exr` path. Current controlled render:


    `C:\tmp\scene_output_rgba_deep_probe_####.exr`


    from `D:\blender_projects\light-passes-test-v001.blend` using runtime-only scene settings


    (`.agent/render_scene_output_rgba_deep_probe.py`).


  - Root cause of the remaining hard-surface over-fragmentation:


    `intern/cycles/session/deep_output_driver.cpp` was still grouping hard-surface prefix samples by


    full `object+shader+prim` metadata identity. Adjacent triangles from the same visible surface


    therefore stayed split even when normals matched and they should have compacted.


  - Current follow-up changes the prefix grouping comparison to the visible-surface identity


    `object+shader` plus normal continuity, while still using the stored per-sample RGB metadata for


    group color.


  - Fresh verification on the controlled scene-output render:


    - `.agent/check_deep_surface_compaction.py C:\tmp\scene_output_rgba_deep_probe_####.exr D:\blender_projects\rendered\test\ViewLayer\RGBA\ViewLayer_RGBA_v001_0002.exr`


      now reports `overfragmented_pixels=0` (was `3469` before this follow-up).


    - traced far-ground pixel `(1855, 128)` collapsed from `3` thin hard-surface samples to `1`


      sample with `A=0.09375`.


    - traced mixed edge pixel `(302, 150)` collapsed from `4` samples to `2`


      (`foreground + merged background`).


    - `.agent/check_deep_single_surface_alpha.py` on the fresh controlled render reports


      `checked_single_surface_fractional_pixels=4696`,


      `mismatching_single_surface_pixels=0`.


    - `.agent/check_deep_surface_front_alpha.py` on the fresh controlled render reports


      `active_sample_pixels=1807695`, `multi_active_sample_pixels=464917`,


      `multi_surface_pixels=14479`, `fractional_front_checked_pixels=8`,


      `violating_front_surface_alpha_pixels=0`, `flat_alpha_mismatching_pixels=0`.


  - Separate note: the older front-sample true-color/noise check on fully opaque pixels is still a


    different issue from this compaction fix and remains out of the current change.


  - Current interpretation after expanded review: treat the metadata-aware compaction code in


    `intern/cycles/session/deep_output_driver.cpp` as partial future groundwork until the kernel


    metadata write/accumulate path is fully activated.





- **Deep EXR review cleanup follow-up (2026-03-22, in progress):**


  - Applied the verified code-review cleanup items on the active worktree:


    - `intern/cycles/blender/session.cpp` now uses one shared


      `deep_file_debug_enabled()` helper instead of duplicated lambdas, and the env-var-gated deep


      file trace messages were lowered from `LOG_WARNING` to `LOG_DEBUG`.


    - Added explicit host/kernel sync comments for the duplicated deep metadata constants/helpers in


      `intern/cycles/kernel/film/deep_write.h` and `intern/cycles/session/deep_buffers.h`.


  - Current commit-prep cleanup scope keeps unrelated renderer/debug branch work untouched.


  - Touched tracked files in the current staged helper/doc set were normalized back to CRLF for


    this checkout before re-staging.





- **Deep merge matrix validation refresh (2026-03-22, in progress):**


  - Confirmed the current renderer-side state is no longer the earlier direct-scene overfragmented


    22-sample case on the traced seam pixel. Latest direct scene-output render of


    `D:\blender_projects\light-passes-test-v001.blend` frame 2 now gives pixel `(302, 150)` only


    **4** deep samples in `D:\blender_projects\rendered\test\trash_output\.exr`.


  - Confirmed the untouched blend still ships only an **alpha-only** compositor Deep EXR output


    node (`ViewLayer--Deep` linked from `ViewLayer.Alpha`). The file at


    `D:\blender_projects\rendered\test\ViewLayer\Deep\ViewLayer_Deep_v001_0002.exr` therefore has


    channels `A/Z/ZBack` only by design, even though the node format says `RGBA`.


  - Added runtime-only validation helper


    `E:\blender_modify\blender_deep_surface_coverage\.agent\render_temp_compositor_rgba_deep.py`.


    It rewires the existing compositor deep file-output node to `ViewLayer.Image`, writes to


    `D:\blender_projects\rendered\test\TempDeepRGBA\`, and renders frame 2 without modifying the


    saved blend.


  - Updated matrix checker default compositor RGBA path to the runtime temp output:


    `E:\blender_modify\blender_deep_surface_coverage\.agent\check_deep_merge_matrix.py`


    now expects `D:\blender_projects\rendered\test\TempDeepRGBA\ViewLayer_Deep_v001_0002.exr`


    unless overridden.


  - Verification:


    - direct scene-output deep:


      `D:\blender_projects\rendered\test\trash_output\.exr`


    - runtime compositor RGBA deep:


      `D:\blender_projects\rendered\test\TempDeepRGBA\ViewLayer_Deep_v001_0002.exr`


    - untouched compositor alpha-only deep:


      `D:\blender_projects\rendered\test\ViewLayer\Deep\ViewLayer_Deep_v001_0002.exr`


    - traced seam pixel `(302, 150)` now matches structurally across all 3:


      `sample_count=4`, same `A/Z/ZBack`; direct + temp compositor RGBA also match the same


      nonzero `R/G/B`.


    - Current matrix command passes:


      `check_deep_merge_matrix.py --compositor-alpha-only-deep D:\blender_projects\rendered\test\ViewLayer\Deep\ViewLayer_Deep_v001_0002.exr`





- **Deep EXR seam debug direction correction (2026-03-22, in progress):**


  - Reverted the temporary compositor `alpha_only=false` experiment in


    `source/blender/nodes/composite/nodes/node_composite_file_output.cc`. The user-confirmed


    policy stays unchanged: if the File Output Deep EXR node is linked from `Alpha`, it remains an


    alpha-only deep output.


  - Switched the active visual debug target away from the compositor/deep-recolor path to a


    **straight scene-output Deep EXR** test using the unchanged


    `D:\blender_projects\light-passes-test-v001.blend` scene saved as


    `C:\tmp\light_passes_test_deep_saved.blend` with scene Deep EXR output path


    `C:\tmp\scene_output_rgba_deep_saved_####.exr`.


  - Current direct scene-output visual check (Nuke graph kept on the same DeepMerge setup, but


    `DeepMerge1` fed directly from `DeepRead1` instead of `DeepRecolor1`) still shows the white


    seam. Latest reporting PNG:


    `C:\tmp\direct_scene_output_saved_write1.png`.


  - Added repeatable helper script:


    `E:\blender_modify\blender_deep_surface_coverage\.agent\run_nuke_direct_scene_output_test.py`


    It copies the current direct scene-output Deep EXR to a stable exact filename, repoints the


    authored direct DeepRead node in the Nuke test (`DeepRead2` after the user 2026-03-22 update),


    renders the existing `Write1` preview, and exports the authored mask node (`Shuffle1`).


  - Current paired Nuke artifacts per test round:


    - preview: `C:\tmp\direct_scene_output_saved_write1.png`


    - mask: `C:\tmp\direct_scene_output_saved_mask.png`


  - After the user updated both the Nuke script and the blend output settings on 2026-03-22, the


    direct scene-output Deep EXR under test is now:


    `D:\blender_projects\rendered\test\trash_output\.exr`


  - The direct scene-output file now proves the compositor RGB-loss hypothesis is not the active


    blocker for this test:


    - `C:\tmp\scene_output_rgba_deep_saved_####.exr` contains nonzero RGB deep samples.


    - Traced seam pixel `(302, 150)` currently has **22** deep samples, with 19 front hard-surface


      samples before the far ground suffix.


  - Current debug focus is back on **hard-surface prefix compaction / coverage behavior** on the


    direct scene-output path. Volume handling remains untouched.








- **Deep EXR direct scene-output + hard-surface grouping debug (2026-03-21, in progress):**


  - Verified the host-side direct scene-output Deep EXR finalization bug and kept the fix in


    `intern/cycles/blender/session.cpp`: when full-frame Combined / Debug Sample Count buffers are


    unavailable at finalize time, we now skip `set_beauty_buffer(nullptr, ...)` /


    `set_sample_count_buffer(nullptr, ...)` instead of clearing the already accumulated


    `processed_cache_`.


  - Repro/verification on `C:\tmp\light_passes_test_deep_saved.blend` frame 2 with


    `CYCLES_DEEP_FILE_DEBUG=1`:


    - before the guard, direct scene-output Deep EXR finalization wrote `nonempty_pixels=0`,


      `total_samples=0`, and produced tiny empty files;


    - after the guard, finalization reports `nonempty_pixels=1807695`,


      `total_samples=69538624`, `max_samples=235`, and the saved file grew to


      `C:\tmp\scene_output_rgba_deep_saved_####.exr` ≈ `1.47 GB`.


  - Continued hard-surface seam tracing on checker pixel `(302, 150)` in saved EXR space


    (`DeepOutputDriver` debug pixel `(302, 929)`):


    - current raw prefix is 24 teapot hits + 3 ground hits with `beauty_a=0.84375`;


    - the prior grouping path was still merging by `object+shader` only, so 24 different teapot


      primitive hits collapsed into one front sample at alpha `0.75`.


  - Current narrow hypothesis test in `intern/cycles/session/deep_output_driver.cpp` switches that


    merge to exact `surface_key` matching (preserving primitive identity during prefix grouping).


    On the traced pixel this changes the saved deep result from 2 samples


    (`0.75` teapot + `0.375` ground) to 22 samples whose front teapot alphas start at


    `0.03125`, `0.032258`, `0.033333`, ... while preserving cumulative pixel alpha


    (`deep_total_alpha ≈ 0.84375001`, flat alpha `0.84375`).


  - Regression sanity after the grouping change:


    - CPU/factory-startup rerender of `D:\blender_projects\deep-branch-test.blend` frame 1


      completed cleanly with the current build.


    - `.agent/check_deep_single_surface_alpha.py` still reports


      `checked_single_surface_fractional_pixels=6657`,


      `mismatching_single_surface_pixels=0`.


    - `.agent/check_deep_surface_front_alpha.py` still reports


      `active_sample_pixels=872806`, `multi_active_sample_pixels=0`,


      `violating_front_surface_alpha_pixels=0`.


  - Nuke CLI validation is currently blocked by local parsing of `E:\blender_modify\deep_merge_test.nk`


    (`OctaneExport: Unknown command` from the existing test script path section), so the white-seam


    visual pass/fail is still pending and not yet claimed fixed.


- **Deep EXR hard-surface compaction debug (2026-03-21, in progress):**


  - Traced the remaining over-fragmented DeepMerge seam on


    `D:\blender_projects\light-passes-test-v001.blend` frame 2 to a hard-surface prefix grouping


    split, not a volume rewrite.


  - Root cause pixel: checker pixel `(302, 150)` in saved EXR space (debug pixel `(302, 929)` in


    `DeepOutputDriver` buffer space). Raw prefix had 24 teapot hits + 3 ground hits, but the


    teapot prefix was split into two groups because one valid foreground hit had normal dot


    `~0.9679` against the rest while `deep_surface_normal_dot_threshold` was `0.98`.


  - Current narrow follow-up test lowers the hard-surface prefix normal grouping threshold to


    `0.95`, which collapses that pixel from 3 saved thin surface samples to the expected 2


    (`foreground alpha 0.75`, `background alpha 0.375`).


  - Verification on the current worktree/build:


    - CPU/factory-startup render of `light-passes-test-v001.blend` frame 2 completed cleanly.


    - `.agent/check_deep_surface_compaction.py` now reports `overfragmented_pixels=0`


      (was `1` on the traced repro after the stale `v001` files were refreshed).


    - Updated Nuke AgX Punchy validation PNG written to


      `C:\tmp\current_deep_merge_agx_punchy.png`.


- **Deep EXR surface follow-up (2026-03-20, in progress):** The current worktree source is back on the older e720-like hard-surface path (the broad surface-coverage experiment is not the active code path). The next narrow fix only reconstructs front hard-surface prefixes and keeps volume suffix handling on the existing path.


- **Deep EXR:** Merged and complete. Code reviewed, all critical/high issues fixed.


- **Deep EXR front-prefix surface fix (2026-03-20):**


  - Implemented a narrow hard-surface-only follow-up on the current e720-like Deep EXR path.


  - Deep output now collapses single-active-sample pixels before export and reconstructs only the


    front opaque-surface prefix when later samples are volume-only, while leaving the suffix on the


    existing volume/scaled path.


  - Validation:


    - `D:\blender_projects\deep-branch-test.blend` rendered with `--factory-startup` on CPU.


    - `.agent/check_deep_single_surface_alpha.py C:\tmp\surface_compoutput_flat0001.exr C:\tmp\surface_compoutput_deep0001.exr`


      reports `checked_single_surface_fractional_pixels=6657`,


      `mismatching_single_surface_pixels=0`.


    - `.agent/check_deep_surface_front_alpha.py C:\tmp\surface_compoutput_deep0001.exr`


      reports `multi_sample_pixels=39349`, `violating_front_alpha_pixels=0`.


    - `D:\blender_projects\light-passes-test-v001.blend` mixed-case safety check remains clean:


      `.agent/check_deep_mixed_surface_volume_case1.py` reports `mismatching_pixels=0`.


    - On `light-passes-test-v001.blend`, the legacy front-alpha script still flags many pixels due


      to inactive zero deep samples, but a direct inspection ignoring inactive samples found


      `multi_active_pixels=0` and `violating_front_alpha_pixels=0` for that scene.


- **CUDA runtime kernel copy regression (2026-03-20):**


  - The fresh GPU illegal-address/OOM repro on `D:\blender_projects\light-passes-test-v001.blend`


    was traced to a **stale runtime CUDA cubin** issue, not the newest Deep EXR surface-coverage


    source logic.


  - Root cause: `intern/cycles/kernel/CMakeLists.txt` hooked


    `cycles_add_runtime_copy(...)` **before** `add_subdirectory(device/cuda)` /


    `add_subdirectory(device/optix)` created the backend kernel targets. The


    `if(TARGET ${target})` guard then failed during configure, so the local-build runtime copies


    into `bin/<config>/<version>/scripts/addons_core/cycles/lib` were never generated.


  - Symptom: host code from `ca742cc58b8`+ loaded old runtime cubins from 2026-03-17, producing a


    host/kernel layout mismatch around `kernel_params` and later `shader_eval_background` illegal


    addresses on CUDA.


  - Verification before the source fix: manually copying the freshly rebuilt


    `intern/cycles/kernel/device/cuda/kernel_sm_*.cubin.zst` files into the runtime `scripts`


    folder restored clean CUDA rendering on `light-passes-test-v001.blend` under


    `--factory-startup` with exactly one CUDA device enabled.


  - Verification after the source fix (2026-03-20):


    - Rebuilt `blender` in `E:\blender_modify\build_deep_surface_coverage`; build output now shows


      `Copying Cycles kernel binaries for cycles_kernel_cuda` and


      `Copying Cycles kernel binaries for cycles_kernel_optix`.


    - Clean repro rerun with `--factory-startup`, `compute_device_type='CUDA'`, and exactly one


      CUDA device enabled completed successfully on


      `D:\blender_projects\light-passes-test-v001.blend` frame 1.


    - Cycles reported `Path tracing on: NVIDIA GeForce RTX 4080 SUPER (CUDA)` and no illegal


      address / invalid-context failures occurred.


    - Runtime/intermediate CUDA cubin verification: `kernel_sm_89.cubin.zst` SHA256 matched between


      `intern/cycles/kernel/device/cuda/` and


      `bin/Release/5.2/scripts/addons_core/cycles/lib/`


      (`C84A8CF183A81EFE745605212D68FA00944A66F5BAD78B4BB10AC03C2B6B8FC0`).


- **Deep EXR memory:** Implemented user-controlled budget (default 1024 MB) + tile clamp; RenderResult deep storage skipped unless compositor needs it. Added tiled deep accumulation to avoid last-tile-only deep outputs. Merged into `vfx-rendering-branch` on 2026-02-22; code review checklist resolved.


- **Validation:** Rendered `D:\blender_projects\deep-branch-test.blend` on 2026-02-22; deep compositor outputs saved and tile rendering confirmed.


  Compositor deep EXRs now large (e.g., `test_compoutput_deep0001.exr` ~584 MB).


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

---

## 2026-03-25 Deep EXR Update

- Worktree/branch: `E:\blender_modify\blender_deep_surface_coverage` / `feature/deep-exr-surface-coverage`
- Build used: `E:\blender_modify\build_deep_surface_coverage\bin\Release\blender.exe`
- New Deep EXR direction landed locally:
  - add explicit metadata-based pixel classification in `intern/cycles/session/deep_output_driver.cpp`
  - only allow the hard-surface correction path for the proven safe **surface-front-prefix + volume-behind** case
  - keep all other mixed/volume cases on the legacy fallback path
- Important supporting fix kept:
  - direct generic early merge in `process_device_buffers()` remains disabled
- Verification on `D:\blender_projects\light-passes-test-v001.blend` with runtime RGBA deep override:
  - output deep: `D:\blender_projects\rendered\test\TempDeepRGBA\ViewLayer_Deep_v001_0002.exr`
  - Nuke preview: `C:\tmp\deep_test_direct_write1_case1_gate.png`
  - reference preview: `C:\tmp\old_scene_output_rgba_deep_0002_write1.png`
  - flatten-vs-flat check now passes:
    - `mean_abs_rgb=(8.509848703397438e-05, 7.660665141884238e-05, 9.596627205610275e-05)`
    - `pixels_gt_0.05=13`
    - `flatten_matches_flat=1`
- Residual mismatch pixels are only 13 tiny highlight/noise pixels around approximately:
  - `(1282-1287, 345-347)`
  - `(1271-1274, 488-490)`
- Current read: this is now a strong candidate for the remaining Deep EXR visual fix, with the previous broad volume regression no longer reproducing in the flatten-vs-flat check.

## 2026-03-25 Deep EXR Matrix Recheck Snapshot

- User requested a full recheck against `.agent/DEEP_EXR_TEST_MATRIX.md` before continuing.
- Fresh matched validation files were generated in `C:\tmp\` for:
  - CPU direct / compositor RGBA / compositor alpha-only
  - OptiX direct / compositor RGBA / compositor alpha-only
- Fresh Nuke previews exported to `C:\tmp\`:
  - `matrix_cpu_direct_preview.png`
  - `matrix_cpu_comp_rgba_preview.png`
  - `matrix_optix_direct_preview.png`
  - `matrix_optix_comp_rgba_preview.png`
- Current matrix result:
  - **Build:** pass
  - **CPU:** not acceptable yet
  - **OptiX:** current required script gate passes on the refreshed files
  - **Diagnostic front-alpha checker:** still crashes in the local OIIO/Python path (`EXIT:-1073741819`)
- Fresh CPU failure that now blocks full acceptance:
  - `check_deep_flatten_matches_flat.py C:\tmp\matrix_cpu_direct_flat_0002.exr C:\tmp\matrix_cpu_direct_deep_0002.exr`
    reports:
    - `mean_abs_rgb=(0.011504311114549637, 0.009117967449128628, 0.013690846040844917)`
    - `pixels_gt_0.05=173970`
    - `flatten_matches_flat=0`
  - `check_deep_flatten_matches_flat.py C:\tmp\matrix_cpu_comp_rgba_flat_0002.exr C:\tmp\matrix_cpu_comp_rgba_deep_0002.exr`
    reports:
    - `mean_abs_rgb=(0.011510949581861496, 0.009124506264925003, 0.013697427697479725)`
    - `pixels_gt_0.05=174058`
    - `flatten_matches_flat=0`
- Visual interpretation of the failing CPU pair:
  - the old white seam is not the dominant issue in the failing flatten check
  - mismatch is broad across the lit image, with strongest peaks in bright/specular regions
  - debug previews saved under `C:\tmp\cpu_flatten_debug\`
  - maximum sampled mismatch during flatten-vs-flat inspection was at pixel `(1466, 381)`, where
    flattened deep RGB was much brighter than the flat image in a hot highlight
- Approved next step after this snapshot commit: keep the current code state, then debug the
  **CPU-wide flatten-vs-flat RGB mismatch** without assuming the old seam bug has returned.

## 2026-03-25 Deep EXR Beauty / Sample-Count Capture Y Fix Checkpoint

- Worktree/branch: `E:\blender_modify\blender_deep_surface_coverage` /
  `feature/deep-exr-surface-coverage`
- Build used: `E:\blender_modify\build_deep_surface_coverage\bin\Release\blender.exe`
- Active source fix is in `intern/cycles/blender/output_driver.cpp`, not in the OpenEXR save path.
- Confirmed root cause:
  - full-frame Combined and Debug Sample Count caches were copied from render tiles using
    bottom-left tile coordinates
  - later deep reconstruction indexed those caches in top-to-bottom image coordinates
  - result: deep alpha/color reconstruction could read the vertically mirrored beauty/sample-count
    pixel and make edge samples falsely opaque
- Implemented fix:
  - cache tile rows into the full-frame Combined buffer with
    `dst_y = full_height - tile.offset.y - tile.size.y + y`
  - apply the same row mapping to the captured Debug Sample Count buffer
- Smoking-gun validation:
  - failing pixel `(1873,127)` was reading the mirrored flat pixel `(1873,952)` before the fix
  - flat at `(1873,127)`: `rgba=(0.0278625, 0.0278625, 0.0278625, 0.03125)`
  - mirrored flat at `(1873,952)`: `rgba≈(0.91455, 0.90918, 0.91895, 1.0)`
  - this exactly explained the old bogus `beauty_a=1` opaque deep result on the edge pixel
- Important correction:
  - the earlier save-path Y-flip hypothesis in `IMB_exr_save_deep(...)` was rejected/reverted
  - current checkpoint should treat the capture-side row-layout fix as the real landed fix
- Fresh locked-matrix outputs in `C:\tmp\`:
  - `matrix_cpu_direct_deep_0002.exr`
  - `matrix_cpu_comp_rgba_deep_0002.exr`
  - `matrix_optix_direct_deep_0002.exr`
  - `matrix_optix_comp_rgba_deep_0002.exr`
- Fresh Nuke previews in `C:\tmp\`:
  - `matrix_cpu_direct_preview.png`
  - `matrix_cpu_comp_rgba_preview.png`
  - `matrix_optix_direct_preview.png`
  - `matrix_optix_comp_rgba_preview.png`
- Current verification status on `D:\blender_projects\light-passes-test-v001.blend`:
  - `check_deep_single_surface_alpha.py`: **pass**
    - CPU direct / CPU comp RGBA / OptiX direct / OptiX comp RGBA
  - `check_deep_mixed_surface_volume_case1.py`: **pass**
    - CPU direct / CPU comp RGBA / OptiX direct / OptiX comp RGBA
  - `check_deep_flatten_matches_flat.py`:
    - CPU direct: **fail**, `mean_abs_rgb=(0.0115043, 0.0091180, 0.0136908)`,
      `pixels_gt_0.05=173970`
    - CPU comp RGBA: **fail**, `mean_abs_rgb=(0.0115109, 0.0091245, 0.0136974)`,
      `pixels_gt_0.05=174058`
    - OptiX direct: **pass**, `mean_abs_rgb=(0.0007401, 0.0006672, 0.0006575)`,
      `pixels_gt_0.05=916`
    - OptiX comp RGBA: **pass**, `mean_abs_rgb=(0.0007481, 0.0006748, 0.0006651)`,
      `pixels_gt_0.05=1038`
- Current checkpoint read:
  - the old opaque edge-alpha regression is fixed enough for the single-surface and mixed case-1
    checks to pass again
  - OptiX flatten-vs-flat is currently acceptable
  - the remaining blocker is now a **CPU-only broad flatten-vs-flat RGB mismatch**, strongest in
    brighter/specular regions, and should be debugged next without reviving the reverted writer-flip
    idea

## 2026-03-26 Deep EXR CPU Flatten RGB Root Cause / Fix

- Worktree/branch: `E:\blender_modify\blender_deep_surface_coverage` /
  `feature/deep-exr-surface-coverage`
- Clean code fix now isolated to:
  - `intern/cycles/kernel/integrator/shade_volume.h`
- Confirmed root cause of the CPU-wide flatten-vs-flat RGB mismatch:
  - deep surface sample indices were copied for surface direct-light shadow paths
  - but `integrate_volume_direct_light()` did **not** copy `deep_surface_sample_idx` into the
    spawned shadow path
  - later direct-light contributions seen through primary-transmit / volume-traversed paths then hit
    `sample_idx = DEEP_INVALID_SAMPLE_INDEX` and were silently dropped from deep surface RGB
- Smoking-gun trace result on the known bad compositor-RGBA crop pixel `(1013, 867)`:
  - before fix: many shadow contributions for the same camera sample arrived with
    `sample_idx=4294967295`
  - before fix: traced raw deep sample mean was `(~0.8876, ~0.8761, ~0.8990)`
  - after the one-line copy fix: invalid shadow events for that pixel dropped to zero
  - after fix: traced raw deep sample mean became `(~1.7610, ~1.6649, ~1.8544)`, matching the flat
    pixel and the previously-good OptiX raw mean
- Fresh full-frame verification on `D:\blender_projects\light-passes-test-v001.blend`:
  - CPU compositor RGBA deep:
    - deep: `C:\tmp\matrix_cpu_comp_rgba_deep_fix_0002.exr`
    - flat ref: `C:\tmp\matrix_cpu_comp_rgba_flat_0002.exr`
    - `mean_abs_rgb=(0.0007372, 0.0006663, 0.0006554)`
    - `pixels_gt_0.05=912`
    - `flatten_matches_flat=1`
  - OptiX compositor RGBA deep:
    - deep: `C:\tmp\matrix_optix_comp_rgba_deep_fix_0002.exr`
    - flat ref: `C:\tmp\matrix_optix_comp_rgba_flat_0002.exr`
    - `mean_abs_rgb=(0.0007401, 0.0006671, 0.0006574)`
    - `pixels_gt_0.05=916`
    - `flatten_matches_flat=1`
- Current status:
  - the previously-blocking CPU broad RGB mismatch is resolved by the `shade_volume.h` fix
  - debug trace instrumentation used to prove the bug has been removed again from the worktree
  - branch is **not committed yet**; only the minimal functional fix remains staged in the source

## 2026-03-26 Deep EXR Surface Metadata Widening / Cleanup

- Follow-up after code review:
  - widened hard-surface metadata from packed `uint64_t surface_key` to explicit
    `surface_object`, `surface_prim`, `surface_shader` 32-bit fields
  - removed the unused `deep_hash_uint32()` helper
- Important implementation note:
  - the widened metadata still fits in the existing 48-byte `KernelDeepSample` /
    `DeepSampleData` layout because the old layout had enough tail padding
  - result: large-scene object/shader collisions are removed **without increasing per-sample deep
    memory**
- Grouping/export behavior remains the same:
  - export-side opaque hard-surface grouping still merges by **object + shader** continuity plus
    normal similarity
  - `surface_prim` is now preserved explicitly for future exact-surface logic, but is not used as
    the current grouping discriminator
- Build verification:
  - `Release|x64` Blender build succeeded in
    `E:\blender_modify\build_deep_surface_coverage`
  - CPU/CUDA/OptiX kernel compilation completed successfully as part of that build
