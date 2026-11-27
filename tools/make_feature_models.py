#!/usr/bin/env python3
"""Generates the models of the feature samples, as .glb files, with the writer of
make_sample_models.py:

- samples/assets_3d, for the materials and environment samples: spheres of plastic and of metal,
  from mirror-smooth to fully rough, one model each, since a material belongs to a model (see
  docs/guides/materials.md); and lamps glowing blue and pink, beside the shared orange one.
- samples/forest: a tree's trunk and a lumpy canopy, and a rock, with enough triangles that the
  levels of detail made at build time are worth it (see docs/adr/0032).

Usage: python tools/make_feature_models.py

Needs only Python's standard library, and gives the same files every run. The results are
committed, so this runs only to change them.
"""

import math
from pathlib import Path

from make_sample_models import sphere, write_glb

SAMPLES = Path(__file__).resolve().parent.parent / "samples"
ROUGHNESS = [0.05, 0.25, 0.5, 0.75, 1.0]


def make_material_spheres():
    output = SAMPLES / "assets_3d" / "models"
    output.mkdir(parents=True, exist_ok=True)
    for index, roughness in enumerate(ROUGHNESS):
        # A white plastic, which the scene tints, and silver, whose color is its reflectance.
        write_glb(output / f"plastic_{index}.glb", *sphere(), (1, 1, 1, 1), roughness=roughness)
        write_glb(output / f"metal_{index}.glb", *sphere(), (0.95, 0.93, 0.88, 1), metallic=1.0,
                  roughness=roughness)
    # Emission brighter than white, as the shared lamp's, which bloom spreads.
    for name, color in (("lamp_blue", (0.25, 0.55, 1.0)), ("lamp_pink", (1.0, 0.25, 0.65))):
        write_glb(output / f"{name}.glb", *sphere(), (0.05, 0.05, 0.05, 1), roughness=0.4,
                  emissive=color, emissive_strength=2.5)


def lumpy_sphere(rows, columns, bumps, seed):
    """A sphere pushed in and out by a few smooth waves, so it isn't perfectly round: a canopy or a
    rock. Normals come from the shape, averaged over the triangles around each vertex."""
    positions, uvs, indices = [], [], []
    for row in range(rows + 1):
        theta = math.pi * row / rows
        for column in range(columns + 1):
            phi = 2 * math.pi * (column % columns) / columns
            x, y, z = math.sin(theta) * math.sin(phi), math.cos(theta), math.sin(theta) * math.cos(phi)
            radius = 1.0 + bumps * (math.sin(3 * phi + seed) * math.sin(2 * theta + seed * 0.7)
                                    + 0.5 * math.sin(7 * phi - seed) * math.sin(5 * theta))
            positions.append((x * radius, y * radius, z * radius))
            uvs.append((column / columns, row / rows))
    for row in range(rows):
        for column in range(columns):
            a = row * (columns + 1) + column
            b = a + columns + 1
            indices += [a, b, b + 1, a, b + 1, a + 1]
    normals = [[0.0, 0.0, 0.0] for _ in positions]
    for i in range(0, len(indices), 3):
        a, b, c = (positions[indices[i + k]] for k in range(3))
        u = [b[k] - a[k] for k in range(3)]
        v = [c[k] - a[k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for k in range(3):
            for axis in range(3):
                normals[indices[i + k]][axis] += n[axis]
    # The seam's two columns, and each pole's ring, share a normal so they shade alike.
    for row in range(rows + 1):
        first, last = row * (columns + 1), row * (columns + 1) + columns
        total = [normals[first][k] + normals[last][k] for k in range(3)]
        normals[first], normals[last] = list(total), list(total)
    for row in (0, rows):
        ring = range(row * (columns + 1), row * (columns + 1) + columns + 1)
        total = [sum(normals[i][k] for i in ring) for k in range(3)]
        for i in ring:
            normals[i] = list(total)
    unit = []
    for n in normals:
        length = math.sqrt(sum(c * c for c in n)) or 1.0
        unit.append((n[0] / length, n[1] / length, n[2] / length))
    return positions, unit, uvs, indices


def cylinder(radius_bottom, radius_top, height, sides):
    """An open tube standing on the origin, as a trunk, narrowing toward the top."""
    positions, normals, uvs, indices = [], [], [], []
    slope = (radius_bottom - radius_top) / height
    for ring, (y, radius) in enumerate(((0.0, radius_bottom), (height, radius_top))):
        for side in range(sides + 1):
            angle = 2 * math.pi * (side % sides) / sides
            positions.append((math.sin(angle) * radius, y, math.cos(angle) * radius))
            length = math.sqrt(1 + slope * slope)
            normals.append((math.sin(angle) / length, slope / length, math.cos(angle) / length))
            uvs.append((side / sides, 1.0 - ring))
    for side in range(sides):
        a, b = side, side + sides + 1
        indices += [a, a + 1, b + 1, a, b + 1, b]
    return positions, normals, uvs, indices


def make_forest():
    output = SAMPLES / "forest" / "assets" / "models"
    output.mkdir(parents=True, exist_ok=True)
    write_glb(output / "trunk.glb", *cylinder(0.18, 0.1, 2.0, 12), (0.30, 0.19, 0.11, 1),
              roughness=0.9)
    write_glb(output / "canopy.glb", *lumpy_sphere(32, 48, 0.12, 1.3), (0.13, 0.32, 0.10, 1),
              roughness=0.8)
    write_glb(output / "rock.glb", *lumpy_sphere(20, 32, 0.2, 4.1), (0.42, 0.41, 0.38, 1),
              roughness=0.85)


if __name__ == "__main__":
    make_material_spheres()
    make_forest()
