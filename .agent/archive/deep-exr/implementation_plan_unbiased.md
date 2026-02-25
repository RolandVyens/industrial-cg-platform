# Deep EXR Unbiased Volume Implementation Plan

> **Goal**: Enable deep EXR output for unbiased (null scattering) volume rendering

---

## User Review Required

> [!IMPORTANT]
> **Trade-off Decision**: Unbiased deep volume output will add overhead to the null scattering integration path. This is unavoidable since we need to track transmittance at depth boundaries.

---

## Proposed Changes

### 1. Volume Integration - Deep Sample Collection

Currently, unbiased volumes use `volume_integrate_heterogeneous` which iterates through octree nodes. We can collect deep samples at **octree node boundaries** since each node represents a discrete spatial region.

#### [MODIFY] shade_volume.h
`intern/cycles/kernel/integrator/shade_volume.h`

**Add deep sample collection in the integration loop** (after line 1863):

```cpp
// Inside the while loop, after volume_integrate_step_scattering:
#ifdef __DEEP_OUTPUT__
    if (kernel_data.film.use_deep_output && (INTEGRATOR_STATE(state, path, bounce) == 0)) {
      // Compute transmittance-based alpha for this octree segment
      float segment_alpha = 1.0f - reduce_max(vstate.throughput / result.indirect_throughput);
      
      if (segment_alpha > 1e-6f) {
        const uint32_t pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
        ccl_global KernelDeepSample *deep_samples = ...;
        
        // Use octree node boundaries as depth values
        float z_front = camera_z_depth(kg, ray->P + ray->D * octree.t.min);
        float z_back = camera_z_depth(kg, ray->P + ray->D * octree.t.max);
        
        film_write_deep_sample_volume(kg, pixel_index, deep_samples, deep_sample_counts,
                                     zero_spectrum(), segment_alpha, z_front, z_back);
      }
    }
#endif
```

---

### 2. Alternative: Fixed Depth Slicing

If octree-based sampling produces too many or irregular samples, we can use **fixed depth intervals**:

#### [MODIFY] shade_volume.h

**Add a separate deep sampling pass** at the end of `volume_integrate_heterogeneous`:

```cpp
#ifdef __DEEP_OUTPUT__
  // Write deep samples at regular depth intervals
  if (kernel_data.film.use_deep_output && (INTEGRATOR_STATE(state, path, bounce) == 0)) {
    const float slice_size = 0.1f;  // World units per slice
    const float ray_length = ray->tmax - ray->tmin;
    const int num_slices = min((int)(ray_length / slice_size), 64);
    
    for (int i = 0; i < num_slices; i++) {
      float t_front = ray->tmin + i * slice_size;
      float t_back = t_front + slice_size;
      
      // Estimate transmittance for this slice using the final result
      float slice_alpha = 1.0f - expf(-vstate.optical_depth * slice_size / ray_length);
      
      // Write deep sample for this slice
      ...
    }
  }
#endif
```

---

## Verification Plan

### Automated Tests
1. Render test scene with `volume_biased = False`
2. Verify deep EXR file size > 45 MB (indicates volume samples captured)
3. Open in Nuke and verify volume alpha gradients

### Manual Verification
1. Load deep EXR in Nuke → Check Sample count per pixel
2. Compare alpha ramps between biased and unbiased modes
3. Verify no visual artifacts in deep holdout

---

## Recommendation

**Start with Approach 1 (octree-based)** since:
- Leverages existing spatial subdivision
- Natural boundaries at density changes
- Lower overhead than fixed slicing

If results are unsatisfactory, switch to Approach 2.
