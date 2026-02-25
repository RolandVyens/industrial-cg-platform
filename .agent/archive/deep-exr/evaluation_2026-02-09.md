# Deep EXR Uncommitted Code Evaluation

> **Date:** 2026-02-09  
> **Status:** Read-only evaluation (no changes made)

---

## Summary

The uncommitted changes consist of **4 modified files** with improvements to volume deep alpha handling, sample merging, and recolor processing. Overall, **the direction is clear and workable**, but there are some concerns worth discussing before committing.

---

## Files Changed (Uncommitted)

| File | Lines Changed | Purpose |
|------|---------------|---------|
| `shade_volume.h` | +117/-62 | Ray-marching deep sample logic overhaul |
| `deep_output_driver.cpp` | +23 | Beauty unpremultiply + linear alpha fallback |
| `deep_buffers.cpp` | +9/-1 | Skip merging volume segments |
| `session.cpp` | +8 | Auto-increase deep samples for volume scenes |

---

## Detailed Analysis

### 1. `shade_volume.h` - Ray-Marching Deep Sample Logic

**Changes:**
- Added `write_deep` guard, checked once per ray
- Introduced `deep_only` mode: after both scatter events, skip shading but continue recording deep samples
- Changed deep alpha calculation from **throughput ratio** to **transmittance-only** (`1 - avg(transmittance)`)
- Modified loop exit condition to continue for deep recording even after scatter

**Assessment:**
| Aspect | Rating | Notes |
|--------|--------|-------|
| **Clarity** | ⚠️ Moderate | The `do_shading` / `deep_only` branching adds complexity. The logic is correct but could benefit from inline comments explaining the state machine. |
| **Correctness** | ✅ Good | Transmittance-only alpha avoids the throughput rescaling artifacts (donut/ring artifacts). This matches AGENT_HANDOFF notes about ratio-tracking issues. |
| **Performance** | ⚠️ Concern | The loop now continues past scatter events when `write_deep == true`. This means more iterations for deep output. Should be acceptable since it only affects camera rays (bounce == 0). |
| **Risk** | 🔴 Medium | The `break` vs `continue` logic is subtle. The condition `!write_deep || throughput_zero || !(direct_scatter && indirect_scatter)` could have edge cases. Recommend adding test coverage. |

**Suggestion:** Add a brief comment block at the top explaining:
1. Why we continue looping after scatter (deep recording)
2. Why we use transmittance-only (not throughput ratio)

---

### 2. `deep_output_driver.cpp` - Beauty Unpremultiply + Linear Fallback

**Changes:**
- **Unpremultiply beauty RGB** before per-sample alpha association
- **Linear alpha fallback** for near-transparent pixels (`target_alpha < 0.1` or `deep_alpha < 0.1`)

**Assessment:**
| Aspect | Rating | Notes |
|--------|--------|-------|
| **Clarity** | ✅ Good | The unpremultiply block is well-structured with clear threshold checks |
| **Correctness** | ✅ Good | Fixes significant RGB mismatch for volume scenes (as noted in AGENT_HANDOFF) |
| **Edge Cases** | ⚠️ Concern | The `0.1` threshold for linear fallback is hardcoded. Consider making this a tunable parameter or match the `alpha_merge_tolerance`. |

**Observation:** The log-based alpha scaling can produce haloing on near-transparent pixels. The linear fallback is a valid workaround. The threshold `0.1` seems empirically chosen.

---

### 3. `deep_buffers.cpp` - Skip Merging Volume Segments

**Changes:**
- Detect volumetric samples by `z_back > z + 1e-6f`
- Skip merging if either current or previous sample is volumetric

**Assessment:**
| Aspect | Rating | Notes |
|--------|--------|-------|
| **Clarity** | ✅ Good | The volume detection heuristic is simple and documented |
| **Correctness** | ✅ Good | Prevents halo artifacts from merging volume segments with different depth extents |
| **Unused Variables** | 🔴 Bug | Lines 220-221 remove `counts` and `offset` declarations but they are likely still used in the prefix sum computation below. This could cause a build error or undefined behavior. |

**Action Needed:** Verify the `compute_sample_offsets()` function compiles correctly. The diff shows variables being removed but doesn't show if they're truly unused.

---

### 4. `session.cpp` - Auto-Increase Deep Samples for Volume

**Changes:**
- When `volume_ray_marching()` is enabled, ensure `max_deep_samples >= min(volume_max_steps, 256)`

**Assessment:**
| Aspect | Rating | Notes |
|--------|--------|-------|
| **Clarity** | ✅ Good | Clear intent and bounds checking |
| **Correctness** | ✅ Good | Prevents under-allocation for volume scenes |
| **User Expectation** | ⚠️ Info | This silently increases memory usage. Users might be surprised if they set a low deep sample count expecting lower memory. Consider logging when override happens. |

---

## Overall Direction Assessment

| Category | Assessment |
|----------|------------|
| **Direction** | ✅ **Clear and Workable** |
| **Goal Alignment** | ✅ Addresses known issues from AGENT_HANDOFF (donut artifacts, RGB mismatch, halo on merge) |
| **Code Quality** | ⚠️ Moderate - some complexity in `shade_volume.h` branching |
| **Test Coverage** | ❓ Unknown - no automated tests visible for verification |
| **Build Status** | ❓ Needs verification - possible unused variable issue in `deep_buffers.cpp` |

---

## Recommendations

1. **Verify Build** - Compile to ensure `deep_buffers.cpp` changes are valid (unused variable removal)

2. **Add Comments** - The `shade_volume.h` changes are algorithmically complex; inline documentation would help future maintainers

3. **Consider Constants** - The `0.1` threshold in `deep_output_driver.cpp` and `1e-6f` epsilon in multiple files should be named constants or configuration values

4. **Test Before Commit** - Run the existing verification:
   ```bash
   # Build
   cmake --build E:\blender_modify\build_windows_x64_vc17_Release --target blender --config Release
   
   # Render test scene
   blender.exe -b "D:\blender_projects\test_volume_alpha_deep.blend" --python-expr "..." -f 1
   
   # Alpha diff
   python deep_alpha_diff.py --flat ... --deep ...
   ```

5. **Document the Changes** - Update AGENT_HANDOFF.md and TASK.md after confirming the changes work

---

## Questions for Decision

1. Should the `0.1` linear fallback threshold be configurable via UI or remain hardcoded?

2. Is the performance impact of continuing the ray-march loop after scatter acceptable for production scenes?

3. Should we add a stdout log when auto-increasing deep samples for volume scenes?

---

## Conclusion

**The current uncommitted code direction is clear and addresses real issues documented in the project history.** The implementation approach is sound. The main concerns are:
- Minor complexity in `shade_volume.h` that could use better documentation
- Potential build issue with unused variable removal in `deep_buffers.cpp`
- Hardcoded thresholds that might benefit from being named constants

**Recommendation:** Proceed with testing (build + render verification) before committing.
