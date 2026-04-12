import argparse
import sys

import OpenImageIO as oiio


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Check that single-sample opaque deep surface pixels match the fractional flat alpha "
            "at antialiased edges."
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
    parser.add_argument(
        "--max-report",
        type=int,
        default=10,
        help="Maximum number of mismatching pixels to print",
    )
    args = parser.parse_args()

    flat = oiio.ImageBuf(args.flat)
    deep = oiio.ImageBuf(args.deep)

    flat_spec = flat.spec()
    deep_spec = deep.spec()

    if deep_spec is None or not deep_spec.deep:
        raise RuntimeError(f"{args.deep} is not a deep image")

    if flat_spec.width != deep_spec.width or flat_spec.height != deep_spec.height:
        raise RuntimeError("Flat/deep dimensions do not match")

    flat_alpha_channel = list(flat_spec.channelnames).index("rgba.A")
    deep_alpha_channel = list(deep_spec.channelnames).index("A")
    flat_pixels = flat.get_pixels(oiio.FLOAT, oiio.ROI())

    checked = 0
    mismatching = 0
    examples = []

    for y in range(flat_spec.height):
      for x in range(flat_spec.width):
        flat_alpha = float(flat_pixels[y, x, flat_alpha_channel])
        if flat_alpha <= args.tolerance or flat_alpha >= 1.0 - args.tolerance:
          continue

        samples = deep.deep_samples(x, y)
        if samples != 1:
          continue

        deep_alpha = float(deep.deep_value(x, y, 0, deep_alpha_channel, 0))
        checked += 1
        if abs(deep_alpha - flat_alpha) > args.tolerance:
          mismatching += 1
          if len(examples) < args.max_report:
            examples.append((x, y, flat_alpha, deep_alpha))

    print(f"checked_single_surface_fractional_pixels={checked}")
    print(f"mismatching_single_surface_pixels={mismatching}")
    for x, y, flat_alpha, deep_alpha in examples:
      print(
          f"  pixel=({x},{y}) flat_alpha={flat_alpha:.6f} deep_alpha={deep_alpha:.6f}"
      )

    return 1 if mismatching > 0 else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
