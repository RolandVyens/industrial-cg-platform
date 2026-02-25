import OpenImageIO as oiio
import os

paths = [
    r"C:\\tmp\\test_direct_caseA.exr",
    r"C:\\tmp\\test_compoutput_deep.exr",
    r"C:\\tmp\\test_compoutput_deep0001.exr",
]

def info(path):
    if not os.path.exists(path):
        print(f"MISSING: {path}")
        return
    inp = oiio.ImageInput.open(path)
    if not inp:
        print(f"FAILED OPEN: {path}")
        return
    spec = inp.spec()
    print("\nFILE:", path)
    print("  deep:", spec.deep)
    print("  size:", spec.width, "x", spec.height, "channels:", spec.nchannels)
    print("  channels:", ", ".join(spec.channelnames))
    if spec.deep:
        deep = inp.read_native_deep_image()
        if deep is None:
            print("  deep read: FAILED")
        else:
            w, h = spec.width, spec.height
            test_coords = [(0,0), (w//2, h//2), (w-1, h-1)]
            for x,y in test_coords:
                idx = y * w + x
                try:
                    ns = deep.samples(idx)
                except Exception as e:
                    ns = f"ERR: {e}"
                print(f"  samples ({x},{y}) idx {idx}: {ns}")
    inp.close()

for p in paths:
    info(p)
