# MoonRay Deep Volume Research

> Research document for understanding how MoonRay handles deep volume output.

## Objective
Understand MoonRay's approach to deep volume rendering to fix Blender Cycles' blocky deep volume alpha issue.

## Key Files Analyzed

| File | Purpose |
|------|---------|\
| `DeepBuffer.h` | Data structures for deep samples |
| `DeepBuffer.cc` | Core implementation - merging, writing |
| `PathIntegratorVolume.cc` | Volume integration with deep |
| `VolumeProperties.h` | Per-segment volume properties |
| `DcxDeepFlags.h` | OpenDCX interpolation flags |

---

## Critical Findings

### 1. Volume Deep Storage: sigma_t, NOT alpha

MoonRay stores **extinction coefficient (sigma_t)** per segment, NOT alpha:

```cpp
// VolumeInputSegment (from DeepBuffer.h:270-276)
struct VolumeInputSegment {
    VolumeInputSegment *mNext;
    float mTFront;      // front depth
    float mTBack;       // back depth  
    Color mSigmaT;      // extinction coefficient (NOT alpha!)
};
```

Alpha is computed **at write time** using Beer's law:
```cpp
// DeepBuffer.cc:1145-1150
float deltaT = vs->mTBack - vs->mTFront;
Color transmittance = exp(-vs->mSigmaT * deltaT);
Color alpha3 = Color(1,1,1) - transmittance;
float alpha = luminance(alpha3);
```

### 2. Volume Samples: Accumulated Transmittance

For radiance samples, MoonRay stores accumulated transmittance from camera:

```cpp
// VolumeSample (from DeepBuffer.h:290-296)
struct VolumeSample {
    float mT;                    // ray t distance
    Color mTransmittance;        // ACCUMULATED from camera to this point
    float mChannels[0];          // beauty + aov data
};
```

### 3. Segment Merging Algorithm

Multiple samples' sigma_t values are merged using this formula:
```
sigmaT_merged = -log((exp(-sigmaT_0*dt) + exp(-sigmaT_1*dt) + ...) / N) / dt
```

This ensures correct transmittance when compositing.

### 4. Hard Surface Deep Handling

Hard surfaces store:
- Per-segment alpha (accumulated across samples)
- Total weight for normalization
- Subpixel mask (8x8 = 64 bits)

```cpp
// HardSurfaceSegment (from DeepBuffer.h:146-164)
struct HardSurfaceSegment {
    float mTFront, mTBack;       // depth range
    float mRayZ;                 // for t→z conversion
    uint64_t mMask;              // 8x8 subpixel mask
    Vec3f mNormal;
    float mAlpha;                // accumulated alpha
    float mTotalWeight;          // for normalization
    float mIDsAndChannels[0];
};
```

At write time: `alpha = hs->mAlpha / hs->mTotalWeight`

---

## OpenDCX Deep Format Details

### Interpolation Flags
```cpp
// DcxDeepFlags.h
static const uint32_t LINEAR_INTERP = 0x00001;  // Hard surface (linear interp)
// If LINEAR_INTERP is NOT set → volumetric (log interp)
```

### Key Distinction
- **Hard surface (LINEAR_INTERP)**: Linear interpolation between Zf/Zb
- **Volumetric (no flag)**: Logarithmic interpolation between Zf/Zb

### What Nuke Expects
Nuke's deep compositing uses this flag to determine how to interpolate:
- Hard surfaces: `alpha_interpolated = lerp(0, alpha, (z - Zf) / (Zb - Zf))`
- Volumes: Uses Beer's law based interpolation

---

## Cycles Current Implementation

### Data Structure
```cpp
// deep_write.h
struct KernelDeepSample {
    float r, g, b, a;   // Only 'a' (alpha) is used for deep
    float z;            // front depth
    float z_back;       // back depth
};
```

