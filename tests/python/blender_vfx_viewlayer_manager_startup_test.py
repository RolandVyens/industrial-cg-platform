# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for BQt startup retry and selector ownership."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
STARTUP_POLICY_PATH = (
    ROOT / "scripts" / "modules" / "blender_vfx_viewlayer_manager_startup.py"
)
STARTUP_BRIDGE_PATH = (
    ROOT / "scripts" / "startup" / "bl_operators" / "blender_vfx_viewlayer_manager.py"
)


def _load_module(module_name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_startup_bridge(module_name: str):
    timers = types.SimpleNamespace(
        is_registered=lambda _callback: False,
        register=lambda *_args, **_kwargs: None,
        unregister=lambda _callback: None,
    )
    bpy_module = types.ModuleType("bpy")
    bpy_module.context = types.SimpleNamespace(
        preferences=types.SimpleNamespace(
            extensions=types.SimpleNamespace(repos=[]),
        ),
    )
    bpy_module.app = types.SimpleNamespace(background=False, timers=timers)

    handlers_module = types.ModuleType("bpy.app.handlers")
    handlers_module.load_post = []
    handlers_module.persistent = lambda callback: callback
    translations_module = types.ModuleType("bpy.app.translations")
    translations_module.pgettext_rpt = lambda text: text
    types_module = types.ModuleType("bpy.types")
    types_module.Operator = type("Operator", (), {})

    stub_modules = {
        "bpy": bpy_module,
        "bpy.app.handlers": handlers_module,
        "bpy.app.translations": translations_module,
        "bpy.types": types_module,
    }
    previous_modules = {name: sys.modules.get(name) for name in stub_modules}
    modules_path = str(STARTUP_POLICY_PATH.parent)
    sys.path.insert(0, modules_path)
    try:
        sys.modules.update(stub_modules)
        return _load_module(module_name, STARTUP_BRIDGE_PATH)
    finally:
        sys.path.remove(modules_path)
        for name, previous in previous_modules.items():
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous


class StartupRetryPolicyTests(unittest.TestCase):
    def test_repository_readiness_retry_is_bounded_and_backed_off(self):
        policy = _load_module("bqt_startup_policy_under_test", STARTUP_POLICY_PATH)

        delays = [policy.repository_retry_delay(attempt) for attempt in range(1, 7)]

        self.assertEqual(delays, [0.25, 0.5, 1.0, 1.0, None, None])

    def test_permanent_failure_is_not_retryable(self):
        policy = _load_module("bqt_startup_permanent_under_test", STARTUP_POLICY_PATH)

        self.assertIsNone(policy.retry_delay(policy.PrewarmFailure.PERMANENT, attempt=1))
        self.assertEqual(
            policy.retry_delay(policy.PrewarmFailure.REPOSITORY_NOT_READY, attempt=1),
            0.25,
        )

    def test_repository_probes_do_not_repeat_extension_enable_work(self):
        bridge = _load_startup_bridge("bqt_startup_bridge_repository_under_test")
        enable_calls = 0

        def enable_extension(**_kwargs):
            nonlocal enable_calls
            enable_calls += 1
            return object()

        bridge._startup_prewarm_supported = lambda: True
        bridge._extension_module_name = lambda: None
        bridge._enable_extension = enable_extension

        delays = [bridge._startup_prewarm_timer() for _index in range(5)]

        self.assertEqual(delays, [0.25, 0.5, 1.0, 1.0, None])
        self.assertEqual(enable_calls, 0)

    def test_permanent_enable_failure_runs_once_and_stops(self):
        bridge = _load_startup_bridge("bqt_startup_bridge_permanent_under_test")
        enable_calls = 0

        def fail_enable(**_kwargs):
            nonlocal enable_calls
            enable_calls += 1
            raise RuntimeError("broken wheel")

        bridge._startup_prewarm_supported = lambda: True
        bridge._extension_module_name = lambda: "bl_ext.system.viewlayer"
        bridge._enable_extension = fail_enable

        delay = bridge._startup_prewarm_timer()

        self.assertIsNone(delay)
        self.assertEqual(enable_calls, 1)
        self.assertFalse(bridge._startup_prewarm_timer_queued)


class ExtensionSelectorTests(unittest.TestCase):
    def test_system_repository_is_preferred_and_layouts_are_supported(self):
        policy = _load_module("bqt_startup_selector_under_test", STARTUP_POLICY_PATH)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            preferred = root / "preferred"
            alternate = root / "alternate"
            preferred_manifest = preferred / "viewlayer" / "blender_manifest.toml"
            alternate_manifest = alternate / "alternate" / "viewlayer" / "blender_manifest.toml"
            preferred_manifest.parent.mkdir(parents=True)
            alternate_manifest.parent.mkdir(parents=True)
            preferred_manifest.write_text("id = 'viewlayer'\n", encoding="utf-8")
            alternate_manifest.write_text("id = 'viewlayer'\n", encoding="utf-8")
            repos = [
                types.SimpleNamespace(
                    enabled=True,
                    use_remote_url=False,
                    source="SYSTEM",
                    module="alternate",
                    directory=str(alternate),
                ),
                types.SimpleNamespace(
                    enabled=True,
                    use_remote_url=False,
                    source="SYSTEM",
                    module="system",
                    directory=str(preferred),
                ),
            ]

            selection = policy.select_extension(repos, "viewlayer", preferred_module="system")

        self.assertEqual(selection.module_name, "bl_ext.system.viewlayer")
        self.assertEqual(selection.manifest_path, preferred_manifest)

    def test_disabled_remote_and_non_system_repositories_are_ignored(self):
        policy = _load_module("bqt_startup_filter_under_test", STARTUP_POLICY_PATH)
        repos = [
            types.SimpleNamespace(
                enabled=False,
                use_remote_url=False,
                source="SYSTEM",
                module="disabled",
                directory="C:/missing",
            ),
            types.SimpleNamespace(
                enabled=True,
                use_remote_url=True,
                source="SYSTEM",
                module="remote",
                directory="C:/missing",
            ),
            types.SimpleNamespace(
                enabled=True,
                use_remote_url=False,
                source="USER",
                module="user",
                directory="C:/missing",
            ),
        ]

        self.assertIsNone(policy.select_extension(repos, "viewlayer"))


class NativeSelectorContractTests(unittest.TestCase):
    def test_view_layer_selector_contract_is_source_complete(self):
        rna_types = (ROOT / "source/blender/makesrna/RNA_types.hh").read_text(encoding="utf-8")
        rna_scene = (
            ROOT / "source/blender/makesrna/intern/rna_scene.cc"
        ).read_text(encoding="utf-8")
        interface_utils = (
            ROOT / "source/blender/editors/interface/interface_utils.cc"
        ).read_text(encoding="utf-8")

        self.assertIn("PROP_COLLECTION_SEARCH_KEEP_ORDER", rna_types)
        self.assertIn(
            "RNA_def_property_flag(prop, PROP_COLLECTION_SEARCH_KEEP_ORDER);",
            rna_scene,
        )
        self.assertIn("search_flag & PROP_COLLECTION_SEARCH_KEEP_ORDER", interface_utils)


if __name__ == "__main__":
    unittest.main()
