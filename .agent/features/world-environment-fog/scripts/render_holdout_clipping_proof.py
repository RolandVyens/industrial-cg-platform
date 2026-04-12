from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
OUTPUT_DIR = ROOT / "validation" / "final_matrix_2026-04-08"
BASELINE_PATH = OUTPUT_DIR / "foggy_street_holdout_baseline.png"
HOLDOUT_PATH = OUTPUT_DIR / "foggy_street_holdout_clipped.png"
REPORT_PATH = OUTPUT_DIR / "foggy_street_holdout_report.txt"


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def make_holdout_material() -> bpy.types.Material:
    material = bpy.data.materials.new(name="FogHoldoutProofMaterial")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    holdout = nodes.new(type="ShaderNodeHoldout")
    output = nodes.new(type="ShaderNodeOutputMaterial")
    links.new(holdout.outputs["Holdout"], output.inputs["Surface"])
    return material


def create_holdout_plane() -> bpy.types.Object:
    scene = bpy.context.scene
    camera = scene.camera
    if camera is None:
        raise RuntimeError("Active scene has no camera.")

    bpy.ops.mesh.primitive_plane_add()
    plane = bpy.context.active_object
    plane.name = "FogHoldoutProof"
    plane.parent = camera
    plane.matrix_parent_inverse = camera.matrix_world.inverted()
    plane.location = (0.42, 0.0, -1.0)
    plane.rotation_euler = (0.0, 0.0, 0.0)
    plane.scale = (0.72, 1.25, 1.0)
    return plane


def render(output_path: Path) -> None:
    scene = bpy.context.scene
    scene.render.filepath = str(output_path)
    bpy.ops.render.render(write_still=True)


def load_image(path: Path) -> tuple[list[float], int, int]:
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = list(image.pixels[:])
    width = image.size[0]
    height = image.size[1]
    bpy.data.images.remove(image)
    return pixels, width, height


def compute_roi_stats(
    pixels: list[float],
    width: int,
    height: int,
    x0: float,
    x1: float,
    y0: float,
    y1: float,
) -> dict[str, float]:
    ix0 = int(width * x0)
    ix1 = int(width * x1)
    iy0 = int(height * y0)
    iy1 = int(height * y1)

    total_luma = 0.0
    total_alpha = 0.0
    transparent_pixels = 0
    samples = 0

    for y in range(iy0, iy1):
        for x in range(ix0, ix1):
            index = (y * width + x) * 4
            r = pixels[index]
            g = pixels[index + 1]
            b = pixels[index + 2]
            a = pixels[index + 3]
            total_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b
            total_alpha += a
            if a < 0.01:
                transparent_pixels += 1
            samples += 1

    return {
        "avg_luma": total_luma / samples,
        "avg_alpha": total_alpha / samples,
        "transparent_ratio": transparent_pixels / samples,
    }


def main() -> None:
    ensure_dir(OUTPUT_DIR)
    scene = bpy.context.scene
    original_samples = scene.cycles.samples
    original_adaptive = scene.cycles.use_adaptive_sampling
    original_film_transparent = scene.render.film_transparent
    original_color_mode = scene.render.image_settings.color_mode

    holdout_material = make_holdout_material()
    holdout_plane = None

    try:
        scene.cycles.samples = 16
        scene.cycles.use_adaptive_sampling = False
        scene.render.film_transparent = True
        scene.render.image_settings.file_format = "PNG"
        scene.render.image_settings.color_mode = "RGBA"

        render(BASELINE_PATH)

        holdout_plane = create_holdout_plane()
        holdout_plane.data.materials.append(holdout_material)
        render(HOLDOUT_PATH)

        baseline_pixels, width, height = load_image(BASELINE_PATH)
        holdout_pixels, _, _ = load_image(HOLDOUT_PATH)
        roi = (0.55, 0.95, 0.15, 0.9)
        baseline_stats = compute_roi_stats(baseline_pixels, width, height, *roi)
        holdout_stats = compute_roi_stats(holdout_pixels, width, height, *roi)

        REPORT_PATH.write_text(
            "\n".join(
                [
                    "World Environment Fog Holdout Clipping Proof",
                    "Setup:",
                    "- film transparent enabled",
                    "- baseline render from the saved foggy street scene",
                    "- second render adds a camera-parented holdout plane over the right side of frame",
                    f"- ROI used for measurement: x={roi[0]:.2f}-{roi[1]:.2f}, y={roi[2]:.2f}-{roi[3]:.2f}",
                    "",
                    f"Baseline ROI avg luma: {baseline_stats['avg_luma']:.6f}",
                    f"Baseline ROI avg alpha: {baseline_stats['avg_alpha']:.6f}",
                    f"Baseline ROI transparent ratio: {baseline_stats['transparent_ratio']:.6f}",
                    f"Holdout ROI avg luma: {holdout_stats['avg_luma']:.6f}",
                    f"Holdout ROI avg alpha: {holdout_stats['avg_alpha']:.6f}",
                    f"Holdout ROI transparent ratio: {holdout_stats['transparent_ratio']:.6f}",
                ]
            ),
            encoding="utf-8",
        )
    finally:
        if holdout_plane is not None:
            bpy.data.objects.remove(holdout_plane, do_unlink=True)
        bpy.data.materials.remove(holdout_material)
        scene.render.film_transparent = original_film_transparent
        scene.render.image_settings.color_mode = original_color_mode
        scene.cycles.samples = original_samples
        scene.cycles.use_adaptive_sampling = original_adaptive


if __name__ == "__main__":
    main()
