# Deep EXR Merged Surface Color Design

**Date:** 2026-03-22

**Goal:** Re-enable hard-surface deep merging while preserving correct near/far per-surface color for both direct scene-output Deep EXR and compositor RGBA Deep EXR.

## Problem

The current Deep EXR path preserves opaque surface duplicates during pre-save deep merging. This was introduced to protect the edge-alpha reconstruction work, but it leaves direct scene-output Deep EXRs with many redundant hard-surface samples. When those redundant samples are later merged or recolored using flattened beauty RGB, near/far edge samples can inherit antialiased flat beauty color instead of true grouped surface color.

## Requirements

- Re-enable deep merging for hard-surface output.
- Preserve true grouped per-surface color after merge.
- Apply the behavior to:
  - direct scene-output Deep EXR
  - compositor RGBA Deep EXR
- Keep compositor alpha-only Deep EXR behavior unchanged for color expectations.
- Keep volume deep behavior unchanged.

## Design

### 1. Separate hard-surface color from flat beauty color

For multi-surface hard-surface pixels, grouped deep samples must carry RGB derived from their own grouped surface hits, not from flattened antialiased beauty RGB. Beauty alpha may still be used as a coverage reference when needed, but beauty RGB must stop being the source of truth for grouped multi-surface deep samples.

### 2. Re-enable deep merge

The pre-save deep merge stage should no longer globally preserve opaque surface duplicates. Instead, it should merge nearby hard-surface samples normally according to the configured deep merge tolerances.

### 3. Keep grouped surface color stable across merge

The export path should emit grouped hard-surface samples with correct premultiplied RGB for each grouped segment:

- front/near grouped sample gets front/near grouped RGB
- back/far grouped sample gets back/far grouped RGB

This should make later deep merging safe without forcing all samples back to the same resolved flat beauty color.

### 4. Leave volume unchanged

Volume write, grouping, merge, and color behavior remain on the current path.

## Expected Outcomes

- Direct scene-output Deep EXR should no longer keep obviously redundant hard-surface duplicates just because they are opaque.
- Compositor RGBA Deep EXR should preserve correct grouped near/far sample color after merge.
- Alpha-only compositor deep is allowed to keep flat recolor limitations; it is checked only for merge/sample structure, not white-edge color.

## Validation Matrix

1. **Direct Deep EXR**
   - Check DeepMerge white-edge result in Nuke.
   - Check representative seam pixel sample count and per-sample RGB.

2. **Compositor RGBA Deep EXR**
   - Check DeepMerge white-edge result in Nuke.
   - Check representative seam pixel sample count and per-sample RGB.

3. **Compositor Alpha-Only Deep EXR**
   - Check whether deep merge/sample-count reduction actually happens.
   - Do not use white-edge color as pass/fail.

## Risks

- Re-enabling opaque surface merge may regress earlier edge-alpha fixes if grouped surface color is not fully separated from flat beauty RGB.
- Direct and compositor save paths may differ in where merge occurs, so both paths must be verified explicitly.
