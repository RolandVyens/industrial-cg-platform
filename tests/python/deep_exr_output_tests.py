# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import shutil
import tempfile
import unittest
from pathlib import Path

import bpy
import OpenImageIO as oiio


class DeepExrOutputTests(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.output_root = Path(self._temporary_directory.name)
        self.scene, self.file_output = self._create_scene()

    def tearDown(self):
        self._temporary_directory.cleanup()

    def _create_scene(self):
        scene = bpy.context.scene
        scene.render.engine = "CYCLES"
        scene.cycles.device = "CPU"
        scene.cycles.samples = 16
        scene.cycles.seed = 0
        scene.cycles.use_denoising = False
        scene.render.resolution_x = 16
        scene.render.resolution_y = 16
        scene.render.resolution_percentage = 100
        scene.render.image_settings.file_format = "PNG"

        bpy.ops.object.camera_add(location=(0.0, 0.0, 5.0))
        camera = bpy.context.object
        camera.data.lens = 50.0
        scene.camera = camera

        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.4)
        sphere = bpy.context.object
        sphere.name = "DeepSurface"
        material = bpy.data.materials.new("DeepSurface")
        material.diffuse_color = (0.8, 0.2, 0.1, 1.0)
        sphere.data.materials.append(material)

        bpy.ops.object.light_add(type="AREA", location=(2.0, 2.0, 4.0))
        bpy.context.object.data.energy = 500.0
        bpy.context.object.data.shape = "DISK"
        bpy.context.object.data.size = 4.0

        scene.use_nodes = True
        node_tree = bpy.data.node_groups.new("DeepPrecedenceCompositor", "CompositorNodeTree")
        scene.compositing_node_group = node_tree
        node_tree.nodes.clear()

        render_layers = node_tree.nodes.new("CompositorNodeRLayers")
        file_output = node_tree.nodes.new("CompositorNodeOutputFile")
        file_output.format.media_type = "IMAGE"
        file_output.format.file_format = "DEEP_EXR"
        file_output.format.color_mode = "RGBA"
        file_output.format.color_depth = "32"
        file_output.format.deep_merge_tolerance = 0.0
        file_output.format.deep_alpha_merge_tolerance = 0.0
        item = file_output.file_output_items.new("RGBA", "Deep")
        node_tree.links.new(render_layers.outputs["Image"], file_output.inputs[item.name])

        node_tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
        group_output = node_tree.nodes.new("NodeGroupOutput")
        node_tree.links.new(render_layers.outputs["Image"], group_output.inputs[0])
        return scene, file_output

    def _render_deep(self, name, scene_tolerance, color_depth="32", node_tolerance=0.0):
        output_directory = self.output_root / name
        if output_directory.exists():
            shutil.rmtree(output_directory)
        output_directory.mkdir(parents=True)

        self.file_output.directory = str(output_directory)
        self.file_output.format.color_depth = color_depth
        self.file_output.format.deep_merge_tolerance = node_tolerance
        self.file_output.format.deep_alpha_merge_tolerance = node_tolerance
        image_settings = self.scene.render.image_settings
        image_settings.deep_merge_tolerance = scene_tolerance
        image_settings.deep_alpha_merge_tolerance = scene_tolerance
        bpy.ops.render.render()

        paths = sorted(output_directory.rglob("*.exr"))
        self.assertEqual(len(paths), 1, f"Expected one Deep EXR in {output_directory}")
        return paths[0]

    def _render_sample_counts(self, name, scene_tolerance, node_tolerance=0.0):
        path = self._render_deep(name, scene_tolerance, node_tolerance=node_tolerance)

        image_input = oiio.ImageInput.open(str(path))
        self.assertIsNotNone(image_input, f"Unable to open {path}")
        try:
            spec = image_input.spec()
            self.assertTrue(spec.deep, f"Expected a Deep EXR: {path}")
            deep_data = image_input.read_native_deep_image()
            return [deep_data.samples(index) for index in range(spec.width * spec.height)]
        finally:
            image_input.close()

    def test_deep_node_settings_override_scene_defaults_when_local(self):
        unmerged_counts = self._render_sample_counts("scene_unmerged", 0.0)
        scene_merged_counts = self._render_sample_counts("scene_merged", 1.0)

        self.assertTrue(
            any(count > 1 for count in unmerged_counts),
            "The generated scene must exercise pixels with multiple Deep samples",
        )
        self.assertEqual(
            scene_merged_counts,
            unmerged_counts,
            "A compositor node's local merge settings must own its Deep EXR output",
        )

    def test_deep_node_merge_is_not_reapplied_by_matching_scene_tolerance(self):
        node_only_counts = self._render_sample_counts("node_merge_only", 0.0, node_tolerance=0.1)
        matching_scene_counts = self._render_sample_counts(
            "node_and_scene_merge", 0.1, node_tolerance=0.1
        )

        self.assertEqual(
            matching_scene_counts,
            node_only_counts,
            "A compositor node's merge policy must be applied exactly once",
        )

    def test_unmapped_deep_file_output_does_not_write_fallback_layer(self):
        output_directory = self.output_root / "unmapped"
        output_directory.mkdir(parents=True)
        self.file_output.directory = str(output_directory)

        node_tree = self.scene.compositing_node_group
        for input_socket in self.file_output.inputs:
            for link in list(input_socket.links):
                node_tree.links.remove(link)

        bpy.ops.render.render()

        self.assertEqual(
            list(output_directory.rglob("*.exr")),
            [],
            "An unmapped Deep File Output must not write fallback view-layer data",
        )

    def test_deep_exr_codec_defaults_to_zips(self):
        scene_format = self.scene.render.image_settings
        scene_format.file_format = "PNG"
        scene_format.exr_codec = "NONE"
        scene_format.file_format = "DEEP_EXR"
        self.assertEqual(scene_format.exr_codec, "ZIPS")

        file_output = self.scene.compositing_node_group.nodes.new("CompositorNodeOutputFile")
        file_output.format.media_type = "IMAGE"
        file_output.format.file_format = "PNG"
        file_output.format.exr_codec = "NONE"
        file_output.format.file_format = "DEEP_EXR"
        self.assertEqual(file_output.format.exr_codec, "ZIPS")

    def test_existing_project_deep_node_ignores_disabled_legacy_toggle(self):
        self.scene.cycles["use_deep_output"] = False
        blend_path = self.output_root / "existing_project.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))

        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.wm.open_mainfile(filepath=str(blend_path))
        self.scene = bpy.context.scene
        self.assertNotIn(
            "use_deep_output",
            type(self.scene.cycles).bl_rna.properties.keys(),
            "Deep capture must not expose or deserialize a hidden user-controlled enable switch",
        )
        self.file_output = next(
            node
            for node in self.scene.compositing_node_group.nodes
            if node.bl_idname == "CompositorNodeOutputFile"
            and node.format.file_format == "DEEP_EXR"
        )

        sample_counts = self._render_sample_counts("existing_project", 0.0)
        self.assertTrue(
            any(count > 0 for count in sample_counts),
            "A valid Deep File Output node must capture samples after loading an existing project, "
            "regardless of the deprecated hidden toggle stored in the blend file",
        )

    def test_deep_rgba_half_channels_honor_color_depth(self):
        path = self._render_deep("half_channels", 0.0, color_depth="16")

        image_input = oiio.ImageInput.open(str(path))
        self.assertIsNotNone(image_input, f"Unable to open {path}")
        try:
            spec = image_input.spec()
            self.assertTrue(spec.deep, f"Expected a Deep EXR: {path}")
            channel_formats = {
                channel_name: (
                    spec.channelformats[spec.channelindex(channel_name)]
                    if spec.channelformats
                    else spec.format
                )
                for channel_name in ("R", "G", "B", "A", "Z", "ZBack")
            }
        finally:
            image_input.close()

        for channel_name in ("R", "G", "B", "A"):
            self.assertEqual(channel_formats[channel_name], oiio.HALF)
        for channel_name in ("Z", "ZBack"):
            self.assertEqual(channel_formats[channel_name], oiio.FLOAT)

    def test_mixed_surface_volume_deep_rgb_matches_flat_output(self):
        self.scene.render.resolution_x = 32
        self.scene.render.resolution_y = 32
        self.scene.cycles.samples = 128
        self.scene.view_layers[0].cycles.pass_debug_sample_count = True

        self.scene.objects["DeepSurface"].hide_render = True
        bpy.ops.mesh.primitive_plane_add(size=0.15, location=(0.0, 0.0, 0.0))
        surface = bpy.context.object
        surface.data.materials.append(bpy.data.materials["DeepSurface"])

        bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, -4.5))
        volume = bpy.context.object
        material = bpy.data.materials.new("DeepVolume")
        material.use_nodes = True
        material.node_tree.nodes.clear()
        output = material.node_tree.nodes.new("ShaderNodeOutputMaterial")
        principled_volume = material.node_tree.nodes.new("ShaderNodeVolumePrincipled")
        principled_volume.inputs["Color"].default_value = (0.1, 0.25, 0.8, 1.0)
        principled_volume.inputs["Density"].default_value = 0.35
        material.node_tree.links.new(principled_volume.outputs["Volume"], output.inputs["Volume"])
        volume.data.materials.append(material)

        node_tree = self.scene.compositing_node_group
        render_layers = next(
            node for node in node_tree.nodes if node.bl_idname == "CompositorNodeRLayers"
        )
        flat_output = node_tree.nodes.new("CompositorNodeOutputFile")
        flat_output.format.media_type = "IMAGE"
        flat_output.format.file_format = "OPEN_EXR"
        flat_output.format.color_mode = "RGBA"
        flat_output.format.color_depth = "32"
        flat_item = flat_output.file_output_items.new("RGBA", "Flat")
        node_tree.links.new(render_layers.outputs["Image"], flat_output.inputs[flat_item.name])

        deep_directory = self.output_root / "mixed_surface_volume" / "deep"
        flat_directory = self.output_root / "mixed_surface_volume" / "flat"
        deep_directory.mkdir(parents=True)
        flat_directory.mkdir(parents=True)
        self.file_output.directory = str(deep_directory)
        flat_output.directory = str(flat_directory)
        bpy.ops.render.render()

        deep_paths = sorted(deep_directory.rglob("*.exr"))
        flat_paths = sorted(flat_directory.rglob("*.exr"))
        self.assertEqual(len(deep_paths), 1, f"Expected one Deep EXR in {deep_directory}")
        self.assertEqual(len(flat_paths), 1, f"Expected one flat EXR in {flat_directory}")

        deep_input = oiio.ImageInput.open(str(deep_paths[0]))
        flat_input = oiio.ImageInput.open(str(flat_paths[0]))
        self.assertIsNotNone(deep_input, f"Unable to open {deep_paths[0]}")
        self.assertIsNotNone(flat_input, f"Unable to open {flat_paths[0]}")
        try:
            deep_spec = deep_input.spec()
            flat_spec = flat_input.spec()
            self.assertTrue(deep_spec.deep, f"Expected a Deep EXR: {deep_paths[0]}")
            self.assertEqual((deep_spec.width, deep_spec.height), (flat_spec.width, flat_spec.height))
            deep_data = deep_input.read_native_deep_image()
            flat_pixels = flat_input.read_image("float")
            deep_channels = {
                channel_name: deep_spec.channelindex(channel_name)
                for channel_name in ("R", "G", "B", "A", "Z", "ZBack")
            }
            flat_channels = {
                channel_name: flat_spec.channelindex(channel_name)
                for channel_name in ("R", "G", "B")
            }
        finally:
            deep_input.close()
            flat_input.close()

        mixed_pixel_errors = []
        worst_pixel = None
        for y in range(deep_spec.height):
            for x in range(deep_spec.width):
                pixel_index = y * deep_spec.width + x
                transparency = 1.0
                flattened_rgb = [0.0, 0.0, 0.0]
                has_fractional_surface = False
                has_contributing_volume = False
                for sample_index in range(deep_data.samples(pixel_index)):
                    alpha = float(deep_data.deep_value(pixel_index, deep_channels["A"], sample_index))
                    z = float(deep_data.deep_value(pixel_index, deep_channels["Z"], sample_index))
                    z_back = float(
                        deep_data.deep_value(pixel_index, deep_channels["ZBack"], sample_index)
                    )
                    has_fractional_surface = has_fractional_surface or (
                        abs(z_back - z) <= 1.0e-5 and 1.0e-5 < alpha < 1.0 - 1.0e-5
                    )
                    has_contributing_volume = has_contributing_volume or (
                        z_back > z + 1.0e-5 and alpha > 1.0e-5
                    )
                    for channel_index, channel_name in enumerate(("R", "G", "B")):
                        value = float(
                            deep_data.deep_value(
                                pixel_index, deep_channels[channel_name], sample_index
                            )
                        )
                        flattened_rgb[channel_index] += transparency * value
                    transparency *= 1.0 - alpha

                if has_fractional_surface and has_contributing_volume:
                    flat_rgb = [
                        float(flat_pixels[y, x, flat_channels[channel_name]])
                        for channel_name in ("R", "G", "B")
                    ]
                    error = max(
                        abs(deep_value - flat_value)
                        for deep_value, flat_value in zip(flattened_rgb, flat_rgb)
                    )
                    mixed_pixel_errors.append(error)
                    if worst_pixel is None or error > worst_pixel[0]:
                        worst_pixel = (error, x, y, flattened_rgb, flat_rgb)

        self.assertTrue(
            mixed_pixel_errors,
            "The generated scene must contain a fractional surface prefix and contributing volume tail",
        )
        self.assertLessEqual(
            max(mixed_pixel_errors),
            0.02,
            f"mixed pixels={len(mixed_pixel_errors)}, worst={worst_pixel}",
        )


if __name__ == "__main__":
    unittest.main(argv=[__file__])
