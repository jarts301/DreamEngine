"""Writes the platformer sample's pixel art (see docs/guides/sprites.md).

- tiles.png: a 4 x 2 tileset of 16 x 16 tiles: grass, dirt, stone bricks, a wooden platform, a
  bush, a flower, and two halves of a cloud.
- hero.png: a 4 x 2 sprite sheet of 16 x 16 frames: two idle frames, four running frames, jumping
  and falling.
- coin.png: a 4 x 1 sprite sheet of 16 x 16 frames of a turning coin.

Run from the repository's root:

    python tools/make_sprite_art.py

It needs only Python's standard library, and gives the same files every run.
"""

import struct
import zlib
from pathlib import Path

OUTPUT = Path("samples/platformer/assets/sprites")
CLEAR = (0, 0, 0, 0)


class Image:
    def __init__(self, width: int, height: int):
        self.width, self.height = width, height
        self.pixels = [[CLEAR] * width for _ in range(height)]

    def put(self, x: int, y: int, color):
        if 0 <= x < self.width and 0 <= y < self.height:
            self.pixels[y][x] = color if len(color) == 4 else (*color, 255)

    def rect(self, x: int, y: int, w: int, h: int, color):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.put(i, j, color)

    def png(self) -> bytes:
        def chunk(kind: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + kind + data
                    + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

        raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in self.pixels)
        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def noise(x: int, y: int, seed: int) -> int:
    """A fixed pseudo-random number from 0 to 255 for a pixel."""
    n = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return (n ^ (n >> 16)) & 0xFF


GRASS = [(56, 168, 72), (88, 200, 88), (40, 128, 56)]
DIRT = [(150, 96, 56), (128, 80, 44), (170, 116, 70)]
STONE = [(128, 128, 140), (104, 104, 118), (150, 150, 160)]
WOOD = [(176, 120, 64), (140, 92, 48), (200, 148, 84)]


def dirt(img: Image, ox: int, oy: int, top: int = 0):
    for y in range(top, 16):
        for x in range(16):
            n = noise(x, y, 1)
            img.put(ox + x, oy + y, DIRT[0] if n < 170 else DIRT[1] if n < 230 else DIRT[2])
    # Pebbles.
    for px, py in ((3, 11), (11, 7), (8, 13)):
        if py >= top:
            img.rect(ox + px, oy + py, 2, 1, DIRT[2])


def tiles() -> Image:
    img = Image(64, 32)
    # 0: grass on dirt.
    dirt(img, 0, 0, 4)
    for x in range(16):
        depth = 4 + (1 if noise(x, 0, 2) > 140 else 0)
        for y in range(depth):
            img.put(x, y, GRASS[0] if y > 0 else GRASS[1])
        if noise(x, 1, 3) > 200:
            img.put(x, depth, GRASS[2])
    # 1: dirt.
    dirt(img, 16, 0)
    # 2: stone bricks.
    for y in range(16):
        for x in range(16):
            row = y // 4
            shift = 4 if row % 2 else 0
            mortar = y % 4 == 3 or (x + shift) % 8 == 7
            n = noise(x, y, 4)
            img.put(32 + x, y, STONE[1] if mortar else STONE[0] if n < 200 else STONE[2])
    # 3: a wooden platform, the top half of the tile.
    for y in range(7):
        for x in range(16):
            edge = y == 0 or y == 6
            plank = x % 8 == 0
            img.put(48 + x, y, WOOD[1] if edge or plank else WOOD[0] if noise(x, y, 5) < 210
                    else WOOD[2])
    for x in (3, 12):
        img.put(48 + x, 3, (90, 60, 30))
    # 4: a bush.
    for y in range(16):
        for x in range(16):
            dx, dy = x - 7.5, y - 11
            if dx * dx / 64 + dy * dy / 30 <= 1.0:
                n = noise(x, y, 6)
                img.put(x, 16 + y, GRASS[2] if n < 90 else GRASS[0] if n < 220 else GRASS[1])
    # 5: a flower.
    img.rect(16 + 7, 16 + 9, 1, 7, GRASS[2])
    img.rect(16 + 5, 16 + 12, 2, 1, GRASS[0])
    for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
        img.rect(16 + 6 + dx * 2, 16 + 5 + dy * 2, 3, 3, (232, 88, 120))
    img.rect(16 + 6, 16 + 5, 3, 3, (250, 220, 80))
    # 6 and 7: a cloud, in two halves.
    for y in range(16):
        for x in range(32):
            inside = False
            for cx, cy, r in ((8, 10, 6), (16, 7, 7), (24, 10, 6)):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    inside = True
            if inside and y <= 14:
                shade = (236, 244, 252) if y < 11 else (206, 218, 236)
                img.put(32 + x, 16 + y, shade)
    return img


