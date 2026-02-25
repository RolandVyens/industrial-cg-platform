# Shadow Color Branch â€?Code Review Report

> **Branch:** `feature/shadow-color`
> **Commit:** `f113ec6adcc5` ¡ª *Cycles: add per-light and world shadow color*
> **Diff:** 16 files changed, +122 / -5 lines
> **Reviewed:** 2026-02-20

---

## Overview

The feature adds a per-light and per-world shadow color property to Blender Cycles.
When shadows are cast, occluded regions are tinted toward the configured color instead
of going fully dark. Default `{0,0,0}` preserves existing behavior.

**Functional status:** CPU and GPU/OptiX verified by user on 2026-02-19.
**Review status:** All MUST-FIX and NICE-TO-HAVE items addressed, except optional duplication note.

---

## Files Changed

| File | Layer | Change |
|------|-------|--------|
| `source/blender/makesdna/DNA_light_types.h` | DNA | `shadow_color[3]` + `_pad3` |
| `source/blender/makesrna/intern/rna_light.cc` | RNA | `PROP_COLOR` property |
| `source/blender/blenloader/intern/versioning_510.cc` | Versioning | Default `{0,0,0}` at (502,5) |
| `source/blender/blenkernel/BKE_blender_version.h` | Version | Subversion 4 â†?5 |
| `intern/cycles/scene/light.h` | Cycles Scene | `NODE_SOCKET_API(float3, shadow_color)` |
| `intern/cycles/scene/light.cpp` | Cycles Scene | Socket registration + device update |
| `intern/cycles/kernel/types.h` | Kernel | `KernelLight.shadow_color[3]` + padding |
| `intern/cycles/blender/light.cpp` | Sync | Per-light + world background sync |
| `intern/cycles/kernel/integrator/shade_shadow.h` | Kernel | Shadow color tinting logic |
| `intern/cycles/kernel/integrator/intersect_shadow.h` | Kernel | Opaque-hit redirect for shadow color |
| `intern/cycles/kernel/integrator/shade_light.h` | Kernel | NEE unshadowed_throughput update |
| `intern/cycles/kernel/integrator/shade_surface.h` | Kernel | Init unshadowed_throughput |
| `intern/cycles/kernel/integrator/shade_volume.h` | Kernel | Init unshadowed_throughput |
| `intern/cycles/kernel/integrator/shadow_state_template.h` | Kernel | Widen feature gate to PATH_TRACING |
| `intern/cycles/blender/addon/properties.py` | Addon | `CyclesWorldSettings.shadow_color` |
| `intern/cycles/blender/addon/ui.py` | Addon | Light + world settings UI |
| `scripts/startup/bl_ui/properties_data_light.py` | Blender UI | EEVEE shadow panel |

---

## Findings

### MEDIUM â€?Must Fix Before PR

#### M1. Commit Message Missing Body ¡ª **Fixed**

