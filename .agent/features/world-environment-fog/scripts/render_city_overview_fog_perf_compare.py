from pathlib import Path
import time

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
OUTPUT_DIR = ROOT / "validation" / "perf_city_overview_2026-04-11"
REPORT_PATH = OUTPUT_DIR / "city_overview_fog_perf_compare_report.txt"
FOG_OFF_PATH = OUTPUT_DIR / "cyberpunk_city_overview_fog_off_0001.png"
FOG_ON_PATH = OUTPUT_DIR / "cyberpunk_city_overview_fog_on_0001.png"


def find_fog_node():
    world = bpy.context.scene.world
    if not world or not world.node_tree:
        raise RuntimeError("Active world has no node tree.")

    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeEnvironmentFog":
            return node

    raise RuntimeError("ShaderNodeEnvironmentFog not found in active world.")


def ensure_optix():
    scene = bpy.context.scene
    scene.cycles.device = "GPU"

    prefs = bpy.context.preferences.addons["cycles"].preferences
    compute_device_type = getattr(prefs, "compute_device_type", "")
    if compute_device_type != "OPTIX":
        raise RuntimeError(f"Expected OPTIX compute device type, got '{compute_device_type}'.")

    return [
        f"scene.cycles.device: {scene.cycles.device}",
        f"preferences.compute_device_type: {compute_device_type}",
    ]


def render_with_density(output_path: Path, density_value: float):
    scene = bpy.context.scene
    fog_node = find_fog_node()
    density_socket = fog_node.inputs["Density"]
    density_socket.default_value = density_value
    scene.render.filepath = str(output_path)

    start = time.perf_counter()
    bpy.ops.render.render(write_still=True)
    render_result = bpy.data.images.get("Render Result")
    if render_result is None:
        raise RuntimeError("Render Result image not available after render.")
    render_result.save_render(filepath=str(output_path), scene=scene)
    return time.perf_counter() - start


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    scene = bpy.context.scene
    fog_node = find_fog_node()
    density_socket = fog_node.inputs["Density"]
    original_density = density_socket.default_value
    original_filepath = scene.render.filepath

    report_lines = [
        "World Environment Fog Performance Compare - Cyberpunk City Overview",
        f"blend: {bpy.data.filepath}",
        f"original fog density: {original_density}",
    ]
    report_lines.extend(ensure_optix())

    try:
        fog_off_seconds = render_with_density(FOG_OFF_PATH, 0.0)
        report_lines.append(f"fog_off_seconds: {fog_off_seconds:.6f}")
        report_lines.append(f"fog_off_output: {FOG_OFF_PATH}")

        fog_on_seconds = render_with_density(FOG_ON_PATH, original_density)
        report_lines.append(f"fog_on_seconds: {fog_on_seconds:.6f}")
        report_lines.append(f"fog_on_output: {FOG_ON_PATH}")

        delta_seconds = fog_on_seconds - fog_off_seconds
        report_lines.append(f"fog_on_vs_off_delta_seconds: {delta_seconds:.6f}")
        if fog_off_seconds > 0.0:
            report_lines.append(
                f"fog_on_vs_off_delta_percent: {(delta_seconds / fog_off_seconds) * 100.0:.3f}"
            )
    finally:
        density_socket.default_value = original_density
        scene.render.filepath = original_filepath

    REPORT_PATH.write_text("\n".join(report_lines) + "\n", encoding="utf-8")

    for line in report_lines:
        print(line)


if __name__ == "__main__":
    main()
