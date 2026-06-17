# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--blender", required=True)
    parser.add_argument("--test-script", required=True)
    parser.add_argument("--outdir", required=True)
    return parser.parse_args(argv)


def resolve_blender_command(blender_exe: Path) -> Path:
    launcher = blender_exe.with_name("blender-launcher.exe")
    if os.name == "nt" and launcher.exists():
        return launcher
    return blender_exe


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    blender_exe = Path(args.blender)
    test_script = Path(args.test_script)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    command = [
        str(resolve_blender_command(blender_exe)),
        "--background",
        "--factory-startup",
        "--python",
        str(test_script),
        "--",
        "--outdir",
        str(outdir),
    ]
    env = os.environ.copy()
    env["BLENDER_USER_RESOURCES"] = str(outdir / "isolated_user_resources")
    workspace_root = blender_exe.resolve().parents[2]

    completed = subprocess.run(command, cwd=workspace_root, env=env, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
