# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for BQt runtime provenance and catalog identity."""

from __future__ import annotations

import importlib.util
import os
import pathlib
import sys
import tempfile
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
QT_WRAPPER_PATH = ROOT / "scripts" / "modules" / "blender_vfx_qt" / "__init__.py"
CATALOG_PATH = (
    ROOT
    / "release"
    / "extensions"
    / "system"
    / "blender_vfx_viewlayer_manager"
    / "property_catalog.py"
)


def _load_module(module_name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_extension_module(module_name: str, path: pathlib.Path):
    package_name = f"{module_name}_package"
    package = types.ModuleType(package_name)
    package.__path__ = [str(path.parent)]
    sys.modules[package_name] = package
    try:
        return _load_module(f"{package_name}.{path.stem}", path)
    finally:
        for loaded_name in tuple(sys.modules):
            if loaded_name == package_name or loaded_name.startswith(f"{package_name}."):
                sys.modules.pop(loaded_name, None)


class _RnaProperty:
    def __init__(self, identifier: str):
        self.identifier = identifier
        self.name = identifier
        self.type = "BOOLEAN"
        self.is_readonly = False


class _Owner:
    def __init__(self, properties: tuple[str, ...]):
        self.set_properties(properties)

    def set_properties(self, properties: tuple[str, ...]) -> None:
        self.bl_rna = types.SimpleNamespace(
            properties=[_RnaProperty(identifier) for identifier in properties]
        )


class RuntimeProvenanceTests(unittest.TestCase):
    def test_runtime_extension_paths_trust_preexisting_extension_local_path(self):
        wrapper = _load_module("bqt_wrapper_extension_paths_under_test", QT_WRAPPER_PATH)
        with tempfile.TemporaryDirectory() as temp_dir:
            extension_store = pathlib.Path(temp_dir) / "extensions"
            user_path = extension_store / ".user" / "system" / "blender_vfx_qt_runtime"
            package_path = extension_store / ".local" / "lib" / "python3.13" / "site-packages"
            user_path.mkdir(parents=True)
            package_path.mkdir(parents=True)
            bpy_stub = types.ModuleType("bpy")
            bpy_stub.utils = types.SimpleNamespace(
                extension_path_user=lambda _package, path="", create=False: str(user_path)
            )
            previous_bpy = sys.modules.get("bpy")
            sys.modules["bpy"] = bpy_stub
            sys.path.append(str(package_path))
            try:
                trusted_paths = wrapper._runtime_extension_package_paths(
                    "bl_ext.system.blender_vfx_qt_runtime"
                )
            finally:
                sys.path.remove(str(package_path))
                if previous_bpy is None:
                    sys.modules.pop("bpy", None)
                else:
                    sys.modules["bpy"] = previous_bpy

        self.assertEqual(trusted_paths, (package_path.resolve(),))

    def test_runtime_extension_is_enabled_before_runtime_packages_are_imported(self):
        wrapper = _load_module("bqt_wrapper_under_test", QT_WRAPPER_PATH)
        events = []
        bqt = types.SimpleNamespace(register=lambda: None, add=lambda *_args: None)
        qapplication = types.SimpleNamespace(instance=lambda: object())
        previous_disable_wrap = os.environ.get("BQT_DISABLE_WRAP")
        try:
            os.environ["BQT_DISABLE_WRAP"] = "1"
            wrapper._enable_runtime_extension = lambda: events.append("enable") or (
                "runtime",
                pathlib.Path("C:/runtime/blender_manifest.toml"),
                (),
            )
            wrapper._import_runtime_packages = lambda: (
                events.append("import") or bqt,
                qapplication,
                None,
            )
            wrapper._verify_runtime_package_provenance = lambda _info: events.append("verify")

            wrapper.ensure_bqt_runtime()
        finally:
            if previous_disable_wrap is None:
                os.environ.pop("BQT_DISABLE_WRAP", None)
            else:
                os.environ["BQT_DISABLE_WRAP"] = previous_disable_wrap

        self.assertEqual(events, ["enable", "import", "verify"])

    def test_preloaded_foreign_runtime_is_rejected(self):
        wrapper = _load_module("bqt_wrapper_foreign_under_test", QT_WRAPPER_PATH)
        foreign_bqt = types.SimpleNamespace(__file__="C:/foreign/bqt/__init__.py")
        bqt = types.SimpleNamespace(register=lambda: None, add=lambda *_args: None)
        qapplication = types.SimpleNamespace(instance=lambda: object())
        previous_bqt = sys.modules.get("bqt")
        previous_disable_wrap = os.environ.get("BQT_DISABLE_WRAP")
        try:
            sys.modules["bqt"] = foreign_bqt
            os.environ["BQT_DISABLE_WRAP"] = "1"
            wrapper._import_runtime_packages = lambda: (bqt, qapplication, None)
            with self.assertRaises(RuntimeError):
                wrapper.ensure_bqt_runtime()
        finally:
            if previous_disable_wrap is None:
                os.environ.pop("BQT_DISABLE_WRAP", None)
            else:
                os.environ["BQT_DISABLE_WRAP"] = previous_disable_wrap
            if previous_bqt is None:
                sys.modules.pop("bqt", None)
            else:
                sys.modules["bqt"] = previous_bqt


class CatalogIdentityTests(unittest.TestCase):
    def test_same_version_build_or_spec_change_rebuilds_catalog(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cache_root = pathlib.Path(temp_dir)
            view_layer = _Owner(("use_pass_first",))
            bpy_stub = types.ModuleType("bpy")
            bpy_stub.app = types.SimpleNamespace(version=(5, 2, 0), build_hash="first-build")
            bpy_stub.context = types.SimpleNamespace(
                scene=types.SimpleNamespace(view_layers=[view_layer])
            )
            bpy_stub.types = types.SimpleNamespace()

            def extension_path_user(_package, path="", create=False):
                directory = cache_root / path
                if create:
                    directory.mkdir(parents=True, exist_ok=True)
                return str(directory)

            bpy_stub.utils = types.SimpleNamespace(
                extension_path_user=extension_path_user,
                user_resource=lambda _resource_type, create=False: str(cache_root),
            )

            previous_bpy = sys.modules.get("bpy")
            try:
                sys.modules["bpy"] = bpy_stub
                catalog_module = _load_extension_module("bqt_catalog_under_test", CATALOG_PATH)
                first_specs = (("First", "view_layer", (("use_pass_first", "First"),)),)
                second_specs = (("Second", "view_layer", (("use_pass_second", "Second"),)),)

                first_catalog = catalog_module.load_view_layer_pass_catalog(
                    eevee_specs=first_specs,
                    cycles_specs=(),
                )
                bpy_stub.app.build_hash = "second-build"
                view_layer.set_properties(("use_pass_first", "use_pass_second"))
                second_catalog = catalog_module.load_view_layer_pass_catalog(
                    eevee_specs=second_specs,
                    cycles_specs=(),
                )
            finally:
                if previous_bpy is None:
                    sys.modules.pop("bpy", None)
                else:
                    sys.modules["bpy"] = previous_bpy

        self.assertNotEqual(first_catalog, second_catalog)
        self.assertEqual(second_catalog["eevee_specs"][0][0], "Second")


if __name__ == "__main__":
    unittest.main()
