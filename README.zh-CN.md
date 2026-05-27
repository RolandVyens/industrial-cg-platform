<!--
Keep this document short & concise,
linking to external resources instead of including content in-line.
See 'release/text/readme.html' for the end user read-me.
-->

# Industrial CG Platform
[English](README.md) | 简体中文

<img width="3840" height="1920" alt="splash_v002" src="https://github.com/user-attachments/assets/d39387c0-57a6-4420-81f9-67a1e31b1915" />
<p align="center">
  <strong>为 VFX 而生。基于 Blender。面向真实镜头生产。</strong><br>
  一个面向高级 VFX 工作流的 Blender 生产平台：内置 Qt 运行时支持、Deep EXR、
  Lightgroup Pass 工作流、阴影颜色控制，以及更适合合成环节的渲染数据输出。
</p>

---

Industrial CG Platform 是一个基于 Blender 衍生出的生产型发行版，专注于那些通常决定一个镜头能否顺利进入真实 VFX 流程的关键能力：

- 面向流程工具的内置 Qt 运行时支持
- 原生 Deep EXR 输出
- 为精细合成控制而设计的 Lightgroup 分光与 Light AOV 工作流
- 尽可能贴近上游 Blender、便于长期维护的源码级功能增强

它不是一个随意堆叠额外功能的通用 Blender 分支，而是一个面向艺术家、TD、流程开发者和小型团队的生产型 Blender 平台，目标是在 Blender 工具链内直接提供更贴近 VFX 实战的渲染与流程能力。

### 网站与文档: https://rolandvyens.github.io/industrial-cg-platform-docs/

---

## 支持 / 赞助

Industrial CG Platform 以生产导向的开源研发方式持续推进。

如果这个项目对你的工作室、课程、流程建设或个人创作有帮助，赞助可以让开发节奏更稳定、更快。

