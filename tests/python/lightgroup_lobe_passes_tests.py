# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import unittest
from importlib import util
from pathlib import Path

import bpy


GENERATED_ITEMS_PROP = "cycles_lightgroup_split_generated_items"


class _RecordingRenderEngine:
    def __init__(self):
        self.pass_names = []

    def register_pass(self, _scene, _view_layer, name, _channels, _channel_ids, _channel_type):
        self.pass_names.append(name)


def _load_cycles_engine_module():
    source_root = Path(__file__).resolve().parents[2]
    engine_path = source_root / "intern" / "cycles" / "blender" / "addon" / "engine.py"
    spec = util.spec_from_file_location("cycles_engine_under_test", engine_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load Cycles engine module from {engine_path}")
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


engine = _load_cycles_engine_module()


class LightgroupLobePassesTests(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        scene = bpy.context.scene
        view_layer = bpy.context.view_layer
        scene.use_nodes = True
        node_tree = bpy.data.node_groups.new("LightgroupCompositor", "CompositorNodeTree")
        scene.compositing_node_group = node_tree
        node_tree.nodes.clear()
        self.scene = scene
        self.view_layer = view_layer
        self.node = node_tree.nodes.new("CompositorNodeOutputFile")

    def enable_diffuse_split_passes(self):
        cycles = self.view_layer.cycles
        cycles.use_lightgroup_light_pass_aovs = True
        cycles.use_lightgroup_light_pass_aov_diffuse_combined = True
        cycles.use_lightgroup_light_pass_aov_diffuse_direct = True

    def add_splittable_lightgroup(self, name):
        self.view_layer.lightgroups.add(name=name)
        light_data = bpy.data.lights.new(name, "POINT")
        light_object = bpy.data.objects.new(name, light_data)
        bpy.context.collection.objects.link(light_object)
        light_object.lightgroup = name
        self.view_layer.update()

    def register_render_passes(self):
        render_engine = _RecordingRenderEngine()
        engine.register_passes(render_engine, self.scene, self.view_layer)
        return render_engine.pass_names

    def assert_pass_names_are_unique_and_length_safe(self, pass_names):
        self.assertEqual(len(pass_names), len(set(pass_names)))
        self.assertTrue(all(len(name.encode("utf-8")) <= 63 for name in pass_names))

    def test_generated_file_output_cleanup_preserves_user_items(self):
        user_item = self.node.file_output_items.new("RGBA", "diffuse_user_custom")
        user_item_name = user_item.name
        generated_item = self.node.file_output_items.new("RGBA", "diffuse_direct_generated_stale")
        generated_item_name = generated_item.name
        self.node[GENERATED_ITEMS_PROP] = [generated_item_name]

        engine.prune_stale_lightgroup_split_file_outputs(self.scene)

        self.assertIsNotNone(
            self.node.file_output_items.get(user_item_name),
            "Prefix-shaped user File Output items must not be treated as generated lightgroup items",
        )
        self.assertIsNone(
            self.node.file_output_items.get(generated_item_name),
            "Registry-owned generated lightgroup File Output items should be removed when stale",
        )

    def test_generated_pass_names_are_unique_and_length_safe(self):
        self.enable_diffuse_split_passes()
        self.add_splittable_lightgroup("foo")
        self.add_splittable_lightgroup("direct_foo")

        pass_names = self.register_render_passes()

        self.assert_pass_names_are_unique_and_length_safe(pass_names)
        self.assertIn("diffuse_direct_foo", pass_names)
        self.assertIn("diffuse___lg1", pass_names)

    def test_split_pass_descriptors_register_all_variants(self):
        descriptors = engine._LIGHTGROUP_SPLIT_PASS_DESCRIPTORS
        self.assertEqual(len(descriptors), 12)
        self.assertEqual(len({descriptor[0] for descriptor in descriptors}), 12)
        self.assertEqual(len({descriptor[1] for descriptor in descriptors}), 12)

        cycles = self.view_layer.cycles
        cycles.use_lightgroup_light_pass_aovs = True
        for property_name, _pass_name_prefix in descriptors:
            self.assertTrue(hasattr(cycles, property_name), property_name)
            setattr(cycles, property_name, True)
        self.add_splittable_lightgroup("key")

        pass_names = self.register_render_passes()
        expected = {pass_name_prefix + "key" for _property_name, pass_name_prefix in descriptors}
        self.assertTrue(expected.issubset(pass_names))
        self.assertEqual(len(expected), 12)

    def test_lightgroup_pass_name_over_63_utf8_bytes_is_encoded(self):
        self.add_splittable_lightgroup("x" * 63)

        pass_names = self.register_render_passes()

        self.assert_pass_names_are_unique_and_length_safe(pass_names)
        self.assertIn("Combined___lg0", pass_names)

    def test_lightgroup_pass_name_over_63_multibyte_utf8_bytes_is_encoded(self):
        self.add_splittable_lightgroup("\u00e9" * 31)

        pass_names = self.register_render_passes()

        self.assert_pass_names_are_unique_and_length_safe(pass_names)
        self.assertIn("Combined___lg0", pass_names)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
