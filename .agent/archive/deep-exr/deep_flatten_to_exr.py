import argparse
import sys

import OpenImageIO as oiio
import numpy as np


def find_channel_indices(channel_names):
    lowered = [name.lower() for name in channel_names]
    def idx_for(keys):
        for key in keys:
            if key in lowered:
                return lowered.index(key)
        return None

    r = idx_for(["r", "red"])
    g = idx_for(["g", "green"])
    b = idx_for(["b", "blue"])
    a = idx_for(["a", "alpha"])

    if r is None or g is None or b is None:
        r, g, b = 0, 1, 2
    return r, g, b, a


def flatten_deep(input_path, output_path):
    buf = oiio.ImageBuf(input_path)
    if not buf.read():
        raise RuntimeError(f"Failed to read deep EXR: {input_path}\n{buf.geterror()}")

    spec = buf.spec()
    if not spec.deep:
        raise RuntimeError(f"Not a deep image: {input_path}")

    width = spec.width
    height = spec.height
    r_idx, g_idx, b_idx, a_idx = find_channel_indices(list(spec.channelnames))
    if a_idx is None or a_idx >= spec.nchannels:
        raise RuntimeError("Deep image missing alpha channel")

    out = np.zeros((height, width, 4), dtype=np.float32)

    for y in range(height):
        for x in range(width):
            samples = buf.deep_samples(x, y)
            if samples <= 0:
                continue
            acc_r = 0.0
            acc_g = 0.0
            acc_b = 0.0
            acc_a = 0.0
            for s in range(samples):
                r = buf.deep_value(x, y, 0, r_idx, s)
                g = buf.deep_value(x, y, 0, g_idx, s)
                b = buf.deep_value(x, y, 0, b_idx, s)
                a = buf.deep_value(x, y, 0, a_idx, s)
                if a < 0.0:
                    a = 0.0
                elif a > 1.0:
                    a = 1.0
                one_minus = 1.0 - acc_a
                acc_r += r * one_minus
                acc_g += g * one_minus
                acc_b += b * one_minus
                acc_a += a * one_minus
            out[y, x, 0] = acc_r
            out[y, x, 1] = acc_g
            out[y, x, 2] = acc_b
            out[y, x, 3] = acc_a

    out_spec = oiio.ImageSpec(width, height, 4, oiio.FLOAT)
    out_spec.channelnames = ["R", "G", "B", "A"]
    out_buf = oiio.ImageBuf(out_spec)
    roi = oiio.ROI(0, width, 0, height, 0, 1, 0, 4)
    if not out_buf.set_pixels(roi, np.ascontiguousarray(out)):
        raise RuntimeError(f"Failed to set output pixels for {output_path}")
    if not out_buf.write(output_path):
        raise RuntimeError(f"Failed to write {output_path}\n{out_buf.geterror()}")


def main():
    parser = argparse.ArgumentParser(description="Flatten deep EXR to flat RGBA EXR.")
    parser.add_argument("input", help="Input deep EXR path")
    parser.add_argument("output", help="Output flat EXR path")
    args = parser.parse_args()
    flatten_deep(args.input, args.output)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("ERROR:", exc)
        sys.exit(1)
