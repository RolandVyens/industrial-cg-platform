import bpy

print("FILE_FORMAT", bpy.context.scene.render.image_settings.file_format)
print("DEEP_MERGE_TOLERANCE", bpy.context.scene.render.image_settings.deep_merge_tolerance)
print("DEEP_ALPHA_MERGE_TOLERANCE",
      bpy.context.scene.render.image_settings.deep_alpha_merge_tolerance)

for scene in bpy.data.scenes:
    print("SCENE", scene.name)
    if hasattr(scene, "use_nodes"):
        print("SCENE_USE_NODES", scene.use_nodes)
    comp_group = getattr(scene, "compositing_node_group", None)
    print("SCENE_COMP_GROUP", getattr(comp_group, "name", None))
    nt = getattr(scene, "node_tree", None)
    if nt is not None:
        print("SCENE_NODE_TREE", nt.name)
        for node in nt.nodes:
            if node.type == "OUTPUT_FILE":
                fmt = node.format
                print(
                    "FILE_OUTPUT",
                    node.name,
                    "mute",
                    node.mute,
                    "format",
                    fmt.file_format,
                    "deep_merge_tolerance",
                    fmt.deep_merge_tolerance,
                    "deep_alpha_merge_tolerance",
                    fmt.deep_alpha_merge_tolerance,
                )
    else:
        print("SCENE_NODE_TREE", "NONE")

for nt in bpy.data.node_groups:
    if nt.bl_idname == "CompositorNodeTree":
        print("NODE_GROUP", nt.name)
        for node in nt.nodes:
            if node.type == "OUTPUT_FILE":
                fmt = node.format
                print(
                    "FILE_OUTPUT",
                    node.name,
                    "linked_inputs",
                    sum(1 for sock in node.inputs if sock.is_linked),
                    "format",
                    fmt.file_format,
                    "deep_merge_tolerance",
                    fmt.deep_merge_tolerance,
                    "deep_alpha_merge_tolerance",
                    fmt.deep_alpha_merge_tolerance,
                )
                base_path = getattr(node, "base_path", None)
                if base_path is None:
                    base_path = getattr(node, "directory", None)
                print("FILE_OUTPUT_BASE_PATH", base_path)
                print("FILE_OUTPUT_FILE_NAME", getattr(node, "file_name", None))
                print("FILE_OUTPUT_RNA_PROPS", [p.identifier for p in node.bl_rna.properties])
                items = getattr(node, "file_output_items", None)
                if items is not None:
                    print("FILE_OUTPUT_ITEMS_COUNT", len(items))
                    for item in items:
                        item_path = getattr(item, "path", None)
                        item_name = getattr(item, "name", None)
                        print("FILE_OUTPUT_ITEM", item_name, item_path)
