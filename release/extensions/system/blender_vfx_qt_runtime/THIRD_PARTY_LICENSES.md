# Third-Party Licenses

This bundled System Extension ships third-party wheels inside `./wheels/`.
The authoritative license texts remain embedded in each wheel's `.dist-info`
payload and travel with the final ZIP package. The wheel filenames, sizes, and
SHA-256 hashes are recorded in `third_party_sources.md`.

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

Release-facing note:

- The final Windows ZIP must preserve the bundled wheels intact so the upstream
  `.dist-info` license payloads remain available to recipients.
- If any wheel is repacked, stripped, or replaced in a way that omits embedded
  license texts, the omitted license texts must be shipped separately alongside
  the runtime package. This is especially important for Qt/PySide payloads,
  whose upstream license texts must remain available even when only a compact
  commercial license reference is embedded in the wheel metadata.