### Key Issues
1. **No sigma_t storage**: Cannot reconstruct physically correct transmittance
2. **No interpolation flag**: Compositor can't distinguish volume from surface
3. **Per-segment alpha**: Currently stores per-segment `1 - transmittance`
4. **No merging**: Multiple samples at same depth just accumulate

---

## Comparison: MoonRay vs Cycles

| Aspect | MoonRay | Cycles (Current) |
|--------|---------|------------------|
| Volume storage | sigma_t + dt | alpha only |
| Alpha calculation | At write time | At sample time |
| Segment merging | Mathematical formula | None |
| Interpolation flag | LINEAR_INTERP for surfaces | None |
| Subpixel mask | 8x8 (64 bits) | None |
| Sample weight | Per-sample normalization | None |

---

## Root Cause of Blocky Volume Alpha

The blocky appearance happens because:
1. Dense volume regions have high alpha (~1.0) per-segment
2. When Nuke composites front-to-back, first segment blocks everything
3. Each segment should contribute **independently**, not block

**The fix**: Either:
- Store sigma_t and let compositor calculate alpha (MoonRay approach)
- Store per-segment alpha where each segment is **independent** (not cumulative)

---

## Implementation Options

### Option A: Store sigma_t (MoonRay Approach)
**Pros**: Physically correct, enables segment merging
**Cons**: Larger change, need to modify:
- `KernelDeepSample` struct to include sigma_t
- EXR export to calculate alpha at write time
- Add depth delta (z_back - z_front) storage

### Option B: Fix Per-Segment Alpha
**Pros**: Minimal change
**Cons**: Less flexible
**Current status**: We already store per-segment alpha correctly: `1 - reduce_max(transmittance)`

### Option C: Add Interpolation Flags
**Pros**: Tells compositor how to interpret samples
**Cons**: May require EXR metadata

---

## Recommended Fix Priority

1. **First**: Verify per-segment alpha is correct (not accumulated)
2. **Second**: Check if Nuke needs interpolation flag
3. **Third**: Consider sigma_t storage if needed

---

## Octane Deep Rendering Approach

### Deep Sample Structure
Octane stores per-sample:
- **R, G, B, A**: Color and alpha
- **Z**: Front depth
- **ZBack**: Back depth

### Volume vs Point Samples
- **Volume samples**: Z < ZBack (range of depth)
- **Point samples**: Z >= ZBack (hard surface)

### Key Parameters
- **Max Depth Samples**: Limits samples per pixel (memory control)
- **Depth Tolerance**: Merge threshold for similar-depth samples
  - If relative depth difference < tolerance → samples merge
  - Reduces memory, may reduce accuracy

### How Octane Handles Volume Alpha
- Each volume sample has both Z (front) and ZBack (back) depths
- Alpha represents transparency at that depth range
- Compositor uses A(Z) function to calculate alpha up to any depth
- "Deep bin distribution" characterizes depth samples efficiently

### Limitations Noted
- Memory-intensive for extensive volumes
- Max Depth Samples too low → volume gets cut off
- Large volumes may need many "bins" to represent fully

---

## Summary: Industry Approaches

| Renderer | Volume Storage | Merging | Key Feature |
|----------|---------------|---------|-------------|
| **MoonRay** | sigma_t | Yes (formula) | OpenDCX flags |
| **Arnold** | alpha | Tolerance-based | Sample merging |
| **Octane** | alpha | Depth tolerance | Z/ZBack ranges |
| **Cycles** | alpha | None | Basic deep |

**Common Pattern**: All use Z/ZBack for volume depth range, merge similar samples.

---

## References
- [MoonRay GitHub](https://github.com/dreamworksanimation/moonray)
- [OpenDCX Format](https://github.com/dreamworksanimation/opendcx)
- [Octane Deep Image Rendering](https://docs.otoy.com)
- Arnold: Uses tolerance-based sample merging for deep
- Source: `moonray/lib/rendering/pbr/core/DeepBuffer.cc`