**Standard:** [Blender Commit Message Guidelines](https://developer.blender.org/docs/handbook/guidelines/commit_messages/) â€?*"More user level information about how this feature works"*

**Current:**
```
Cycles: add shadow color for lights and world
```

**Fix â€?rewrite to:**
```
Cycles: add per-light and world shadow color

Add a shadow color property to lights and the world background light.
When a shadow is cast, the occluded region is tinted toward the
configured color instead of going fully dark. Default black preserves
existing behavior.

The tinting is applied in integrator_shade_shadow() using the ratio
of shadowed to unshadowed throughput. For fully opaque hits, the
shadow path is redirected from intersect_shadow to shade_shadow
so the tint can still be applied.
```

---

#### M2. Missing Algorithm Comments in Kernel ¡ª **Fixed**

**Standard:** [Blender C/C++ Style â€?Comments](https://wiki.blender.org/wiki/Style_Guide/C_Cpp#comments) â€?*"Be sure to explain non-obvious algorithms, hidden assumptions, implicit dependencies."*

**File:** `intern/cycles/kernel/integrator/shade_shadow.h` (lines 172â€?24)

The transmittance ratio computation and tinting formula have no explanatory comment.

**Fix â€?add before line 172:**
```cpp
  /* Shadow color tinting.
   * Look up the shadow color from the light that cast this shadow ray. If set,
   * compute the transmittance as the ratio of shadowed to unshadowed throughput,
   * then linearly interpolate: full occlusion (T=0) yields shadow_color, no
   * occlusion (T=1) yields white. Formula: tinted = T + (1 - T) * shadow_color. */
```

**File:** `intern/cycles/kernel/integrator/intersect_shadow.h` (lines 171â€?94)

The opaque-hit redirect has no comment explaining *why* we don't just terminate.

**Fix â€?add before line 172:**
```cpp
    /* If the light has a non-zero shadow color, redirect to shade_shadow
     * even on opaque hits instead of terminating. Set throughput to zero
     * to signal full occlusion; shade_shadow will apply the shadow color. */
```

---

#### M3. EEVEE Panel Shows Non-Functional Property ¡ª **Fixed**

**File:** `scripts/startup/bl_ui/properties_data_light.py` (lines 186â€?87)

`shadow_color` is shown in `DATA_PT_EEVEE_light_shadow` (COMPAT_ENGINES = `BLENDER_EEVEE`), but EEVEE does not implement shadow color tinting. This will confuse users.

**Fix â€?remove from EEVEE panel:**
```diff
-        col = layout.column()
-        col.prop(light, "shadow_color", text="Color")
-
```

The property is already correctly shown in the Cycles addon UI (`CYCLES_LIGHT_PT_settings`), which is sufficient.

---

### LOW â€?Nice to Have

#### L1. DNA Field Missing Inline Comment ¡ª **Fixed**

**File:** `source/blender/makesdna/DNA_light_types.h` (line 116)

**Current:**
```cpp
  float shadow_color[3] = {0.0f, 0.0f, 0.0f};
  float _pad3 = 0.0f;
```

**Preferred:** Add a brief comment per Blender convention:
```cpp
  /* Shadow tint color, black means no tinting. */
  float shadow_color[3] = {0.0f, 0.0f, 0.0f};
  float _pad3 = 0.0f;
```

---

#### L2. Duplicated Shadow Color Reading in Kernel

**Files:** `intersect_shadow.h` (lines 180â€?83) and `shade_shadow.h` (lines 184â€?87)

The same 4-line block reads and clamps shadow_color from `KernelLight`. A small helper
function (e.g. `kernel_light_shadow_color()`) could reduce duplication, but Cycles kernel
code routinely inlines for performance. Acceptable as-is.

---

#### L3. RNA Property Missing Explicit Range ¡ª **Fixed**

**File:** `source/blender/makesrna/intern/rna_light.cc` (lines 349â€?53)

Added `RNA_def_property_range(prop, 0.0f, 1.0f)` for explicit clamping.

---

## Passes (No Issues)

| Check | Status |
|-------|--------|
| Line length â‰?100 chars | âœ?All lines within limit |
| K&R braces in kernel code | âœ?Correct |
| Allman braces in Blender source | âœ?Correct |
| `snake_case` naming | âœ?`shadow_color` throughout |
| Versioning block with `MAIN_VERSION_FILE_ATLEAST` | âœ?Correct at (502, 5) |
| `BLENDER_FILE_SUBVERSION` bumped | âœ?4 â†?5 |
| DNA padding for 16-byte alignment | âœ?`_pad3` added |
| KernelLight padding for GPU alignment | âœ?`pad` â†?`pad[2]` |
| Cycles socket registration (`SOCKET_COLOR`) | âœ?|
| Portal lights zero-initialized shadow_color | âœ?Explicit `{0,0,0}` |
| Addon `FloatVectorProperty` with correct subtype | âœ?`subtype='COLOR'` |
| `unshadowed_throughput` feature gate broadened | âœ?`KERNEL_FEATURE_PATH_TRACING` |
| Tinting math correctness | âœ?`T + (1-T)*C` verified |
| World background shadow color sync | âœ?Via `get_float3(cworld, "shadow_color")` |

---

## Action Items

- [x] **M1.** Rewrite commit message with body text
- [x] **M2.** Add algorithm comments in `shade_shadow.h` and `intersect_shadow.h`
- [x] **M3.** Remove `shadow_color` from EEVEE panel in `properties_data_light.py`
- [x] **L1.** Add inline comment in `DNA_light_types.h` (optional)
- [ ] **L2.** Consider helper for duplicated kernel code (optional)
- [x] **L3.** Consider explicit RNA range (optional)



