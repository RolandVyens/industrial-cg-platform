import sys

import OpenImageIO as oiio
import numpy as np


def load_rgba(path):
    image_input = oiio.ImageInput.open(path)
    if image_input is None:
        raise RuntimeError(f"Failed to open image: {path}")

    spec = image_input.spec()
    pixels = np.array(image_input.read_image(format=oiio.FLOAT))
    image_input.close()

    if pixels.size != spec.width * spec.height * spec.nchannels:
        raise RuntimeError(f"Unexpected pixel count for image: {path}")

    pixels = pixels.reshape(spec.height, spec.width, spec.nchannels)
    if spec.nchannels < 4:
        raise RuntimeError(f"Expected RGBA image, got {spec.nchannels} channels: {path}")

    return pixels[:, :, :4]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_feature4_gpu_flat_alpha_hole.py <cpu_flat.exr> <gpu_flat.exr>")
        return 2

    cpu = load_rgba(sys.argv[1])
    gpu = load_rgba(sys.argv[2])

    if cpu.shape != gpu.shape:
        print(f"shape_mismatch cpu={cpu.shape} gpu={gpu.shape}")
        return 2

    cpu_alpha = cpu[:, :, 3]
    gpu_alpha = gpu[:, :, 3]
    alpha_diff = np.abs(cpu_alpha - gpu_alpha)

    hole_mask = (cpu_alpha > 0.95) & (gpu_alpha < 0.05)
    hole_count = int(hole_mask.sum())
    diff_count = int((alpha_diff > 0.01).sum())
    max_alpha_diff = float(alpha_diff.max())

    worst_index = np.unravel_index(np.argmax(alpha_diff), alpha_diff.shape)
    worst_xy = (int(worst_index[1]), int(worst_index[0]))

    print(f"hole_pixel_count={hole_count}")
    print(f"alpha_diff_pixels_gt_0.01={diff_count}")
    print(f"alpha_max_diff={max_alpha_diff}")
    print(
        "worst_alpha_pixel="
        f"{worst_xy} cpu_alpha={float(cpu_alpha[worst_index])} gpu_alpha={float(gpu_alpha[worst_index])}"
    )

    if hole_count > 1000 or diff_count > 5000 or max_alpha_diff > 0.25:
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
