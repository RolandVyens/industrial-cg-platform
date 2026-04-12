from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
VALIDATION_DIR = ROOT / "validation"
OUTPUT_DIR = VALIDATION_DIR / "light_type_scatter_cyberpunk"

LIGHT_VARIANTS = (
    ("point", {"POINT"}),
    ("spot", {"SPOT"}),
    ("area", {"AREA"}),
    ("sun", {"SUN"}),
)


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def find_fog_node() -> bpy.types.Node:
    world = bpy.context.scene.world
    if world is None or not world.use_nodes or world.node_tree is None:
        raise RuntimeError("Active scene world does not use nodes.")

    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeEnvironmentFog":
            return node

    raise RuntimeError("Environment Fog node not found in active world.")


def snapshot_light_state() -> dict[str, tuple[float, bool]]:
    state = {}
    for obj in bpy.data.objects:
        if obj.type == "LIGHT":
            state[obj.name] = (obj.data.energy, obj.hide_render)
    return state


def apply_light_variant(enabled_types: set[str]) -> None:
    for obj in bpy.data.objects:
        if obj.type != "LIGHT":
            continue
        obj.hide_render = obj.data.type not in enabled_types


def restore_light_state(state: dict[str, tuple[float, bool]]) -> None:
    for obj in bpy.data.objects:
        if obj.type != "LIGHT":
            continue
        energy, hide_render = state[obj.name]
        obj.data.energy = energy
        obj.hide_render = hide_render


def render_variant(tag: str, enabled_types: set[str], fog_density: float) -> None:
    scene = bpy.context.scene
    fog = find_fog_node()
    apply_light_variant(enabled_types)
    fog.inputs["Density"].default_value = fog_density
    scene.render.filepath = str(OUTPUT_DIR / f"{tag}_fog_{'on' if fog_density > 0.0 else 'off'}")
    bpy.ops.render.render(write_still=True)


def main() -> None:
    ensure_dir(OUTPUT_DIR)
    scene = bpy.context.scene
    scene.render.image_settings.file_format = "PNG"
    scene.cycles.device = "GPU"

    fog = find_fog_node()
    original_density = fog.inputs["Density"].default_value
    light_state = snapshot_light_state()

    try:
        for tag, enabled_types in LIGHT_VARIANTS:
            render_variant(tag, enabled_types, original_density)
            render_variant(tag, enabled_types, 0.0)
    finally:
        fog.inputs["Density"].default_value = original_density
        restore_light_state(light_state)


if __name__ == "__main__":
    main()
