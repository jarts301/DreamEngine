#!/usr/bin/env python3
"""Generates the original models shared by the samples, in samples/assets/models, as .glb files.

Textures are PNG files of their own in models/textures, which the build cooks into KTX2 (see
docs/adr/0024-texture-cooking.md); the models refer to them by name. Every material states its
metallic and roughness factors, since glTF's defaults are fully metallic and fully rough (see
docs/guides/materials.md). The brick wall has a normal map and an occlusion-roughness-metallic
map, and no tangents, which the engine's tangent generator makes. The glass pane is blended, the
fence is cut out of a board by its texture's alpha, and the lamp glows. The tentacle is skinned to a
chain of joints and has two animations, "sway" and "curl" (see docs/guides/animation.md).

Usage: python tools/make_sample_models.py

Needs only Python's standard library. The results are committed, so this runs only to change them.
"""

import json
import math
import struct
import zlib
from pathlib import Path

OUTPUT = Path(__file__).resolve().parent.parent / "samples" / "assets" / "models"


def png_bytes(width, height, pixel):
    """Encodes an image, where pixel(x, y) returns (r, g, b), or (r, g, b, a) for an image with
    alpha. The first pixel decides which."""

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    channels = len(pixel(0, 0))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))
    color_type = 6 if channels == 4 else 2
    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )


def write_glb(
    path,
    positions,
    normals,
    uvs,
    indices,
    base_color,
    image=None,
    metallic=0.0,
    roughness=0.7,
    normal_image=None,
    orm_image=None,
    alpha_mode=None,
    alpha_cutoff=None,
    double_sided=False,
    emissive=None,
    emissive_strength=None,
):
    """Writes one mesh with one material. Each image is (file name, PNG bytes), saved in the
    textures folder beside the model: `image` is the base color, `normal_image` a normal map, and
    `orm_image` occlusion in red, roughness in green and metallic in blue. `alpha_mode` is glTF's
    "MASK" or "BLEND", and `emissive` a linear color, which `emissive_strength` multiplies through
    KHR_materials_emissive_strength."""
    views = []
    accessors = []
    binary = bytearray()

    def add(data, target=None):
        while len(binary) % 4:
            binary.append(0)
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        binary.extend(data)
        views.append(view)
        return len(views) - 1

    def add_floats(values, count, kind, extra=None):
        flat = [component for value in values for component in value]
        data = struct.pack(f"<{len(flat)}f", *flat)
        accessor = {
            "bufferView": add(data, 34962),
            "componentType": 5126,
            "count": count,
            "type": kind,
        }
        accessor.update(extra or {})
        accessors.append(accessor)
        return len(accessors) - 1

    bounds = {
        "min": [min(p[axis] for p in positions) for axis in range(3)],
        "max": [max(p[axis] for p in positions) for axis in range(3)],
    }
    position_accessor = add_floats(positions, len(positions), "VEC3", bounds)
    normal_accessor = add_floats(normals, len(normals), "VEC3")
    uv_accessor = add_floats(uvs, len(uvs), "VEC2")

    index_view = add(struct.pack(f"<{len(indices)}H", *indices), 34963)
    accessors.append(
        {"bufferView": index_view, "componentType": 5123, "count": len(indices), "type": "SCALAR"}
    )
    index_accessor = len(accessors) - 1

    material = {
        "pbrMetallicRoughness": {
            "baseColorFactor": list(base_color),
            "metallicFactor": metallic,
            "roughnessFactor": roughness,
        },
        "name": path.stem,
    }
    if alpha_mode is not None:
        material["alphaMode"] = alpha_mode
    if alpha_cutoff is not None:
        material["alphaCutoff"] = alpha_cutoff
    if double_sided:
        material["doubleSided"] = True
    if emissive is not None:
        material["emissiveFactor"] = list(emissive)
    if emissive_strength is not None:
        material["extensions"] = {
            "KHR_materials_emissive_strength": {"emissiveStrength": emissive_strength}
        }
    gltf = {
        "asset": {"version": "2.0", "generator": "DreamEngine make_sample_models.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": path.stem}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": position_accessor,
                            "NORMAL": normal_accessor,
                            "TEXCOORD_0": uv_accessor,
                        },
                        "indices": index_accessor,
                        "material": 0,
                    }
                ]
            }
        ],
        "materials": [material],
        "accessors": accessors,
        "bufferViews": views,
    }

    def add_texture(named_png):
        name, png = named_png
        (path.parent / "textures").mkdir(exist_ok=True)
        (path.parent / "textures" / name).write_bytes(png)
        gltf.setdefault("images", []).append({"uri": f"textures/{name}"})
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
        textures = gltf.setdefault("textures", [])
        textures.append({"source": len(gltf["images"]) - 1, "sampler": 0})
        return len(textures) - 1

    if image is not None:
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": add_texture(image)}
    if normal_image is not None:
        material["normalTexture"] = {"index": add_texture(normal_image)}
    if orm_image is not None:
        index = add_texture(orm_image)
        material["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {"index": index}
        material["occlusionTexture"] = {"index": index}

    if emissive_strength is not None:
        gltf["extensionsUsed"] = ["KHR_materials_emissive_strength"]

    while len(binary) % 4:
        binary.append(0)
    gltf["buffers"] = [{"byteLength": len(binary)}]

    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * ((4 - len(text) % 4) % 4)

    total = 12 + 8 + len(text) + 8 + len(binary)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, total)
        + struct.pack("<II", len(text), 0x4E4F534A)
        + text
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )
    print(f"{path.name}: {total} bytes, {len(positions)} vertices, {len(indices) // 3} triangles")


