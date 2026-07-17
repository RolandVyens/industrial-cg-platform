# Third-Party Licenses

This bundled System Extension ships third-party wheels inside `./wheels/`.
Wheel filenames, sizes, and SHA-256 hashes are recorded in
`third_party_sources.md`. The Qt/PySide copyleft texts are shipped separately
under `./licenses/` because the bundled wheels carry only a compact commercial
license reference.

Bundled runtime components:

- `bqt 2.2.0`
  - package license expression: `MPL-2.0`
  - bundled license file:
    `bqt-2.2.0.dist-info/licenses/LICENSE`
  - upstream: `https://github.com/techartorg/bqt`
- `blender-qt-stylesheet 0.0.3`
  - package license: `Mozilla Public License Version 2.0`
  - bundled license file:
    `blender_qt_stylesheet-0.0.3.dist-info/LICENSE`
  - upstream: `https://github.com/hannesdelbeke/blender-qt-stylesheet`
- `QtPy 2.4.3`
  - package license: `MIT`
  - bundled license file:
    `QtPy-2.4.3.dist-info/LICENSE.txt`
  - upstream: `https://github.com/spyder-ide/qtpy`
- `packaging 26.2`
  - package license expression: `Apache-2.0 OR BSD-2-Clause`
  - bundled license files:
    `packaging-26.2.dist-info/licenses/LICENSE`
    `packaging-26.2.dist-info/licenses/LICENSE.APACHE`
    `packaging-26.2.dist-info/licenses/LICENSE.BSD`
  - upstream: `https://github.com/pypa/packaging`
- `PySide6 6.11.0`
  - package license: `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`
  - bundled license file:
    `pyside6-6.11.0.dist-info/licenses/LicenseRef-Qt-Commercial.txt`
  - upstream licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `PySide6_Essentials 6.11.0`
  - package license: `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`
  - bundled license file:
    `pyside6_essentials-6.11.0.dist-info/licenses/LicenseRef-Qt-Commercial.txt`
  - upstream licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `PySide6_Addons 6.11.0`
  - package license: `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`
  - bundled license file:
    `pyside6_addons-6.11.0.dist-info/licenses/LicenseRef-Qt-Commercial.txt`
  - upstream licenses: `https://doc.qt.io/qtforpython-6/licenses.html`
- `shiboken6 6.11.0`
  - package license: `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`
  - bundled license file:
    `shiboken6-6.11.0.dist-info/licenses/LicenseRef-Qt-Commercial.txt`
  - upstream licenses: `https://doc.qt.io/qtforpython-6/licenses.html`

Qt/PySide shared license payload:

- `licenses/LGPL-3.0-or-later.txt`
- `licenses/GPL-2.0-or-later.txt`
- `licenses/GPL-3.0-or-later.txt`

Release-facing note:

- The final Windows ZIP must preserve the bundled wheels and `licenses/`
  directory intact.
- If any wheel is repacked, stripped, or replaced, release owners must review
  its license payload and update the shipped notices before publication.
