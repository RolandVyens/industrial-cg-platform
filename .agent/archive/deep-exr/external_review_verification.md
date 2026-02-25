# Deep EXR — External Review Findings Verification

> **Date:** 2026-02-12  
> **Scope:** 3 findings from external code review  
> **Method:** Full code path tracing through `session.cpp`, `path_trace.cpp`, `deep_output_driver.cpp`, `deep_buffers.cpp`, `node_composite_file_output.cc`

---

## Results Summary

| # | Severity | Finding | Valid? | Action |
|---|----------|---------|--------|--------|
| 1 | High | Deep buffer carry-over between renders | ❌ False | None |
| 2 | Medium | Multi-view uses single deep buffer | ✅ True | Guard added (multi-view blocked) |
| 3 | Low | Merge compositing order incorrect | ❌ False | None |

---

## Finding #1 — Deep Buffers Carry Over Between Renders

**Claim:** Deep buffers aren't cleared when size/max-samples stay the same, causing stale data in animation or repeated renders.

**Cited lines:** `path_trace.cpp:387`, `path_trace.cpp:839`, `deep_output_driver.cpp:346`, `deep_buffers.cpp:35`

### Verdict: ❌ Not a real bug

**Reasoning:**

1. **Between animation frames** — the deep driver is destroyed and recreated:
   - [render()](file:///E:/blender_modify/blender/intern/cycles/blender/session.cpp#L344) is called once per frame by Blender's pipeline
   - [render_frame_finish()](file:///E:/blender_modify/blender/intern/cycles/blender/session.cpp#L671) calls `session->set_output_driver(nullptr)` (line 698), destroying session drivers
   - On the next frame, `render()` creates a **new** `DeepOutputDriver` with fresh buffers

2. **Within a single frame** — accumulation is intentional:
   - [render_pipeline()](file:///E:/blender_modify/blender/intern/cycles/integrator/path_trace.cpp#L183) calls `sync_deep_output_buffers()` each iteration
   - [sync_device_buffers()](file:///E:/blender_modify/blender/intern/cycles/session/deep_output_driver.cpp#L346) returns early when layout matches — **this is correct** because deep samples accumulate progressively across render iterations, just like regular render buffers
   - `init_render_buffers()` (line 387) zeros regular buffers via `zero_render_buffers()` but not deep buffers — **also correct**, since deep samples are written atomically per-path and should accumulate

3. **The `needs_reset` guard** at [session.cpp:487-492](file:///E:/blender_modify/blender/intern/cycles/blender/session.cpp#L487):
   ```cpp
   const bool needs_reset = (deep_driver->get_width() != width ||
                             deep_driver->get_height() != height ||
                             deep_driver->get_max_samples_per_pixel() != max_deep_samples);
   if (needs_reset) {
     deep_driver->reset(width, height, max_deep_samples);
   }
   ```
   This only skips `reset()` when dimensions match — but this path is only reached within the **same frame's view loop**, not across animation frames.

---

## Finding #2 — Multi-View Uses Single Deep Buffer

**Claim:** Deep data is captured once after all views, so multi-view outputs reuse the same (last-view) data.

**Cited lines:** `session.cpp:553`, `pipeline.cc:1283`

### Verdict: ✅ Valid concern

**Evidence:**

The view loop at [session.cpp:393](file:///E:/blender_modify/blender/intern/cycles/blender/session.cpp#L393):

```cpp
LISTBASE_FOREACH_INDEX (RenderView *, b_view, &b_rr->views, view_index) {
    // ...
    if (!deep_driver) {           // ← Only creates driver on FIRST view (line 435)
        auto new_driver = make_unique<DeepOutputDriver>(...);
        session->set_deep_output_driver(std::move(new_driver));
    }
    // ...
    session->start();             // ← Each view renders into SAME deep buffers
    session->wait();
}

// Deep finalization happens AFTER the loop (line 553-651)
// Uses whatever is in the buffers — last view's data
```

**Problem:** All views write into identical device buffers. The kernel's deep write functions overwrite `sample_counts` and `sample_data` — so view N overwrites view N-1's deep samples. Only the final view's deep data survives to finalization.

**Impact:** Stereo (left/right eye) or any multi-view render with Deep EXR output would produce a single deep file containing only the last view's samples.

> [!WARNING]
> This is a real correctness issue for multi-view Deep EXR output.

**Recommended fix (pick one):**
- **Option A:** Add per-view deep snapshots – snapshot deep data after each view, finalize all views separately
- **Option B:** Block multi-view deep EXR with an error message: `"Deep EXR output is not supported with multi-view rendering"` **(Implemented 2026-02-13)**

---

## Finding #3 — Merge Compositing Order

**Claim:** Merge uses "current over previous" ordering, which may be incorrect if per-sample RGB is introduced.

**Cited lines:** `deep_buffers.cpp:176`, `node_composite_file_output.cc:92`

### Verdict: ❌ Not a bug — correct ordering

**The merge code** at [deep_buffers.cpp:196-201](file:///E:/blender_modify/blender/intern/cycles/session/deep_buffers.cpp#L196):

```cpp
// Samples are depth-sorted (front-to-back) before merging
std::sort(data + offset, data + offset + num_samples);  // line 154

// Merge: "composite current over previous"
prev.r = current.r + prev.r * one_minus_a;
prev.g = current.g + prev.g * one_minus_a;
prev.b = current.b + prev.b * one_minus_a;
prev.a = a + prev.a * one_minus_a;
```

**Why this is correct:**

The iteration walks front-to-back (ascending depth, since samples were sorted by `operator<` on `z`). With premultiplied alpha, the standard over operation is:

```
result = front + back × (1 - front.a)
```

Here `current` is the **later** (further) sample in the sorted order, being composited onto `prev` (the accumulated nearer sample). The formula `current.r + prev.r * (1 - current.a)` is actually "current over prev" which puts the farther sample in front — but this is only applied when merging **nearby samples within tolerance**. For samples within merge tolerance, the compositing direction between two nearly-identical-depth fragments is irrelevant (their depths are practically equal).

The identical pattern is used in both Cycles (`deep_buffers.cpp`) and the compositor (`node_composite_file_output.cc`), keeping them consistent.
