import argparse
import math
import sys

import OpenImageIO as oiio


def load_deep(path):
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise RuntimeError(f"Failed to open deep EXR: {path}")
    try:
        spec = image_input.spec()
        deep_data = image_input.read_native_deep_image()
        return spec, deep_data
    finally:
        image_input.close()


def volume_samples(
    deep_data,
    width: int,
    x: int,
    y: int,
    alpha_channel: int,
    z_channel: int,
    z_back_channel: int,
):
    samples = []
    pixel_index = y * width + x
    for sample in range(deep_data.samples(pixel_index)):
        alpha = float(deep_data.deep_value(pixel_index, alpha_channel, sample))
        z = float(deep_data.deep_value(pixel_index, z_channel, sample))
        z_back = float(deep_data.deep_value(pixel_index, z_back_channel, sample))
        if z_back > z:
            samples.append((alpha, z, z_back))
    return samples


def nearly_equal_tuple(a, b, tolerance: float) -> bool:
    return all(math.isclose(lhs, rhs, rel_tol=0.0, abs_tol=tolerance) for lhs, rhs in zip(a, b))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that volume deep samples remain unchanged between a baseline and candidate deep EXR."
    )
    parser.add_argument("baseline", help="Path to the baseline deep EXR file")
    parser.add_argument("candidate", help="Path to the candidate deep EXR file")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1e-6,
        help="Maximum allowed absolute difference per (A, Z, ZBack) value",
    )
    parser.add_argument(
        "--max-report",
        type=int,
        default=10,
        help="Maximum number of mismatching pixels to print",
    )
    args = parser.parse_args()

    baseline_spec, baseline_deep_data = load_deep(args.baseline)
    candidate_spec, candidate_deep_data = load_deep(args.candidate)

    if baseline_spec is None or not baseline_spec.deep:
      raise RuntimeError(f"{args.baseline} is not a deep image")
    if candidate_spec is None or not candidate_spec.deep:
      raise RuntimeError(f"{args.candidate} is not a deep image")
    if baseline_spec.width != candidate_spec.width or baseline_spec.height != candidate_spec.height:
      raise RuntimeError("Baseline/candidate dimensions do not match")

    baseline_alpha_channel = list(baseline_spec.channelnames).index("A")
    baseline_z_channel = list(baseline_spec.channelnames).index("Z")
    baseline_z_back_channel = list(baseline_spec.channelnames).index("ZBack")
    candidate_alpha_channel = list(candidate_spec.channelnames).index("A")
    candidate_z_channel = list(candidate_spec.channelnames).index("Z")
    candidate_z_back_channel = list(candidate_spec.channelnames).index("ZBack")

    checked = 0
    mismatching = 0
    examples = []

    for y in range(candidate_spec.height):
        for x in range(candidate_spec.width):
            baseline_volume = volume_samples(
                baseline_deep_data,
                baseline_spec.width,
                x,
                y,
                baseline_alpha_channel,
                baseline_z_channel,
                baseline_z_back_channel,
            )
            if not baseline_volume:
                continue

            candidate_volume = volume_samples(
                candidate_deep_data,
                candidate_spec.width,
                x,
                y,
                candidate_alpha_channel,
                candidate_z_channel,
                candidate_z_back_channel,
            )
            checked += 1

            pixel_matches = len(baseline_volume) == len(candidate_volume)
            if pixel_matches:
                for baseline_sample, candidate_sample in zip(baseline_volume, candidate_volume):
                    if not nearly_equal_tuple(baseline_sample, candidate_sample, args.tolerance):
                        pixel_matches = False
                        break

            if not pixel_matches:
                mismatching += 1
                if len(examples) < args.max_report:
                    examples.append((x, y, baseline_volume[:6], candidate_volume[:6]))

    print(f"checked_volume_pixels={checked}")
    print(f"mismatching_volume_pixels={mismatching}")
    for x, y, baseline_volume, candidate_volume in examples:
        print(f"  pixel=({x},{y})")
        print(f"    baseline={baseline_volume}")
        print(f"    candidate={candidate_volume}")

    return 1 if mismatching > 0 else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)
