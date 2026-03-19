import sys
from pathlib import Path

import OpenImageIO as oiio
import numpy as np

REQUIRED_SUBIMAGES = {
    'RGBA_env',
    'RGBA_key',
    'RGBA_emissive',
    'diffuse_env',
    'diffuse_direct_env',
    'diffuse_indirect_env',
    'glossy_env',
    'glossy_direct_env',
    'glossy_indirect_env',
    'transmission_env',
    'transmission_direct_env',
    'transmission_indirect_env',
    'volume_env',
    'volume_direct_env',
    'volume_indirect_env',
    'diffuse_key',
    'diffuse_direct_key',
    'diffuse_indirect_key',
    'glossy_key',
    'glossy_direct_key',
    'glossy_indirect_key',
    'transmission_key',
    'transmission_direct_key',
    'transmission_indirect_key',
    'volume_key',
    'volume_direct_key',
    'volume_indirect_key',
}

FORBIDDEN_EMISSIVE_SPLITS = {
    'diffuse_emissive',
    'diffuse_direct_emissive',
    'diffuse_indirect_emissive',
    'glossy_emissive',
    'glossy_direct_emissive',
    'glossy_indirect_emissive',
    'transmission_emissive',
    'transmission_direct_emissive',
    'transmission_indirect_emissive',
    'volume_emissive',
    'volume_direct_emissive',
    'volume_indirect_emissive',
}


def load_subimages(path: Path):
    image_input = oiio.ImageInput.open(str(path))
    if image_input is None:
        raise RuntimeError(f'Failed to open image: {path}')

    subimages = {}
    subimage_index = 0
    while True:
        spec = image_input.spec()
        name = spec.getattribute('oiio:subimagename') or f'subimage_{subimage_index}'
        pixels = np.array(image_input.read_image(format=oiio.FLOAT), dtype=np.float32)
        pixels = pixels.reshape(spec.height, spec.width, spec.nchannels)
        subimages[name] = pixels[:, :, : min(spec.nchannels, 4)]
        if not image_input.seek_subimage(subimage_index + 1, 0):
            break
        subimage_index += 1

    image_input.close()
    return subimages


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_feature4_lightgroup_subimages.py <multilayer.exr>')
        return 2

    path = Path(sys.argv[1])
    subimages = load_subimages(path)
    names = set(subimages)

    missing = sorted(REQUIRED_SUBIMAGES - names)
    forbidden = sorted(FORBIDDEN_EMISSIVE_SPLITS & names)

    key_activity = {
        name: float(np.max(subimages[name][:, :, :3]))
        for name in sorted(name for name in names if name.endswith('_key') and name != 'RGBA_key')
    }
    env_activity = {
        name: float(np.max(subimages[name][:, :, :3]))
        for name in sorted(name for name in names if name.endswith('_env') and name != 'RGBA_env')
    }

    print(f'subimage_count={len(subimages)}')
    print(f'missing_required={missing}')
    print(f'forbidden_emissive_splits={forbidden}')
    print(f'key_split_max_rgb={key_activity}')
    print(f'env_split_max_rgb={env_activity}')

    inactive_key = sorted(name for name, value in key_activity.items() if value <= 0.0)
    inactive_env = sorted(name for name, value in env_activity.items() if value <= 0.0)
    print(f'inactive_key_splits={inactive_key}')
    print(f'inactive_env_splits={inactive_env}')

    if missing or forbidden or inactive_key or inactive_env:
        return 1

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
