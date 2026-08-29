<!--
Keep this document short and link to the public documentation site for details.
See release/text/readme.html for packaged end-user notes.
-->

# Industrial CG Platform

[Website](https://cgweave.com/en/industrial-cg-platform/) |
[简体中文](README.zh-CN.md) |
[中文官网](https://cgweave.com/zh/industrial-cg-platform/) |
[Français](https://cgweave.com/fr/industrial-cg-platform/) |
[Releases](https://github.com/RolandVyens/industrial-cg-platform/releases)

<p align="center">
  <a href="https://cgweave.com/en/industrial-cg-platform/">
    <img width="180" alt="Industrial CG Platform Logo" src="https://cgweave.com/logo.webp">
  </a>
</p>

<p align="center">
  <strong>Built for VFX. Built from Blender. Built for shots.</strong><br>
  A Blender-based production platform for advanced VFX workflows.
</p>

<p align="center">
  <a href="https://cgweave.com/en/industrial-cg-platform/guide/getting-started">Getting Started</a>
  ·
  <a href="https://cgweave.com/en/industrial-cg-platform/features/deep-exr">Features</a>
  ·
  <a href="https://cgweave.com/en/industrial-cg-platform/api/">API Reference</a>
  ·
  <a href="https://cgweave.com/en/industrial-cg-platform/releases/">Release Notes</a>
</p>

## Platform Highlights

Industrial CG Platform is the Blender VFX rendering branch for shot-based production work.
The current public documentation is maintained on the CGWeave Industrial 3D site.

| Feature | What it adds |
| --- | --- |
| [Deep EXR Output](https://cgweave.com/en/industrial-cg-platform/features/deep-exr) | Native deep compositing output for Cycles, writing per-sample depth data for lossless deep merges in Nuke and other compositing tools. |
| [EXR Overscan](https://cgweave.com/en/industrial-cg-platform/features/exr-overscan) | Native EXR overscan support in Cycles, rendering extra pixels outside the camera frame for lens distortion, camera shake, and image transformations. |
| [Lightgroup Lobe Passes](https://cgweave.com/en/industrial-cg-platform/features/lightgroup-lobe-passes) | Per-lightgroup diffuse, glossy, transmission, and volume passes with direct and indirect separation for fine-grained relighting control. |
| [Shadow Color](https://cgweave.com/en/industrial-cg-platform/features/shadow-color) | Artistic per-light and per-world shadow color control, tinting shadows without affecting the rest of the lighting. |
| [ViewLayer Manager](https://cgweave.com/en/industrial-cg-platform/features/viewlayer-manager) | Qt-based ViewLayer management with presets, pass grouping, and batch ViewLayer operations from a dedicated manager window. |

## Documentation

- [User Guide](https://cgweave.com/en/industrial-cg-platform/guide/getting-started)
- [Features Reference](https://cgweave.com/en/industrial-cg-platform/features/deep-exr)
- [API Reference](https://cgweave.com/en/industrial-cg-platform/api/)
- [Release Notes](https://cgweave.com/en/industrial-cg-platform/releases/)
- [Industrial 3D Ecosystem](https://cgweave.com/en/)

## Repository Notes

This repository is the public continuation branch for Industrial CG Platform.
For source builds, start with Blender's upstream build documentation and build this repository so the Industrial CG Platform rendering and UI changes are included.

- [Blender Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Blender Developer Documentation](https://developer.blender.org/docs/)

## Support the Development

Industrial CG Platform is developed as production-focused open-source R&D.

- [Support on Patreon](https://www.patreon.com/cw/RolandVyens)
- [Support on Afdian](https://www.ifdian.net/a/mogubobi2)

## License

Released under the Blender License, GNU GPL v3 or later.
Bundled third-party runtime components keep their own upstream license notices in the repository.

Copyright (C) 2026 RolandVyens
