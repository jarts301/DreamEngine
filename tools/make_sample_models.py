#!/usr/bin/env python3
"""Generates the original models shared by the samples, in samples/assets/models, as .glb files.

Textures are PNG files of their own in models/textures, which the build cooks into KTX2 (see
docs/adr/0024-texture-cooking.md); the models refer to them by name. Every material states its
metallic and roughness factors, since glTF's defaults are fully metallic and fully rough (see
docs/guides/materials.md). The brick wall has a normal map and an occlusion-roughness-metallic
map, and no tangents, which the engine's tangent generator makes.

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
    """Encodes an image, where pixel(x, y) returns (r, g, b)."""

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
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
):
    """Writes one mesh with one material. Each image is (file name, PNG bytes), saved in the
    textures folder beside the model: `image` is the base color, `normal_image` a normal map, and
    `orm_image` occlusion in red, roughness in green and metallic in blue."""
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


if __name__ == "__main__":
    OUTPUT.mkdir(parents=True, exist_ok=True)
    make_crate()
    make_block()
    make_ground()
    make_ball()
    make_gold_ball()
    make_brick_wall()
