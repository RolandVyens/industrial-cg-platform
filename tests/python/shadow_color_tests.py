# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import tempfile
import unittest
from pathlib import Path

import bpy
import numpy as np
import OpenImageIO as oiio
from mathutils import Vector


class ShadowColorTests(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.output_dir = tempfile.TemporaryDirectory()
        self.scene, self.world = self.create_scene()

    def tearDown(self):
        self.output_dir.cleanup()

    @staticmethod
    def point_at(obj, target):
        obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()

    def create_scene(self):
        scene = bpy.context.scene
        scene.render.engine = "CYCLES"
        scene.cycles.device = "CPU"
        scene.cycles.samples = 64
        scene.cycles.use_adaptive_sampling = False
        scene.cycles.use_denoising = False
        scene.cycles.seed = 7
        scene.cycles.max_bounces = 2
        scene.render.resolution_x = 64
        scene.render.resolution_y = 64
        scene.render.resolution_percentage = 100
        scene.render.image_settings.file_format = "OPEN_EXR"
        scene.render.image_settings.color_mode = "RGBA"
        scene.render.image_settings.color_depth = "32"
        scene.render.film_transparent = False

        world = bpy.data.worlds.new("ConstantWorld")
        world.use_nodes = True
        background = world.node_tree.nodes.get("Background")
        background.inputs["Color"].default_value = (0.8, 0.8, 0.8, 1.0)
        background.inputs["Strength"].default_value = 1.0
        world.cycles.sampling_method = "AUTOMATIC"
        scene.world = world

        material = bpy.data.materials.new("DiffuseWhite")
        material.use_nodes = True
        principled = material.node_tree.nodes.get("Principled BSDF")
        principled.inputs["Base Color"].default_value = (0.8, 0.8, 0.8, 1.0)
        principled.inputs["Roughness"].default_value = 1.0

        bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
        bpy.context.object.data.materials.append(material)

        bpy.ops.mesh.primitive_uv_sphere_add(
            segments=32, ring_count=16, radius=0.8, location=(0.0, 0.0, 1.0)
        )
        bpy.context.object.data.materials.append(material)

        camera_data = bpy.data.cameras.new("Camera")
        camera = bpy.data.objects.new("Camera", camera_data)
        bpy.context.collection.objects.link(camera)
        camera.location = (4.0, -6.0, 5.0)
        self.point_at(camera, (0.0, 0.0, 0.5))
        camera_data.type = "ORTHO"
        camera_data.ortho_scale = 6.0
        scene.camera = camera
        return scene, world

    def render(self, label, shadow_color):
        self.world.cycles.shadow_color = shadow_color
        output_path = Path(self.output_dir.name) / f"constant_world_{label}.exr"
        self.scene.render.filepath = str(output_path)
        self.assertEqual(bpy.ops.render.render(write_still=True), {"FINISHED"})

        image_input = oiio.ImageInput.open(str(output_path))
        self.assertIsNotNone(image_input)
        try:
            spec = image_input.spec()
            pixels = image_input.read_image("float")
            return np.asarray(pixels, dtype=np.float32).reshape(
                spec.height, spec.width, spec.nchannels
            )
        finally:
            image_input.close()

    def test_constant_world_uses_shadow_color(self):
        black = self.render("black", (0.0, 0.0, 0.0))
        red = self.render("red", (1.0, 0.0, 0.0))
        delta = red[..., :3] - black[..., :3]
        red_gain = delta[..., 0]

        self.assertGreater(float(np.max(red_gain)), 0.01)
        self.assertGreater(int(np.count_nonzero(red_gain > 0.001)), 8)
        self.assertGreater(float(np.mean(red_gain)), float(np.mean(delta[..., 1])) + 1.0e-4)
        self.assertGreater(float(np.mean(red_gain)), float(np.mean(delta[..., 2])) + 1.0e-4)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
