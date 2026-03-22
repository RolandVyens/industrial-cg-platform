#!/usr/bin/env python3
"""Validate deep merge behavior across direct/compositor deep outputs.

Task-1 TDD intent:
- Record seam-pixel deep metrics for 3 outputs.
- Assert expectations that should fail on current direct output behavior.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from dataclasses import asdict, dataclass

import OpenImageIO as oiio


LEGACY_ALPHA_ONLY_PATH = (
    r"D:\blender_projects\rendered\test\ViewLayer\Deep\ViewLayer_Deep_$version$_0002.exr"
)


@dataclass
class PixelRecord:
    label: str
    path: str
    exists: bool
    is_deep: bool
    sample_count: int
    first_r: float
    first_g: float
    first_b: float
    first_a: float
    first_rgb_nonzero: bool
    read_error: str = ""


class DeepReadError(RuntimeError):
    """Raised when an EXR cannot be read reliably via OIIO."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--direct-deep",
        default=r"D:\blender_projects\rendered\test\trash_output\.exr",
    )
    parser.add_argument(
        "--compositor-rgba-deep",
        default=r"D:\blender_projects\rendered\test\ViewLayer\Deep\ViewLayer_Deep_v001_0002.exr",
    )
    parser.add_argument(
        "--compositor-alpha-only-deep",
        default=None,
        help=(
            "Explicit compositor alpha-only deep EXR path. If omitted, script uses "
            "legacy fallback path and enforces freshness checks."
        ),
    )
    parser.add_argument(
        "--legacy-alpha-only-deep",
        default=LEGACY_ALPHA_ONLY_PATH,
        help="Legacy fallback alpha-only deep path used when --compositor-alpha-only-deep is omitted.",
    )
    parser.add_argument("--pixel-x", type=int, default=302)
    parser.add_argument("--pixel-y", type=int, default=150)
    parser.add_argument(
        "--max-direct-samples",
        type=int,
        default=12,
        help="Expected post-merge upper bound for direct deep seam sample count.",
    )
    parser.add_argument(
        "--rgb-epsilon",
        type=float,
        default=1e-8,
        help="Threshold for considering first-sample RGB nonzero.",
    )
    return parser.parse_args()


def _oiio_error_suffix() -> str:
    err = oiio.geterror()
    return f" OIIO error: {err}" if err else ""


def _buf_has_error(image_buf: oiio.ImageBuf) -> bool:
    has_error = getattr(image_buf, "has_error", False)
    return bool(has_error() if callable(has_error) else has_error)


def _buf_get_error(image_buf: oiio.ImageBuf) -> str:
    geterror = getattr(image_buf, "geterror", "")
    if callable(geterror):
        return str(geterror())
    return str(geterror)


def _open_spec(path: str) -> oiio.ImageSpec:
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise DeepReadError(f"OIIO failed to open image: {path}.{_oiio_error_suffix()}")

    try:
        spec = image_input.spec()
        if spec is None:
            raise DeepReadError(f"OIIO returned no ImageSpec: {path}.{_oiio_error_suffix()}")
        return spec
    finally:
        image_input.close()


def read_pixel_record(
    label: str, path: str, x: int, y: int, rgb_epsilon: float
) -> PixelRecord:
    exists = os.path.exists(path)
    if not exists:
        return PixelRecord(label, path, False, False, 0, 0.0, 0.0, 0.0, 0.0, False)

    try:
        spec = _open_spec(path)
        is_deep = bool(spec.deep)
        if not is_deep:
            return PixelRecord(label, path, True, False, 0, 0.0, 0.0, 0.0, 0.0, False)

        if not (0 <= x < int(spec.width) and 0 <= y < int(spec.height)):
            raise DeepReadError(
                f"Seam pixel ({x}, {y}) out of bounds for '{path}' "
                f"with size ({spec.width}, {spec.height})."
            )

        img = oiio.ImageBuf(path)
        if _buf_has_error(img):
            raise DeepReadError(f"OIIO ImageBuf init failed for {path}: {_buf_get_error(img)}")

        sample_count = int(img.deep_samples(x, y, 0))
        if _buf_has_error(img):
            raise DeepReadError(f"OIIO deep_samples failed for {path}: {_buf_get_error(img)}")

        first_r = 0.0
        first_g = 0.0
        first_b = 0.0
        first_a = 0.0
        if sample_count > 0:
            ci_r = spec.channelindex("R")
            ci_g = spec.channelindex("G")
            ci_b = spec.channelindex("B")
            ci_a = spec.channelindex("A")
            if ci_r >= 0:
                first_r = float(img.deep_value(x, y, 0, ci_r, 0))
            if ci_g >= 0:
                first_g = float(img.deep_value(x, y, 0, ci_g, 0))
            if ci_b >= 0:
                first_b = float(img.deep_value(x, y, 0, ci_b, 0))
            if ci_a >= 0:
                first_a = float(img.deep_value(x, y, 0, ci_a, 0))
            if _buf_has_error(img):
                raise DeepReadError(f"OIIO deep_value failed for {path}: {_buf_get_error(img)}")

        first_rgb_nonzero = (
            (math.fabs(first_r) > rgb_epsilon)
            or (math.fabs(first_g) > rgb_epsilon)
            or (math.fabs(first_b) > rgb_epsilon)
        )

        return PixelRecord(
            label=label,
            path=path,
            exists=True,
            is_deep=True,
            sample_count=sample_count,
            first_r=first_r,
            first_g=first_g,
            first_b=first_b,
            first_a=first_a,
            first_rgb_nonzero=first_rgb_nonzero,
        )
    except Exception as ex:
        return PixelRecord(
            label=label,
            path=path,
            exists=True,
            is_deep=False,
            sample_count=0,
            first_r=0.0,
            first_g=0.0,
            first_b=0.0,
            first_a=0.0,
            first_rgb_nonzero=False,
            read_error=str(ex),
        )