def wood_pixel(x, y):
    """Horizontal planks with a darker frame, like a crate."""
    border = x < 5 or y < 5 or x >= 59 or y >= 59
    if border:
        return (120, 78, 42)
    plank = (y - 5) // 9
    grain = 12 if (x * 3 + plank * 17) % 23 < 6 else 0
    seam = (y - 5) % 9 == 0
    base = 150 + (plank * 37) % 30 - grain
    if seam:
        base -= 70
    return (base, int(base * 0.72), int(base * 0.42))


def checker_pixel(x, y):
    light = (x // 16 + y // 16) % 2 == 0
    return (168, 178, 160) if light else (92, 104, 88)


CUBE_FACES = [
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
]


def block_pixel(x, y):
    """White with a slightly darker rim, so a tinted block keeps its edges."""
    edge = min(x, y, 63 - x, 63 - y)
    shade = 255 if edge >= 5 else 200
    return (shade, shade, shade)


def box(half, texture_size=None):
    """A box with these half sizes along X, Y and Z. Without a texture size, each face shows the
    whole texture; with one, in meters, the texture repeats across faces at that size."""
    corners = [(-1, -1, (0, 1)), (1, -1, (1, 1)), (1, 1, (1, 0)), (-1, 1, (0, 0))]
    positions, normals, uvs, indices = [], [], [], []
    for normal, u, v in CUBE_FACES:
        first = len(positions)
        width = sum(abs(u[i]) * half[i] for i in range(3)) * 2
        height = sum(abs(v[i]) * half[i] for i in range(3)) * 2
        for su, sv, uv in corners:
            positions.append(
                tuple(half[i] * (normal[i] + su * u[i] + sv * v[i]) for i in range(3))
            )
            normals.append(normal)
            if texture_size is None:
                uvs.append(uv)
            else:
                uvs.append((uv[0] * width / texture_size, uv[1] * height / texture_size))
        indices += [first, first + 1, first + 2, first, first + 2, first + 3]
    return positions, normals, uvs, indices


def cube(path, image, roughness):
    """A cube 2 m across, with the image on each face."""
    write_glb(path, *box((1, 1, 1)), (1, 1, 1, 1), image, roughness=roughness)


def make_block():
    # Nearly white, so scenes can give each block any color with a tint.
    cube(OUTPUT / "block.glb", ("block.png", png_bytes(64, 64, block_pixel)), 0.5)


def make_crate():
    cube(OUTPUT / "crate.glb", ("crate.png", png_bytes(64, 64, wood_pixel)), 0.8)


# Bricks of the wall's texture: four rows, two bricks across, each row shifted by half a brick,
# with mortar between them.
BRICK_SIZE = 128
BRICK_ROWS = 4
BRICK_MORTAR = 5


def brick_height(x, y):
    """How far the surface stands out, from 0 in the mortar to 1 on a brick's face, with bevelled
    edges and a little roughness on the face."""
    row_height = BRICK_SIZE // BRICK_ROWS
    brick_width = BRICK_SIZE // 2
    row = (y % BRICK_SIZE) // row_height
    shifted = (x + (brick_width // 2 if row % 2 else 0)) % BRICK_SIZE
    inside_y = y % row_height
    inside_x = shifted % brick_width
    edge = min(inside_x, brick_width - 1 - inside_x, inside_y, row_height - 1 - inside_y)
    if edge < BRICK_MORTAR // 2:
        return 0.0
    bevel = min(1.0, (edge - BRICK_MORTAR // 2 + 1) / 3)
    speckle = ((x * 73 + y * 151 + (x * y) % 37) % 17) / 17 * 0.08
    return bevel * (0.92 + speckle)


def brick_color(x, y):
    height = brick_height(x, y)
    if height == 0.0:
        return (150, 146, 138)
    row = (y % BRICK_SIZE) // (BRICK_SIZE // BRICK_ROWS)
    brick = (x + (BRICK_SIZE // 4 if row % 2 else 0)) // (BRICK_SIZE // 2) + row * 2
    shade = 0.82 + ((brick * 41) % 7) / 7 * 0.25
    return tuple(min(255, int(c * shade)) for c in (168, 74, 52))


def brick_normal(x, y):
    """The tangent-space normal of brick_height, as glTF stores it: red along +U, green toward the
    top of the image, blue out of the surface."""

    def height(px, py):
        return brick_height(px % BRICK_SIZE, py % BRICK_SIZE)

    strength = 2.5
    along_u = (height(x + 1, y) - height(x - 1, y)) * 0.5
    # The image's rows grow downward, so up the image is toward smaller y.
    up_image = (height(x, y - 1) - height(x, y + 1)) * 0.5
    nx, ny, nz = -along_u * strength, -up_image * strength, 1.0
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    return tuple(int(round((c / length * 0.5 + 0.5) * 255)) for c in (nx, ny, nz))


def brick_orm(x, y):
    """Occlusion darkens the mortar's grooves; bricks are rough, mortar rougher, nothing metal."""
    height = brick_height(x, y)
    occlusion = 0.55 + 0.45 * height
    roughness = 0.95 - 0.15 * height
    return (int(occlusion * 255), int(roughness * 255), 0)


def make_brick_wall():
    # 3 m long, 1.5 m high and 0.3 m thick, with the texture's four rows of bricks each meter.
    write_glb(
        OUTPUT / "brick_wall.glb",
        *box((1.5, 0.75, 0.15), texture_size=1.0),
        (1, 1, 1, 1),
        ("brick.png", png_bytes(BRICK_SIZE, BRICK_SIZE, brick_color)),
        roughness=1.0,
        normal_image=("brick_normal.png", png_bytes(BRICK_SIZE, BRICK_SIZE, brick_normal)),
        orm_image=("brick_orm.png", png_bytes(BRICK_SIZE, BRICK_SIZE, brick_orm)),
    )


def make_ground():
    positions = [(-1, 0, 1), (1, 0, 1), (1, 0, -1), (-1, 0, -1)]
    normals = [(0, 1, 0)] * 4
    uvs = [(0, 8), (8, 8), (8, 0), (0, 0)]
    write_glb(
        OUTPUT / "ground.glb",
        positions,
        normals,
        uvs,
        [0, 1, 2, 0, 2, 3],
        (1, 1, 1, 1),
        ("ground.png", png_bytes(64, 64, checker_pixel)),
        roughness=0.9,
    )


def sphere():
    rows, columns = 16, 24
    positions, normals, uvs, indices = [], [], [], []
    for row in range(rows + 1):
        theta = math.pi * row / rows
        for column in range(columns + 1):
            phi = 2 * math.pi * column / columns
            point = (math.sin(theta) * math.sin(phi), math.cos(theta), math.sin(theta) * math.cos(phi))
            positions.append(point)
            normals.append(point)
            uvs.append((column / columns, row / rows))
    for row in range(rows):
        for column in range(columns):
            a = row * (columns + 1) + column
            b = a + columns + 1
            indices += [a, b, b + 1, a, b + 1, a + 1]
    return positions, normals, uvs, indices


def make_ball():
    # White, so scenes can give each ball any color with a tint: a smooth, glossy plastic.
    write_glb(OUTPUT / "ball.glb", *sphere(), (1, 1, 1, 1), roughness=0.35)


def make_gold_ball():
    # Gold's reflectance, as linear color: metals take their color from the base color.
    write_glb(OUTPUT / "gold_ball.glb", *sphere(), (1.0, 0.71, 0.29, 1), metallic=1.0, roughness=0.3)


def make_glass_pane():
    # Clear glass: blended, nearly see-through, and smooth, so it shows the sun's highlight.
    write_glb(
        OUTPUT / "glass_pane.glb",
        *box((0.6, 0.8, 0.02)),
        (0.55, 0.75, 0.85, 0.25),
        roughness=0.05,
        alpha_mode="BLEND",
    )


def fence_pixel(x, y):
    """Wooden slats in a lattice, with see-through holes between them: alpha 0 there."""
    bar = x % 32 < 8 or y % 32 < 8
    if not bar:
        return (0, 0, 0, 0)
    grain = 14 if (x * 5 + y * 3) % 19 < 5 else 0
    return (150 - grain, 108 - grain, 66 - grain, 255)


def make_fence():
    # A lattice cut out of a thin board by its texture's alpha, seen from both sides.
    write_glb(
        OUTPUT / "fence.glb",
        *box((0.9, 0.6, 0.02), texture_size=0.6),
        (1, 1, 1, 1),
        ("fence.png", png_bytes(128, 128, fence_pixel)),
        roughness=0.8,
        alpha_mode="MASK",
        double_sided=True,
    )


def make_lamp():
    # A glowing ball: its emission is brighter than white, which HDR keeps and tone mapping
    # rolls off.
    write_glb(
        OUTPUT / "lamp.glb",
        *sphere(),
        (0.1, 0.06, 0.02, 1),
        roughness=0.4,
        emissive=(1.0, 0.4, 0.08),
        emissive_strength=2.0,
    )


TENTACLE_JOINTS = 5
TENTACLE_SEGMENT = 0.4  # Meters between joints.
TENTACLE_RINGS = 24
TENTACLE_SIDES = 12


def quat_axis_angle(axis, angle):
    s = math.sin(angle / 2)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2))


def make_tentacle():
    """A tapered tube standing on the ground, skinned to a chain of joints up its middle, with two
    looping animations: "sway", a wave running up it, and "curl", bending over and back."""
    height = TENTACLE_SEGMENT * (TENTACLE_JOINTS - 1)
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    for ring in range(TENTACLE_RINGS + 1):
        v = ring / TENTACLE_RINGS
        y = v * height
        radius = 0.28 * (1 - v) + 0.06
        # Each ring follows the two joints around it, by how near it is to each.
        along = y / TENTACLE_SEGMENT
        lower = min(int(along), TENTACLE_JOINTS - 2)
        upper_weight = min(max(along - lower, 0.0), 1.0)
        for side in range(TENTACLE_SIDES + 1):
            u = side / TENTACLE_SIDES
            angle = u * 2 * math.pi
            nx, nz = math.sin(angle), math.cos(angle)
            positions.append((nx * radius, y, nz * radius))
            normals.append((nx, 0.0, nz))
            uvs.append((u, 1 - v))
            joints.append((lower, lower + 1, 0, 0))
            weights.append((1 - upper_weight, upper_weight, 0.0, 0.0))
    row = TENTACLE_SIDES + 1
    for ring in range(TENTACLE_RINGS):
        for side in range(TENTACLE_SIDES):
            a = ring * row + side
            b = a + row
            indices += [a, a + 1, b + 1, a, b + 1, b]
    # A cap on top, so it reads as solid from above.
    top = len(positions)
    positions.append((0.0, height, 0.0))
    normals.append((0.0, 1.0, 0.0))
    uvs.append((0.5, 0.0))
    joints.append((TENTACLE_JOINTS - 1, 0, 0, 0))
    weights.append((1.0, 0.0, 0.0, 0.0))
    last = TENTACLE_RINGS * row
    for side in range(TENTACLE_SIDES):
        indices += [last + side, last + side + 1, top]

    views, accessors, binary = [], [], bytearray()

    def add(data, target=None):
        while len(binary) % 4:
            binary.append(0)
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        binary.extend(data)
        views.append(view)
        return len(views) - 1

    def accessor(data, count, kind, component, target=None, extra=None):
        entry = {"bufferView": add(data, target), "componentType": component, "count": count,
                 "type": kind}
        entry.update(extra or {})
        accessors.append(entry)
        return len(accessors) - 1

    def floats(values, kind, target=None, extra=None):
        flat = [c for value in values for c in value]
        return accessor(struct.pack(f"<{len(flat)}f", *flat), len(values), kind, 5126, target,
                        extra)

    bounds = {"min": [min(p[i] for p in positions) for i in range(3)],
              "max": [max(p[i] for p in positions) for i in range(3)]}
    position_accessor = floats(positions, "VEC3", 34962, bounds)
    normal_accessor = floats(normals, "VEC3", 34962)
    uv_accessor = floats(uvs, "VEC2", 34962)
    flat_joints = [c for value in joints for c in value]
    joints_accessor = accessor(struct.pack(f"<{len(flat_joints)}B", *flat_joints), len(joints),
                               "VEC4", 5121, 34962)
    weights_accessor = floats(weights, "VEC4", 34962)
    index_accessor = accessor(struct.pack(f"<{len(indices)}H", *indices), len(indices), "SCALAR",
                              5123, 34963)
    # Each joint at its height when bound; the inverse moves the mesh back down to it.
    inverse_bind = []
    for joint in range(TENTACLE_JOINTS):
        y = joint * TENTACLE_SEGMENT
        inverse_bind.append((1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -y, 0, 1))
    inverse_accessor = floats(inverse_bind, "MAT4")

    # Two looping animations, keyed every tenth of a second.
    def clip(name, duration, rotation_of):
        steps = int(round(duration * 10))
        times = [i / 10 for i in range(steps + 1)]
        time_accessor = floats([(t,) for t in times], "SCALAR", extra={"min": [0.0],
                                                                         "max": [times[-1]]})
        samplers, channels = [], []
        for joint in range(TENTACLE_JOINTS):
            keys = [rotation_of(joint, t / duration) for t in times]
            samplers.append({"input": time_accessor, "output": floats(keys, "VEC4"),
                             "interpolation": "LINEAR"})
            channels.append({"sampler": joint, "target": {"node": 1 + joint, "path": "rotation"}})
        return {"name": name, "samplers": samplers, "channels": channels}

    def sway(joint, phase):
        # A wave that runs up the tentacle, each joint a little behind the one below.
        angle = 0.32 * math.sin(2 * math.pi * phase - joint * 0.9) * (0.4 + joint * 0.2)
        return quat_axis_angle((0, 0, 1), angle)

    def curl(joint, phase):
        # Every joint bends the same way, so the whole tentacle curls over, then straightens.
        bend = 0.5 * (1 - math.cos(2 * math.pi * phase))
        return quat_axis_angle((1, 0, 0), bend * 0.45 * (0.3 + joint * 0.25))

    animations = [clip("sway", 2.0, sway), clip("curl", 3.0, curl)]

    nodes = [{"mesh": 0, "skin": 0, "name": "tentacle"}]
    for joint in range(TENTACLE_JOINTS):
        node = {"name": f"joint{joint}", "translation": [0.0, 0.0 if joint == 0 else
                                                         TENTACLE_SEGMENT, 0.0]}
        if joint + 1 < TENTACLE_JOINTS:
            node["children"] = [2 + joint]
        nodes.append(node)

    while len(binary) % 4:
        binary.append(0)
    gltf = {
        "asset": {"version": "2.0", "generator": "DreamEngine make_sample_models.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": nodes,
        "skins": [{"joints": list(range(1, 1 + TENTACLE_JOINTS)),
                   "inverseBindMatrices": inverse_accessor, "skeleton": 1}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": position_accessor, "NORMAL": normal_accessor,
                           "TEXCOORD_0": uv_accessor, "JOINTS_0": joints_accessor,
                           "WEIGHTS_0": weights_accessor},
            "indices": index_accessor, "material": 0}]}],
        "materials": [{"name": "tentacle", "pbrMetallicRoughness": {
            "baseColorFactor": [0.55, 0.12, 0.45, 1], "metallicFactor": 0.0,
            "roughnessFactor": 0.45}}],
        "animations": animations,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * ((4 - len(text) % 4) % 4)
    path = OUTPUT / "tentacle.glb"
    total = 12 + 8 + len(text) + 8 + len(binary)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, total)
        + struct.pack("<II", len(text), 0x4E4F534A)
        + text
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )
    print(f"{path.name}: {total} bytes, {len(positions)} vertices, {len(indices) // 3} triangles, "
          f"{TENTACLE_JOINTS} joints, {len(animations)} animations")


if __name__ == "__main__":
    OUTPUT.mkdir(parents=True, exist_ok=True)
    make_crate()
    make_block()
    make_ground()
    make_ball()
    make_gold_ball()
    make_brick_wall()
    make_glass_pane()
    make_fence()
    make_lamp()
    make_tentacle()
