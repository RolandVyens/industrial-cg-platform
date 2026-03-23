#!/usr/bin/env python3
"""Check that front hard-surface deep samples carry object color, not flat edge color."""

from __future__ import annotations

import argparse
import math
import sys

import OpenImageIO as oiio


SURFACE_EPSILON = 1e-5


def rgb_distance(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return math.sqrt(sum((x - y) * (x - y) for x, y in zip(a, b)))


def get_rgb(spec: oiio.ImageSpec, pixel: tuple[float, ...], prefix: str) -> tuple[float, float, float]:
    return tuple(float(pixel[spec.channelindex(f"{prefix}.{channel}")]) for channel in "RGB")


def get_surface_samples(
    deep_data: oiio.DeepData, deep_spec: oiio.ImageSpec, x: int, y: int
) -> list[dict[str, object]]:
    pixel_index = y * deep_spec.width + x
    samples = []
    for sample_index in range(deep_data.samples(pixel_index)):
        alpha = float(deep_data.deep_value(pixel_index, deep_data.A_channel, sample_index))
        z = float(deep_data.deep_value(pixel_index, deep_data.Z_channel, sample_index))
        z_back = float(deep_data.deep_value(pixel_index, deep_data.Zback_channel, sample_index))
        if abs(z_back - z) > SURFACE_EPSILON or alpha <= 0.0:
            continue
        rgb = tuple(
            float(deep_data.deep_value(pixel_index, deep_spec.channelindex(channel), sample_index))
            for channel in "RGB"
        )
        unpremult = tuple(channel / alpha for channel in rgb)
        samples.append(
            {
                "sample_index": sample_index,
                "alpha": alpha,
                "z": z,
                "rgb": rgb,
                "unpremult": unpremult,
            }
        )
    samples.sort(key=lambda sample: float(sample["z"]))
    return samples


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("flat_exr")
    parser.add_argument("deep_exr")
    parser.add_argument("--edge-x", type=int, default=655)
    parser.add_argument("--edge-y", type=int, default=403)
    parser.add_argument("--interior-x", type=int, default=654)
    parser.add_argument("--interior-y", type=int, default=403)
    parser.add_argument(
        "--max-ratio",
        type=float,
        default=0.8,
        help=(
            "Require the front deep sample color distance to the interior surface to be at most "
            "this ratio of its distance to the flat edge color."
        ),
    )
    args = parser.parse_args()

    flat = oiio.ImageBuf(args.flat_exr)
    deep = oiio.ImageBuf(args.deep_exr)
    flat_spec = flat.spec()
    deep_spec = deep.spec()
    deep_data = deep.deepdata()

    if deep_data is None or not deep_spec.deep:
        raise RuntimeError(f"{args.deep_exr} is not a readable deep image")

    edge_pixel = flat.getpixel(args.edge_x, args.edge_y)
    interior_pixel = flat.getpixel(args.interior_x, args.interior_y)
    edge_rgb = get_rgb(flat_spec, edge_pixel, "rgba")
    interior_rgb = get_rgb(flat_spec, interior_pixel, "rgba")

    surface_samples = get_surface_samples(deep_data, deep_spec, args.edge_x, args.edge_y)
    if len(surface_samples) < 2:
        raise RuntimeError(
            f"Expected at least 2 surface samples at edge pixel {(args.edge_x, args.edge_y)}, "
            f"got {len(surface_samples)}"
        )

    front = surface_samples[0]
    front_rgb = tuple(float(channel) for channel in front["unpremult"])
    dist_to_edge = rgb_distance(front_rgb, edge_rgb)
    dist_to_interior = rgb_distance(front_rgb, interior_rgb)
    ratio = dist_to_interior / max(dist_to_edge, 1e-12)

    print(f"edge_pixel=({args.edge_x},{args.edge_y}) flat_edge_rgb={edge_rgb}")
    print(f"interior_pixel=({args.interior_x},{args.interior_y}) flat_interior_rgb={interior_rgb}")
    print(
        "front_surface_sample="
        f"index={front['sample_index']} alpha={front['alpha']:.9f} z={front['z']:.9f} "
        f"unpremult_rgb={front_rgb}"
    )
    print(f"distance_to_edge={dist_to_edge:.9f}")
    print(f"distance_to_interior={dist_to_interior:.9f}")
    print(f"interior_to_edge_ratio={ratio:.9f}")

    ok = dist_to_interior <= (dist_to_edge * args.max_ratio)
    print(f"surface_color_matches_interior={int(ok)}")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
