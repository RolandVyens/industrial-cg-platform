import OpenImageIO as oiio
import numpy as np
import sys

flat_path = r"C:\tmp\test_volume_alpha_flat_fix7.exr"
deep_path = r"C:\tmp\test_volume_alpha_deep_fix7.exr"

if len(sys.argv) > 2:
    flat_path = sys.argv[1]
    deep_path = sys.argv[2]

flat = oiio.ImageBuf(flat_path)
fa = flat.get_pixels(oiio.FLOAT)[:, :, 3]

inp = oiio.ImageInput.open(deep_path)
spec = inp.spec()
assert spec.deep
w, h = spec.width, spec.height
deep = inp.read_native_deep_image()

min_diff = 1e9
min_xy = (0, 0)
min_fa = 0.0
min_da = 0.0

for y in range(h):
    row_index = y * w
    for x in range(w):
        pixel_index = row_index + x
        ns = deep.samples(pixel_index)
        a_accum = 0.0
        for s in range(ns):
            a = deep.deep_value(pixel_index, 3, s)
            if a < 0.0:
                a = 0.0
            elif a > 1.0:
                a = 1.0
            a_accum = a_accum + a * (1.0 - a_accum)
        diff = float(fa[y, x]) - a_accum
        if diff < min_diff:
            min_diff = diff
            min_xy = (x, y)
            min_fa = float(fa[y, x])
            min_da = a_accum

x, y = min_xy
pixel_index = y * w + x
ns = deep.samples(pixel_index)
print('min diff', min_diff, 'at', min_xy, 'flat', min_fa, 'deep', min_da, 'samples', ns)
if ns:
    alphas = [deep.deep_value(pixel_index, 3, s) for s in range(min(ns, 10))]
    print('first alphas', alphas)

inp.close()
