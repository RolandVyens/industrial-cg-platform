#!/usr/bin/env python3
"""Run the unchanged Nuke DeepMerge test against a direct scene-output Deep EXR."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

import nuke


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--nuke-script",
        default=r"E:\blender_modify\deep_merge_test.nk",
        help="Path to the unchanged Nuke test script.",
    )
    parser.add_argument(
        "--deep-input",
        default=r"D:\blender_projects\rendered\test\trash_output\.exr",
        help="Direct scene-output Deep EXR to test.",
    )
    parser.add_argument(
        "--nuke-deep-read",
        default="DeepRead2",
        help="DeepRead node to repoint.",
    )
    parser.add_argument(
        "--nuke-merge",
        default="DeepMerge1",
        help="DeepMerge node whose A input should receive the deep input.",
    )
    parser.add_argument(
        "--write-node",
        default="Write1",
        help="Existing write node in the Nuke script.",
    )
    parser.add_argument(
        "--output-png",
        default=r"C:\tmp\direct_scene_output_saved_write1.png",
        help="PNG preview path.",
    )
    parser.add_argument(
        "--mask-node",
        default="Shuffle1",
        help="Mask/output node from the authored Nuke script.",
    )
    parser.add_argument(
        "--mask-png",
        default=r"C:\tmp\direct_scene_output_saved_mask.png",
        help="Mask PNG path.",
    )
    parser.add_argument(
        "--exact-deep-copy",
        default=r"C:\tmp\direct_scene_output_test_input.exr",
        help="Stable exact filename used to avoid Nuke frame-sequence expansion.",
    )
    return parser.parse_args()


def require_node(name: str):
    node = nuke.toNode(name)
    if node is None:
        raise RuntimeError(f"Nuke node '{name}' not found.")
    return node


def prepare_exact_input(source_path: Path, exact_copy_path: Path) -> Path:
    if not source_path.exists():
        raise FileNotFoundError(f"Deep input not found: {source_path}")

    exact_copy_path.parent.mkdir(parents=True, exist_ok=True)
    if exact_copy_path.resolve() != source_path.resolve():
        shutil.copyfile(source_path, exact_copy_path)
    return exact_copy_path


def main() -> None:
    args = parse_args()

    source_path = Path(args.deep_input)
    exact_copy_path = prepare_exact_input(source_path, Path(args.exact_deep_copy))
    output_path = Path(args.output_png)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    nuke.scriptOpen(args.nuke_script)

    deep_read = require_node(args.nuke_deep_read)
    deep_merge = require_node(args.nuke_merge)
    write = require_node(args.write_node)
    mask_node = require_node(args.mask_node)

    deep_read["file"].setValue(str(exact_copy_path).replace("\\", "/"))
    deep_merge.setInput(0, deep_read)
    write["file"].setValue(str(output_path).replace("\\", "/"))
    mask_output_path = Path(args.mask_png)
    mask_output_path.parent.mkdir(parents=True, exist_ok=True)

    frame = 1
    if "frame" in write.knobs():
        try:
            frame = int(round(float(write["frame"].value())))
        except Exception:
            pass
    elif "first_frame" in nuke.root().knobs():
        frame = int(round(float(nuke.root()["first_frame"].value())))

    cleanup_nodes = []
    try:
        mask_write = nuke.nodes.Write(inputs=[mask_node])
        cleanup_nodes.append(mask_write)
        mask_write["file"].setValue(str(mask_output_path).replace("\\", "/"))
        mask_write["file_type"].setValue("png")
        if "channels" in mask_write.knobs():
            mask_write["channels"].setValue("rgba")

        nuke.execute(write, frame, frame)
        nuke.execute(mask_write, frame, frame)
    finally:
        for node in reversed(cleanup_nodes):
            nuke.delete(node)

    print(f"nuke_script={args.nuke_script}")
    print(f"deep_input_source={source_path}")
    print(f"deep_input_exact_copy={exact_copy_path}")
    print(f"deep_read_node={deep_read.name()}")
    print(f"deep_merge_node={deep_merge.name()}")
    print(f"write_node={write.name()}")
    print(f"mask_node={mask_node.name()}")
    print(f"frame={frame}")
    print(f"output_png={output_path}")
    print(f"mask_png={mask_output_path}")


if __name__ == "__main__":
    main()
