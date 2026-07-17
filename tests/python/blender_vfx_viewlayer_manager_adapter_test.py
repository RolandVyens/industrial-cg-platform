# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unit tests for the ViewLayer Manager Blender adapter."""

from __future__ import annotations

import importlib.util
import pathlib
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ADAPTER_PATH = (
    ROOT
    / "release"
    / "extensions"
    / "system"
    / "blender_vfx_viewlayer_manager"
    / "blender_adapter.py"
)


def _load_module(module_name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _ViewLayers(list):
    def move(self, source: int, target: int) -> None:
        self.insert(target, self.pop(source))


def _view_layer(name: str):
    return types.SimpleNamespace(
        name=name,
        use=True,
        use_deep=False,
        eevee=types.SimpleNamespace(use_pass_transparent=False),
        cycles=types.SimpleNamespace(use_pass_shadow_catcher=False),
    )


class BlenderAdapterTests(unittest.TestCase):
    def test_target_resolution_is_shared_and_strict(self):
        adapter_module = _load_module("bqt_adapter_targets_under_test", ADAPTER_PATH)
        view_layer = _view_layer("Main")

        self.assertIs(adapter_module.resolve_target(view_layer, "view_layer"), view_layer)
        self.assertIs(
            adapter_module.resolve_target(view_layer, "view_layer.eevee"),
            view_layer.eevee,
        )
        self.assertIs(
            adapter_module.resolve_target(view_layer, "view_layer.cycles"),
            view_layer.cycles,
        )
        with self.assertRaises(KeyError):
            adapter_module.resolve_target(view_layer, "view_layer.invalid")

    def test_selection_write_and_reorder_share_one_scene_adapter(self):
        adapter_module = _load_module("bqt_adapter_scene_under_test", ADAPTER_PATH)
        layers = _ViewLayers([_view_layer("A"), _view_layer("B"), _view_layer("C")])
        adapter = adapter_module.ViewLayerAdapter(types.SimpleNamespace(view_layers=layers))

        self.assertEqual(adapter.names(), ["A", "B", "C"])
        self.assertIs(adapter.selected("missing", "B"), layers[1])
        self.assertTrue(
            adapter.set_property("B", "view_layer.cycles", "use_pass_shadow_catcher", True)
        )
        self.assertTrue(layers[1].cycles.use_pass_shadow_catcher)
        self.assertFalse(
            adapter.set_property("B", "view_layer.cycles", "use_pass_shadow_catcher", True)
        )

        self.assertTrue(adapter.reorder(["C", "A", "B"]))
        self.assertEqual(adapter.names(), ["C", "A", "B"])
        self.assertFalse(adapter.reorder(["C", "A", "B"]))


if __name__ == "__main__":
    unittest.main()
