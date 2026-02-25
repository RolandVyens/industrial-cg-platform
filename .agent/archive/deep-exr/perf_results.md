# Deep EXR Performance Test Results

**Scene:** `D:\blender_projects\4.0test.blend`
**Resolution:** 1920x1080
**Render Samples:** 32
**Date:** 2026-01-02 23:51:43

## Results Summary

| Test | Render Time | Memory Δ | Flat EXR | Deep EXR |
|------|-------------|----------|----------|----------|
| Baseline (No Deep) | 59.47s | N/A | 21.03 MB | - |
| Deep Output (16 max samples) | 64.49s (+8.4%) | N/A | 21.03 MB | 45.44 MB |
| Deep Output (64 max samples) | 72.18s (+21.4%) | N/A | 21.03 MB | 64.92 MB |
| Deep Output (128 max samples) | 75.94s (+27.7%) | N/A | 21.03 MB | 64.92 MB |

## Analysis

- **Deep Output Overhead:** 8.4% additional render time
- **Deep EXR File Size:** 2.2x larger than flat EXR

## Output Files

- `C:\tmp\deep_perf_test\perf_test_flat.exr`
- `C:\tmp\deep_perf_test\perf_test_deep_16_deep.exr`
- `C:\tmp\deep_perf_test\perf_test_deep_64_deep.exr`
- `C:\tmp\deep_perf_test\perf_test_deep_128_deep.exr`
