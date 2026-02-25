import argparse
import sys

import OpenImageIO as oiio


def load_alpha(path):
    buf = oiio.ImageBuf(path)
    spec = buf.spec()
    if spec.nchannels < 4:
        raise RuntimeError(f"{path}: expected >=4 channels, got {spec.nchannels}")
    pixels = buf.get_pixels(oiio.FLOAT, oiio.ROI())
    return pixels[:, :, 3], spec.width, spec.height


def load_deep_alpha(path):
    deep = oiio.ImageBuf(path)
    spec = deep.spec()
    if not spec.deep:
        raise RuntimeError(f"{path}: not a deep image")
    width, height = spec.width, spec.height
    alpha = [[0.0] * width for _ in range(height)]
    for y in range(height):
        row = alpha[y]
        for x in range(width):
            samples = deep.deep_samples(x, y)
            if samples <= 0:
                row[x] = 0.0
                continue
            a_accum = 0.0
            for s in range(samples):
                a = deep.deep_value(x, y, 0, 3, s)
                if a < 0.0:
                    a = 0.0
                elif a > 1.0:
                    a = 1.0
                a_accum = a_accum + a * (1.0 - a_accum)
            row[x] = a_accum
    return alpha, width, height


def main():
    parser = argparse.ArgumentParser(description="Compare flat alpha vs deep alpha (flattened).")
    parser.add_argument("--flat", required=True, help="Path to flat EXR")
    parser.add_argument("--deep", required=True, help="Path to deep EXR")
    parser.add_argument("--threshold", type=float, default=0.05, help="Diff threshold")
    args = parser.parse_args()

    flat_alpha, flat_w, flat_h = load_alpha(args.flat)
    deep_alpha, deep_w, deep_h = load_deep_alpha(args.deep)

    if flat_w != deep_w or flat_h != deep_h:
        raise RuntimeError("Flat/deep dimensions do not match")

    count = flat_w * flat_h
    sum_diff = 0.0
    sum_abs = 0.0
    min_diff = 1e9
    max_diff = -1e9
    high_pos = 0
    high_neg = 0

    for y in range(flat_h):
        for x in range(flat_w):
            diff = float(flat_alpha[y, x]) - float(deep_alpha[y][x])
            sum_diff += diff
            abs_diff = diff if diff >= 0.0 else -diff
            sum_abs += abs_diff
            if diff < min_diff:
                min_diff = diff
            if diff > max_diff:
                max_diff = diff
            if diff > args.threshold:
                high_pos += 1
            elif diff < -args.threshold:
                high_neg += 1

    print(
        "alpha diff stats: mean_diff={:.6f} mean_abs_diff={:.6f} min_diff={:.6f} "
        "max_diff={:.6f} | diff>{}={} diff<{}={}".format(
            sum_diff / count,
            sum_abs / count,
            min_diff,
            max_diff,
            args.threshold,
            high_pos,
            -args.threshold,
            high_neg,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("ERROR:", exc)
        sys.exit(1)
