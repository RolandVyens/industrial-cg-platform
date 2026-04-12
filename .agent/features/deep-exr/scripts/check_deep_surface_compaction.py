#!/usr/bin/env python3
"""Check for over-fragmented hard-surface deep pixels.

This targets the current Deep EXR issue where fractional hard-surface AA pixels
export one thin surface sample per contributing camera hit instead of a compact
front-to-back grouped representation.
"""

from __future__ import annotations

import argparse
import sys

import OpenImageIO as oiio


SURFACE_EPSILON = 1e-5


def get_channel_index(spec: oiio.ImageSpec, name: str) -> int:
    index = spec.channelindex(name)
    if index < 0:
        raise RuntimeError(f"Channel '{name}' not found in {list(spec.channelnames)}")
    return index


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("deep_exr")
    parser.add_argument("flat_exr")
    parser.add_argument(
        "--max-surface-samples",
        type=int,
        default=2,
        help="Maximum acceptable number of thin surface samples on a fractional-alpha pixel.",
    )
    parser.add_argument(
        "--max-examples",
        type=int,
        default=20,
        help="Maximum example pixels to print.",
    )
    args = parser.parse_args()

    deep = oiio.ImageBuf(args.deep_exr)
    flat = oiio.ImageBuf(args.flat_exr)

    deep_spec = deep.spec()
    flat_spec = flat.spec()
    if not deep_spec.deep:
      raise RuntimeError(f"{args.deep_exr} is not a deep file.")

    if (deep_spec.width, deep_spec.height) != (flat_spec.width, flat_spec.height):
        raise RuntimeError("Deep/flat resolution mismatch.")

    deep_data = deep.deepdata()
    if deep_data is None:
        raise RuntimeError("Could not read deep sample data.")

    flat_alpha_channel = get_channel_index(flat_spec, "rgba.A")
    width = deep_spec.width
    height = deep_spec.height

    checked_fractional_pixels = 0
    all_surface_fractional_pixels = 0
    overfragmented_pixels = 0
    examples = []

    for y in range(height):
        for x in range(width):
            flat_pixel = flat.getpixel(x, y)
            flat_alpha = flat_pixel[flat_alpha_channel]
            if not (0.0 < flat_alpha < 1.0):
                continue

            checked_fractional_pixels += 1
            pixel_index = y * width + x
            sample_count = deep_data.samples(pixel_index)
            if sample_count <= 0:
                continue

            thin_surface_samples = []
            has_volume = False
            for sample_index in range(sample_count):
                z = deep_data.deep_value(pixel_index, deep_data.Z_channel, sample_index)
                z_back = deep_data.deep_value(pixel_index, deep_data.Zback_channel, sample_index)
                alpha = deep_data.deep_value(pixel_index, deep_data.A_channel, sample_index)
                if abs(z_back - z) <= SURFACE_EPSILON:
                    thin_surface_samples.append((sample_index, z, z_back, alpha))
                else:
                    has_volume = True
                    break

            if has_volume or not thin_surface_samples:
                continue

            all_surface_fractional_pixels += 1
            if len(thin_surface_samples) > args.max_surface_samples:
                overfragmented_pixels += 1
                if len(examples) < args.max_examples:
                    examples.append(
                        {
                            "pixel": (x, y),
                            "flat_alpha": flat_alpha,
                            "surface_samples": [
                                (index, round(z, 6), round(z_back, 6), round(alpha, 6))
                                for index, z, z_back, alpha in thin_surface_samples
                            ],
                        }
                    )

    print(f"checked_fractional_pixels={checked_fractional_pixels}")
    print(f"all_surface_fractional_pixels={all_surface_fractional_pixels}")
    print(f"overfragmented_pixels={overfragmented_pixels}")
    if examples:
        print("examples:")
        for example in examples:
            print(example)

    return 0 if overfragmented_pixels == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
