# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import sys
import unittest
from pathlib import Path

import bpy


current_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.append(current_dir)

from modules.exr_overscan_utils import (
    WINDOW_FIELDS,
    render_case,
    render_item_override_case,
    render_primary_png_case,
    render_view_layer_preservation_case,
)


class ExrOverscanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.outdir = Path(args.outdir)
        cls.outdir.mkdir(parents=True, exist_ok=True)
        cls._case_cache = {}

    @classmethod
    def _run_case(cls, case_name: str, *, crop: bool, overscan: bool):
        if case_name not in cls._case_cache:
            cls._case_cache[case_name] = render_case(
                case_name,
                cls.outdir,
                crop=crop,
                overscan=overscan,
            )
        return cls._case_cache[case_name]

    def test_output_panel_uses_unique_exr_identifier(self):
        panel = getattr(bpy.types, "RENDER_PT_exr_overscan", None)
        self.assertIsNotNone(panel)
        self.assertEqual(panel.bl_idname, "RENDER_PT_exr_overscan")
        self.assertFalse(hasattr(bpy.types, "RENDER_PT_overscan"))

    def test_offline_window_contract_matrix(self):
        cases = (
            ("no_crop_overscan_off", False, False),
            ("no_crop_overscan_on", False, True),
            ("crop_overscan_off", True, False),
            ("crop_overscan_on", True, True),
        )
        for case_name, crop, overscan in cases:
            with self.subTest(case=case_name):
                summary = self._run_case(case_name, crop=crop, overscan=overscan)
                self.assertEqual(summary["status"], "passed")
                self.assertEqual(summary["exr_window_parity"]["status"], "passed")
                self.assertEqual(summary["exr_window_placement"]["status"], "passed")
                self.assertTrue(summary["settings"]["compositor_is_file_output_only"])
                self.assertTrue(summary["exr_outputs"]["compositor"]["DEEP_EXR"]["spec"]["deep"])

    def test_render_region_disables_overscan_output(self):
        crop_off = self._run_case("crop_overscan_off", crop=True, overscan=False)
        crop_on = self._run_case("crop_overscan_on", crop=True, overscan=True)

        self.assertFalse(crop_on["settings"]["effective_overscan_expected"])
        self.assertTrue(crop_on["settings"]["requested_overscan"])

        for lane in ("compositor", "direct_render"):
            for output_type in crop_off["exr_outputs"][lane]:
                with self.subTest(lane=lane, output_type=output_type):
                    off_spec = crop_off["exr_outputs"][lane][output_type]["spec"]
                    on_spec = crop_on["exr_outputs"][lane][output_type]["spec"]
                    for field in WINDOW_FIELDS:
                        self.assertEqual(off_spec[field], on_spec[field])

    def test_overscan_expands_full_frame_without_moving_delivery_window(self):
        off_summary = self._run_case("no_crop_overscan_off", crop=False, overscan=False)
        on_summary = self._run_case("no_crop_overscan_on", crop=False, overscan=True)

        off_spec = off_summary["exr_outputs"]["compositor"]["OPEN_EXR_MULTILAYER"]["spec"]
        on_spec = on_summary["exr_outputs"]["compositor"]["OPEN_EXR_MULTILAYER"]["spec"]

        self.assertGreater(on_spec["width"], off_spec["width"])
        self.assertGreater(on_spec["height"], off_spec["height"])
        self.assertEqual(on_spec["full_x"], 0)
        self.assertEqual(on_spec["full_y"], 0)
        self.assertEqual(on_spec["full_width"], off_spec["width"])
        self.assertEqual(on_spec["full_height"], off_spec["height"])

    def test_item_format_override_to_exr_activates_overscan(self):
        summary = render_item_override_case("item_override_exr", self.outdir)
        self.assertFalse(summary["mismatches"], summary)

    def test_overscan_preserves_completed_view_layers(self):
        for overscan in (False, True):
            with self.subTest(overscan=overscan):
                summary = render_view_layer_preservation_case(
                    "view_layer_preservation_" + ("on" if overscan else "off"),
                    self.outdir,
                    overscan=overscan,
                )
                self.assertTrue(summary["passed"], summary)

    def test_tiled_overscan_restores_pass_windows_after_writeback(self):
        summary = render_view_layer_preservation_case(
            "view_layer_preservation_tiled", self.outdir, overscan=True, tiled=True
        )
        self.assertTrue(summary["passed"], summary)

    def test_cross_scene_overscan_is_rejected_before_render(self):
        bpy.ops.wm.read_factory_settings(use_empty=False)
        scene = bpy.context.scene
        scene.render.engine = "CYCLES"
        scene.cycles.overscan_size = 15.0

        other = bpy.data.scenes.new("OtherScene")
        other.collection.objects.link(scene.camera)
        other.camera = scene.camera

        tree = bpy.data.node_groups.new("CrossSceneOverscan", "CompositorNodeTree")
        effect = scene.compositor_effects.new("Cross-scene Overscan")
        effect.node_group = tree
        effect.enable_for_render = True
        source = tree.nodes.new("CompositorNodeRLayers")
        source.scene = other
        target = tree.nodes.new("CompositorNodeOutputFile")
        target.directory = str(self.outdir / "cross_scene_overscan")
        target.file_name = "rejected"
        target.format.media_type = "IMAGE"
        target.format.file_format = "OPEN_EXR"
        target.file_output_items.new("RGBA", "Image")
        tree.links.new(source.outputs["Image"], target.inputs["Image"])

        with self.assertRaisesRegex(RuntimeError, "Overscan only supports single-scene rendering"):
            bpy.ops.render.render()
        self.assertFalse(Path(target.directory).exists())

    def test_asymmetric_vertical_overscan_crops_non_exr_from_correct_side(self):
        pads = (0, 0, 4, 20)
        reference = render_primary_png_case(
            "asymmetric_reference", self.outdir, overscan=False, overscan_pixels=pads
        )
        overscanned = render_primary_png_case(
            "asymmetric_overscan", self.outdir, overscan=True, overscan_pixels=pads
        )
        self.assertEqual(reference.shape, overscanned.shape)
        zero_shift_error = float(abs(reference[..., :3] - overscanned[..., :3]).mean())
        vertical_mismatch = pads[3] - pads[2]
        shifted_up_error = float(
            abs(
                reference[vertical_mismatch:, ..., :3]
                - overscanned[:-vertical_mismatch, ..., :3]
            ).mean()
        )
        shifted_down_error = float(
            abs(
                reference[:-vertical_mismatch, ..., :3]
                - overscanned[vertical_mismatch:, ..., :3]
            ).mean()
        )
        self.assertLess(zero_shift_error, 0.1)
        self.assertLess(zero_shift_error, shifted_up_error)
        self.assertLess(zero_shift_error, shifted_down_error)


if __name__ == "__main__":
    if "--" in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index("--") + 1 :]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser(
        description="Run self-contained EXR overscan window-contract tests."
    )
    parser.add_argument("--outdir", required=True)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)
