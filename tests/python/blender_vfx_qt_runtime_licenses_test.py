# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression checks for the BQt runtime's shipped Qt/PySide license texts."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME_LICENSES = (
    ROOT
    / "release"
    / "extensions"
    / "system"
    / "blender_vfx_qt_runtime"
    / "licenses"
)
SPDX_LICENSES = ROOT / "release" / "license" / "spdx"
LICENSE_FILES = (
    "LGPL-3.0-or-later.txt",
    "GPL-2.0-or-later.txt",
    "GPL-3.0-or-later.txt",
)


class RuntimeLicensePayloadTests(unittest.TestCase):
    def test_qt_pyside_license_texts_are_shipped_with_runtime_extension(self):
        for filename in LICENSE_FILES:
            runtime_copy = RUNTIME_LICENSES / filename
            source_copy = SPDX_LICENSES / filename
            self.assertTrue(runtime_copy.is_file(), f"missing runtime license: {runtime_copy}")
            self.assertEqual(runtime_copy.read_bytes(), source_copy.read_bytes())


if __name__ == "__main__":
    unittest.main()
