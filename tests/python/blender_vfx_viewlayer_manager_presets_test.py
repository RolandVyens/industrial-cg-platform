# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unit tests for ViewLayer Manager pass preset schema normalization/apply."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import types
import unittest
from typing import Any


_PRESETS_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "release"
    / "extensions"
    / "system"
    / "blender_vfx_viewlayer_manager"
    / "presets.py"
)


def _build_bpy_stub(engine: str = "CYCLES") -> types.ModuleType:
    bpy_stub = types.ModuleType("bpy")
    bpy_stub.context = types.SimpleNamespace(
        scene=types.SimpleNamespace(
            render=types.SimpleNamespace(engine=engine),
        ),
    )
    bpy_stub.utils = types.SimpleNamespace(
        extension_path_user=lambda _package, path="", create=False: "",
        user_resource=lambda _resource_type, create=False: "",
    )
    return bpy_stub


def _load_presets_module():
    package_name = "blender_vfx_viewlayer_manager_presets_test_package"
    package = types.ModuleType(package_name)
    package.__path__ = [str(_PRESETS_PATH.parent)]
    previous_bpy = sys.modules.get("bpy")
    try:
        sys.modules["bpy"] = _build_bpy_stub()
        sys.modules[package_name] = package
        spec = importlib.util.spec_from_file_location(
            f"{package_name}.presets",
            _PRESETS_PATH,
        )
        if spec is None or spec.loader is None:
            raise RuntimeError(f"Unable to import presets module from {_PRESETS_PATH}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        if previous_bpy is None:
            sys.modules.pop("bpy", None)
        else:
            sys.modules["bpy"] = previous_bpy
        for loaded_name in tuple(sys.modules):
            if loaded_name == package_name or loaded_name.startswith(f"{package_name}."):
                sys.modules.pop(loaded_name, None)


presets = _load_presets_module()


class _AttrObject:
    def __init__(self, **kwargs: Any):
        self.__dict__.update(kwargs)


class _FailOnWriteObject(_AttrObject):
    def __init__(self, fail_prop: str, **kwargs: Any):
        self.__dict__["_fail_prop"] = fail_prop
        super().__init__(**kwargs)

    def __setattr__(self, name: str, value: Any) -> None:
        if name == self.__dict__.get("_fail_prop") and value:
            raise RuntimeError(f"Refusing write to {name}")
        super().__setattr__(name, value)


def _make_view_layer() -> _AttrObject:
    view_layer_data: dict[str, Any] = {}
    for prop_name in presets.SHARED_VIEW_LAYER_PASS_PROPS:
        view_layer_data[prop_name] = False
    for prop_name in presets.SHARED_VIEW_LAYER_VALUE_PROPS:
        view_layer_data[prop_name] = 6
    for prop_name in presets.EEVEE_ENGINE_VIEW_LAYER_PASS_PROPS:
        view_layer_data[prop_name] = False
    for prop_name in presets.CYCLES_ENGINE_VIEW_LAYER_PASS_PROPS:
        view_layer_data[prop_name] = False

    view_layer = _AttrObject(**view_layer_data)
    view_layer.eevee = _AttrObject(
        use_pass_volume_direct=False,
        use_pass_transparent=False,
    )

    cycles_data = {
        "use_pass_volume_direct": False,
        "use_pass_volume_indirect": False,
        "use_pass_shadow_catcher": False,
        presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP: False,
    }
    for prop_name in presets.CYCLES_LIGHT_PASS_AOV_PROPS:
        cycles_data[prop_name] = False
    view_layer.cycles = _AttrObject(**cycles_data)
    return view_layer


class PresetsSchemaTests(unittest.TestCase):
    def test_cycles_apply_updates_visible_cycles_light_pass_settings(self):
        light_prop = presets.CYCLES_LIGHT_PASS_AOV_PROPS[0]
        view_layer = _make_view_layer()

        changed = presets.apply_live_pass_state(
            view_layer,
            engine=presets.ENGINE_CYCLES,
            pass_states=(("view_layer", "use_pass_uv", True),),
            use_lightgroup_light_pass_aovs=True,
            cycles_light_pass_states=((light_prop, True),),
        )

        self.assertTrue(changed)
        self.assertTrue(view_layer.use_pass_uv)
        self.assertTrue(
            getattr(view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP)
        )
        self.assertTrue(getattr(view_layer.cycles, light_prop))

    def test_eevee_apply_preserves_hidden_cycles_light_pass_settings(self):
        view_layer = _make_view_layer()
        setattr(view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP, True)
        expected_cycles_state = {
            prop_name: bool(index % 2)
            for index, prop_name in enumerate(presets.CYCLES_LIGHT_PASS_AOV_PROPS)
        }
        for prop_name, value in expected_cycles_state.items():
            setattr(view_layer.cycles, prop_name, value)

        changed = presets.apply_live_pass_state(
            view_layer,
            engine=presets.ENGINE_BLENDER_EEVEE,
            pass_states=(("view_layer", "use_pass_shadow", True),),
            use_lightgroup_light_pass_aovs=False,
            cycles_light_pass_states=tuple(
                (prop_name, not value) for prop_name, value in expected_cycles_state.items()
            ),
        )

        self.assertTrue(changed)
        self.assertTrue(view_layer.use_pass_shadow)
        self.assertTrue(
            getattr(view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP)
        )
        self.assertEqual(
            {
                prop_name: getattr(view_layer.cycles, prop_name)
                for prop_name in presets.CYCLES_LIGHT_PASS_AOV_PROPS
            },
            expected_cycles_state,
        )

    def test_normalize_legacy_schema_payload_is_backward_compatible(self):
        light_prop = presets.CYCLES_LIGHT_PASS_AOV_PROPS[0]
        legacy_preset = {
            "schema_version": presets.PRESET_SCHEMA_VERSION_LEGACY,
            "kind": presets.PRESET_KIND,
            "name": "LegacyPreset",
            "saved_at_utc": "2026-05-17T00:00:00Z",
            "eevee": {
                "view_layer": {
                    "use_deep": True,
                    "use_pass_combined": True,
                    "use_pass_shadow": True,
                    "use_pass_cryptomatte_object": True,
                    "pass_cryptomatte_depth": 8,
                },
                "view_layer.eevee": {
                    "use_pass_transparent": True,
                },
            },
            "cycles": {
                "view_layer": {
                    "use_deep": False,
                    "use_pass_combined": False,
                    "use_pass_uv": True,
                    "use_pass_cryptomatte_object": False,
                    "use_pass_cryptomatte_material": True,
                    "pass_cryptomatte_depth": 10,
                },
                "view_layer.cycles": {
                    "use_pass_shadow_catcher": True,
                },
            },
            "cycles_light_pass": {
                presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP: True,
                "aovs": {light_prop: True},
            },
        }

        normalized = presets._normalize_preset_data(legacy_preset)

        self.assertEqual(normalized["schema_version"], presets.PRESET_SCHEMA_VERSION)
        self.assertEqual(normalized["name"], "LegacyPreset")
        self.assertIn("shared", normalized)
        self.assertIn("engines", normalized)
        self.assertFalse(normalized["shared"]["view_layer"]["use_deep"])
        self.assertFalse(normalized["shared"]["view_layer"]["use_pass_combined"])
        self.assertFalse(normalized["shared"]["view_layer"]["use_pass_cryptomatte_object"])
        self.assertTrue(normalized["shared"]["view_layer"]["use_pass_cryptomatte_material"])
        self.assertEqual(normalized["shared"]["view_layer"]["pass_cryptomatte_depth"], 10)
        self.assertTrue(
            normalized["engines"][presets.ENGINE_BLENDER_EEVEE]["view_layer"]["use_pass_shadow"]
        )
        self.assertTrue(
            normalized["engines"][presets.ENGINE_BLENDER_EEVEE]["view_layer.eevee"]["use_pass_transparent"]
        )
        self.assertTrue(
            normalized["engines"][presets.ENGINE_CYCLES]["view_layer"]["use_pass_uv"]
        )
        self.assertTrue(
            normalized["engines"][presets.ENGINE_CYCLES]["view_layer.cycles"]["use_pass_shadow_catcher"]
        )
        self.assertTrue(
            normalized["engines"][presets.ENGINE_CYCLES]["cycles_light_pass"][presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP]
        )
        self.assertTrue(
            normalized["engines"][presets.ENGINE_CYCLES]["cycles_light_pass"]["aovs"][light_prop]
        )

    def test_collect_writes_shared_and_engine_specific_blocks(self):
        light_prop = presets.CYCLES_LIGHT_PASS_AOV_PROPS[1]
        view_layer = _make_view_layer()
        view_layer.use_deep = True
        view_layer.use_pass_combined = True
        view_layer.use_pass_cryptomatte_asset = True
        view_layer.pass_cryptomatte_depth = 12
        view_layer.use_pass_shadow = True
        view_layer.eevee.use_pass_transparent = True
        view_layer.use_pass_uv = True
        view_layer.cycles.use_pass_shadow_catcher = True
        setattr(view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP, True)
        setattr(view_layer.cycles, light_prop, True)

        collected = presets.collect_pass_preset(view_layer, preset_name="CurrentPreset")

        self.assertEqual(collected["schema_version"], presets.PRESET_SCHEMA_VERSION)
        self.assertTrue(collected["shared"]["view_layer"]["use_deep"])
        self.assertTrue(collected["shared"]["view_layer"]["use_pass_combined"])
        self.assertTrue(collected["shared"]["view_layer"]["use_pass_cryptomatte_asset"])
        self.assertEqual(collected["shared"]["view_layer"]["pass_cryptomatte_depth"], 12)
        self.assertNotIn(
            "use_pass_combined",
            collected["engines"][presets.ENGINE_BLENDER_EEVEE].get("view_layer", {}),
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_BLENDER_EEVEE]["view_layer"]["use_pass_shadow"]
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_BLENDER_EEVEE]["view_layer.eevee"]["use_pass_transparent"]
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_CYCLES]["view_layer"]["use_pass_uv"]
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_CYCLES]["view_layer.cycles"]["use_pass_shadow_catcher"]
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_CYCLES]["cycles_light_pass"][presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP]
        )
        self.assertTrue(
            collected["engines"][presets.ENGINE_CYCLES]["cycles_light_pass"]["aovs"][light_prop]
        )

    def test_apply_supports_legacy_and_current_schema(self):
        legacy_light_prop = presets.CYCLES_LIGHT_PASS_AOV_PROPS[2]
        current_light_prop = presets.CYCLES_LIGHT_PASS_AOV_PROPS[3]

        # Legacy schema applied in Cycles mode.
        cycles_view_layer = _make_view_layer()
        presets.bpy.context.scene.render.engine = presets.ENGINE_CYCLES
        legacy_preset = {
            "schema_version": presets.PRESET_SCHEMA_VERSION_LEGACY,
            "kind": presets.PRESET_KIND,
            "name": "LegacyApply",
            "eevee": {
                "view_layer": {
                    "use_deep": True,
                    "use_pass_combined": True,
                    "use_pass_cryptomatte_object": True,
                    "pass_cryptomatte_depth": 14,
                },
            },
            "cycles": {
                "view_layer": {
                    "use_pass_combined": True,
                    "use_pass_uv": True,
                    "use_pass_cryptomatte_object": True,
                },
                "view_layer.cycles": {
                    "use_pass_shadow_catcher": True,
                },
            },
            "cycles_light_pass": {
                presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP: True,
                "aovs": {legacy_light_prop: True},
            },
        }

        changed = presets.apply_pass_preset(cycles_view_layer, legacy_preset)
        self.assertTrue(changed)
        self.assertTrue(cycles_view_layer.use_deep)
        self.assertTrue(cycles_view_layer.use_pass_combined)
        self.assertTrue(cycles_view_layer.use_pass_uv)
        self.assertTrue(cycles_view_layer.use_pass_cryptomatte_object)
        self.assertEqual(cycles_view_layer.pass_cryptomatte_depth, 14)
        self.assertTrue(cycles_view_layer.cycles.use_pass_shadow_catcher)
        self.assertTrue(getattr(cycles_view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP))
        self.assertTrue(getattr(cycles_view_layer.cycles, legacy_light_prop))

        # Current schema applied in Eevee mode.
        eevee_view_layer = _make_view_layer()
        presets.bpy.context.scene.render.engine = presets.ENGINE_BLENDER_EEVEE
        current_preset = {
            "schema_version": presets.PRESET_SCHEMA_VERSION,
            "kind": presets.PRESET_KIND,
            "name": "CurrentApply",
            "shared": {
                "view_layer": {
                    "use_deep": True,
                    "use_pass_combined": True,
                    "use_pass_cryptomatte_asset": True,
                    "pass_cryptomatte_depth": 16,
                },
            },
            "engines": {
                presets.ENGINE_BLENDER_EEVEE: {
                    "view_layer": {
                        "use_pass_shadow": True,
                    },
                    "view_layer.eevee": {
                        "use_pass_transparent": True,
                    },
                },
                presets.ENGINE_CYCLES: {
                    "view_layer": {
                        "use_pass_uv": True,
                    },
                    "cycles_light_pass": {
                        presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP: True,
                        "aovs": {current_light_prop: True},
                    },
                },
            },
        }

        changed = presets.apply_pass_preset(eevee_view_layer, current_preset)
        self.assertTrue(changed)
        self.assertTrue(eevee_view_layer.use_deep)
        self.assertTrue(eevee_view_layer.use_pass_combined)
        self.assertTrue(eevee_view_layer.use_pass_cryptomatte_asset)
        self.assertEqual(eevee_view_layer.pass_cryptomatte_depth, 16)
        self.assertTrue(eevee_view_layer.use_pass_shadow)
        self.assertTrue(eevee_view_layer.eevee.use_pass_transparent)
        self.assertFalse(eevee_view_layer.use_pass_uv)
        self.assertFalse(getattr(eevee_view_layer.cycles, presets.CYCLES_LIGHT_PASS_AOV_MASTER_PROP))
        self.assertFalse(getattr(eevee_view_layer.cycles, current_light_prop))

    def test_save_new_rejects_normalized_filename_collision(self):
        view_layer = _make_view_layer()
        with tempfile.TemporaryDirectory() as temp_dir:
            original_get_directory = presets.get_preset_directory
            presets.get_preset_directory = lambda create=False: temp_dir
            try:
                filepath = presets.save_pass_preset(
                    view_layer,
                    "A B",
                    overwrite=False,
                )
                original_contents = pathlib.Path(filepath).read_text(encoding="utf-8")
                view_layer.use_deep = True

                with self.assertRaises(FileExistsError):
                    presets.save_pass_preset(view_layer, "A_B", overwrite=False)
                self.assertEqual(
                    pathlib.Path(filepath).read_text(encoding="utf-8"),
                    original_contents,
                )
            finally:
                presets.get_preset_directory = original_get_directory

    def test_multi_layer_apply_rolls_back_when_later_target_write_fails(self):
        first_view_layer = _make_view_layer()
        second_view_layer = _make_view_layer()
        second_view_layer.eevee = _FailOnWriteObject(
            "use_pass_transparent",
            use_pass_volume_direct=False,
            use_pass_transparent=False,
        )
        preset_data = {
            "schema_version": presets.PRESET_SCHEMA_VERSION,
            "kind": presets.PRESET_KIND,
            "name": "AtomicApply",
            "shared": {"view_layer": {"use_deep": True}},
            "engines": {
                presets.ENGINE_BLENDER_EEVEE: {
                    "view_layer.eevee": {"use_pass_transparent": True},
                },
                presets.ENGINE_CYCLES: {},
            },
        }
        presets.bpy.context.scene.render.engine = presets.ENGINE_BLENDER_EEVEE

        with self.assertRaisesRegex(RuntimeError, "Refusing write"):
            presets.apply_pass_preset_to_view_layers(
                (first_view_layer, second_view_layer),
                preset_data,
            )

        self.assertFalse(first_view_layer.use_deep)
        self.assertFalse(first_view_layer.eevee.use_pass_transparent)
        self.assertFalse(second_view_layer.use_deep)
        self.assertFalse(second_view_layer.eevee.use_pass_transparent)


if __name__ == "__main__":
    unittest.main()
