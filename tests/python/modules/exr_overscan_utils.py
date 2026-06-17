# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from pathlib import Path

import bpy
import OpenImageIO as oiio
from mathutils import Vector


WINDOW_FIELDS = (
    "x",
    "y",
    "width",
    "height",
    "full_x",
    "full_y",
    "full_width",
    "full_height",
)


def _set_object_color(obj: bpy.types.Object, rgba: tuple[float, float, float, float]) -> None:
    material = bpy.data.materials.new(name=f"{obj.name}_Material")
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = rgba
    obj.data.materials.clear()
    obj.data.materials.append(material)


def _look_at(camera_obj: bpy.types.Object, target: Vector) -> None:
    direction = target - camera_obj.location
    camera_obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def exr_spec(path: Path) -> dict[str, object]:
    image_input = oiio.ImageInput.open(str(path))
    if image_input is None:
        raise RuntimeError(f"OpenImageIO failed to open {path}")

    try:
        spec = image_input.spec()
        return {
            "x": spec.x,
            "y": spec.y,
            "width": spec.width,
            "height": spec.height,
            "full_x": spec.full_x,
            "full_y": spec.full_y,
            "full_width": spec.full_width,
            "full_height": spec.full_height,
            "channels": list(spec.channelnames),
            "deep": bool(spec.deep),
        }
    finally:
        image_input.close()


def expected_exr_window(
    render_width: int,
    render_height: int,
    data_width: int,
    data_height: int,
    effective_overscan: bool,
) -> dict[str, int]:
    if not effective_overscan:
        return {
            "x": 0,
            "y": 0,
            "width": data_width,
            "height": data_height,
            "full_x": 0,
            "full_y": 0,
            "full_width": data_width,
            "full_height": data_height,
        }

    data_left = (render_width - data_width) // 2
    data_bottom = (render_height - data_height) // 2
    return {
        "x": data_left,
        "y": render_height - (data_bottom + data_height),
        "width": data_width,
        "height": data_height,
        "full_x": 0,
        "full_y": 0,
        "full_width": render_width,
        "full_height": render_height,
    }


def _exr_window_matches_expected(
    outputs: dict[str, dict[str, dict[str, object]]],
    expected: dict[str, int],
) -> dict[str, dict[str, dict[str, int | object]]]:
    mismatches: dict[str, dict[str, dict[str, int | object]]] = {}
    for lane, lane_outputs in outputs.items():
        for output_type, output in lane_outputs.items():
            actual = output["spec"]
            differences = {
                field: {"expected": expected[field], "actual": actual.get(field)}
                for field in WINDOW_FIELDS
                if actual.get(field) != expected[field]
            }
            if differences:
                mismatches[f"{lane}:{output_type}"] = differences
    return mismatches


def _exr_windows_match(
    compositor_outputs: dict[str, dict[str, object]],
    direct_outputs: dict[str, dict[str, object]],
    require_deep: bool,
) -> tuple[bool, list[str]]:
    required_compositor = ["OPEN_EXR", "OPEN_EXR_MULTILAYER"]
    if require_deep:
        required_compositor.append("DEEP_EXR")
    required_direct = ["OPEN_EXR", "OPEN_EXR_MULTILAYER"]

    missing = [
        f"compositor:{output_type}"
        for output_type in required_compositor
        if output_type not in compositor_outputs
    ]
    missing.extend(
        f"direct:{output_type}" for output_type in required_direct if output_type not in direct_outputs
    )
    if missing:
        return False, missing

    reference = compositor_outputs["OPEN_EXR_MULTILAYER"]["spec"]
    all_outputs = [
        compositor_outputs[output_type]["spec"] for output_type in required_compositor
    ] + [direct_outputs[output_type]["spec"] for output_type in required_direct]
    matches = all(
        all(spec.get(field) == reference.get(field) for field in WINDOW_FIELDS)
        for spec in all_outputs
    )
    return matches, []


