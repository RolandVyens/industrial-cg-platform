import argparse
import sys

import OpenImageIO as oiio


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that front deep surface samples are not fully opaque when deeper "
        "surface samples exist in the same pixel."
    )
    parser.add_argument("path", help="Path to a deep EXR file")
    parser.add_argument(
        "--alpha-threshold",
        type=float,
        default=0.999,
        help="Treat front alpha values at or above this threshold as invalid",
    )
    parser.add_argument(
        "--max-report",
        type=int,
        default=10,
        help="Maximum number of violating pixels to print",
    )
    args = parser.parse_args()

    image = oiio.ImageBuf(args.path)
    spec = image.spec()
    if spec is None:
        raise RuntimeError(f"Failed to open {args.path}")
    if not spec.deep:
        raise RuntimeError(f"{args.path} is not a deep image")

    if "A" not in spec.channelnames:
        raise RuntimeError(f"{args.path} does not contain an alpha channel")

    alpha_channel = spec.channelnames.index("A")

    violating = 0
    examples = []
    multi_sample_pixels = 0

    for y in range(spec.height):
        for x in range(spec.width):
            samples = image.deep_samples(x, y)
            if samples < 2:
                continue

            multi_sample_pixels += 1
            front_alpha = float(image.deep_value(x, y, 0, alpha_channel, 0))
            if front_alpha >= args.alpha_threshold:
                violating += 1
                if len(examples) < args.max_report:
                    examples.append((x, y, samples, front_alpha))

    print(f"multi_sample_pixels={multi_sample_pixels}")
    print(f"violating_front_alpha_pixels={violating}")
    for x, y, samples, alpha in examples:
        print(f"  pixel=({x},{y}) samples={samples} front_alpha={alpha:.6f}")

    return 1 if violating > 0 else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
