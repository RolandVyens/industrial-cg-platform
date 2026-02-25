PR Title: WIP: Cycles: Add deep EXR output support

Cycles: Add deep EXR output support

Add deep EXR rendering to Cycles, storing per-pixel variable-depth
samples that enable compositing transparent and volumetric layers
with correct depth ordering.

Surfaces write alpha-only deep samples at primary ray intersections.
Volumes write per-segment opacity samples using physical transmittance.
A Deep Recolor post-process distributes beauty pass RGB to samples
using log-domain alpha scaling, producing correctly premultiplied
deep data matching the Combined pass.

Deep data can be written through two paths:
- Direct: Output Properties > Format > Deep EXR
- Compositor: File Output node set to Deep EXR

New scene properties control depth and alpha merge tolerances.
Deep EXR supports NONE, RLE, and ZIPS compression.
Multi-view rendering reports an error when deep output is requested.

Technical notes:
- Kernel uses atomic sample counting with fixed-size per-pixel buffers
- Multi-device rebalancing preserves deep data via snapshot/restore
- OpenEXR deep scanline output uses per-scanline buffering to minimize
  peak memory
- Deep data is stored per RenderLayer; compositor deep output selects the
  matching view layer based on linked Render Layers nodes
- Compositor view-layer lookup resolves indices against the original scene
  to avoid evaluated view-layer list reordering
- DNA versioning in versioning_510.cc initializes defaults

---

The text above is the commit message. Additional context for
reviewers follows below.

## 1. Problem

Cycles has no deep EXR output capability. Deep compositing is a
standard technique in VFX for combining CG elements without hold-out
mattes or manual depth sorting, and it is widely supported in
production renderers. Without it, Blender cannot integrate into
compositing workflows that rely on deep data.

## 2. Proposed Solution

A full deep EXR pipeline from kernel to file I/O:

**Kernel**: Deep samples are written at the first camera-ray
intersection. Surfaces store alpha + depth; volumes write per-segment
opacity during ray marching derived from transmittance. Only alpha and
depth are stored in the kernel (no RGB), following industry practice.
A "Deep Recolor" post-process then distributes the Combined pass RGB
into samples using log-domain alpha scaling. This avoids 3x memory
overhead and complex atomic color accumulation in the kernel.

**Session**: `DeepRenderBuffers` provides fixed-max per-pixel sample
storage with device memory management. `DeepOutputDriver` handles tile
synchronization, multi-device rebalance preservation (snapshot/restore),
deep recolor, and processed data caching.

**Blender integration**: New `R_IMF_IMTYPE_DEEP_EXR` format enum in
DNA, RNA properties with codec filtering (NONE/RLE/ZIPS only),
`IMB_exr_save_deep()` using OpenEXR deep scanline API with per-scanline
buffering for memory efficiency, and Compositor File Output node
support.

## 3. Alternative Solutions Considered

**Full RGB in kernel**: Would accumulate color per deep sample directly
during path tracing. Rejected because it multiplies per-sample memory
by 3x and requires complex atomic operations for color accumulation
across bounces. Alpha-only + Deep Recolor is the enough solution for 
day to day work.

**Variable-size per-pixel buffers**: Would use GPU-side dynamic
allocation instead of fixed-max arrays. Rejected because GPU dynamic
allocation is complex and fragile. Fixed-max with configurable limit
is simpler and matches Arnold's approach.

**Deep output via existing AOV system**: Would write deep data as a
special AOV pass. Rejected because deep data has fundamentally
different structure (variable samples per pixel) that doesn't fit the
fixed-stride AOV buffer layout.

## 4. Limitations

- Multi-view rendering is not supported with deep output (error reported
  via RE_engine_report)
- Deep post-processing (recolor, merge) runs on CPU after device-to-host
  copy
- Maximum samples per pixel is bounded by a configurable limit
- Half-float precision not yet implemented for deep channels
- GPU tested on CUDA; other GPU backends may need verification

## 5. User Interface

**Output Properties > Format > "Deep EXR (.exr)"**
- Depth Merge Tolerance (default: 0.01) - controls Z-distance threshold
  for merging adjacent samples
- Alpha Merge Tolerance (default: 0.01) - controls opacity-difference
  threshold for merging
- Compression codec selector filtered to NONE, RLE, ZIPS

**Compositor > File Output Node > Format > "Deep EXR (.exr)"**
- Same merge tolerance and compression controls as direct output

The resulting .exr files are standard OpenEXR deep scanline files,
readable by deep-aware compositors.

## Scope

46 files changed, ~2900 lines added. Key new files:
- intern/cycles/kernel/film/deep_write.h
- intern/cycles/session/deep_buffers.cpp/h
- intern/cycles/session/deep_output_driver.cpp/h
- source/blender/imbuf/IMB_deep_sample.hh
- source/blender/render/RE_deep_data.hh

## Testing

- CPU rendering (AMD)
- Mixed CPU+GPU rendering with multi-device rebalancing
- GPU rendering (optix)
- Scenes with transparent surfaces, glass, volumes, mixed geometry
- Direct output and Compositor File Output node paths
- Deep EXR files verified with OpenImageIO-based scripts and manually verification in Nuke


