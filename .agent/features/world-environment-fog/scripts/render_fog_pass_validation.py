import os

import bpy


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FEATURE_DIR = os.path.dirname(SCRIPT_DIR)
OUTPUT_DIR = os.path.join(FEATURE_DIR, "validation", "fog_pass_2026-04-09")

LIGHT_TYPES_ENV = "FOG_ENABLED_LIGHT_TYPES"
PASS_TAG_ENV = "FOG_PASS_TAG"
DEFAULT_PASS_TAG = "all_lights"


def active_pass_tag():
    tag = os.environ.get(PASS_TAG_ENV, "").strip()
    if tag:
        return tag

    light_types = parse_enabled_light_types()
    if light_types:
        return "_".join(sorted(t.lower() for t in light_types))

    return DEFAULT_PASS_TAG


def exr_stem():
    return f"foggy_street_cyberpunk_test_optix_fog_pass_{active_pass_tag()}_exr_"


def beauty_stem():
    return f"foggy_street_cyberpunk_test_optix_beauty_validation_{active_pass_tag()}_"


def default_fog_exr():
    return os.path.join(
        OUTPUT_DIR, f"foggy_street_cyberpunk_test_optix_fog_pass_{active_pass_tag()}_0001.exr"
    )


def unique_path(path):
    if not os.path.exists(path):
        return path

    stem, ext = os.path.splitext(path)
    index = 2
    while True:
        candidate = f"{stem}_rerun{index}{ext}"
        if not os.path.exists(candidate):
            return candidate
        index += 1


def parse_enabled_light_types():
    raw = os.environ.get(LIGHT_TYPES_ENV, "").strip()
    if not raw:
        return set()

    return {part.strip().upper() for part in raw.split(",") if part.strip()}


def snapshot_light_state():
    state = {}
    for obj in bpy.data.objects:
        if obj.type == "LIGHT":
            state[obj.name] = obj.hide_render
    return state


def apply_light_filter(enabled_types):
    if not enabled_types:
        return

    for obj in bpy.data.objects:
        if obj.type != "LIGHT":
            continue
        obj.hide_render = obj.data.type not in enabled_types


def restore_light_state(state):
    for obj in bpy.data.objects:
        if obj.type == "LIGHT" and obj.name in state:
            obj.hide_render = state[obj.name]


def add_output_node(tree, fog_socket, file_stem):
    node = tree.nodes.new("CompositorNodeOutputFile")
    node.directory = OUTPUT_DIR
    node.file_name = file_stem
    node.use_file_extension = True
    node.format.file_format = "OPEN_EXR_MULTILAYER"
    node.save_as_render = False
    node.location = (360.0, 0.0)

    output_item = node.file_output_items.new("RGBA", name="Fog")
    output_item.override_node_format = True
    output_item.save_as_render = False
    output_item.format.file_format = "OPEN_EXR"
    output_item.format.color_mode = "RGBA"
    output_item.format.color_depth = "16"
    output_item.format.exr_codec = "ZIP"

    tree.links.new(fog_socket, node.inputs[output_item.name])
    return node


def build_compositor(scene):
    tree = bpy.data.node_groups.new("EnvironmentFogPassValidation", "CompositorNodeTree")

    render_layers = tree.nodes.new("CompositorNodeRLayers")
    render_layers.layer = scene.view_layers[0].name
    render_layers.location = (0.0, 0.0)

    if "Fog" not in render_layers.outputs:
        raise RuntimeError("Render Layers node does not expose the Fog output socket.")

    fog_socket = render_layers.outputs["Fog"]
    add_output_node(tree, fog_socket, exr_stem())

    return tree


def pick_output_path(default_path):
    return unique_path(default_path)


def move_first_matching_output(directory, stem, extension, destination_path):
    candidates = []
    for name in os.listdir(directory):
        lower_name = name.lower()
        if not lower_name.endswith(extension):
            continue
        if not name.startswith(stem):
            continue
        candidates.append(os.path.join(directory, name))

    if not candidates:
        raise RuntimeError(f"No compositor output found for stem '{stem}' and extension '{extension}'.")

    candidates.sort(key=os.path.getmtime)
    source_path = candidates[-1]

    if os.path.abspath(source_path) != os.path.abspath(destination_path):
        os.rename(source_path, destination_path)

    return destination_path


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    scene = bpy.context.scene
    view_layer = scene.view_layers[0]

    if not hasattr(view_layer.cycles, "use_pass_fog"):
        raise RuntimeError("Cycles fog pass property is unavailable in this build.")

    fog_exr = pick_output_path(default_fog_exr())
    enabled_light_types = parse_enabled_light_types()

    previous_group = scene.compositing_node_group if hasattr(scene, "compositing_node_group") else None
    previous_use_nodes = scene.use_nodes
    previous_use_compositing = scene.render.use_compositing
    previous_filepath = scene.render.filepath
    previous_format = scene.render.image_settings.file_format
    previous_color_mode = scene.render.image_settings.color_mode
    light_state = snapshot_light_state()

    scene.use_nodes = True
    scene.render.use_compositing = True
    view_layer.cycles.use_pass_fog = True
    apply_light_filter(enabled_light_types)

    compositor_tree = build_compositor(scene)
    scene.compositing_node_group = compositor_tree

    scene.render.use_file_extension = True
    scene.render.filepath = os.path.join(OUTPUT_DIR, beauty_stem())
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"

    print(f"Fog pass enabled on view layer: {view_layer.name}")
    if enabled_light_types:
        print(f"Fog pass light filter: {sorted(enabled_light_types)}")
    print(f"Fog EXR target: {fog_exr}")

    try:
        bpy.ops.render.render(write_still=True)
        final_exr = move_first_matching_output(OUTPUT_DIR, exr_stem(), ".exr", fog_exr)
        print(f"Fog EXR saved to: {final_exr}")
        print("Fog compositor validation render finished.")
    finally:
        scene.compositing_node_group = previous_group
        scene.use_nodes = previous_use_nodes
        scene.render.use_compositing = previous_use_compositing
        scene.render.filepath = previous_filepath
        scene.render.image_settings.file_format = previous_format
        scene.render.image_settings.color_mode = previous_color_mode
        restore_light_state(light_state)
        bpy.data.node_groups.remove(compositor_tree)


if __name__ == "__main__":
    main()
