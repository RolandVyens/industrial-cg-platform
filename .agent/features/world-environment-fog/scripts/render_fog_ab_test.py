from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
VALIDATION_DIR = ROOT / "validation"
REPORT_PATH = VALIDATION_DIR / "fog_ab_report.txt"
FOG_OFF_PATH = VALIDATION_DIR / "foggy_street_cyberpunk_fog_off.png"
FOG_ON_PATH = VALIDATION_DIR / "foggy_street_cyberpunk_fog_on.png"


def find_fog_node():
    world = bpy.context.scene.world
    if not world or not world.node_tree:
        raise RuntimeError("Active world has no node tree")

    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeEnvironmentFog":
            return node

    raise RuntimeError("ShaderNodeEnvironmentFog not found in world")


def render_and_measure(output_path: Path):
    scene = bpy.context.scene
    scene.render.filepath = str(output_path)
    bpy.ops.render.render(write_still=True)

    image = bpy.data.images.load(str(output_path), check_existing=False)
    pixels = image.pixels[:]
    pixel_count = len(pixels) // 4

    total_r = 0.0
    total_g = 0.0
    total_b = 0.0
    total_luma = 0.0

    for i in range(0, len(pixels), 4):
        r = pixels[i]
        g = pixels[i + 1]
        b = pixels[i + 2]
        total_r += r
        total_g += g
        total_b += b
        total_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b

    stats = {
        "avg_r": total_r / pixel_count,
        "avg_g": total_g / pixel_count,
        "avg_b": total_b / pixel_count,
        "avg_luma": total_luma / pixel_count,
    }
    bpy.data.images.remove(image)
    return stats


def write_report(off_stats, on_stats, original_density):
    delta_luma = on_stats["avg_luma"] - off_stats["avg_luma"]
    delta_percent = 0.0
    if off_stats["avg_luma"] != 0.0:
      delta_percent = (delta_luma / off_stats["avg_luma"]) * 100.0

    REPORT_PATH.write_text(
        "\n".join(
            [
                "World Environment Fog A/B Report (Cyberpunk Scene)",
                f"Original density: {original_density}",
                f"Fog OFF avg RGB: {off_stats['avg_r']:.6f}, {off_stats['avg_g']:.6f}, {off_stats['avg_b']:.6f}",
                f"Fog OFF avg luma: {off_stats['avg_luma']:.6f}",
                f"Fog ON avg RGB: {on_stats['avg_r']:.6f}, {on_stats['avg_g']:.6f}, {on_stats['avg_b']:.6f}",
                f"Fog ON avg luma: {on_stats['avg_luma']:.6f}",
                f"Luma delta: {delta_luma:.6f}",
                f"Luma delta percent: {delta_percent:.3f}",
            ]
        ),
        encoding="utf-8",
    )


def main():
    fog_node = find_fog_node()
    density_socket = fog_node.inputs["Density"]
    original_density = density_socket.default_value

    density_socket.default_value = 0.0
    off_stats = render_and_measure(FOG_OFF_PATH)

    density_socket.default_value = original_density
    on_stats = render_and_measure(FOG_ON_PATH)

    write_report(off_stats, on_stats, original_density)


if __name__ == "__main__":
    main()
