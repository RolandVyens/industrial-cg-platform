# Deep EXR Implementation — Blender Standards Code Review

> **Date:** 2026-02-12 (Updated after Round 3 fixes)  
> **Scope:** Full diff `main..feature/deep-exr-output` (~50 files)  
> **Against:** [Blender C/C++ Style Guide](https://developer.blender.org/docs/handbook/guidelines/c_cpp/)

---

## Executive Summary

The Deep EXR implementation is **feature-complete and functionally correct**. After three rounds of fixes (11 fix commits), the code now **fully conforms to Blender's coding standards**. All originally identified issues have been addressed. The only remaining item is git history cleanup (agent markdown file deletions visible in diff), which requires an interactive rebase before upstream submission.

---

## ✅ Resolved Issues (Fixed in Round 1-2)

### 1. ~~Type-Erased `void*`~~ → ✅ Fixed
New `RE_deep_data.hh` defines typed `RenderDeepData` struct. All `void*` usage replaced across `RE_pipeline.h`, `COM_render_context.hh`, `render_result.cc`, `session.cpp`, `node_composite_file_output.cc`.

### 2. ~~Duplicate Deep Sample Structs~~ → ✅ Fixed
`DeepSampleExport` eliminated entirely. `DeepSample` unified in new `IMB_deep_sample.hh` (single definition). `DeepSampleData` (device-side) and `KernelDeepSample` (GPU kernel) remain separate by necessity (alignment/device_vector requirements).

### 5. ~~`ensure_processed_cache()` 200+ Lines~~ → ✅ Fixed
Decomposed into 8 focused helpers: `process_device_buffers()`, `init_processed_cache()`, `merge_slice_into_cache()`, `populate_pixel_samples()`, `get_beauty_pixel()`, `compute_scaled_alphas()`, `ensure_processed_cache()` (now ~80 lines).

### 6. ~~`sync_device_buffers()` 295 Lines~~ → ✅ Fixed
Decomposed into 6 methods: `layout_matches()`, `compute_deep_bytes()`, `build_device_estimates()`, `check_device_memory()`, `snapshot_device_buffers()`, `init_device_buffers()`, `restore_snapshots()`. The orchestrator `sync_device_buffers()` is now ~25 lines.

### 7. ~~Hardcoded Magic Numbers~~ → ✅ Fixed
Named `constexpr` constants in anonymous namespace: `deep_alpha_epsilon`, `deep_alpha_linear_fallback`, `deep_alpha_log_min_transparency`, `deep_memory_headroom_bytes`.

### 8. ~~Comment Style~~ → ✅ Fixed
Comment punctuation corrected across `deep_output_driver.cpp`, `deep_buffers.h/cpp`. Doxygen `\name` sections added to `deep_buffers.h`.

### 9. ~~`std::vector` vs Blender Containers~~ → ✅ Fixed
Private members in `deep_output_driver.h` now use `ccl::vector` (`beauty_buffer_`, `device_buffers_`, snapshot members). `std::vector` remains only where required for API boundaries (e.g., `std::unique_ptr<std::vector<...>>` for processed cache).

### 12. ~~`kCamelCase` Constants~~ → ✅ Fixed
`kDefaultDeepMergeTolerance` removed entirely.

### 11. ~~Section Headers for Deep Blocks~~ → ✅ Fixed
Added in commit `Cycles: add deep output section headers`.

### 14. ~~`deep_alpha_merge_tolerance` Not in UI~~ → ✅ Fixed
Now exposed in `RENDER_PT_output_deep_exr` panel alongside `deep_merge_tolerance`.

### 15. ~~Double Namespace Block in `session.h`~~ → ✅ Fixed
Consolidated into a single `CCL_NAMESPACE_BEGIN/END` block. `BlenderOutputDriver` forward-declared inline.

### 16. ~~Unused `path_trace_work.h` Accessors~~ → ✅ Fixed
`get_effective_full_params()` and `get_effective_big_tile_params()` removed. Only `get_effective_buffer_params()` retained.

---

## 🔴 Remaining Critical

### 3. ~~`.gitignore` Dev-Doc Patterns~~ → ✅ Fixed
All dev-doc patterns moved to `.git/info/exclude`. The `.gitignore` diff now only contains a trailing newline.

### 4. Deleted Agent Markdown Files in Git History

Still present as deletions in the commit history (`AGENT.md`, `CONTEXT_INDEX.md`, `DEEP_INTEGRATION_DESIGN.md`). Requires interactive rebase to squash or remove before upstream submission.

---

## Summary Table (Final — Round 3)

| Category | Original | Remaining | Status |
|----------|----------|-----------|--------|
| 🔴 Critical | 4 | 1 | Git history cleanup only (agent `.md` deletions in diff) |
| 🟡 Moderate | 6 | 0 | All resolved |
| 🟢 Minor | 7 | 0 | All resolved |

### Pre-Upstream Submission Checklist
1. Interactive rebase to squash/remove agent markdown file add+delete from git history
2. Verify CRLF→LF on all new files (git will auto-normalize on commit if `.gitattributes` is set)