def _resolve_alpha_only_path(args: argparse.Namespace) -> tuple[str, bool]:
    if args.compositor_alpha_only_deep:
        return args.compositor_alpha_only_deep, False
    return args.legacy_alpha_only_deep, True


def _mtime(path: str) -> float:
    return os.path.getmtime(path)


def main() -> int:
    args = parse_args()

    alpha_only_path, using_legacy_fallback = _resolve_alpha_only_path(args)

    records = [
        read_pixel_record("direct", args.direct_deep, args.pixel_x, args.pixel_y, args.rgb_epsilon),
        read_pixel_record(
            "compositor_rgba",
            args.compositor_rgba_deep,
            args.pixel_x,
            args.pixel_y,
            args.rgb_epsilon,
        ),
        read_pixel_record(
            "compositor_alpha_only",
            alpha_only_path,
            args.pixel_x,
            args.pixel_y,
            args.rgb_epsilon,
        ),
    ]

    print("deep_merge_matrix_pixel", (args.pixel_x, args.pixel_y))
    print(json.dumps([asdict(record) for record in records], indent=2))

    failures = []
    for record in records:
        if not record.exists:
            failures.append(f"{record.label}: missing file: {record.path}")
            continue
        if record.read_error:
            failures.append(f"{record.label}: unreadable/corrupt deep file: {record.read_error}")
            continue
        if not record.is_deep:
            failures.append(f"{record.label}: file is not deep EXR: {record.path}")
            continue
        if record.sample_count <= 0:
            failures.append(f"{record.label}: no deep samples at seam pixel")

    if using_legacy_fallback:
        direct_exists = os.path.exists(args.direct_deep)
        rgba_exists = os.path.exists(args.compositor_rgba_deep)
        alpha_exists = os.path.exists(alpha_only_path)
        if direct_exists and rgba_exists and alpha_exists:
            newest_current = max(_mtime(args.direct_deep), _mtime(args.compositor_rgba_deep))
            alpha_mtime = _mtime(alpha_only_path)
            if alpha_mtime < newest_current:
                failures.append(
                    "compositor_alpha_only: legacy fallback path appears stale compared to direct/compositor-rgba outputs; "
                    "pass --compositor-alpha-only-deep explicitly."
                )

    direct = records[0]
    comp_rgba = records[1]
    comp_alpha = records[2]

    # Intentionally strict for current Task-1 baseline: expected to fail now.
    if direct.exists and direct.is_deep and direct.sample_count > args.max_direct_samples:
        failures.append(
            f"direct: sample_count={direct.sample_count} exceeds max_direct_samples={args.max_direct_samples}"
        )

    if comp_rgba.exists and comp_rgba.is_deep and not comp_rgba.first_rgb_nonzero:
        failures.append("compositor_rgba: first deep sample RGB is zero; expected nonzero RGB")

    if comp_alpha.exists and comp_alpha.is_deep and comp_alpha.first_rgb_nonzero:
        failures.append("compositor_alpha_only: first deep sample RGB is nonzero; expected alpha-only")

    if failures:
        print("FAIL")
        for failure in failures:
            print(" -", failure)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
