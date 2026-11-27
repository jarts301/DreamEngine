"""Writes the top-down sample's pixel art and its Tiled map (see docs/guides/sprites.md).

- sprites/terrain.png: an 8 x 2 tileset of 16 x 16 tiles: grass, grass with flowers, a path, two
  frames of water, a stone wall, a hedge, and a flower bed.
- sprites/props.png: a 4 x 1 tileset of 16 x 32 tiles: an oak, a pine, a standing stone and a lamp.
- sprites/gardener.png: a 4 x 2 sprite sheet of 16 x 16 frames: walking down, up, left and right,
  two frames each.
- maps/garden.tmj and maps/props.tsj: the garden as Tiled 1.10 saves it in JSON, with the terrain
  tileset in the map and the props tileset in a file of its own. Open garden.tmj in Tiled to change
  it; this script only makes the first version.

Run from the repository's root:

    python tools/make_top_down_art.py

It needs only Python's standard library, and gives the same files every run.
"""

import json
from pathlib import Path

from make_sprite_art import Image, noise

OUTPUT = Path("samples/top_down/assets")

GRASS = [(96, 168, 80), (84, 152, 72), (112, 184, 92)]
PATH = [(204, 176, 128), (186, 158, 112), (220, 196, 150)]
WATER = [(64, 124, 200), (92, 152, 220), (176, 216, 244)]
STONE = [(140, 140, 150), (112, 112, 124), (164, 164, 172)]
HEDGE = [(48, 112, 56), (36, 92, 44), (68, 136, 72)]
BARK = [(110, 76, 48), (88, 60, 38)]
LEAVES = [(60, 140, 64), (44, 116, 52), (88, 168, 84)]


def grass(img: Image, ox: int, oy: int, seed: int = 1):
    for y in range(16):
        for x in range(16):
            n = noise(ox + x, oy + y, seed)
            img.put(ox + x, oy + y, GRASS[1] if n < 50 else GRASS[2] if n > 225 else GRASS[0])


def terrain() -> Image:
    img = Image(128, 32)
    # 0: grass.
    grass(img, 0, 0)
    # 1: grass with flowers.
    grass(img, 16, 0)
    for px, py, color in ((19, 4, (240, 240, 250)), (27, 9, (250, 214, 80)),
                          (22, 12, (240, 240, 250)), (29, 3, (232, 104, 136))):
        img.rect(px, py, 2, 2, color)
    # 2: a path.
    for y in range(16):
        for x in range(16):
            n = noise(x, y, 3)
            img.put(32 + x, y, PATH[1] if n < 60 else PATH[2] if n > 220 else PATH[0])
    # 3 and 4: water, its ripples moved along from one frame to the next.
    for frame in range(2):
        ox = 48 + frame * 16
        for y in range(16):
            for x in range(16):
                ripple = (x + y * 2 + frame * 5) % 12
                color = WATER[2] if ripple == 0 and y % 4 == 1 else (
                    WATER[1] if ripple < 3 else WATER[0])
                img.put(ox + x, y, color)
    # 5: a stone wall.
    for y in range(16):
        for x in range(16):
            row = y // 5
            shift = 4 if row % 2 else 0
            mortar = y % 5 == 4 or (x + shift) % 8 == 7
            n = noise(x, y, 5)
            img.put(80 + x, y, STONE[1] if mortar else STONE[2] if n > 210 else STONE[0])
    # 6: a hedge.
    for y in range(16):
        for x in range(16):
            n = noise(x, y, 6)
            shade = HEDGE[2] if y < 3 or n > 215 else HEDGE[1] if n < 60 or y > 13 else HEDGE[0]
            img.put(96 + x, y, shade)
    # 7: a flower bed.
    for y in range(16):
        for x in range(16):
            img.put(112 + x, y, (112, 80, 56) if noise(x, y, 7) < 200 else (96, 68, 46))
    for px, py in ((2, 2), (8, 3), (13, 2), (4, 8), (10, 9), (2, 13), (8, 13), (13, 12)):
        img.rect(112 + px, py, 2, 2, (232, 88, 120) if (px + py) % 3 else (250, 214, 80))
        img.put(112 + px, py + 2, GRASS[1])
    # Row 2, 8 to 15: left empty, for more terrain.
    return img


def canopy(img: Image, ox: int, top: int, bottom: int, half: int, pine: bool):
    for y in range(top, bottom):
        for x in range(16):
            dx = x - 7.5
            if pine:
                width = (y - top + 1) * half / (bottom - top)
                inside = abs(dx) <= width
            else:
                dy = (y - (top + bottom) / 2) / ((bottom - top) / 2)
                inside = dx * dx / (half * half) + dy * dy <= 1.0
            if inside:
                n = noise(ox + x, y, 8)
                img.put(ox + x, y, LEAVES[2] if n > 200 or dx < -half / 2 else
                        LEAVES[1] if n < 70 or dx > half / 2 else LEAVES[0])


