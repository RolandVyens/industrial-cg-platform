import argparse
import math
import sys

import OpenImageIO as oiio


def find_alpha_channel(channelnames):
    if "A" in channelnames:
        return channelnames.index("A")

    for index, name in enumerate(channelnames):
        if name.endswith(".A"):
            return index

    raise RuntimeError("Image does not contain an alpha channel")


def is_inactive_sample(alpha, z, z_back, epsilon):
    return abs(alpha) <= epsilon and abs(z) <= epsilon and abs(z_back) <= epsilon


def is_surface_sample(z, z_back, depth_epsilon):
    return z_back <= z + depth_epsilon


def active_samples(image, x, y, channels, inactive_epsilon):
    samples = []
    for sample_index in range(image.deep_samples(x, y)):
        alpha = float(image.deep_value(x, y, sample_index, channels["A"], 0))
        z = float(image.deep_value(x, y, sample_index, channels["Z"], 0))
        z_back = float(image.deep_value(x, y, sample_index, channels["ZBack"], 0))
        if is_inactive_sample(alpha, z, z_back, inactive_epsilon):
            continue
        samples.append((alpha, z, z_back))
    return samples


def group_surface_samples(samples, depth_epsilon):
    surface_groups = []
    for alpha, z, z_back in samples:
        if not is_surface_sample(z, z_back, depth_epsilon):
            continue
        if not surface_groups or abs(z - surface_groups[-1][0][1]) > depth_epsilon:
            surface_groups.append([])
        surface_groups[-1].append((alpha, z, z_back))
    return surface_groups


def flattened_alpha(samples):
    transparency = 1.0
    for alpha, _z, _z_back in samples:
        transparency *= 1.0 - max(0.0, min(1.0, alpha))
    return 1.0 - transparency


def load_flat_alpha(path):
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise RuntimeError(f"Failed to open flat EXR: {path}")
    try:
        spec = image_input.spec()
        pixels = image_input.read_image("float")
        alpha_channel = find_alpha_channel(spec.channelnames)
        return spec.width, spec.height, pixels[..., alpha_channel]
    finally:
        image_input.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check front hard-surface deep alpha using active samples only, and optionally "
        "compare flattened deep alpha against a flat EXR."
    )
    parser.add_argument("deep_path", help="Path to a deep EXR file")
    parser.add_argument(
        "flat_path",
        nargs="?",
        default=None,
        help="Optional path to a flat EXR for flattened-alpha comparison",
    )
    parser.add_argument(
        "--alpha-threshold",
        type=float,
        default=0.999,
        help="Treat front surface alpha values at or above this threshold as invalid",
    )
    parser.add_argument(
        "--flat-tolerance",
        type=float,
        default=1e-4,
        help="Maximum allowed absolute difference between flattened deep alpha and flat alpha",
    )
    parser.add_argument(
        "--surface-depth-epsilon",
        type=float,
        default=1e-6,
        help="Depth epsilon used to group hard-surface samples",
    )
    parser.add_argument(
        "--inactive-epsilon",
        type=float,
        default=1e-8,
        help="Value epsilon used to ignore inactive zero deep samples",
    )
    parser.add_argument(
        "--max-report",
        type=int,
        default=10,
        help="Maximum number of example pixels to print per category",
    )
    args = parser.parse_args()

    image = oiio.ImageBuf(args.deep_path)
    spec = image.spec()
    if spec is None:
        raise RuntimeError(f"Failed to open {args.deep_path}")
    if not spec.deep:
        raise RuntimeError(f"{args.deep_path} is not a deep image")

    required_channels = ("A", "Z", "ZBack")
    for name in required_channels:
        if name not in spec.channelnames:
            raise RuntimeError(f"{args.deep_path} does not contain required channel {name}")
    channels = {name: spec.channelnames.index(name) for name in required_channels}

    flat_alpha = None
    if args.flat_path:
        flat_width, flat_height, flat_alpha = load_flat_alpha(args.flat_path)
        if flat_width != spec.width or flat_height != spec.height:
            raise RuntimeError("Flat/deep dimensions do not match")

    active_sample_pixels = 0
    multi_active_sample_pixels = 0
    multi_surface_pixels = 0
    violating_front_surface_alpha_pixels = 0
    front_examples = []

    flat_alpha_checked_pixels = 0
    flat_alpha_mismatching_pixels = 0
    flat_examples = []

    for y in range(spec.height):
        for x in range(spec.width):
            samples = active_samples(image, x, y, channels, args.inactive_epsilon)
            if not samples:
                continue

            active_sample_pixels += 1
            if len(samples) > 1:
                multi_active_sample_pixels += 1

            if not is_surface_sample(samples[0][1], samples[0][2], args.surface_depth_epsilon):
                continue

            surface_groups = group_surface_samples(samples, args.surface_depth_epsilon)
            if len(surface_groups) < 2:
                continue

            multi_surface_pixels += 1
            front_alpha = surface_groups[0][0][0]
            if front_alpha >= args.alpha_threshold:
                violating_front_surface_alpha_pixels += 1
                if len(front_examples) < args.max_report:
                    front_examples.append((x, y, len(samples), len(surface_groups), front_alpha))

            if flat_alpha is not None:
                flat_alpha_checked_pixels += 1
                deep_alpha = flattened_alpha(samples)
                flat_pixel_alpha = float(flat_alpha[y, x])
                diff = abs(deep_alpha - flat_pixel_alpha)
                if diff > args.flat_tolerance:
                    flat_alpha_mismatching_pixels += 1
                    if len(flat_examples) < args.max_report:
                        flat_examples.append((x, y, flat_pixel_alpha, deep_alpha, diff, len(samples)))

    print(f"active_sample_pixels={active_sample_pixels}")
    print(f"multi_active_sample_pixels={multi_active_sample_pixels}")
    print(f"multi_surface_pixels={multi_surface_pixels}")
    print(f"violating_front_surface_alpha_pixels={violating_front_surface_alpha_pixels}")
    for x, y, sample_count, surface_groups, alpha in front_examples:
        print(
            f"  front_violation pixel=({x},{y}) active_samples={sample_count} "
            f"surface_groups={surface_groups} front_alpha={alpha:.6f}"
        )

    if flat_alpha is not None:
        print(f"flat_alpha_checked_pixels={flat_alpha_checked_pixels}")
        print(f"flat_alpha_mismatching_pixels={flat_alpha_mismatching_pixels}")
        for x, y, flat_pixel_alpha, deep_alpha, diff, sample_count in flat_examples:
            print(
                f"  flat_mismatch pixel=({x},{y}) flat_alpha={flat_pixel_alpha:.9f} "
                f"deep_flatten_alpha={deep_alpha:.9f} diff={diff:.9f} active_samples={sample_count}"
            )

    return 1 if violating_front_surface_alpha_pixels > 0 or flat_alpha_mismatching_pixels > 0 else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