- 在 [Patreon](https://www.patreon.com/cw/RolandVyens) 支持作者
- 在 [爱发电](https://www.ifdian.net/a/mogubobi2) 支持作者
- 给仓库点 Star，并分享给需要更强生产流程能力的 Blender 与 VFX 从业者

赞助尤其有助于持续投入那些很难通过小插件单独完成的方向：渲染输出行为、Deep 数据、Qt 运行时打包、Light Pass 设计，以及面向流程的文档建设。

## 为什么做这个项目

Blender 已经是一个很强的创作工具，但 VFX 生产通常需要的不只是漂亮的视口结果。

真实的镜头流程需要能够稳定进入合成环节的渲染数据、能按镜头继续微调的灯光控制，以及更符合艺术家、TD 与农场部署习惯的输出行为。

Industrial CG Platform 的目标，就是在尽量贴近上游 Blender 的前提下，把 Blender 往这个方向再推进一步，让后续维护依然可行。

## 当前已具备的生产特性

这里列出的都不是路线图承诺，而是当前仓库里已经存在的能力。

- **Deep EXR 输出**<br>
  Blender 中面向真实深度合成流程的 Deep EXR 能力。

  <img width="797" height="260" alt="image" src="https://github.com/user-attachments/assets/6912ec1b-1505-44dc-8117-cc08a29575bc" />

- **Cycles 灯光与世界阴影颜色控制**<br>
  适用于风格化灯光、镜头级阴影微调，以及更灵活的艺术指导。

  <img width="1164" height="665" alt="image" src="https://github.com/user-attachments/assets/5b254d1b-a674-4f4c-8ca7-6c7f50101525" />

- **Cycles 中的 Lightgroup 分光与 Light AOV 工作流**<br>
  支持将 Lightgroup 拆分到材质相关通道中，给合成阶段提供更细粒度的控制。

  <img width="878" height="405" alt="image" src="https://github.com/user-attachments/assets/626ecc8b-67db-4e63-b16a-c63726d7b2f7" />

- **内置 bQt 运行时集成**<br>
  仓库内包含 `scripts/modules/blender_vfx_qt` 下的分支自有 Qt 包装层，以及面向 Qt 工具的打包 System Extension 运行时载荷。

  <img width="1834" height="1277" alt="image" src="https://github.com/user-attachments/assets/1abe6843-2f21-4a61-9dd6-81c0e876260e" />

## 路线图

后续计划中的 VFX / 生产向方向包括：

- 仅间接光照对象，用于更有针对性的灯光控制
- 集合级与对象级材质覆盖工作流
- 类似 `aiFog`、偏向直射光使用场景的世界环境雾行为
- 更完整的镜头渲染与合成工作流文档

## 适用人群

Industrial CG Platform 面向：

- 计划将 Blender 引入生产流程的 VFX 工作室
- 需要更强 Pass 控制能力的灯光师
- 需要 Blender 输出更好渲染数据的合成师
- 正在评估 Blender 镜头工作流可行性的 TD 与流程开发者
- 以 Blender 为核心搭建 VFX 流程的小型团队
- 需要源码级能力增强而不只依赖插件的技术美术

## 如何获取

- **未来的二进制发布：** 打包版本会发布在仓库的 [Releases](https://github.com/RolandVyens/industrial-cg-platform/releases) 页面。
- **当前最可靠的体验路径：** 目前更稳妥的评估方式仍然是直接从本仓库源码构建。
- **面向最终用户的说明：** 打包应用指南和发布说明可以单独维护在 [`release/text/readme.html`](release/text/readme.html)。

## 从源码构建

先参考 Blender 官方的上游构建文档：

- [Blender Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Blender Developer Documentation](https://developer.blender.org/docs/)

然后再补上这个分支自己的注意事项：

- 请构建这个仓库，而不是上游 Blender，这样才能包含这里的生产向 Cycles 和 UI 改动。
- 当前内置 Qt 运行时以 System Extension 的形式放在 `release/extensions/system/blender_vfx_qt_runtime`。
- 这个运行时目前声明的平台是 `windows-x64`，因此当前仓库里内置 Qt 工作流的主要支持平台是 Windows。
- 不要删除、重命名或拍平 `release/extensions/system/blender_vfx_qt_runtime/wheels/` 里的 wheel 载荷；运行时和许可证载荷都依赖这些文件保持原样。
- 如果你要把构建结果重新打包分发，请保留 `blender_vfx_qt_runtime` 扩展、它的 manifest，以及配套的第三方许可证文件。
- `scripts/modules/blender_vfx_qt` 下的分支自有包装层依赖这个内置运行时扩展；如果运行时缺失，它会明确报错。

## 与上游的关系

Industrial CG Platform 派生自 Blender。<br>
Blender 仍然是这个应用、文档生态和大部分开发工作流的上游基础。

- [blender.org](https://www.blender.org)
- [Blender Manual](https://docs.blender.org/manual/en/latest/index.html)
- [Blender Developer Portal](https://developer.blender.org/docs/)

## 致谢

Industrial CG Platform 由 Roland Vyens 主导与维护，开发、文档、打包和研发过程通过 Codex 与 Claude 的 AI 辅助工作流获得加速。

这些工具是项目工作流的一部分，但平台方向、功能取舍、发布决策和分支维护仍然由项目本身负责。

项目也感谢 MoonRay 项目；我们在这个分支里探索的一些渲染侧实现方式，参考了他们公开呈现出的部分实现思路。

## 许可证

Industrial CG Platform 的核心应用沿用 Blender 的 GPL 许可模式，同时还内置了一个包含额外第三方组件的 bQt 运行时扩展。

- Blender 衍生出的应用代码仍受 GNU General Public License Version 3 约束，个别文件可能使用不同但兼容的许可证。
- 位于 [`release/extensions/system/blender_vfx_qt_runtime`](release/extensions/system/blender_vfx_qt_runtime) 的 Qt 运行时扩展内含 `bqt`、`QtPy`、`PySide6`、`PySide6_Essentials`、`PySide6_Addons`、`shiboken6`、`packaging` 和 `blender-qt-stylesheet` 等第三方包。
- 这些打包组件各自带有其上游许可证，包括 MPL-2.0、MIT、Apache-2.0、BSD-2-Clause、LGPL-3.0-only、GPL-2.0-only 和 GPL-3.0-only。
- 关于该运行时载荷的许可证与源码信息，请查看 [`THIRD_PARTY_LICENSES.md`](release/extensions/system/blender_vfx_qt_runtime/THIRD_PARTY_LICENSES.md) 和 [`third_party_sources.md`](release/extensions/system/blender_vfx_qt_runtime/third_party_sources.md)。

关于 Blender 核心许可证，请参阅 [blender.org/about/license](https://www.blender.org/about/license) 和仓库中的 [COPYING](COPYING) 文件。
