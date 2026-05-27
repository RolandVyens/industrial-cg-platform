<!--
Keep this document short & concise,
linking to external resources instead of including content in-line.
See 'release/text/readme.html' for the end user read-me.
-->

# Industrial CG Platform
[English](README.md) | [简体中文](README.zh-CN.md)

<img width="3840" height="1920" alt="splash_v002" src="https://github.com/user-attachments/assets/d39387c0-57a6-4420-81f9-67a1e31b1915" />
<p align="center">
  <strong>Built for VFX. Built from Blender. Built for shots.</strong><br>
  A Blender-based production platform for advanced VFX workflows: bundled Qt runtime support,
  Deep EXR, lightgroup pass workflows, shadow color control, and compositor-friendly render data.
</p>

---

Industrial CG Platform is a Blender-derived production distribution focused on the parts of CG that usually decide whether a shot can survive a real VFX pipeline:

- bundled Qt runtime support for pipeline-facing tools
- native Deep EXR output
- lightgroup split and Light AOV workflows for finer compositing control
- source-level features that can still be maintained close to upstream Blender

It is not a general Blender fork with random extras. It is a production-oriented Blender platform for artists, TDs, pipeline developers, and small teams who need VFX-style rendering behavior directly inside a Blender-based toolchain.

### Website & Documents: https://rolandvyens.github.io/industrial-cg-platform-docs/

---

## Support / Sponsor

Industrial CG Platform is developed as production-focused open-source R&D.

If this project helps your studio, course, pipeline, or personal production work, sponsorship helps keep development moving faster.

- Support the creator on [Patreon](https://www.patreon.com/cw/RolandVyens)
- Support the creator on [afdian](https://www.ifdian.net/a/mogubobi2)
- Star the repository and share it with Blender and VFX artists who need stronger production workflows

Sponsorship is especially helpful for funding features that are difficult to justify as small add-ons: render output behavior, deep data, Qt runtime packaging, light pass design, and pipeline-facing documentation.

## Why This Exists

Blender is already a strong creative tool, but VFX production often needs more than beautiful viewport results.

A real shot pipeline needs render data that can travel cleanly into compositing, lighting adjustments that remain controllable per shot, and output behavior that makes sense for artists, TDs, and render farms.

Industrial CG Platform exists to push Blender further in that direction while staying close enough to upstream Blender that future maintenance remains practical.

## Current Production-Focused Features

These features are based on work already present in this repository, not roadmap-only promises.

- **Deep EXR output**  
  The very first deep exr function for blender
  
  <img width="797" height="260" alt="image" src="https://github.com/user-attachments/assets/6912ec1b-1505-44dc-8117-cc08a29575bc" />

- **Cycles light and world shadow color controls**  
  For stylized lighting, shot-specific shadow tuning, and more flexible art direction.
  
  <img width="1164" height="665" alt="image" src="https://github.com/user-attachments/assets/5b254d1b-a674-4f4c-8ca7-6c7f50101525" />

- **Lightgroup split and Light AOV workflows in Cycles**  
  Including support for lightgroups that splitted to material passes.
  
  <img width="878" height="405" alt="image" src="https://github.com/user-attachments/assets/626ecc8b-67db-4e63-b16a-c63726d7b2f7" />

- **Bundled bQt runtime integration**  
  The repository includes a fork-owned Qt wrapper in `scripts/modules/blender_vfx_qt` and a bundled System Extension runtime payload for Qt-based tools.

  <img width="1834" height="1277" alt="image" src="https://github.com/user-attachments/assets/1abe6843-2f21-4a61-9dd6-81c0e876260e" />

## Roadmap

Planned VFX and production-focused directions include:

- indirect-light-only objects for more targeted lighting control
- collection and object-level material override workflows
- world environment fog behavior similar to `aiFog`, focused on direct-light use cases
- deeper documentation for shot-based rendering and compositing workflows

## Who This Is For

Industrial CG Platform is intended for:

- VFX studios adopting Blender into a production pipeline
- lighting artists who need stronger pass control
- compositors who need better render data from Blender
- TDs and pipeline developers evaluating Blender for shot-based work
- small studios building a Blender-centered VFX workflow
- technical artists who need source-level features rather than only add-ons

## Getting the Platform

- **Future binary releases:** packaged builds are intended to appear on the repository's [Releases](https://github.com/RolandVyens/industrial-cg-platform/releases) page.
- **Current access path:** today, the reliable way to evaluate the platform is to build it from source from this repository.
- **End-user notes:** packaged-app guidance and release notes can live separately in [`release/text/readme.html`](release/text/readme.html).

## Building From Source

Start with Blender's upstream build documentation:

- [Blender Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Blender Developer Documentation](https://developer.blender.org/docs/)

Then account for the repo-specific parts of this fork:

- Build this repository, not upstream Blender, so the production-focused Cycles and UI changes are included.
- The current bundled Qt runtime is staged as the System Extension `release/extensions/system/blender_vfx_qt_runtime`.
- That runtime is currently declared for `windows-x64`, so Windows is the primary supported platform for the bundled Qt workflow in the current repo state.
- Do not strip, rename, or flatten the wheel payloads inside `release/extensions/system/blender_vfx_qt_runtime/wheels/`; the runtime and its license payloads depend on those bundled files remaining intact.
- If you package a build for distribution, keep the `blender_vfx_qt_runtime` extension, its manifest, and its third-party license files together with the final package.
- The fork-owned wrapper in `scripts/modules/blender_vfx_qt` expects the bundled runtime extension to be available in the build and will report an error if it is missing.

## Upstream Relationship

Industrial CG Platform is derived from Blender.  
Blender remains the upstream foundation for the application, documentation ecosystem, and much of the development workflow.

- [blender.org](https://www.blender.org)
- [Blender Manual](https://docs.blender.org/manual/en/latest/index.html)
- [Blender Developer Portal](https://developer.blender.org/docs/)

## Credits

Industrial CG Platform is authored and directed by Roland Vyens, with development, documentation, packaging, and research work accelerated through an AI-assisted workflow using Codex and Claude.

These tools are part of the working process behind the project, but the platform direction, feature choices, release decisions, and fork maintenance remain project-owned.

The project also gratefully acknowledges the MoonRay project, whose public implementation ideas helped inform some of the rendering-side approaches explored in this branch.

## License

Industrial CG Platform follows Blender's GPL licensing model for the core application, and it also ships a bundled bQt runtime extension with additional third-party components.

- The Blender-derived application code remains under the GNU General Public License, Version 3, with individual files sometimes using a different but compatible license.
- The bundled Qt runtime extension at [`release/extensions/system/blender_vfx_qt_runtime`](release/extensions/system/blender_vfx_qt_runtime) includes third-party packages such as `bqt`, `QtPy`, `PySide6`, `PySide6_Essentials`, `PySide6_Addons`, `shiboken6`, `packaging`, and `blender-qt-stylesheet`.
- Those bundled components carry their own upstream licenses, including MPL-2.0, MIT, Apache-2.0, BSD-2-Clause, LGPL-3.0-only, GPL-2.0-only, and GPL-3.0-only.
- For the bundled runtime payload, consult [`THIRD_PARTY_LICENSES.md`](release/extensions/system/blender_vfx_qt_runtime/THIRD_PARTY_LICENSES.md) and [`third_party_sources.md`](release/extensions/system/blender_vfx_qt_runtime/third_party_sources.md).

For Blender's core license details, see [blender.org/about/license](https://www.blender.org/about/license) and the repository [COPYING](COPYING) file.
