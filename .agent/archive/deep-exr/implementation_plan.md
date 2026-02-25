# Deep EXR Implementation Plan

> **Status**: Milestones A, B, B7 Complete; Milestone C in progress (GPU validation pending)
> **Last Updated**: 2026-02-05

---

## Overview

Deep EXR output for Blender Cycles, enabling per-pixel depth samples for advanced compositing in Nuke and other deep-aware applications.

---

## Completed Milestones

### Milestone A: MVP on CPU ✅
- Deep sample buffers (DeepRenderBuffers)
- Kernel integration (deep_write.h)
- Surface and volume samples
- Alpha merging (Arnold-style)
- EXR writing (IMB_exr_save_deep)

### Milestone B: Deep EXR File Format ✅
- `R_IMF_IMTYPE_DEEP_EXR = 37` in DNA
- Format dropdown in Output Properties
- Auto-enable when DEEP_EXR selected
- Flat render suppression

### Milestone B7: Compositor Passthrough ✅ (Partial)
- RenderResult deep data fields
- Pipeline integration (pipeline.cc)
- FileOutputOperation execute_deep_exr()
- Auto-enable from compositor detection
- **BUG**: Volume alpha has black holes

---

## Current Focus: Volume Alpha Black Holes

**Hypothesis**: The speckled black hole pattern is caused by:
1.  **Double Merging** (on CPU): Caching fix addresses this.
2.  **Numerical Instability**: Dividing by very small alpha in Deep Recolor.
3.  **Incomplete Beauty Buffer**: Tile accumulation was broken.

**Implementation Plan (Active):**
- [x] **Caching**: `DeepOutputDriver` now caches processed data.
- [x] **Robust Recolor**:
    - Add `1e-6` alpha threshold for division.
    - ~~Clamp unpremultiplied RGB to `[0, 10]`~~ (Removed per user request for data precision).
- [x] **Tile Accumulation**: `BlenderOutputDriver` now correctly accumulates tiles into a full-frame beauty buffer.

**Verification**:
1.  Run direct output test (`test_direct_deep.py`).
2.  Check for holes in Nuke.
3.  Run compositor test.

---

## Future: Milestone C (OptiX GPU)

**Scope**
- Backend: OptiX only (NVIDIA).
- Context: offline/final renders only (includes compositor DEEP_EXR File Output).
- Multi-device: full MultiDevice support (CPU+GPU, mixed backends).
- Memory strategy: slice-aligned deep buffers per device.
- OOM behavior: fail render with clear error (no partial fallback).

**Public API / Interface Changes**
- `PathTraceWork`: add getters for effective buffer params (slice info). **DONE**
- `PathTrace`: per-device deep buffer sync hook after rebalance. **DONE**
- `DeepOutputDriver`: per-device buffer management + per-device kernel sync. **DONE**

**Implementation Steps**
1. Expose per-work slice info via `PathTraceWork` getters. **DONE**
2. Add a PathTrace hook to sync deep buffers after rebalance. **DONE**
3. Allocate one `DeepRenderBuffers` per work/device and store its slice. **DONE**
4. Push per-device `KernelData` with correct deep pointers via `device->const_copy_to("data", ...)`. **DONE**
5. Merge per-device deep data in `ensure_processed_cache()` using slice ranges (warn on overlap). **DONE**
6. Keep existing deep recolor/alpha scaling logic using Combined beauty buffer. **DONE**
7. Ensure per-device deep buffers are freed with `DeepOutputDriver` teardown. **DONE**
8. Estimate deep buffer size per device before allocation; fail early if clearly insufficient. **DONE**
9. Handle slice changes (rebalance) without losing deep history. **DONE**

**Tests**
- OptiX single GPU: direct DEEP_EXR + compositor DEEP_EXR, verify via `oiio_deep_info.py`.
- OptiX + compositor: verify channel set (A/Z/ZBack vs RGBA/Z/ZBack).
- MultiDevice (CPU+GPU): ensure deep output exists and sample counts are non-zero in slices.
- OOM case: confirm render fails and reports error.
- Slice boundary sanity: render a volume spanning device slice boundary; verify no seams.

**Assumptions**
- Deep buffer pointers are per-device and must be written into each device's constant memory.
- Work slices do not overlap (warn if overlap is detected during merge).
- Explicitly documented exclusions: no HIP/Metal/oneAPI GPU support in Milestone C.

---

## Key Files

| Component | Files |
|-----------|-------|
| Buffers | `deep_buffers.h/cpp` |
| Driver | `deep_output_driver.h/cpp` |
| Kernel | `deep_write.h`, `shade_surface.h`, `shade_volume.h` |
| EXR | `openexr_api.cpp` |
| Pipeline | `pipeline.cc`, `RE_pipeline.h` |
| Compositor | `node_composite_file_output.cc`, `COM_render_context.hh` |
