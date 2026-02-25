# Deep EXR Integration - Walkthrough

## Summary
Deep EXR output for Blender Cycles with full volume and surface support, including compositor passthrough.

**Status:** B7 Compositor Complete (with bug) | **Updated:** 2026-01-20

---

## Current Status

### ✅ Working
- Deep EXR via direct output (format = DEEP_EXR)
- Deep EXR via compositor File Output node
- Auto-enable from format or compositor detection
- 9.3M samples, 232MB output file

### 🐛 Bug
- **Volume alpha black holes** in compositor deep EXR path
- Does not affect direct deep EXR output

---

## Key Achievements
- Deep EXR files readable in Nuke
- Arnold-style sample merging (depth=0.01, alpha=0.01)
- `use_deep_output` property deprecated (auto-detected now)
- Compositor File Output node supports DEEP_EXR format

---

## Bug Fixes (Chronological)

1. **Struct alignment** - Added `alignas(32)` to `DeepSampleData`
2. **Alpha luminance** - Changed to solid 1.0 for opaque
3. **Y-flip** - Fixed with `height-1-y`
4. **Volume samples missing** - Moved outside SD_EMISSION block
5. **Blocky volume alpha** - Skip `SD_HAS_ONLY_VOLUME` surfaces
6. **Pixel type mismatch** - Fixed header/DeepSlice type match in EXR save
7. **Finalization timing** - Use driver existence instead of re-checking scene flags

---

## Files Modified (B7)

| File | Changes |
|------|---------|
| `session.cpp` | Store deep data in RenderResult, auto-enable |
| `pipeline.cc` | Pass deep data to compositor, detection function |
| `node_composite_file_output.cc` | execute_deep_exr() |
| `COM_render_context.hh` | Deep data storage |
| `openexr_api.cpp` | Fixed pixel type mismatch |
| `properties.py` | Deprecated use_deep_output |
| `sync.cpp` | Removed property reading |
| `engine.py` | Removed redundant auto-enable |

---

## Usage

### Direct Output
1. Set Output format to DEEP_EXR
2. Render → Deep EXR saved automatically

### Compositor Output
1. Add File Output node in compositor
2. Set format to DEEP_EXR
3. Connect Render Layers
4. Render → Deep EXR saved from compositor
