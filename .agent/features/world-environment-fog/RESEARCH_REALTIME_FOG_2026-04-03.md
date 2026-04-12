# Realtime Environment Fog Shader Research

> Date: `2026-04-03`
>
> Goal: Design an interactive, aiFog-like environment fog shader for Cycles
>
> Constraints: direct lighting only, ignore HDRI, camera ray only, realtime-friendly

## Research Summary

### Arnold aiFog / atmosphere_volume — The Reference Target

Arnold's `atmosphere_volume` is the closest match to the desired behavior:

- **Scene-wide** — assigned in render settings under Environment > Atmosphere, not per-object
- **Single scattering only** — computes how much light from direct scene lights scatters toward camera
- **Selective light support** — works with point, spot, and area lights; does NOT support skydome
  or distant lights (because they lack a position for scattering vector calculation)
- **Key parameters**: density, attenuation (color + distance), anisotropy/eccentricity (Henyey-Greenstein
  phase function, 0 = isotropic, positive = forward, negative = back), samples
- **Contribution controls**: camera, diffuse, specular — indirect GI is off by default because it
  is expensive and washes out the result
- **Not a fluid simulation** — uniform density throughout the scene; complex density requires
  standard_volume + VDB
- **Compositing**: atmosphere_volume does not write depth info the same way surfaces do; best
  rendered as an additive layer

Key takeaway: aiFog is fundamentally a **per-camera-ray, per-light, single-scatter accumulation**
with attenuation. No multi-scatter, no GI, no HDRI.

### Analytic Closed-Form Fog Integrals

Two key references provide the mathematical foundation for efficient fog evaluation without
full ray marching:

#### Isaac Dykeman — "A Simple Shader for Point Lights in Fog"

- Derives a **closed-form integral** for in-scattering from a point light along a camera ray
- The integral evaluates to an `atan`-based expression
- Assumes: isotropic scattering, inverse-square falloff, homogeneous medium
- Does NOT include Beer-Lambert attenuation in the basic form (but notes it can be added)
- Designed for deferred rendering — render a sphere proxy around each light, sum contributions
- Multiple lights: just sum individual contributions

Formula structure:
```
L_scatter = σ_s * I / (4π) * ∫ dt / |x(t) - s|²
```
where `x(t)` is the camera ray parameterization and `s` is the light position.
The integral solves to an expression involving `atan(...)`.

#### Miles Macklin — "In-Scattering Demo"

- Similar approach: closed-form single-scattering integral for isotropic point light
- The result is an `atan`-based analytic expression
- Supports point and spot lights in GLSL
- Avoids numerical ray marching entirely

#### Sun et al. — "A Practical Analytic Single Scattering Model for Real-Time Rendering" (SIGGRAPH 2005)

- More general: handles non-isotropic scattering with Henyey-Greenstein phase function
- Provides analytic airlight integrals for several light types
- More complex formulas but still closed-form for point and directional lights
- Strongest academic reference for the approach

### Analytic Transmittance with Height Fog

For exponential height density `ρ(y) = ρ_0 * exp(-h * y)`:

The optical depth along a ray from `t_0` to `t_1` has a **closed-form solution**:

```
τ(t_0, t_1) = (σ_e * ρ_0 * exp(-h * O_z)) / (h * D_z) * (exp(-h * t_0 * D_z) - exp(-h * t_1 * D_z))
```

Special case: when `D_z ≈ 0` (horizontal ray): `τ = σ_e * ρ_0 * exp(-h * O_z) * (t_1 - t_0)`

Transmittance: `T = exp(-τ)`

This means **height-based density falloff can be computed analytically** — no ray marching needed
for the transmittance term.

### Henyey-Greenstein Phase Function

Standard GPU implementation:
```glsl
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 / (4.0 * PI)) * (1.0 - g2) / pow(denom, 1.5);
}
```

- `g = 0`: isotropic
- `g > 0`: forward scattering (common for fog, mist, haze)
- `g < 0`: back scattering
- `cosTheta = dot(lightDir, viewDir)`

This is trivially cheap to evaluate per light per sample point.

### Beer-Lambert Attenuation

For uniform density:
```
T_view(t) = exp(-σ_t * t)           // camera ray to sample point
T_light(d) = exp(-σ_t * d)          // sample point to light
```

For height-based density, use the analytic formula above.

### EEVEE — Froxel-Based Volumetric (Reference, Not Target)

EEVEE uses a full froxel (frustum-voxel) pipeline:
1. Grid initialization — subdivide camera frustum into 3D cells
2. Light injection — compute scattering in each cell from all lights
3. Temporal reprojection — smooth across frames
4. Integration — accumulate along view depth
5. Composition — blend with surface rendering

Key files: `source/blender/draw/engines/eevee/` — `eevee_volumes.c`,
`shaders/volumetric_*.glsl`

**Not the target architecture** — too heavy for the "fake fog" approach, but useful as
reference for how Blender already handles volumetric light-fog interaction in real-time.

### Unreal Engine — Exponential Height Fog + Volumetric Fog

Two systems:
1. **Exponential Height Fog** — analytic, cheap, non-physical, no light interaction
2. **Volumetric Fog** — froxel-based, supports directional + point/spot with volumetric shadows,
   uses temporal reprojection, integrates into ExponentialHeightFog component

Key observations:
- Volumetric Fog supports directional light (with cascaded shadow maps), point/spot lights
  (with "Cast Volumetric Shadow" enabled), and skylight
- Full froxel architecture — not applicable to Cycles directly
- The separation of "cheap analytic height fog" vs "expensive volumetric fog" is a useful
  design precedent

### Unity HDRP — Volume Override with Froxel Grid

