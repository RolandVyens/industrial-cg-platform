#!/usr/bin/env python3
"""Check known opaque hard-surface pixels whose flattened deep alpha must stay fully opaque."""

from __future__ import annotations

import argparse
import sys

import OpenImageIO as oiio


KNOWN_PIXELS = [
    (655, 403),
]


def flatten_deep_alpha(
    deep_data: oiio.DeepData, width: int, x: int, y: int, alpha_channel: int
) -> tuple[float, int]:
    pixel_index = y * width + x
    samples = deep_data.samples(pixel_index)
    transparency = 1.0
    for sample in range(samples):
        alpha = float(deep_data.deep_value(pixel_index, alpha_channel, sample))
        transparency *= 1.0 - alpha
    return 1.0 - transparency, samples


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Check known opaque hard-surface pixels where flattened deep alpha must match the "
            "flat alpha."
        )
    )
    parser.add_argument("flat", help="Path to the flat EXR file")
    parser.add_argument("deep", help="Path to the deep EXR file")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1e-6,
        help="Maximum allowed absolute alpha difference",
    )
    args = parser.parse_args()

    flat = oiio.ImageInput.open(args.flat)
    deep = oiio.ImageInput.open(args.deep)

    flat_spec = flat.spec() if flat else None
    deep_spec = deep.spec() if deep else None
    if flat_spec is None or deep_spec is None:
        raise RuntimeError("Failed to open input EXRs")
    if not deep_spec.deep:
        raise RuntimeError(f"{args.deep} is not a deep image")
    if flat_spec.width != deep_spec.width or flat_spec.height != deep_spec.height:
        raise RuntimeError("Flat/deep dimensions do not match")

    flat_alpha_channel = list(flat_spec.channelnames).index("rgba.A")
    deep_alpha_channel = list(deep_spec.channelnames).index("A")
    flat_pixels = flat.read_image("float")
    deep_data = deep.read_native_deep_image()
    flat.close()
    deep.close()

    mismatching = 0
    for x, y in KNOWN_PIXELS:
        flat_alpha = float(flat_pixels[y, x, flat_alpha_channel])
        deep_alpha, samples = flatten_deep_alpha(
            deep_data, deep_spec.width, x, y, deep_alpha_channel
        )
        diff = abs(deep_alpha - flat_alpha)
        print(
            f"pixel=({x},{y}) flat_alpha={flat_alpha:.9f} "
            f"deep_alpha={deep_alpha:.9f} diff={diff:.9f} "
            f"samples={samples}"
        )
        if diff > args.tolerance:
            mismatching += 1

    print(f"checked_opaque_pixels={len(KNOWN_PIXELS)}")
    print(f"mismatching_opaque_pixels={mismatching}")
    return 1 if mismatching else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
