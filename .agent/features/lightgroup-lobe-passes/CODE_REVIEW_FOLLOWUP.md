# Lightgroup Lobe Passes — Review of Uncommitted Bug Fix

**Branch**: `feature/per-lightgroup-lobe-passes`  
**Worktree**: `E:\blender_modify\blender_lobe_passes`  
**Date**: 2026-03-19  
**Scope**: Read-only review of uncommitted changes — no code modifications made.

---

## Summary of Changes

The bug fix introduces a **split lightgroup index map** to solve out-of-bounds buffer writes when only a subset of lightgroups have lobe-split passes enabled.

Previously, `film_write_lightgroup_pass` used the raw `lightgroup` index to offset into the render buffer for split lobe passes. If lightgroup indices were sparse (e.g., groups 0, 5, 10), the buffer offset could exceed the allocated space. The fix adds an indirection map that remaps lightgroup indices to a compact sequential range.

---

## Files Changed

| File | Lines | Change |
|------|-------|--------|
| [data_template.h](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/kernel/data_template.h) | +3 | Added `lightgroup_split_index_count` and `lightgroup_split_index_ptr` to kernel film struct |
| [light_passes.h](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/kernel/film/light_passes.h) | +78 / -34 | New `film_get_split_lightgroup_index` + `film_write_lightgroup_split_pass`; replaced all split-lobe calls |
| [devicescene.cpp](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/scene/devicescene.cpp) | +1 | Added `lightgroup_split_index` device vector init |
| [devicescene.h](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/scene/devicescene.h) | +1 | Added `device_vector<int> lightgroup_split_index` member |
| [film.cpp](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/scene/film.cpp) | +54 | New `lightgroup_split_index_map()` + GPU upload + kernel data setup |

**No merge conflicts found.**

---

## Issues

### 1. ⚠️ Mixed Line Endings

All 5 files have Git warnings:
```
warning: LF will be replaced by CRLF the next time Git touches it
```

New code is LF while existing code is CRLF. Normalize before commit.

---

### 2. ⚠️ `nullptr` Check in Kernel Code

In [light_passes.h](file:///E:/blender_modify/blender_lobe_passes/intern/cycles/kernel/film/light_passes.h), `film_get_split_lightgroup_index`:

```cpp
ccl_global const int *split_index_map = (ccl_global const int *)
    kernel_data.film.lightgroup_split_index_ptr;
if (split_index_map == nullptr) {
    return LIGHTGROUP_NONE;
}
```

**Concern**: Using `nullptr` in GPU kernel code. On some GPU backends (CUDA, HIP, Metal), null pointer checks on device pointers may behave differently. Cycles typically uses `kernel_data.film.*_ptr == 0` (integer comparison) rather than casting to a pointer and checking `nullptr`. Consider:

```cpp
if (kernel_data.film.lightgroup_split_index_ptr == 0) {
    return LIGHTGROUP_NONE;
}
```

This is consistent with how deep EXR buffer pointers are checked elsewhere.

---

### 3. 🔵 C-Style Cast for Device Pointer

```cpp
ccl_global const int *split_index_map = (ccl_global const int *)
    kernel_data.film.lightgroup_split_index_ptr;
```

This is a C-style cast from `uint64_t` to `ccl_global const int *`. While this is idiomatic in Cycles kernel code (matching other pointer casts like deep buffer pointers), it's worth noting for consistency. Other Cycles code uses the same pattern, so this is acceptable.

---

### 4. 🔵 `<cstring>` Include in `film.cpp`

```cpp
#include <cstring>
```

Added for `memcpy`. While correct, Blender/Cycles typically uses its own `util/` headers or the existing `<string.h>` already pulled in transitively. This is not wrong, just worth checking whether `memcpy` is already available through existing includes.

---

## Design Review

### `lightgroup_split_index_map()` in `film.cpp` — ✅ Correct

The function:
1. Iterates all passes to find lightgroups that have lobe-split passes
2. Builds a compact index: only lightgroups with actual split passes get a sequential index
3. Lightgroups without splits get `-1` (mapped to `LIGHTGROUP_NONE` by the kernel)

This correctly solves the buffer offset problem.

### `film_write_lightgroup_split_pass()` — ✅ Correct

- Returns early when `split_lightgroup == LIGHTGROUP_NONE`
- Returns early when `pass_offset == PASS_UNUSED`
- Uses the remapped index for the buffer offset
- Clean wrapping of `film_write_pass_spectrum`

### `KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE` — ✅ Correct

The `uint64_t lightgroup_split_index_ptr` correctly uses `KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE` to prevent specialization of the following 64-bit pointer, matching the pattern used by deep buffer pointers.

### Device Memory Lifecycle — ✅ Correct

- Allocated in `devicescene.cpp` constructor
- Populated and uploaded in `film.cpp` `device_update`
- Freed when `split_index_map` is empty via `.free()`
- `.clear_modified()` called after upload

---

## Summary

| Category | Status |
|----------|--------|
| Merge Conflicts | ✅ None |
| Line Endings | ⚠️ LF in new code (5 files) |
| `nullptr` in kernel | ⚠️ Use integer `== 0` check instead |
| Design & Correctness | ✅ Sound approach |
| Buffer Safety | ✅ Proper bounds checking |

> [!IMPORTANT]
> The fix correctly solves the out-of-bounds write bug. Two items to address before commit: normalize line endings, and consider using integer `== 0` check instead of `nullptr` for the GPU pointer.
