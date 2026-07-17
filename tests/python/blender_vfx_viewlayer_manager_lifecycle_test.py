# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for BQt manager and Qt-runtime lifecycle ownership."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
QT_WRAPPER_PATH = ROOT / "scripts" / "modules" / "blender_vfx_qt" / "__init__.py"
MANAGER_PATH = (
    ROOT
    / "release"
    / "extensions"
    / "system"
    / "blender_vfx_viewlayer_manager"
    / "manager.py"
)
EXTENSION_PATH = MANAGER_PATH.with_name("__init__.py")


def _load_module(module_name: str, path: pathlib.Path, *, package: bool = False):
    kwargs = {}
    if package:
        kwargs["submodule_search_locations"] = [str(path.parent)]
    spec = importlib.util.spec_from_file_location(module_name, path, **kwargs)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


class _FakeWindow:
    def __init__(self, *, alive: bool = True):
        self.alive = alive
        self.shutdown_calls = 0

    def shutdown(self) -> None:
        self.shutdown_calls += 1


class ManagerLifecycleTests(unittest.TestCase):
    def _load_manager(self, module_name: str):
        wrapper = types.ModuleType("blender_vfx_qt")
        wrapper.ensure_runtime = lambda: object()
        wrapper.qt_window_is_alive = lambda widget: bool(
            widget is not None and widget.alive
        )
        wrapper.show_unique_window = lambda cache_ref, factory: cache_ref.get("value") or factory()
        previous = sys.modules.get("blender_vfx_qt")
        sys.modules["blender_vfx_qt"] = wrapper
        try:
            return _load_module(module_name, MANAGER_PATH)
        finally:
            if previous is None:
                sys.modules.pop("blender_vfx_qt", None)
            else:
                sys.modules["blender_vfx_qt"] = previous

    def test_shutdown_manager_releases_cached_live_window(self):
        manager = self._load_manager("bqt_manager_shutdown_under_test")
        window = _FakeWindow()
        manager._window_cache["value"] = window

        manager.shutdown_manager()

        self.assertEqual(window.shutdown_calls, 1)
        self.assertIsNone(manager._window_cache["value"])

    def test_shutdown_manager_discards_stale_cached_window(self):
        manager = self._load_manager("bqt_manager_stale_shutdown_under_test")
        window = _FakeWindow(alive=False)
        manager._window_cache["value"] = window

        manager.shutdown_manager()

        self.assertEqual(window.shutdown_calls, 0)
        self.assertIsNone(manager._window_cache["value"])


class ExtensionLifecycleTests(unittest.TestCase):
    def test_unregister_shuts_down_manager_before_translations(self):
        package_name = "bqt_extension_lifecycle_under_test"
        events: list[str] = []
        manager = types.ModuleType(f"{package_name}.manager")
        manager.show_manager = lambda: None
        manager.shutdown_manager = lambda: events.append("shutdown")
        i18n = types.ModuleType(f"{package_name}.i18n")
        i18n.add_translation_entry = lambda *_args, **_kwargs: None
        i18n.pgettext_iface = lambda value: value
        i18n.pgettext_rpt = lambda value: value
        i18n.pgettext_tip = lambda value: value
        i18n.register_translations = lambda: None
        i18n.unregister_translations = lambda: events.append("translations")
        presets = types.ModuleType(f"{package_name}.presets")
        for name in (
            "apply_named_pass_preset",
            "apply_pass_preset",
            "collect_pass_preset",
            "delete_pass_preset",
            "get_preset_directory",
            "get_preset_filepath",
            "list_pass_presets",
            "load_pass_preset",
            "save_pass_preset",
        ):
            setattr(presets, name, lambda: None)

        previous_modules = {
            name: sys.modules.get(name)
            for name in (f"{package_name}.manager", f"{package_name}.i18n", f"{package_name}.presets")
        }
        sys.modules[f"{package_name}.manager"] = manager
        sys.modules[f"{package_name}.i18n"] = i18n
        sys.modules[f"{package_name}.presets"] = presets
        try:
            extension = _load_module(package_name, EXTENSION_PATH, package=True)
            extension.unregister()
        finally:
            sys.modules.pop(package_name, None)
            for name, previous in previous_modules.items():
                if previous is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = previous

        self.assertEqual(events, ["shutdown", "translations"])


class RuntimeRecoveryTests(unittest.TestCase):
    def test_in_process_recovery_fails_without_evicting_qt_modules(self):
        wrapper = _load_module("bqt_wrapper_recovery_under_test", QT_WRAPPER_PATH)
        foreign_bqt = types.SimpleNamespace(__file__="C:/foreign/bqt/__init__.py")
        previous_bqt = sys.modules.get("bqt")
        try:
            sys.modules["bqt"] = foreign_bqt
            with self.assertRaisesRegex(RuntimeError, "process-global"):
                wrapper._clear_runtime_import_state()
            self.assertIs(sys.modules["bqt"], foreign_bqt)
        finally:
            sys.modules.pop("bqt", None)
            if previous_bqt is not None:
                sys.modules["bqt"] = previous_bqt


if __name__ == "__main__":
    unittest.main()