Similar to UE:
- Fog is a Volume override with optional volumetric fog
- Evaluated on a low-res 3D grid in camera frustum
- Interacts with scene lighting via light injection pass
- Uses temporal reprojection

### Godot — Fog Shader System

Godot fog shaders output: `ALBEDO`, `DENSITY`, `EMISSION`
- Engine handles all light integration internally
- Per-light volumetric fog energy control
- Single-scattering model with built-in HG phase function

Key insight: Godot's approach of separating "fog property definition" (density, color) from
"light integration" (engine handles) maps well to the proposed Cycles approach.

### Volumetric Shadow Techniques

For volumetric shadows (light being blocked by objects before reaching fog):
- Standard approach: at each sample point along the camera ray, cast a shadow ray to the light
  and check the shadow map
- For analytical approaches: can skip shadow testing for v1, or use existing Cycles shadow
  ray infrastructure
- Arnold's atmosphere_volume automatically handles volumetric shadows because it's inside the
  renderer

## Design Synthesis: How to Build the Cycles Fog Shader

### Architecture Decision: Analytic Camera-Ray Integration Per Light

Based on the research, the optimal approach for a "realtime, interactive, aiFog-like" fog in
Cycles is **Pattern 3: Analytic Camera-Ray Integral Per Light**, specifically:

1. **For each camera ray that hits background or travels a long distance:**
   - For each scene light (point, spot, area, sun/distant):
     - Compute the single-scattering in-scattering contribution analytically
     - Apply Henyey-Greenstein phase function for anisotropy
     - Apply Beer-Lambert transmittance along both camera ray and light ray
     - For height fog: use the analytic transmittance formula
     - Accumulate per-light fog contribution
   - Skip HDRI/background lighting entirely
   - Write result as additive atmosphere/emission contribution

2. **Integration point in Cycles:**
   - Evaluate fog in `integrator_shade_background()` path — this is where camera rays that
     miss all geometry end up
   - Also evaluate fog for camera rays that DO hit surfaces, accumulating fog between camera
     and surface hit point
   - This means fog evaluation happens in the integrator path, not as a full world volume

3. **Light sampling:**
   - Reuse existing `kernel_data_fetch(lights, lamp)` infrastructure
   - Iterate over scene lights, skip background/HDRI
   - For each light, compute analytic in-scattering integral or use fixed-step sampling

### Mathematical Model for V1

For each camera ray segment `[t_min, t_max]` and each scene light:

```
L_fog = Σ_lights [ fog_contribution(ray, light, t_min, t_max) ]
```

Where for a point/spot/area light at position `P_light` with intensity `I`:

```
fog_contribution = σ_s * I * phase(cosθ) * ∫[t_min, t_max] ρ(ray(t)) * T_view(t) * T_light(t) / |ray(t) - P_light|² dt
```

For **uniform density** (v1 simplest case):
- `ρ(x) = ρ_0` (constant)
- `T_view(t) = exp(-σ_t * t)` (transmittance along camera ray)
- `T_light(t) = exp(-σ_t * |ray(t) - P_light|)` (transmittance from sample to light)

For **directional/sun light** with direction `L`:
- No 1/d² falloff
- `T_light(t)` is computed along the light direction

**V1 simplification**: use fixed-step numerical integration (4-16 steps) along the camera ray
segment rather than full closed-form, because:
- Closed-form requires different formulas per light type
- Closed-form does not easily accommodate Beer-Lambert attenuation along both rays simultaneously
- Fixed-step is simple, predictable, and maps well to Cycles' existing volume sampling patterns
- Can upgrade to analytic later for specific light types

### Where to Hook Into Cycles

1. **New shader node**: `EnvironmentFogNode` — lives in World shader graph only
   - Parameters: color, density, start distance, max distance, height (optional v2),
     height falloff (optional v2), anisotropy (HG g parameter)

2. **Scene-side**: `intern/cycles/scene/background.cpp` — detect fog node in world shader,
   set kernel flag

3. **Kernel-side**: New evaluation in the integrator path
   - Option A: Evaluate in `shade_background.h` after surface miss
   - Option B: Evaluate in a new dedicated `integrate_environment_fog()` function called
     from the path integrator for camera rays
   - Option C: Evaluate at the end of `shade_surface.h` path for camera rays that hit
     surfaces (fog between camera and surface)

4. **Light iteration**: Reuse `kernel_data_fetch(lights, lamp)` to iterate scene lights,
   skip `LIGHT_BACKGROUND` type

## Sources

### Primary Technical References
- Arnold atmosphere_volume: https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-shading/ac-volume-shaders/
- Isaac Dykeman fog shader: https://ijdykeman.github.io/graphics/simple_fog_shader
- Miles Macklin in-scattering demo: https://mmacklin.com/inscatter
- Sun et al. SIGGRAPH 2005: https://www.cs.cmu.edu/~ILIM/publications/PDFs/SRNN-SIGGRAPH05.pdf
- Bart Wronski volumetric fog: https://bartwronski.com/publications/

### Engine References
- Blender EEVEE source: source/blender/draw/engines/eevee/
- Unreal Engine volumetric fog docs
- Unity HDRP fog docs
- Godot fog shader docs: https://docs.godotengine.org/en/4.4/tutorials/shaders/shader_reference/fog_shader.html

### Cycles Source Integration Points
- `intern/cycles/kernel/integrator/shade_background.h` — background evaluation entry
- `intern/cycles/kernel/integrator/shade_volume.h` — volume integration reference (3096 lines)
- `intern/cycles/kernel/integrator/shade_surface.h` — surface direct light sampling reference
- `intern/cycles/scene/background.cpp` — world/background setup
- `intern/cycles/scene/light.cpp` — light distribution and sampling setup
- `intern/cycles/kernel/light/sample.h` — light sampling functions
