import OpenImageIO as oiio
import numpy as np
import sys

path = r"C:\tmp\test_volume_alpha_deep_fix7.exr"
if len(sys.argv) > 1:
    path = sys.argv[1]

inp = oiio.ImageInput.open(path)
if not inp:
    raise SystemExit(f"Failed to open {path}")
spec = inp.spec()
if not spec.deep:
    raise SystemExit("Not a deep image")

w, h = spec.width, spec.height
deep = inp.read_native_deep_image()
counts = np.zeros((h, w), dtype=np.int32)
maxc = 0
for y in range(h):
    row_index = y * w
    for x in range(w):
        c = deep.samples(row_index + x)
        counts[y, x] = c
        if c > maxc:
            maxc = c

print("max_samples", int(maxc))
print("avg_samples", float(counts.mean()))
print("pixels_at_max", int((counts == maxc).sum()))

inp.close()