def _configure_scene(
    crop: bool,
    overscan: bool,
    overscan_percent: float,
    samples: int,
    resolution_x: int,
    resolution_y: int,
) -> bpy.types.Scene:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.render.use_compositing = True
    scene.render.use_border = crop
    scene.render.use_crop_to_border = crop
    scene.render.border_min_x = 0.02
    scene.render.border_max_x = 0.30
    scene.render.border_min_y = 0.02
    scene.render.border_max_y = 0.34
    scene.render.resolution_x = resolution_x
    scene.render.resolution_y = resolution_y
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False

    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.cycles.preview_samples = samples
    scene.cycles.use_adaptive_sampling = False
    if hasattr(scene.cycles, "use_denoising"):
        scene.cycles.use_denoising = False
    if hasattr(scene.cycles, "use_preview_denoising"):
        scene.cycles.use_preview_denoising = False
    scene.cycles.overscan_mode = "PERCENTAGE"
    scene.cycles.overscan_size = overscan_percent if overscan else 0.0

    world = bpy.data.worlds.new("OverscanWorld")
    world.use_nodes = True
    background = world.node_tree.nodes["Background"]
    background.inputs["Color"].default_value = (0.035, 0.04, 0.06, 1.0)
    background.inputs["Strength"].default_value = 0.7
    scene.world = world

    bpy.ops.mesh.primitive_plane_add(location=(0.0, 0.0, -0.8), size=14.0)
    _set_object_color(bpy.context.active_object, (0.16, 0.18, 0.2, 1.0))

    bpy.ops.mesh.primitive_cube_add(location=(0.0, 0.0, 0.1), scale=(0.9, 0.9, 0.9))
    _set_object_color(bpy.context.active_object, (0.24, 0.48, 0.92, 1.0))

    bpy.ops.mesh.primitive_uv_sphere_add(location=(2.95, 0.15, 0.3), scale=(0.95, 0.95, 0.95))
    _set_object_color(bpy.context.active_object, (0.92, 0.28, 0.16, 1.0))

    bpy.ops.object.light_add(type="SUN", location=(2.0, -4.0, 6.0))
    sun = bpy.context.active_object
    sun.data.energy = 3.0

    bpy.ops.object.camera_add(location=(0.0, -6.2, 1.15))
    camera = bpy.context.active_object
    camera.data.lens = 40.0
    _look_at(camera, Vector((0.7, 0.0, 0.1)))
    scene.camera = camera

    return scene


def _create_file_output_node(
    node_tree: bpy.types.NodeTree,
    render_layers_node: bpy.types.Node,
    output_dir: Path,
    output_type: str,
    stem: str,
) -> None:
    node = node_tree.nodes.new("CompositorNodeOutputFile")
    node.name = f"Test{output_type}"
    node.label = f"Test {output_type}"
    node.directory = str(output_dir)
    node.file_name = stem
    if output_type == "OPEN_EXR_MULTILAYER":
        node.format.media_type = "MULTI_LAYER_IMAGE"
    else:
        node.format.media_type = "IMAGE"
    node.format.file_format = output_type
    item = node.file_output_items.new("RGBA", f"{output_type}_Input")
    node_tree.links.new(render_layers_node.outputs["Image"], node.inputs[item.name])


def _configure_file_output_tree(scene: bpy.types.Scene, output_dir: Path, include_deep: bool) -> None:
    scene.use_nodes = True
    node_tree = bpy.data.node_groups.new("OverscanCompositor", "CompositorNodeTree")
    scene.compositing_node_group = node_tree
    node_tree.nodes.clear()

    render_layers = node_tree.nodes.new("CompositorNodeRLayers")
    render_layers.location = (0.0, 0.0)

    _create_file_output_node(node_tree, render_layers, output_dir, "OPEN_EXR", "comp_single_")
    _create_file_output_node(
        node_tree, render_layers, output_dir, "OPEN_EXR_MULTILAYER", "comp_multi_"
    )
    if include_deep:
        _create_file_output_node(node_tree, render_layers, output_dir, "DEEP_EXR", "comp_deep_")


def _find_output(path_root: Path, prefix: str) -> Path:
    matches = sorted(path_root.rglob(f"{prefix}*.exr"))
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one EXR matching {prefix!r} under {path_root}, found {matches}")
    return matches[0]


