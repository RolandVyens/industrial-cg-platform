import bpy

scene = bpy.context.scene
scene.render.filepath = r'C:/tmp/scene_output_rgba_flat_probe_####'
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.image_settings.color_depth = '32'

try:
    scene.use_nodes = False
except Exception as e:
    print('use_nodes_disable_failed', e)

scene.cycles.device = 'CPU'
prefs = bpy.context.preferences.addons['cycles'].preferences
prefs.compute_device_type = 'NONE'

print('configured filepath', scene.render.filepath)
print('configured format', scene.render.image_settings.file_format)
print('configured color_mode', scene.render.image_settings.color_mode)
print('configured color_depth', scene.render.image_settings.color_depth)
