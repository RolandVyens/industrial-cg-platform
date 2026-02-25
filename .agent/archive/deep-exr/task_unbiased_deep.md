# Deep EXR - Unbiased Volume Support

## Task Overview
Implement deep EXR output for unbiased (null scattering) volume rendering in Cycles.

---

## Current Status

### ✅ Completed (Biased Volumes)
- [x] Deep samples written during ray marching steps (`shade_volume.h:2382-2435`)
- [x] Per-segment alpha with optical density normalization
- [x] Arnold-style sample merging
- [x] Nuke-compatible deep EXR output

### ✅ Completed (Unbiased Volumes)
- [x] Research null scattering integration path
- [x] Design deep sample collection strategy
- [x] Implement transmittance-based deep sampling
- [x] Test with unbiased volumes
- [x] Verify in Nuke ✅

**Test Results:**
| Mode | File Size | Status |
|------|-----------|--------|
| Unbiased (before fix) | 42.15 MB | ❌ No volumes |
| Unbiased (after fix) | 93.98 MB | ✅ Volumes captured |
| Biased | 532.12 MB | ✅ Volumes captured |

---

## Technical Analysis

### Problem
Unbiased (null scattering) volume rendering uses **probabilistic delta tracking** instead of discrete ray marching steps. There are no natural "front/back" depth segments to write as deep samples.

### Key Differences

| Aspect | Biased (Ray Marching) | Unbiased (Null Scattering) |
|--------|----------------------|---------------------------|
| Integration | Discrete steps | Probabilistic sampling |
| Segments | Clear front/back Z | No discrete segments |
| Transmittance | Per-step accumulation | Stochastic estimation |
| Deep Output | ✅ Natural fit | ❌ Requires adaptation |

### Solution Approach: Transmittance Slicing

Generate synthetic deep samples by:
1. **Divide volume into depth slices** at regular intervals (e.g., every 0.1 units)
2. **Track accumulated transmittance** at each slice boundary
3. **Write deep samples** for each slice with alpha = (1 - transmittance_ratio)

This is similar to how Mantra and other production renderers handle deep volumes with stochastic integrators.

---

## Implementation Files

| File | Change |
|------|--------|
| `shade_volume.h` | Add deep sample collection in `volume_integrate_heterogeneous` |
| `film/deep_sample.h` | Add helper for transmittance-to-alpha conversion |
| `types.h` (kernel) | Add deep volume slice configuration |

