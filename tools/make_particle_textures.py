"""Writes the render demo's particle textures (see docs/guides/particles.md).

smoke.png is a soft, lumpy white puff on a clear background: the particle's color tints it, and
its alpha shapes it. Run from the repository's root:

    python tools/make_particle_textures.py

It needs only Python's standard library, and gives the same file every run.
"""

import math
import random
import struct
import zlib
from pathlib import Path

SIZE = 64
OUTPUT = Path("samples/assets_3d/textures/smoke.png")


def value_noise(cells: int, seed: int):
    """A smooth random field over [0, 1)^2 that wraps, with `cells` random values across."""
    rng = random.Random(seed)
    grid = [[rng.random() for _ in range(cells)] for _ in range(cells)]

    def sample(x: float, y: float) -> float:
        fx, fy = x * cells, y * cells
        x0, y0 = int(fx) % cells, int(fy) % cells
        x1, y1 = (x0 + 1) % cells, (y0 + 1) % cells
        tx, ty = fx - int(fx), fy - int(fy)
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        top = grid[y0][x0] * (1 - tx) + grid[y0][x1] * tx
        bottom = grid[y1][x0] * (1 - tx) + grid[y1][x1] * tx
        return top * (1 - ty) + bottom * ty

    return sample


def png(width: int, height: int, rows: list[bytes]) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + row for row in rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def smoke() -> bytes:
    octaves = [(value_noise(4, 1), 0.55), (value_noise(8, 2), 0.3), (value_noise(16, 3), 0.15)]
    rows = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
            distance = math.hypot(u - 0.5, v - 0.5) * 2.0
            falloff = max(0.0, 1.0 - distance)
            falloff = falloff * falloff * (3 - 2 * falloff)
            lumps = sum(noise(u, v) * weight for noise, weight in octaves)
            alpha = max(0.0, min(1.0, falloff * (0.35 + 0.9 * lumps)))
            shade = int(round(255 * (0.85 + 0.15 * lumps)))
            row += bytes((min(shade, 255), min(shade, 255), min(shade, 255),
                          int(round(alpha * 255))))
        rows.append(bytes(row))
    return png(SIZE, SIZE, rows)


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(smoke())
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