def props() -> Image:
    img = Image(64, 32)
    # 0: an oak: a trunk and a round crown.
    img.rect(6, 18, 4, 13, BARK[0])
    img.rect(9, 18, 1, 13, BARK[1])
    img.rect(4, 30, 8, 2, (60, 104, 52))
    canopy(img, 0, 1, 22, 7, False)
    # 1: a pine.
    img.rect(16 + 7, 24, 2, 7, BARK[0])
    img.rect(16 + 5, 30, 6, 2, (60, 104, 52))
    canopy(img, 16, 2, 26, 7, True)
    # 2: a standing stone.
    for y in range(12, 31):
        for x in range(4, 12):
            if (y < 15 and (x < 5 or x > 10)) or (y < 13 and (x < 6 or x > 9)):
                continue
            n = noise(x, y, 9)
            img.put(32 + x, y, STONE[2] if x < 6 else STONE[1] if x > 9 or n < 50 else STONE[0])
    img.rect(32 + 3, 30, 10, 2, (60, 104, 52))
    # 3: a lamp post.
    img.rect(48 + 7, 9, 2, 22, (60, 60, 70))
    img.rect(48 + 5, 30, 6, 2, (60, 60, 70))
    img.rect(48 + 5, 3, 6, 6, (60, 60, 70))
    img.rect(48 + 6, 4, 4, 4, (255, 226, 140))
    return img


SKIN = (244, 196, 150)
HAIR = (120, 72, 40)
SHIRT = (64, 120, 196)
OVERALLS = (88, 132, 64)
BOOT = (70, 50, 40)


