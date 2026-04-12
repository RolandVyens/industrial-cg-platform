import math
from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
VALIDATION_DIR = ROOT / "validation"
BLEND_PATH = VALIDATION_DIR / "foggy_street_boxes_test.blend"
RENDER_PATH = VALIDATION_DIR / "foggy_street_boxes_test.png"


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)

    for datablocks in (
        bpy.data.meshes,
        bpy.data.materials,
        bpy.data.lights,
        bpy.data.cameras,
        bpy.data.worlds,
    ):
        for datablock in list(datablocks):
            if datablock.users == 0:
                datablocks.remove(datablock)


def set_cycles() -> None:
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 192
    scene.cycles.use_adaptive_sampling = True
    scene.cycles.max_bounces = 6
    scene.cycles.diffuse_bounces = 2
    scene.cycles.glossy_bounces = 2
    scene.cycles.transmission_bounces = 2
    scene.cycles.transparent_max_bounces = 8
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(RENDER_PATH)


def make_material(name, base_color, roughness=0.6, metallic=0.0):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    principled = nodes["Principled BSDF"]
    principled.inputs["Base Color"].default_value = (*base_color, 1.0)
    principled.inputs["Roughness"].default_value = roughness
    principled.inputs["Metallic"].default_value = metallic
    return mat