SKIN = (244, 196, 150)
HAIR = (90, 52, 30)
SHIRT = (214, 58, 52)
PANTS = (52, 84, 172)
SHOE = (40, 34, 40)
EYE = (30, 30, 40)


def hero_frame(img: Image, ox: int, oy: int, legs: str, arms: str, bob: int):
    """One frame of a small character facing right, with its feet at the bottom of the frame."""
    top = 2 + bob
    # Head and hair.
    img.rect(ox + 5, oy + top, 6, 5, SKIN)
    img.rect(ox + 5, oy + top, 6, 2, HAIR)
    img.rect(ox + 4, oy + top + 1, 1, 3, HAIR)
    img.put(ox + 9, oy + top + 3, EYE)
    # Body.
    img.rect(ox + 5, oy + top + 5, 6, 4, SHIRT)
    # Arms.
    if arms == "down":
        img.rect(ox + 4, oy + top + 5, 1, 4, SHIRT)
        img.rect(ox + 11, oy + top + 5, 1, 4, SHIRT)
        img.put(ox + 4, oy + top + 9, SKIN)
        img.put(ox + 11, oy + top + 9, SKIN)
    elif arms == "swing_a":
        img.rect(ox + 3, oy + top + 6, 2, 1, SHIRT)
        img.put(ox + 2, oy + top + 6, SKIN)
        img.rect(ox + 11, oy + top + 5, 1, 3, SHIRT)
        img.put(ox + 12, oy + top + 8, SKIN)
    elif arms == "swing_b":
        img.rect(ox + 4, oy + top + 5, 1, 3, SHIRT)
        img.put(ox + 4, oy + top + 8, SKIN)
        img.rect(ox + 11, oy + top + 6, 2, 1, SHIRT)
        img.put(ox + 13, oy + top + 6, SKIN)
    else:  # up
        img.rect(ox + 4, oy + top + 2, 1, 4, SHIRT)
        img.rect(ox + 11, oy + top + 2, 1, 4, SHIRT)
        img.put(ox + 4, oy + top + 1, SKIN)
        img.put(ox + 11, oy + top + 1, SKIN)
    # Legs: pants down to the shoes at the bottom row.
    hip = oy + top + 9
    feet = oy + 15
    if legs == "stand":
        for x in (6, 9):
            img.rect(ox + x, hip, 2, feet - hip, PANTS)
            img.rect(ox + x, feet, 2 + (x == 9), 1, SHOE)
    elif legs == "stride":
        img.rect(ox + 5, hip, 2, feet - hip - 1, PANTS)
        img.rect(ox + 4, feet - 1, 2, 1, PANTS)
        img.rect(ox + 3, feet, 3, 1, SHOE)
        img.rect(ox + 9, hip, 2, feet - hip - 1, PANTS)
        img.rect(ox + 10, feet - 1, 2, 1, PANTS)
        img.rect(ox + 10, feet, 3, 1, SHOE)
    elif legs == "pass":
        img.rect(ox + 7, hip, 2, feet - hip, PANTS)
        img.rect(ox + 7, feet, 3, 1, SHOE)
        img.rect(ox + 5, hip + 1, 2, 2, PANTS)
        img.rect(ox + 4, hip + 3, 2, 1, SHOE)
    else:  # tuck
        img.rect(ox + 5, hip, 2, 3, PANTS)
        img.rect(ox + 9, hip, 2, 2, PANTS)
        img.rect(ox + 4, hip + 3, 3, 1, SHOE)
        img.rect(ox + 10, hip + 2, 3, 1, SHOE)


def hero() -> Image:
    img = Image(64, 32)
    frames = [
        ("stand", "down", 0), ("stand", "down", 1),                       # idle
        ("stride", "swing_a", 0), ("pass", "down", 1),                    # run
        ("stride", "swing_b", 0), ("pass", "down", 1),
        ("tuck", "up", 0), ("stand", "up", 0),                            # jump, fall
    ]
    for index, (legs, arms, bob) in enumerate(frames):
        hero_frame(img, (index % 4) * 16, (index // 4) * 16, legs, arms, bob)
    return img


def coin() -> Image:
    img = Image(64, 16)
    gold, light, dark = (248, 200, 56), (255, 240, 150), (196, 140, 24)
    for index, half_width in enumerate((5, 3, 1, 3)):
        ox = index * 16
        for y in range(3, 13):
            dy = (y - 7.5) / 5.0
            w = half_width * (1 - dy * dy) ** 0.5
            for x in range(16):
                dx = x - 7.5
                if abs(dx) <= w + 0.5:
                    edge = abs(dx) > w - 0.8
                    img.put(ox + x, y, dark if edge else light if dx < -w / 3 else gold)
    return img


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, image in (("tiles.png", tiles()), ("hero.png", hero()), ("coin.png", coin())):
        (OUTPUT / name).write_bytes(image.png())
        print(f"Wrote {OUTPUT / name}")


if __name__ == "__main__":
    main()
