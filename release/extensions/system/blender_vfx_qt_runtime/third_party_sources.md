# Third-Party Source Manifest

This file records the provenance used to stage the current bundled runtime set.

Local provenance cache used during packaging review:

- `E:\blender_modify\blender\.agent\archive\third_party_wheels\bqt_viewlayer_manager_2026-05-11`

Wheel integrity inventory:

| Wheel | Size (bytes) | SHA-256 |
| --- | ---: | --- |
| `blender_qt_stylesheet-0.0.3-py3-none-any.whl` | 45080 | `f89864645fb19af2e41c8895ff00ad042dd0e5c40923a5bdf176099d72769477` |
| `bqt-2.2.0-py3-none-any.whl` | 29672 | `3f3ce2769daf73dc1484f4e760519b34bd790126eed773288f1250f74d864a5f` |
| `packaging-26.2-py3-none-any.whl` | 100195 | `5fc45236b9446107ff2415ce77c807cee2862cb6fac22b8a73826d0693b0980e` |
| `pyside6-6.11.0-cp310-abi3-win_amd64.whl` | 577988 | `9092cb002ca43c64006afb2e0d0f6f51aef17aa737c33a45e502326a081ddcbc` |
| `pyside6_addons-6.11.0-cp310-abi3-win_amd64.whl` | 168723088 | `413e6121c24f5ffdce376298059eddecff74aa6d638e94e0f6015b33d29b889e` |
| `pyside6_essentials-6.11.0-cp310-abi3-win_amd64.whl` | 75793322 | `3b3362882ad9389357a80504e600180006a957731fec05786fced7b038461fdf` |
| `qtpy-2.4.3-py3-none-any.whl` | 95045 | `72095afe13673e017946cc258b8d5da43314197b741ed2890e563cf384b51aa1` |
| `shiboken6-6.11.0-cp310-abi3-win_amd64.whl` | 1222132 | `483ff78a73c7b3189ca924abc694318084f078bcfeaffa68e32024ff2d025ee1` |

Staged runtime set for `blender_vfx_qt_runtime`:

- `bqt-2.2.0-py3-none-any.whl`
  - upstream package: `bqt`
  - upstream source: `https://github.com/techartorg/bqt`
  - local source snapshot used for implementation review:
    `E:\blender_modify\blender\.agent\tmp\bqt_repo_inspect`
- `blender_qt_stylesheet-0.0.3-py3-none-any.whl`
  - upstream package: `blender-qt-stylesheet`
  - upstream source: `https://github.com/hannesdelbeke/blender-qt-stylesheet`
- `qtpy-2.4.3-py3-none-any.whl`
  - upstream package: `QtPy`
  - upstream source: `https://github.com/spyder-ide/qtpy`
- `packaging-26.2-py3-none-any.whl`
  - upstream package: `packaging`
  - upstream source: `https://github.com/pypa/packaging`
- `pyside6-6.11.0-cp310-abi3-win_amd64.whl`
  - upstream package: `PySide6`
  - upstream source / licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `pyside6_essentials-6.11.0-cp310-abi3-win_amd64.whl`
  - upstream package: `PySide6_Essentials`
  - upstream source / licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `pyside6_addons-6.11.0-cp310-abi3-win_amd64.whl`
  - upstream package: `PySide6_Addons`
  - upstream source / licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `shiboken6-6.11.0-cp310-abi3-win_amd64.whl`
  - upstream package: `shiboken6`
  - upstream source / licenses: `https://doc.qt.io/qtforpython-6/licenses.html`

Local patch state:

- no fork-local patch queue is applied to `bqt` yet
- Linux support remains deferred and upstream-first
