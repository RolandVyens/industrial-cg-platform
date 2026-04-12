import math
import random
from pathlib import Path

import bpy


ROOT = Path(r"E:\blender_modify\blender\.agent\features\world-environment-fog")
VALIDATION_DIR = ROOT / "validation"
BLEND_PATH = VALIDATION_DIR / "cyberpunk_city_overview_test.blend"
RENDER_PATH = VALIDATION_DIR / "cyberpunk_city_overview_test.png"

RNG = random.Random(42)

CITY_BLOCK_COUNT_X = 14
CITY_BLOCK_COUNT_Y = 14
BLOCK_SPACING = 8.0
ROAD_WIDTH = 2.2


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
    scene.cycles.device = "GPU"
    scene.cycles.samples = 512
    scene.cycles.use_adaptive_sampling = False
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
    principled = mat.node_tree.nodes["Principled BSDF"]
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


def add_plane(name, size, location, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def add_camera() -> None:
    bpy.ops.object.camera_add(
        location=(0.0, -62.0, 48.0),
        rotation=(math.radians(63.0), 0.0, 0.0),
    )
    camera = bpy.context.active_object
    camera.data.lens = 34
    camera.data.clip_end = 5000.0
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


def build_world() -> None:
    world = bpy.data.worlds.new("CyberpunkCityOverviewWorld")
    world.use_nodes = True
    bpy.context.scene.world = world

    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()

    output = nodes.new(type="ShaderNodeOutputWorld")
    output.location = (450, 0)

    background = nodes.new(type="ShaderNodeBackground")
    background.location = (120, -120)
    background.inputs["Color"].default_value = (0.00018, 0.00032, 0.0011, 1.0)
    background.inputs["Strength"].default_value = 0.0012

    fog = nodes.new(type="ShaderNodeEnvironmentFog")
    fog.location = (-180, 120)
    fog.inputs["Color"].default_value = (0.46, 0.54, 0.72, 1.0)
    fog.inputs["Density"].default_value = 0.055
    fog.inputs["Start Distance"].default_value = 0.0
    fog.inputs["Max Distance"].default_value = 220.0
    fog.inputs["Anisotropy"].default_value = 0.35
    fog.inputs["Samples"].default_value = 32

    links.new(background.outputs["Background"], output.inputs["Surface"])
    links.new(fog.outputs["Volume"], output.inputs["Volume"])


def build_city() -> None:
    half_x = (CITY_BLOCK_COUNT_X - 1) * BLOCK_SPACING * 0.5
    half_y = (CITY_BLOCK_COUNT_Y - 1) * BLOCK_SPACING * 0.5

    ground_mat = make_material("CityGround", (0.012, 0.013, 0.018), roughness=0.98)
    road_mat = make_material("CityRoad", (0.018, 0.019, 0.025), roughness=0.94)
    tower_palette = [
        make_material("TowerA", (0.035, 0.038, 0.05), roughness=0.9, metallic=0.05),
        make_material("TowerB", (0.03, 0.028, 0.04), roughness=0.88, metallic=0.06),
        make_material("TowerC", (0.04, 0.032, 0.045), roughness=0.9, metallic=0.08),
        make_material("TowerD", (0.026, 0.03, 0.036), roughness=0.87, metallic=0.04),
    ]

    add_plane("CityBase", 400.0, (0.0, 0.0, -0.02), ground_mat)
    add_plane("MainRoad", 1.0, (0.0, 0.0, 0.0), road_mat).scale = (120.0, 120.0, 1.0)

    for ix in range(CITY_BLOCK_COUNT_X):
        x = ix * BLOCK_SPACING - half_x
        for iy in range(CITY_BLOCK_COUNT_Y):
            y = iy * BLOCK_SPACING - half_y
            width = RNG.uniform(1.8, 3.1)
            depth = RNG.uniform(1.8, 3.1)
            height = RNG.uniform(4.0, 20.0)
            lot_x = x + RNG.uniform(-1.15, 1.15)
            lot_y = y + RNG.uniform(-1.15, 1.15)
            material = tower_palette[(ix + iy) % len(tower_palette)]
            add_box(
                f"Tower_{ix:02d}_{iy:02d}",
                (lot_x, lot_y, height * 0.5),
                (width, depth, height * 0.5),
                material,
            )

    trim_mat = make_material("StreetTrim", (0.055, 0.06, 0.075), roughness=0.65, metallic=0.35)
    for ix in range(CITY_BLOCK_COUNT_X):
        x = ix * BLOCK_SPACING - half_x
        for iy in range(CITY_BLOCK_COUNT_Y):
            y = iy * BLOCK_SPACING - half_y
            add_box(
                f"Pole_NE_{ix:02d}_{iy:02d}",
                (x + ROAD_WIDTH * 0.55, y + ROAD_WIDTH * 0.55, 1.8),
                (0.07, 0.07, 1.8),
                trim_mat,
            )


def build_lights() -> None:
    half_x = (CITY_BLOCK_COUNT_X - 1) * BLOCK_SPACING * 0.5
    half_y = (CITY_BLOCK_COUNT_Y - 1) * BLOCK_SPACING * 0.5
    light_colors = [
        (1.0, 0.42, 0.18),
        (1.0, 0.12, 0.62),
        (0.14, 0.92, 1.0),
        (0.26, 1.0, 0.58),
    ]

    light_index = 0
    for ix in range(CITY_BLOCK_COUNT_X):
        x = ix * BLOCK_SPACING - half_x
        for iy in range(CITY_BLOCK_COUNT_Y):
            y = iy * BLOCK_SPACING - half_y
            base_color = light_colors[(ix + iy) % len(light_colors)]
            add_light(
                "AREA",
                f"StreetArea_{ix:02d}_{iy:02d}",
                (x + ROAD_WIDTH * 0.55, y + ROAD_WIDTH * 0.55, 3.9),
                rotation=(math.radians(90.0), 0.0, 0.0),
                energy=7600.0 + (light_index % 5) * 400.0,
                color=base_color,
                shape="RECTANGLE",
                size=0.34,
                size_y=0.34,
            )
            light_index += 1

    point_colors = [
        (1.0, 0.35, 0.16),
        (0.15, 0.86, 1.0),
        (1.0, 0.14, 0.75),
    ]
    for ix in range(0, CITY_BLOCK_COUNT_X, 2):
        x = ix * BLOCK_SPACING - half_x
        for iy in range(0, CITY_BLOCK_COUNT_Y, 2):
            y = iy * BLOCK_SPACING - half_y
            color = point_colors[(ix + iy) % len(point_colors)]
            add_light(
                "POINT",
                f"AccentPoint_{ix:02d}_{iy:02d}",
                (x - 0.8, y + 0.6, 6.0 + ((ix + iy) % 3) * 1.6),
                energy=3400.0,
                color=color,
                shadow_soft_size=0.85,
            )

    beam_specs = [
        ((-36.0, -44.0, 16.0), (66.0, 0.0, 34.0), (0.12, 0.94, 1.0), 62000.0, 13.0),
        ((38.0, -18.0, 18.0), (70.0, 0.0, -40.0), (1.0, 0.18, 0.62), 66000.0, 12.0),
        ((-12.0, 42.0, 20.0), (72.0, 0.0, 188.0), (1.0, 0.52, 0.16), 64000.0, 12.0),
        ((32.0, 36.0, 17.0), (68.0, 0.0, 208.0), (0.28, 1.0, 0.62), 60000.0, 13.0),
        ((-48.0, 4.0, 15.0), (69.0, 0.0, 78.0), (0.18, 0.86, 1.0), 52000.0, 11.0),
        ((50.0, 8.0, 15.5), (68.0, 0.0, -84.0), (1.0, 0.2, 0.48), 52000.0, 11.0),
        ((0.0, -52.0, 16.5), (67.0, 0.0, 2.0), (1.0, 0.55, 0.18), 56000.0, 10.0),
        ((6.0, 52.0, 16.5), (71.0, 0.0, 180.0), (0.24, 1.0, 0.66), 56000.0, 10.0),
    ]
    for index, (location, rotation_deg, color, energy, spot_deg) in enumerate(beam_specs):
        spot = add_light(
            "SPOT",
            f"SkyBeam_{index}",
            location=location,
            rotation=tuple(math.radians(v) for v in rotation_deg),
            energy=energy,
            color=color,
            shadow_soft_size=0.25,
        )
        spot.data.spot_size = math.radians(spot_deg)
        spot.data.spot_blend = 0.06

    sun = add_light(
        "SUN",
        "CityMoonSun",
        location=(0.0, 0.0, 120.0),
        rotation=(math.radians(58.0), math.radians(0.0), math.radians(-22.0)),
        energy=0.08,
        color=(0.15, 0.24, 0.6),
    )
    sun.data.angle = math.radians(2.0)


def save_and_render() -> None:
    ensure_dir(VALIDATION_DIR)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND_PATH))
    bpy.ops.render.render(write_still=True)
    render_result = bpy.data.images.get("Render Result")
    if render_result is not None:
        render_result.save_render(filepath=str(RENDER_PATH), scene=bpy.context.scene)


def main() -> None:
    ensure_dir(VALIDATION_DIR)
    clear_scene()
    set_cycles()
    build_city()
    add_camera()
    build_lights()
    build_world()
    save_and_render()


if __name__ == "__main__":
    main()