def gardener() -> Image:
    img = Image(64, 32)
    for index in range(8):
        ox, oy = (index % 4) * 16, (index // 4) * 16
        facing = index // 2       # 0 down, 1 up, 2 left, 3 right.
        step = index % 2
        # A straw hat.
        img.rect(ox + 4, oy + 1, 8, 2, (232, 200, 112))
        img.rect(ox + 3, oy + 3, 10, 1, (204, 172, 88))
        # The head.
        img.rect(ox + 5, oy + 4, 6, 4, HAIR if facing == 1 else SKIN)
        if facing == 0:
            img.put(ox + 6, oy + 5, (30, 30, 40))
            img.put(ox + 9, oy + 5, (30, 30, 40))
        elif facing == 2:
            img.put(ox + 6, oy + 5, (30, 30, 40))
            img.rect(ox + 9, oy + 4, 2, 3, HAIR)
        elif facing == 3:
            img.put(ox + 9, oy + 5, (30, 30, 40))
            img.rect(ox + 5, oy + 4, 2, 3, HAIR)
        # The body, arms swinging with the step.
        img.rect(ox + 4, oy + 8, 8, 4, SHIRT)
        img.rect(ox + 5, oy + 9, 6, 3, OVERALLS)
        arm = 1 if step else 0
        img.rect(ox + 3, oy + 8 + arm, 1, 3, SKIN)
        img.rect(ox + 12, oy + 9 - arm, 1, 3, SKIN)
        # Legs, one forward in the second frame.
        left_leg, right_leg = (13, 13) if step == 0 else (12, 14)
        img.rect(ox + 5, oy + 12, 2, left_leg - 12 + 1, OVERALLS)
        img.rect(ox + 9, oy + 12, 2, right_leg - 12 + 1, OVERALLS)
        img.rect(ox + 5, oy + left_leg + 1, 2, 1, BOOT)
        img.rect(ox + 9, oy + right_leg + 1, 2, 1, BOOT)
    return img


COLUMNS, ROWS = 24, 16


def garden_tiles():
    """The ground and the walls, as tile numbers within the terrain tileset, -1 for none."""
    G, F, P, W, S, H, B = 0, 1, 2, 3, 5, 6, 7
    ground = [[G] * COLUMNS for _ in range(ROWS)]
    walls = [[-1] * COLUMNS for _ in range(ROWS)]
    for y in range(ROWS):
        for x in range(COLUMNS):
            if noise(x, y, 11) > 225:
                ground[y][x] = F
    # A path from the gate on the left, across, and up to the pond.
    for x in range(0, COLUMNS):
        ground[9][x] = P
    for y in range(4, 10):
        ground[y][13] = P
    # The pond.
    for y in range(3, 7):
        for x in range(15, 21):
            ground[y][x] = W
    # Flower beds.
    for x in range(3, 7):
        ground[12][x] = B
        ground[13][x] = B
    # Hedges around the garden, with a gap for the gate, and a low stone wall by the pond.
    for x in range(COLUMNS):
        walls[0][x] = H
        walls[ROWS - 1][x] = H
    for y in range(ROWS):
        if y != 9:
            walls[y][0] = H
        walls[y][COLUMNS - 1] = H
    for x in range(3, 10):
        walls[4][x] = S
    return ground, walls


def tile_layer(layer_id, name, rows, first_tile, properties=None):
    layer = {
        "data": [cell + first_tile if cell >= 0 else 0 for row in rows for cell in row],
        "height": ROWS,
        "id": layer_id,
        "name": name,
        "opacity": 1,
        "type": "tilelayer",
        "visible": True,
        "width": COLUMNS,
        "x": 0,
        "y": 0,
    }
    if properties:
        layer["properties"] = properties
    return layer


def garden_map():
    ground, walls = garden_tiles()
    terrain_first, props_first = 1, 17
    objects = []
    next_id = 1

    def add(obj):
        nonlocal next_id
        obj = {"id": next_id, "name": "", "type": "", "rotation": 0, "visible": True, **obj}
        next_id += 1
        objects.append(obj)

    # Trees and stones, by their foot: x at their left, y at their bottom, in pixels.
    for name, tile, cx, cy in (("oak", 0, 5, 7), ("oak", 0, 9, 11), ("pine", 1, 2, 3),
                               ("pine", 1, 11, 2), ("oak", 0, 21, 12), ("pine", 1, 17, 11),
                               ("stone", 2, 14, 13), ("lamp", 3, 12, 9), ("pine", 1, 19, 14)):
        add({"name": name, "type": "tree" if name in ("oak", "pine") else name,
             "gid": props_first + tile, "x": cx * 16, "y": cy * 16, "width": 16, "height": 32})
    add({"name": "start", "type": "spawn", "point": True, "x": 9 * 16 + 8, "y": 150,
         "width": 0, "height": 0})
    add({"name": "pond", "type": "area", "x": 15 * 16, "y": 3 * 16, "width": 6 * 16,
         "height": 4 * 16, "properties": [{"name": "message", "type": "string",
                                           "value": "Fish swim in the pond"}]})
    return {
        "compressionlevel": -1,
        "height": ROWS,
        "infinite": False,
        "layers": [
            tile_layer(1, "ground", ground, terrain_first,
                       [{"name": "layer", "type": "int", "value": 0}]),
            tile_layer(2, "walls", walls, terrain_first,
                       [{"name": "layer", "type": "int", "value": 0},
                        {"name": "solid", "type": "bool", "value": True}]),
            {"draworder": "topdown", "id": 3, "name": "things", "objects": objects,
             "opacity": 1, "type": "objectgroup", "visible": True, "x": 0, "y": 0,
             "properties": [{"name": "layer", "type": "int", "value": 1}]},
        ],
        "nextlayerid": 4,
        "nextobjectid": next_id,
        "orientation": "orthogonal",
        "renderorder": "right-down",
        "tiledversion": "1.10.2",
        "tileheight": 16,
        "tilesets": [
            {
                "columns": 8, "firstgid": terrain_first, "image": "../sprites/terrain.png",
                "imageheight": 32, "imagewidth": 128, "margin": 0, "name": "terrain",
                "spacing": 0, "tilecount": 16, "tileheight": 16, "tilewidth": 16,
                "tiles": [
                    {"id": 3, "animation": [{"duration": 450, "tileid": 3},
                                            {"duration": 450, "tileid": 4}],
                     "properties": [{"name": "solid", "type": "bool", "value": True}]},
                ],
            },
            {"firstgid": props_first, "source": "props.tsj"},
        ],
        "tilewidth": 16,
        "type": "map",
        "version": "1.10",
        "width": COLUMNS,
    }


def props_tileset():
    return {
        "columns": 4, "image": "../sprites/props.png", "imageheight": 32, "imagewidth": 64,
        "margin": 0, "name": "props", "spacing": 0, "tilecount": 4, "tiledversion": "1.10.2",
        "tileheight": 32, "tilewidth": 16, "type": "tileset", "version": "1.10",
    }


def to_json(data) -> str:
    """JSON as Tiled writes it: indented, with each row of a layer's tiles on a line of its own."""
    rows = {}

    def mark(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "data" and isinstance(value, list):
                    marker = f"@data{len(rows)}@"
                    rows[marker] = value
                    node[key] = marker
                else:
                    mark(value)
        elif isinstance(node, list):
            for value in node:
                mark(value)

    mark(data)
    text = json.dumps(data, indent=1)
    for marker, cells in rows.items():
        lines = [", ".join(str(cell) for cell in cells[i:i + COLUMNS])
                 for i in range(0, len(cells), COLUMNS)]
        text = text.replace(f'"{marker}"', "[" + ",\n    ".join(lines) + "]")
    return text + "\n"


def main() -> None:
    for folder in ("sprites", "maps"):
        (OUTPUT / folder).mkdir(parents=True, exist_ok=True)
    for name, image in (("terrain.png", terrain()), ("props.png", props()),
                        ("gardener.png", gardener())):
        (OUTPUT / "sprites" / name).write_bytes(image.png())
        print(f"Wrote {OUTPUT / 'sprites' / name}")
    for name, data in (("garden.tmj", garden_map()), ("props.tsj", props_tileset())):
        (OUTPUT / "maps" / name).write_text(to_json(data), newline="\n")
        print(f"Wrote {OUTPUT / 'maps' / name}")


if __name__ == "__main__":
    main()
