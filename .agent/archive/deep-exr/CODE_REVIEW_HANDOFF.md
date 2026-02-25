# Deep EXR Code Review - Agent Handoff Prompt

## Context
You are reviewing the Deep EXR output feature for Blender Cycles. This feature enables Cycles to output deep EXR files containing per-pixel depth samples for advanced compositing in Nuke and similar software.

**Milestone A (MVP on CPU) is complete.** The code works correctly and produces deep EXR files that Nuke can read with smooth volume alpha gradients.

---

## Your Task
Review the implementation for:
1. **Code quality** - naming, comments, style consistency with Blender codebase
2. **Correctness** - logic errors, edge cases, potential bugs
3. **Performance** - unnecessary work, memory usage patterns
4. **Maintainability** - could future developers understand this?

---

## Workspace
- **Source:** `E:\blender_modify\blender`
- **Build:** `E:\blender_modify\build_windows_x64_vc17_Release`
- **Branch:** `feature/deep-exr-output`

## Build Command
```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'E:\blender_modify\build_windows_x64_vc17_Release' --target blender --config Release
```

## Test Command
```powershell
E:\blender_modify\build_windows_x64_vc17_Release\bin\Release\blender.exe --background --python E:\blender_modify\test_deep_volume.py
```

---

## Key Files to Review (Priority Order)

### 1. Kernel - Surface Deep Samples
`intern/cycles/kernel/integrator/shade_surface.h` (lines 722-752)
- Writes surface deep samples at primary hits
- **Key fix:** Skips `SD_HAS_ONLY_VOLUME` to prevent α=1.0 at volume boundaries

### 2. Kernel - Volume Deep Samples  
`intern/cycles/kernel/integrator/shade_volume.h` (lines 2380-2435)
- Writes volume deep samples during ray marching
- **Key fix:** Optical density scaling to prevent blocky alpha

### 3. Sample Merging
`intern/cycles/session/deep_buffers.cpp` (function `merge_nearby_samples`)
- Arnold-style merge: requires BOTH depth AND alpha within tolerance
- Tolerances: depth=0.01, alpha=0.01

### 4. Deep EXR Writing
`source/blender/imbuf/intern/openexr/openexr_api.cpp` (function `IMB_exr_save_deep`)
- Full-frame buffer approach for Nuke compatibility
- Y-flip for OpenEXR coordinate system

### 5. Session Integration
`intern/cycles/blender/session.cpp` (around line 416)
- Deep driver setup after sync_data()
- Merge threshold = 0.01

---

## Documentation
- `.agent/TASK.md` - Roadmap and task checklist
- `.agent/AGENT_HANDOFF.md` - Current state summary  
- `.agent/WALKTHROUGH.md` - Implementation history
- `.agent/moonray_deep_research.md` - Research on MoonRay/Arnold approaches

---

## Data Safety Rules
> **DO NOT DELETE** any files in:
> - `E:\blender_modify\blender`
> - `C:\tmp\` (test outputs)
> - `D:\blender_projects\` (user's projects)

---

## Expected Outcome
Provide a code review with:
1. List of issues found (if any) with severity (critical/medium/low)
2. Suggestions for improvement
3. Confirmation that the implementation is sound