def _collect_compositor_outputs(output_dir: Path, include_deep: bool) -> dict[str, dict[str, object]]:
    outputs = {
        "OPEN_EXR": {"path": str(_find_output(output_dir, "comp_single_"))},
        "OPEN_EXR_MULTILAYER": {"path": str(_find_output(output_dir, "comp_multi_"))},
    }
    if include_deep:
        outputs["DEEP_EXR"] = {"path": str(_find_output(output_dir, "comp_deep_"))}

    for output in outputs.values():
        output["spec"] = exr_spec(Path(output["path"]))
    return outputs


def _save_direct_render_outputs(
    render_result: bpy.types.Image,
    scene: bpy.types.Scene,
    output_dir: Path,
) -> dict[str, dict[str, object]]:
    outputs: dict[str, dict[str, object]] = {}
    original_media_type = scene.render.image_settings.media_type
    original_format = scene.render.image_settings.file_format
    try:
        for output_type in ("OPEN_EXR", "OPEN_EXR_MULTILAYER"):
            scene.render.image_settings.media_type = (
                "IMAGE" if output_type == "OPEN_EXR" else "MULTI_LAYER_IMAGE"
            )
            scene.render.image_settings.file_format = output_type
            path = output_dir / f"RenderResult_{output_type}.exr"
            render_result.save_render(filepath=str(path), scene=scene)
            outputs[output_type] = {"path": str(path), "spec": exr_spec(path)}
    finally:
        scene.render.image_settings.media_type = original_media_type
        scene.render.image_settings.file_format = original_format
    return outputs


def render_case(
    case_name: str,
    output_root: Path,
    *,
    crop: bool,
    overscan: bool,
    overscan_percent: float = 10.0,
    samples: int = 1,
    resolution_x: int = 320,
    resolution_y: int = 180,
    include_deep: bool = True,
) -> dict[str, object]:
    case_root = output_root / case_name
    file_output_root = case_root / "file_output"
    direct_output_root = case_root / "direct_render"
    file_output_root.mkdir(parents=True, exist_ok=True)
    direct_output_root.mkdir(parents=True, exist_ok=True)

    scene = _configure_scene(
        crop=crop,
        overscan=overscan,
        overscan_percent=overscan_percent,
        samples=samples,
        resolution_x=resolution_x,
        resolution_y=resolution_y,
    )
    _configure_file_output_tree(scene, file_output_root, include_deep)

    bpy.ops.render.render(write_still=False)

    render_result = bpy.data.images.get("Render Result")
    if render_result is None:
        raise RuntimeError("Render Result is unavailable")

    compositor_outputs = _collect_compositor_outputs(file_output_root, include_deep)
    direct_outputs = _save_direct_render_outputs(render_result, scene, direct_output_root)

    windows_match, missing_outputs = _exr_windows_match(
        compositor_outputs,
        direct_outputs,
        require_deep=include_deep,
    )
    preview_spec = compositor_outputs["OPEN_EXR_MULTILAYER"]["spec"]
    effective_overscan = overscan and not crop
    expected_window = expected_exr_window(
        resolution_x,
        resolution_y,
        preview_spec["width"],
        preview_spec["height"],
        effective_overscan,
    )
    outputs = {
        "compositor": compositor_outputs,
        "direct_render": direct_outputs,
    }
    expected_mismatches = _exr_window_matches_expected(outputs, expected_window)

    return {
        "case_name": case_name,
        "status": "passed" if windows_match and not expected_mismatches else "failed",
        "settings": {
            "crop": crop,
            "requested_overscan": overscan,
            "effective_overscan_expected": effective_overscan,
            "overscan_percent": overscan_percent if overscan else 0.0,
            "use_overscan_latched": bool(getattr(scene.cycles, "use_overscan", False)),
            "compositor_is_file_output_only": all(
                node.bl_idname != "CompositorNodeComposite" for node in scene.compositing_node_group.nodes
            ),
        },
        "exr_outputs": outputs,
        "exr": preview_spec,
        "exr_window_parity": {
            "status": "passed" if windows_match else "failed",
            "missing_formats": missing_outputs,
        },
        "exr_window_placement": {
            "status": "passed" if not expected_mismatches else "failed",
            "expected": expected_window,
            "mismatches": expected_mismatches,
        },
    }
