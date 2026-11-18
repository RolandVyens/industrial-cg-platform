# Third-Party Source Manifest

This file records the provenance used to stage the current bundled runtime set.

Local provenance cache used during packaging review:

- `${LOCAL_PROVENANCE_CACHE}`

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
    `${LOCAL_PROVENANCE_CACHE}`
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

## Linux development payload

Linux installs select `blender_manifest.linux.toml`; Windows installs retain the
original manifest and wheels above. Only the selected platform payload is installed.
The Linux bQt fork starts at upstream2.2.0 commit
`d4311a213ebc6b4edcf0e6f67bb91693ffc79282`, with local commit
`ab19c9d8a3c102fe59f0e26db3848e4ab46baf3b` adding the Linux/X11 event pump,
transient host ownership and lifecycle cleanup. No public fork has been published.
Corresponding complete fork source, including MPL license, is shipped as
`wheels/linux/bqt-2.2.0+icp.linux.1-source.tar.gz`.

| Linux wheel under `wheels/linux` | SHA-256 |
| --- | --- |
| `bqt-2.2.0+icp.linux.1-py3-none-any.whl` | `fbef7b0a3e937e352a60f004e1e560024bf1a20e32e2396072cfd426501a568e` |
| `pyside6-6.11.0-cp310-abi3-manylinux_2_34_x86_64.whl` | `c642e2d25704ca746fd37f56feacf25c5aecc4cd40bef23d18eec81f87d9dc00` |
| `pyside6_addons-6.11.0-cp310-abi3-manylinux_2_34_x86_64.whl` | `ac6fe3d4ef4497dde3efc5e896b0acd53ff6c93be4bf485f045690f919419f35` |
| `pyside6_essentials-6.11.0-cp310-abi3-manylinux_2_34_x86_64.whl` | `dc20e7afd5fc6fe51297db91cef997ce60844be578f7a49fc61b7ab9657a8849` |
| `shiboken6-6.11.0-cp310-abi3-manylinux_2_34_x86_64.whl` | `ad54e64f8192ddbdff0c54ac82b89edcd62ed623f502ea21c960541d19514053` |

Linux PySide/Shiboken are unmodified upstream6.11.0 PyPI wheels. Shared stylesheet,
QtPy and packaging wheels, source links and licenses remain as listed above.
This payload targets x86_64/glibc2.34; WSL prototype validation does not establish
native Rocky desktop qualification. X11/xcb must share Blender's display.

## EL8-floor Linux Beta payload

Linux selects unmodified official PyPI PySide6/Essentials/Addons/shiboken6
6.9.3 manylinux_2_28 wheels. Windows6.11 is unchanged. Retained unselected
Linux6.11 source wheel files must not enter the EL8 Beta package.
All original licenses and bQt fork source archive remain included.

| Selected Qt6.9.3 wheel | SHA256 |
| --- | --- |
| `pyside6-6.9.3-cp39-abi3-manylinux_2_28_x86_64.whl` | `6485aebec8eba4e55d1ec1cebe68ca1413589880cc8ccd8a49acae852ec6cfb3` |
| `pyside6_essentials-6.9.3-cp39-abi3-manylinux_2_28_x86_64.whl` | `c70d5544e892b201a677b615156fab6a0fef865e7fc287f55a0eae00a682e83f` |
| `pyside6_addons-6.9.3-cp39-abi3-manylinux_2_28_x86_64.whl` | `68932327e1c33d729d79b2b94242f97b77601efe0427e757cd3fd588939ea479` |
| `shiboken6-6.9.3-cp39-abi3-manylinux_2_28_x86_64.whl` | `f3f5337a3a8fc660ba1462265bd9a2bdda9588f8d90fbc3d5ac4ce3134c11e59` |
