import sys

import OpenImageIO as oiio


TARGET_PIXELS = [
    (1066, 533),
    (1066, 534),
    (1066, 535),
    (1067, 536),
]

ALPHA_TOLERANCE = 1e-4


def load_flat_rgba(path):
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise RuntimeError(f"Failed to open flat EXR: {path}")
    try:
        spec = image_input.spec()
        pixels = image_input.read_image("float")
        return spec.width, spec.height, pixels
    finally:
        image_input.close()


def load_deep(path):
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise RuntimeError(f"Failed to open deep EXR: {path}")
    try:
        spec = image_input.spec()
        deep_data = image_input.read_native_deep_image()
        alpha_channel = spec.channelindex("A")
        if alpha_channel < 0:
            raise RuntimeError("Deep EXR does not contain A channel")
        return spec.width, spec.height, deep_data, alpha_channel
    finally:
        image_input.close()


def flattened_alpha(deep_data, width, alpha_channel, x, y):
    pixel_index = y * width + x
    transparency = 1.0
    for sample_index in range(deep_data.samples(pixel_index)):
        sample_alpha = float(deep_data.deep_value(pixel_index, alpha_channel, sample_index))
        transparency *= (1.0 - sample_alpha)
    return 1.0 - transparency, deep_data.samples(pixel_index)


def main():
    if len(sys.argv) != 3:
        print("usage: check_deep_mixed_surface_volume_case1.py <flat_exr> <deep_exr>")
        return 2

    flat_width, flat_height, flat_pixels = load_flat_rgba(sys.argv[1])
    deep_width, deep_height, deep_data, alpha_channel = load_deep(sys.argv[2])
    if flat_width != deep_width or flat_height != deep_height:
        print("dimension_mismatch")
        return 2

    mismatches = []
    for x, y in TARGET_PIXELS:
        flat_alpha = float(flat_pixels[y, x, 3])
        deep_alpha, sample_count = flattened_alpha(deep_data, deep_width, alpha_channel, x, y)
        diff = abs(deep_alpha - flat_alpha)
        print(
            f"pixel=({x},{y}) flat_alpha={flat_alpha:.9f} deep_flatten_alpha={deep_alpha:.9f} "
            f"diff={diff:.9f} sample_count={sample_count}"
        )
        if diff > ALPHA_TOLERANCE:
            mismatches.append((x, y, diff))

    print(f"checked_pixels={len(TARGET_PIXELS)}")
    print(f"mismatching_pixels={len(mismatches)}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
