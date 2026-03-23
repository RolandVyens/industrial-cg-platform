"""Render a runtime-only compositor RGBA Deep EXR from the current scene.

This rewires the existing compositor deep output node to `ViewLayer.Image`,
writes to a temp output directory, and renders frame 2 without modifying the
saved blend file.
"""

import bpy
from pathlib import Path


FRAME = 2
OUTPUT_DIRECTORY = Path(r"D:\blender_projects\rendered\test\TempDeepRGBA")
OUTPUT_NODE_NAME = "ViewLayer--Deep"
RENDER_LAYER_NODE_NAME = "ViewLayer"


def main() -> None:
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.frame_set(FRAME)

    node_tree = scene.compositing_node_group or scene.node_tree
    if node_tree is None:
        raise RuntimeError("Compositor node tree is not available.")

    render_layer = node_tree.nodes.get(RENDER_LAYER_NODE_NAME)
    if render_layer is None or render_layer.bl_idname != "CompositorNodeRLayers":
        raise RuntimeError("Render Layers node was not found.")

    deep_node = node_tree.nodes.get(OUTPUT_NODE_NAME)
    if deep_node is None or deep_node.bl_idname != "CompositorNodeOutputFile":
        raise RuntimeError("Deep output file node was not found.")

    deep_node.directory = str(OUTPUT_DIRECTORY)
    deep_node.format.file_format = "DEEP_EXR"
    deep_node.format.color_mode = "RGBA"
    deep_node.format.color_depth = "16"
    deep_node.format.deep_merge_tolerance = 0.1
    deep_node.format.deep_alpha_merge_tolerance = 0.1

    input_socket = None
    for socket in deep_node.inputs:
        if socket.identifier.startswith("__extend__"):
            continue
        input_socket = socket
        while socket.links:
            node_tree.links.remove(socket.links[0])
        break

    if input_socket is None:
        raise RuntimeError("Deep output input socket was not found.")

    image_socket = render_layer.outputs.get("Image")
    if image_socket is None:
        raise RuntimeError("Render Layers Image socket was not found.")

    node_tree.links.new(image_socket, input_socket)
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)

    print("Configured compositor RGBA deep output:")
    print("  directory:", OUTPUT_DIRECTORY)
    print("  links:", [(link.from_node.name, link.from_socket.name) for link in input_socket.links])

    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
