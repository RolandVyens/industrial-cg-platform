import bpy
import os

out_path = r"E:\blender_modify\blender\.agent\volume_node_dump.txt"
with open(out_path, "w", encoding="utf-8") as f:
    for m in bpy.data.materials:
        if not m.use_nodes:
            continue
        for n in m.node_tree.nodes:
            if n.type != 'PRINCIPLED_VOLUME':
                continue
            f.write(f"Material: {m.name}\n")
            for inp in n.inputs:
                if inp.is_linked:
                    link = inp.links[0]
                    f.write(f"  {inp.name}: linked from {link.from_node.name} ({link.from_node.type})\n")
                else:
                    value = getattr(inp, "default_value", None)
                    if hasattr(value, "__len__") and not isinstance(value, str):
                        value = tuple(value)
                    f.write(f"  {inp.name}: {value}\n")
            f.write("\n")
print(out_path)
