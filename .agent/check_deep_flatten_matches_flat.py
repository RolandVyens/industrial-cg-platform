#!/usr/bin/env python3
"""Check that flattened deep RGBA stays reasonably close to the flat render."""

from __future__ import annotations

import argparse
import sys

import numpy as np
import OpenImageIO as oiio


def read_flat_rgba(path: str) -> np.ndarray:
    buf = oiio.ImageBuf(path)
    spec = buf.spec()
    pixels = np.asarray(buf.get_pixels(oiio.FLOAT), dtype=np.float32)
    rgba = np.stack(
        [
            pixels[:, :, spec.channelindex("rgba.R")],
            pixels[:, :, spec.channelindex("rgba.G")],
            pixels[:, :, spec.channelindex("rgba.B")],
            pixels[:, :, spec.channelindex("rgba.A")],
        ],
        axis=2,
    )
    return rgba


def flatten_deep_rgba(path: str) -> np.ndarray:
    inp = oiio.ImageInput.open(path)
    if not inp:
        raise RuntimeError(f"Failed to open deep image: {path}")

    spec = inp.spec()
    if not spec.deep:
        raise RuntimeError(f"Image is not deep: {path}")

    deep = inp.read_native_deep_image()
    inp.close()

    channel_index = {name: i for i, name in enumerate(spec.channelnames)}
    out = np.zeros((spec.height, spec.width, 4), dtype=np.float32)

    for y in range(spec.height):
        for x in range(spec.width):
            pixel_index = y * spec.width + x
            transparency = 1.0
            rgb = np.zeros(3, dtype=np.float32)

            for sample in range(deep.samples(pixel_index)):
                alpha = float(deep.deep_value(pixel_index, channel_index["A"], sample))
                if alpha <= 0.0:
                    continue

                rgb[0] += transparency * float(deep.deep_value(pixel_index, channel_index["R"], sample))
                rgb[1] += transparency * float(deep.deep_value(pixel_index, channel_index["G"], sample))
                rgb[2] += transparency * float(deep.deep_value(pixel_index, channel_index["B"], sample))
                transparency *= 1.0 - alpha

            out[y, x, :3] = rgb
            out[y, x, 3] = 1.0 - transparency

    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("flat_exr")
    parser.add_argument("deep_exr")
    parser.add_argument("--max-mean-abs-rgb", type=float, default=0.05)
    parser.add_argument("--max-pixels-gt-005", type=int, default=150000)
    args = parser.parse_args()

    flat = read_flat_rgba(args.flat_exr)
    deep = flatten_deep_rgba(args.deep_exr)

    diff = np.abs(deep - flat)
    mean_abs_rgb = diff[:, :, :3].mean(axis=(0, 1))
    pixels_gt_005 = int(np.count_nonzero(np.any(diff[:, :, :3] > 0.05, axis=2)))

    print(f"mean_abs_rgb={tuple(float(x) for x in mean_abs_rgb)}")
    print(f"pixels_gt_0.05={pixels_gt_005}")

    ok = bool(np.all(mean_abs_rgb <= args.max_mean_abs_rgb))
    ok = ok and pixels_gt_005 <= args.max_pixels_gt_005
    print(f"flatten_matches_flat={int(ok)}")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
