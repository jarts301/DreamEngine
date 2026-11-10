#!/usr/bin/env python3
"""Generates the original models shared by the samples, in samples/assets/models, as .glb files.

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


def write_glb(path, positions, normals, uvs, indices, base_color, image=None):
    """Writes one mesh with one material, and optionally one embedded PNG."""
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

    material = {"pbrMetallicRoughness": {"baseColorFactor": list(base_color)}, "name": path.stem}
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

    if image is not None:
        gltf["images"] = [{"bufferView": add(image), "mimeType": "image/png"}]
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
        gltf["textures"] = [{"source": 0, "sampler": 0}]
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}

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


def make_crate():
    faces = [
        ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
        ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
        ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
        ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    ]
    corners = [(-1, -1, (0, 1)), (1, -1, (1, 1)), (1, 1, (1, 0)), (-1, 1, (0, 0))]
    positions, normals, uvs, indices = [], [], [], []
    for normal, u, v in faces:
        first = len(positions)
        for su, sv, uv in corners:
            positions.append(tuple(normal[i] + su * u[i] + sv * v[i] for i in range(3)))
            normals.append(normal)
            uvs.append(uv)
        indices += [first, first + 1, first + 2, first, first + 2, first + 3]
    write_glb(
        OUTPUT / "crate.glb",
        positions,
        normals,
        uvs,
        indices,
        (1, 1, 1, 1),
        png_bytes(64, 64, wood_pixel),
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
        png_bytes(64, 64, checker_pixel),
    )


def make_ball():
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
    # White, so scenes can give each ball any color with a tint.
    write_glb(OUTPUT / "ball.glb", positions, normals, uvs, indices, (1, 1, 1, 1))


if __name__ == "__main__":
    OUTPUT.mkdir(parents=True, exist_ok=True)
    make_crate()
    make_ground()
    make_ball()
