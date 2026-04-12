# Environment Fog Shader — Implementation Plan

> Realtime, interactive, aiFog-like, direct lighting only, no HDRI

## Goal

Add an Environment Fog shader to Blender Cycles that behaves like Arnold's `atmosphere_volume`:
a scene-wide participating medium that responds only to direct scene lights (point, spot, area,
sun/distant), ignores HDRI/background lighting, evaluates only for camera rays, and is cheap
enough to feel interactive.

## User Review Required

> [!IMPORTANT]
> This is a "fake" fog — NOT a true world volume. It does not participate in GI, reflection,
> refraction, or indirect transport. It is an additive atmosphere/emission effect on camera rays.

> [!IMPORTANT]
> Unlike Arnold's atmosphere_volume, we WILL support distant/sun lights. Arnold excludes them
> because they have no position, but we can handle them analytically using the directional light's
> constant direction vector for the phase function calculation and transmittance integration.

> [!WARNING]
> V1 will NOT support volumetric shadows (objects blocking light before it reaches the fog).
> Adding shadow ray queries inside the fog integration loop would significantly increase cost.
> This can be added as a V2 feature behind a toggle.

## Architecture

### Core Concept

For every camera ray segment (camera → first surface hit, or camera → background):

```
L_fog = Σ_lights [ ∫ σ_s * ρ(t) * T_cam(t) * L_light(t) * phase(θ) dt ]
```

Where:
- `σ_s` = scattering coefficient (from fog color × density)
- `ρ(t)` = density at sample point (uniform for v1, height-based for v2)
- `T_cam(t)` = transmittance from camera to sample point: `exp(-σ_t * t)`
- `L_light(t)` = light intensity at sample point (includes 1/d² falloff for local lights)
- `phase(θ)` = Henyey-Greenstein phase function with user-controlled anisotropy
- Integration is numerical with 4-16 fixed steps along the ray segment

### Why Not Full Analytic?

Closed-form integrals (Dykeman, Macklin, Sun et al.) exist for simplified cases but:
- Each light type needs a different formula
- Combined camera + light transmittance kills the closed-form elegance
- Fixed-step sampling (4-16 steps) is already very cheap in a path tracer context
- Cycles already has efficient light iteration infrastructure

### Why Not Cycles Volume Path?

Cycles' volume path (`shade_volume.h`) is 3096 lines of sophisticated null-scattering,
octree-based, unbiased volume transport. Using it for "fake fog" would:
- Be massive overkill for a single-scatter effect
- Risk breaking existing volume behavior
- Not deliver the "interactive" feel — volume transport is inherently noisy and slow
- Miss the whole point: this is meant to be a fast, deterministic atmosphere effect

## Proposed Changes

### Component 1: Shader Node (Blender-side)

#### [NEW] source/blender/nodes/shader/nodes/node_shader_environment_fog.cc

New shader node `ShaderNodeEnvironmentFog` for the World shader graph:
- **Inputs**:
  - `Color` (default white) — fog scattering color
  - `Density` (default 0.1) — scattering coefficient
  - `Start Distance` (default 0.0) — distance from camera before fog begins
  - `Max Distance` (default 1000.0) — maximum fog evaluation distance
  - `Anisotropy` (default 0.0) — HG phase function g parameter [-1, 1]
  - `Samples` (default 8) — number of integration steps [1-32]
- **Outputs**:
  - `Fog` — connects to World Output → Volume or a dedicated slot
- World-only node (not available in Material shader graphs for v1)

#### [MODIFY] source/blender/nodes/shader/CMakeLists.txt

Register the new node source file.

#### [MODIFY] source/blender/makesrna/intern/rna_nodetree.cc

Add RNA definition for the new node type.

#### [MODIFY] source/blender/makesdna/DNA_node_types.h

Add `SH_NODE_ENVIRONMENT_FOG` enum value.

#### [MODIFY] source/blender/blenkernel/intern/node.cc

Register the node type.

---

### Component 2: Cycles Scene Integration

#### [MODIFY] intern/cycles/scene/background.h

Add `has_environment_fog` flag and fog parameters to Background.

#### [MODIFY] intern/cycles/scene/background.cpp

Detect EnvironmentFog node in world shader graph, extract parameters,
write to `KernelBackground` struct.

#### [MODIFY] intern/cycles/kernel/types.h

Add fog parameters to `KernelBackground`:
```c
struct KernelBackground {
  // ... existing fields ...
  
  /* Environment fog */
  int use_environment_fog;
  float fog_density;
  float fog_start;
  float fog_max_distance;
  float fog_anisotropy;
  int fog_samples;
  float fog_color_r, fog_color_g, fog_color_b;
};
```

---

### Component 3: Kernel Fog Evaluation

#### [NEW] intern/cycles/kernel/integrator/environment_fog.h

Core fog evaluation function:

