import bpy

x, y = 473, 614
scene = bpy.context.scene
scene.cycles.samples = 1
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.use_compositing = False
scene.use_nodes = False
scene.render.filepath = r"C:\tmp\alpha_probe_flat"

bpy.ops.render.render(write_still=True)

img = bpy.data.images.get('Render Result')
if img:
    w, h = img.size
    px_len = len(img.pixels)
    print('render_result_size', w, h, 'pixels_len', px_len)
    if w > 0 and h > 0 and px_len >= w * h * 4:
        x_clamp = max(0, min(w - 1, x))
        y_clamp = max(0, min(h - 1, y))
        idx = (y_clamp * w + x_clamp) * 4 + 3
        alpha = img.pixels[idx]
        print('render_result_alpha', alpha)
else:
    print('Render Result not found')
