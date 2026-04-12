from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
OUTPUT_DIR = ROOT / "validation" / "final_matrix_2026-04-08"
DARK_PATH = OUTPUT_DIR / "foggy_street_hdri_ignore_dark.png"
BRIGHT_PATH = OUTPUT_DIR / "foggy_street_hdri_ignore_bright.png"
REPORT_PATH = OUTPUT_DIR / "foggy_street_hdri_ignore_report.txt"


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def find_background_node() -> bpy.types.Node:
    world = bpy.context.scene.world
    if world is None or not world.use_nodes or world.node_tree is None:
        raise RuntimeError("Active scene world does not use nodes.")

    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeBackground":
            return node

    raise RuntimeError("Background node not found in active world.")


def make_black_override() -> bpy.types.Material:
    material = bpy.data.materials.new(name="FogHdrIIgnoreBlackOverride")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    principled = nodes.get("Principled BSDF")
    if principled is None:
        raise RuntimeError("Principled BSDF not found in override material.")

    principled.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    principled.inputs["Roughness"].default_value = 1.0
    principled.inputs["Metallic"].default_value = 0.0
    if "Specular IOR Level" in principled.inputs:
        principled.inputs["Specular IOR Level"].default_value = 0.0

    return material


def snapshot_light_state() -> dict[str, bool]:
    state: dict[str, bool] = {}
    for obj in bpy.data.objects:
        if obj.type == "LIGHT":
            state[obj.name] = obj.hide_render
    return state


def restore_light_state(state: dict[str, bool]) -> None:
    for obj in bpy.data.objects:
        if obj.type == "LIGHT" and obj.name in state:
            obj.hide_render = state[obj.name]


def disable_all_lights() -> None:
    for obj in bpy.data.objects:
        if obj.type == "LIGHT":
            obj.hide_render = True


def render(output_path: Path) -> None:
    scene = bpy.context.scene
    scene.render.filepath = str(output_path)
    bpy.ops.render.render(write_still=True)


def load_pixels(path: Path) -> tuple[list[float], int]:
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = list(image.pixels[:])
    width = image.size[0]
    bpy.data.images.remove(image)
    return pixels, width


def compute_stats(pixels: list[float]) -> dict[str, float]:
    pixel_count = len(pixels) // 4
    total_r = 0.0
    total_g = 0.0
    total_b = 0.0
    total_a = 0.0

    for i in range(0, len(pixels), 4):
        total_r += pixels[i]
        total_g += pixels[i + 1]
        total_b += pixels[i + 2]
        total_a += pixels[i + 3]

    return {
        "avg_r": total_r / pixel_count,
        "avg_g": total_g / pixel_count,
        "avg_b": total_b / pixel_count,
        "avg_a": total_a / pixel_count,
    }


def compute_abs_diff(a: list[float], b: list[float]) -> dict[str, float]:
    if len(a) != len(b):
        raise RuntimeError("Image sizes do not match for diff.")

    pixel_count = len(a) // 4
    total_rgb = 0.0
    max_rgb = 0.0

    for i in range(0, len(a), 4):
        dr = abs(a[i] - b[i])
        dg = abs(a[i + 1] - b[i + 1])
        db = abs(a[i + 2] - b[i + 2])
        rgb = (dr + dg + db) / 3.0
        total_rgb += rgb
        if rgb > max_rgb:
            max_rgb = rgb

    return {
        "avg_rgb_abs_diff": total_rgb / pixel_count,
        "max_rgb_abs_diff": max_rgb,
    }


def main() -> None:
    ensure_dir(OUTPUT_DIR)
    scene = bpy.context.scene
    view_layer = bpy.context.view_layer
    background = find_background_node()
    black_override = make_black_override()

    original_samples = scene.cycles.samples
    original_adaptive = scene.cycles.use_adaptive_sampling
    original_film_transparent = scene.render.film_transparent
    original_color_mode = scene.render.image_settings.color_mode
    original_material_override = view_layer.material_override
    original_color = tuple(background.inputs["Color"].default_value)
    original_strength = background.inputs["Strength"].default_value
    original_light_state = snapshot_light_state()

    try:
        scene.cycles.samples = 8
        scene.cycles.use_adaptive_sampling = False
        scene.render.film_transparent = True
        scene.render.image_settings.file_format = "PNG"
        scene.render.image_settings.color_mode = "RGBA"
        view_layer.material_override = black_override
        disable_all_lights()

        background.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
        background.inputs["Strength"].default_value = 0.0
        render(DARK_PATH)

        background.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        background.inputs["Strength"].default_value = 5000.0
        render(BRIGHT_PATH)

        dark_pixels, _ = load_pixels(DARK_PATH)
        bright_pixels, _ = load_pixels(BRIGHT_PATH)
        dark_stats = compute_stats(dark_pixels)
        bright_stats = compute_stats(bright_pixels)
        diff_stats = compute_abs_diff(dark_pixels, bright_pixels)

        REPORT_PATH.write_text(
            "\n".join(
                [
                    "World Environment Fog HDRI Ignore Proof",
                    "Setup:",
                    "- all explicit scene lights hidden from render",
                    "- film transparent enabled",
                    "- view-layer material override set to pure black",
                    "- only world background surface changed between renders",
                    "",
                    f"Dark avg RGBA: {dark_stats['avg_r']:.6f}, {dark_stats['avg_g']:.6f}, {dark_stats['avg_b']:.6f}, {dark_stats['avg_a']:.6f}",
                    f"Bright avg RGBA: {bright_stats['avg_r']:.6f}, {bright_stats['avg_g']:.6f}, {bright_stats['avg_b']:.6f}, {bright_stats['avg_a']:.6f}",
                    f"Average absolute RGB diff: {diff_stats['avg_rgb_abs_diff']:.6f}",
                    f"Max absolute RGB diff: {diff_stats['max_rgb_abs_diff']:.6f}",
                ]
            ),
            encoding="utf-8",
        )
    finally:
        background.inputs["Color"].default_value = original_color
        background.inputs["Strength"].default_value = original_strength
        restore_light_state(original_light_state)
        view_layer.material_override = original_material_override
        scene.render.film_transparent = original_film_transparent
        scene.render.image_settings.color_mode = original_color_mode
        scene.cycles.samples = original_samples
        scene.cycles.use_adaptive_sampling = original_adaptive
        bpy.data.materials.remove(black_override)


if __name__ == "__main__":
    main()
