# Milestone C (OptiX GPU) - Review & Recommendations

> **Date**: 2026-02-05  
> **Status**: Pre-implementation review

---

## Strengths ✅

1. **Clear scope boundaries** - OptiX-only, offline renders, explicit OOM policy
2. **Per-device isolation** - Separate `DeepRenderBuffers` per work/device
3. **Slice-based merge** - Non-overlapping slices simplify the final stitch
4. **Reuses existing logic** - Deep recolor/alpha scaling stays in `ensure_processed_cache()`

---

## Areas of Concern ⚠️

| Issue | Risk | Recommendation |
|-------|------|----------------|
| Memory estimation missing | High | Add upfront memory check before allocating deep buffers |
| No partial-frame fallback | Medium | Consider graceful degradation if OOM |
| Slice boundary artifacts | Medium | Verify volumes spanning slices don't cause seams |
| Timing of buffer setup | Medium | Ensure setup happens after slice params are finalized |
| No HIP/Metal mentioned | Low | Document exclusion to set expectations |

---

## Missing Implementation Details

### 1. `foreach_work()` API Design

Need to expose:
- Device type
- Slice rect
- Device memory handle

Suggested getters:
```cpp
work->get_device()
work->get_effective_buffer_params()
work->get_slice_rect()
```

### 2. Per-device `const_copy_to` Timing

- Must happen **after** each device's deep buffer is allocated
- Must happen **before** that device starts rendering
- Integrate into `PathTraceWork::init_execution()` or similar

### 3. Merge Algorithm

```
For each pixel (x, y):
  Find which device owns this pixel (exactly one)
  Copy samples from that device's buffer to final output
```

Assert if no device claims a pixel (shouldn't happen).

### 4. Device Memory Cleanup

When are per-device deep buffers freed?
- Suggested: free in `PathTraceWork::finalize()` or when `DeepOutputDriver` is destroyed

---

## Suggested Additions to Plan

### Memory Management

```cpp
// Estimate deep buffer size before allocation
size_t deep_size = width * height * max_samples * sizeof(DeepSampleData);  // 32 bytes

// Query device available memory
size_t mem_free = device->stats.mem_free;

// Fail early if insufficient
if (deep_size > 0.8 * mem_free) {
  // Report: "Insufficient GPU memory for deep output"
}
```

### Slice Ownership

- Store `{x_start, y_start, x_end, y_end}` per device
- Assert slices are non-overlapping in `ensure_processed_cache()`
- Merge: iterate pixels, lookup device by (x, y), copy samples

### Testing: Slice Boundary

- Render a volume that spans the slice boundary between devices
- Verify no seams in flattened deep output

---

## Open Questions

1. **Priority**: Is multi-device (CPU+GPU) must-have, or can it be deferred?
2. **Viewport deep output**: Ever a goal, or strictly offline?
3. **Adaptive sampling**: How does it interact with `deep_max_samples_per_pixel`?

---

## Recommended Implementation Order

1. **Phase 1**: Single OptiX GPU (no multi-device)
   - Validate deep buffer allocation on GPU
   - Verify kernel writes to GPU deep buffers
   - Copy back and export

2. **Phase 2**: Multi-device support
   - Per-device buffer management
   - Slice-aware merge
   - CPU+GPU mixed rendering

3. **Phase 3**: Polish
   - Memory estimation and early fail
   - User-facing error messages
   - Documentation updates