def add_box(name, location, scale, material):
    bpy.ops.mesh.primitive_cube_add(location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = scale
    obj.data.materials.append(material)
    return obj


def add_ground(material):
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = "Ground"
    obj.scale = (60.0, 60.0, 1.0)
    obj.data.materials.append(material)
    return obj


def add_camera():
    bpy.ops.object.camera_add(
        location=(0.0, -11.5, 2.4),
        rotation=(math.radians(86.0), 0.0, 0.0),
    )
    camera = bpy.context.active_object
    camera.data.lens = 24
    camera.data.dof.use_dof = False
    bpy.context.scene.camera = camera


def add_light(light_type, name, location, rotation=(0.0, 0.0, 0.0), energy=1000.0, color=(1, 1, 1), **kwargs):
    bpy.ops.object.light_add(type=light_type, location=location, rotation=rotation)
    obj = bpy.context.active_object
    obj.name = name
    light = obj.data
    light.energy = energy
    light.color = color
    for key, value in kwargs.items():
        setattr(light, key, value)
    return obj


def build_world():
    world = bpy.data.worlds.new("FogStreetWorld")
    world.use_nodes = True
    bpy.context.scene.world = world

    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new(type="ShaderNodeOutputWorld")
    output.location = (500, 0)

    background = nodes.new(type="ShaderNodeBackground")
    background.location = (140, -120)
    background.inputs["Color"].default_value = (0.0006, 0.0012, 0.0032, 1.0)
    background.inputs["Strength"].default_value = 0.006

    fog = nodes.new(type="ShaderNodeEnvironmentFog")
    fog.location = (-200, 120)
    fog.inputs["Color"].default_value = (0.78, 0.82, 1.0, 1.0)
    fog.inputs["Density"].default_value = 0.08
    fog.inputs["Start Distance"].default_value = 0.0
    fog.inputs["Max Distance"].default_value = 36.0
    fog.inputs["Anisotropy"].default_value = 0.0
    fog.inputs["Samples"].default_value = 24

    links.new(background.outputs["Background"], output.inputs["Surface"])
    links.new(fog.outputs["Volume"], output.inputs["Volume"])


def build_street():
    ground_mat = make_material("GroundMat", (0.03, 0.032, 0.036), roughness=0.95)
    wall_mat = make_material("WallMat", (0.05, 0.05, 0.05), roughness=0.9)
    trim_mat = make_material("TrimMat", (0.05, 0.05, 0.05), roughness=0.72)

    add_ground(ground_mat)

    for x in (-8.0, -4.4, 4.4, 8.0):
        add_box(f"Block_{x:+.1f}", (x, 4.0, 3.2), (1.6, 10.0, 3.2), wall_mat)

    for i, y in enumerate((-6.0, -1.5, 3.0, 7.5)):
        add_box(f"FacadeInset_L_{i}", (-5.7, y, 1.3), (0.4, 1.1, 1.3), trim_mat)
        add_box(f"FacadeInset_R_{i}", (5.7, y + 0.8, 1.3), (0.4, 1.1, 1.3), trim_mat)

    for i, y in enumerate((-4.0, 0.0, 4.0, 8.0)):
        add_box(f"Crate_L_{i}", (-2.2, y, 0.55), (0.7, 0.7, 0.55), trim_mat)
        add_box(f"Crate_R_{i}", (2.2, y + 1.2, 0.75), (0.9, 0.9, 0.75), trim_mat)

    add_box("RoadDivider", (0.0, 5.0, 0.08), (0.22, 14.0, 0.08), trim_mat)


def build_lights():
    sun = add_light(
        "SUN",
        "MoonSun",
        location=(0.0, 0.0, 12.0),
        rotation=(math.radians(58.0), math.radians(4.0), math.radians(-28.0)),
        energy=0.9,
        color=(0.32, 0.46, 1.0),
    )
    sun.data.angle = math.radians(3.0)

    area_colors = (
        (1.0, 0.56, 0.16),
        (1.0, 0.16, 0.72),
        (0.12, 0.9, 1.0),
    )
    add_light("AREA", "StreetLamp_A", (-3.5, -2.5, 3.8), rotation=(math.radians(90), 0, 0), energy=36000.0, color=area_colors[0], shape="RECTANGLE", size=0.42, size_y=0.42)
    add_light("AREA", "StreetLamp_B", (3.5, 2.0, 3.8), rotation=(math.radians(90), 0, 0), energy=36000.0, color=area_colors[1], shape="RECTANGLE", size=0.42, size_y=0.42)
    add_light("AREA", "StreetLamp_C", (-3.5, 7.0, 3.8), rotation=(math.radians(90), 0, 0), energy=36000.0, color=area_colors[2], shape="RECTANGLE", size=0.42, size_y=0.42)

    # Larger wall-wash area lights so area-light scattering is unmistakable in validation renders.
    add_light("AREA", "WallWash_L", (-5.15, 1.5, 2.2), rotation=(0.0, math.radians(90.0), 0.0), energy=24000.0, color=(1.0, 0.3, 0.1), shape="RECTANGLE", size=0.7, size_y=4.8)
    add_light("AREA", "WallWash_R", (5.15, 5.4, 2.2), rotation=(0.0, math.radians(-90.0), 0.0), energy=22000.0, color=(0.08, 0.84, 1.0), shape="RECTANGLE", size=0.7, size_y=4.2)
    add_light("AREA", "BillboardGlow", (0.0, 9.8, 3.0), rotation=(math.radians(90.0), 0.0, math.radians(180.0)), energy=30000.0, color=(0.7, 0.18, 1.0), shape="RECTANGLE", size=2.8, size_y=1.0)

    for idx, x in enumerate((-3.5, 3.5, -3.5)):
        y = (-2.5, 2.0, 7.0)[idx]
        add_box(f"LampPole_{idx}", (x, y, 1.9), (0.06, 0.06, 1.9), make_material(f"PoleMat_{idx}", (0.11, 0.11, 0.12), roughness=0.5, metallic=0.2))

    point_colors = (
        (1.0, 0.58, 0.14),
        (0.46, 1.0, 0.3),
        (0.08, 0.92, 1.0),
    )
    add_light("POINT", "StreetGlow_A", (-2.9, -2.0, 2.6), energy=7200.0, color=point_colors[0], shadow_soft_size=0.7)
    add_light("POINT", "StreetGlow_B", (2.9, 2.4, 2.6), energy=7200.0, color=point_colors[1], shadow_soft_size=0.7)
    add_light("POINT", "StreetGlow_C", (-2.9, 6.8, 2.6), energy=7200.0, color=point_colors[2], shadow_soft_size=0.7)
    add_light(
        "POINT",
        "BackFill",
        location=(0.0, 10.5, 2.8),
        energy=4600.0,
        color=(0.95, 0.22, 0.5),
        shadow_soft_size=1.1,
    )

    spot = add_light(
        "SPOT",
        "SideSpot",
        location=(6.8, -7.0, 4.8),
        rotation=(math.radians(72.0), 0.0, math.radians(58.0)),
        energy=18000.0,
        color=(0.12, 0.72, 1.0),
        shadow_soft_size=0.2,
    )
    spot.data.spot_size = math.radians(18.0)
    spot.data.spot_blend = 0.08

    beam = add_light(
        "SPOT",
        "BeamSpot",
        location=(0.0, 12.5, 3.2),
        rotation=(math.radians(102.0), 0.0, math.radians(180.0)),
        energy=26000.0,
        color=(1.0, 0.42, 0.18),
        shadow_soft_size=0.15,
    )
    beam.data.spot_size = math.radians(14.0)
    beam.data.spot_blend = 0.03


def save_and_render():
    ensure_dir(VALIDATION_DIR)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND_PATH))
    bpy.ops.render.render(write_still=True)


def main():
    ensure_dir(VALIDATION_DIR)
    clear_scene()
    set_cycles()
    build_street()
    add_camera()
    build_lights()
    build_world()
    save_and_render()


if __name__ == "__main__":
    main()