```c
ccl_device Spectrum integrate_environment_fog(
    KernelGlobals kg,
    IntegratorState state,
    const float3 ray_P,
    const float3 ray_D,
    const float ray_tmin,
    const float ray_tmax)
{
    // 1. Clamp ray segment to [fog_start, min(ray_tmax, fog_max_distance)]
    // 2. For each step along the clamped segment:
    //    a. Compute sample position P_sample = ray_P + ray_D * t
    //    b. Compute camera transmittance T_cam = exp(-σ_t * (t - fog_start))
    //    c. For each scene light (skip LIGHT_BACKGROUND):
    //       - Get light direction and intensity at P_sample
    //       - Compute phase = HG(dot(ray_D, light_dir), anisotropy)
    //       - Compute light contribution = intensity * phase * T_cam * ρ
    //       - Accumulate
    // 3. Multiply accumulated result by fog_color and step_size
    // 4. Return fog contribution
}
```

Key design decisions:
- **Skip `LIGHT_BACKGROUND`**: This implements "ignore HDRI"
- **Camera ray only**: Check `PATH_RAY_CAMERA` flag before evaluating
- **No indirect**: Only evaluate for the first camera bounce
- **Fixed steps**: Deterministic, no noise, no variance
- **No shadow rays**: V1 skips volumetric shadows for speed

#### [MODIFY] intern/cycles/kernel/integrator/shade_background.h

In `integrator_shade_background()`, after `integrate_background()`:
- If `kernel_data.background.use_environment_fog` and ray is camera ray:
  - Call `integrate_environment_fog()` with the full ray length
  - Add result to the background contribution

#### [MODIFY] intern/cycles/kernel/integrator/shade_surface.h

At the end of surface shading for camera rays:
- If `kernel_data.background.use_environment_fog` and ray is camera ray:
  - Call `integrate_environment_fog()` with `[0, hit_distance]`
  - Add result to the surface emission contribution

This ensures fog is visible between the camera and objects, not just in the background.

---

### Component 4: Light Iteration in Fog

The fog evaluation will iterate lights using:
```c
for (int lamp = 0; lamp < kernel_data.integrator.num_lights; lamp++) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(lights, lamp);
    
    // Skip background/HDRI light
    if (klight->type == LIGHT_BACKGROUND) continue;
    
    // Get light properties at sample position
    // Compute contribution
}
```

For each light type:
- **LIGHT_POINT**: position-based, 1/d² falloff, full sphere
- **LIGHT_SPOT**: position-based, 1/d² falloff, cone attenuation
- **LIGHT_AREA**: position-based, approximate as point from fog sample's perspective
- **LIGHT_DISTANT**: direction-only, no falloff, constant direction for phase function
- **LIGHT_BACKGROUND**: SKIP (this is the "ignore HDRI" rule)
- **LIGHT_TRIANGLE** (emissive geometry): Include if already in light list

---

## Per-Light Fog Math

### Point Light

```
L_point(t) = I / |P_sample - P_light|²
contribution += σ_s * L_point * phase(dot(-D, normalize(P_light - P_sample)), g) * T_cam * step_size
```

### Distant/Sun Light

```
L_sun = I  (no falloff)
contribution += σ_s * L_sun * phase(dot(-D, -light_dir), g) * T_cam * step_size
```

### Spot Light

```
L_spot(t) = I * spot_attenuation(angle) / |P_sample - P_light|²
```

### Area Light

```
L_area(t) ≈ I / |P_sample - P_center|²  (approximate as point for v1)
```

---

## Film/AOV Integration

V1: Fog is written as an additive emission-style contribution to the Combined pass.

Future: Dedicated "Fog" AOV pass if the architecture supports it cleanly.

---

## Open Questions

> [!IMPORTANT]
> **Q1: Node placement** — Should the fog node connect to World Output → Volume slot, or should
> we create a new dedicated "Atmosphere" slot on the World Output node? Using Volume would
> conflict with actual world volume shaders. A dedicated slot is cleaner but requires more UI work.

> [!IMPORTANT]
> **Q2: Fog + surface hit** — Should fog accumulate between camera and surface hits (like real
> atmosphere), or only for rays that miss all geometry (background rays only)? Real atmosphere
> should accumulate everywhere. This doubles the integration sites but is the correct behavior.

> [!IMPORTANT]
> **Q3: Height fog in V1?** — Should V1 include height-based density falloff? The analytic formula
> is known and not hard to implement, but it adds complexity. Could be deferred to V2.

> [!IMPORTANT]
> **Q4: Samples default** — Is 8 steps a good default for the integration? Arnold uses a "samples"
> control too. Higher = less banding, lower = faster.

## Verification Plan

### Build Verification
- Compile Cycles with the new shader node and kernel changes
- Verify no regressions in existing scenes

### Functional Testing
1. Create a test scene with a few point/spot/sun lights and no HDRI
2. Add EnvironmentFog node to World shader
3. Verify fog appears and responds to light color/intensity changes
4. Verify fog respects density, start distance, max distance
5. Verify anisotropy works (forward/back scattering visible)
6. Verify HDRI is ignored (change HDRI, fog should not change)
7. Verify fog accumulates between camera and surfaces
8. Compare visually with Arnold atmosphere_volume for similar setups

### Performance Testing
- Render a scene with fog vs without fog
- Verify the overhead is minimal (target: <20% increase for 8-step fog)
- Test with many lights to ensure the per-light loop stays tractable
