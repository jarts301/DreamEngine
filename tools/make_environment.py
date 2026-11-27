"""Writes the render demo's environment: an HDR sky in a Radiance .hdr file (see
docs/guides/environment.md).

The sky is equirectangular, longitude across and latitude down with +Y up, as the engine reads it:
a blue sky brightening toward a hazy horizon, soft clouds, a warm glow around the sun, and darker
ground below. The sun sits where the yard's directional light comes from, so reflections and the
sunlight agree. Run from the repository's root:

    python tools/make_environment.py

It needs only Python's standard library, and gives the same file every run.
"""

import math
from pathlib import Path

OUTPUT = Path("samples/assets_3d/environments/sky.hdr")
WIDTH, HEIGHT = 512, 256

# Toward the sun: the yard's light travels along (-0.5, -1.0, -0.6).
SUN = (0.5, 1.0, 0.6)
_length = math.sqrt(sum(c * c for c in SUN))
SUN = tuple(c / _length for c in SUN)


def value_noise(x: float, y: float, seed: int) -> float:
    def hashed(i: int, j: int) -> float:
        n = (i * 374761393 + j * 668265263 + seed * 2246822519) & 0xFFFFFFFF
        n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
        return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0

    xi, yi = math.floor(x), math.floor(y)
    tx, ty = x - xi, y - yi
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    top = hashed(xi, yi) * (1 - tx) + hashed(xi + 1, yi) * tx
    bottom = hashed(xi, yi + 1) * (1 - tx) + hashed(xi + 1, yi + 1) * tx
    return top * (1 - ty) + bottom * ty


def sky(direction) -> tuple:
    x, y, z = direction
    if y < 0.0:
        # Ground: grassland, dimmer away from the horizon.
        t = min(1.0, -y * 4.0)
        near = (0.30, 0.36, 0.20)
        far = (0.10, 0.13, 0.07)
        return tuple(a * (1 - t) + b * t for a, b in zip(near, far))
    zenith = (0.18, 0.34, 0.75)
    horizon = (0.75, 0.82, 0.92)
    t = (1.0 - y) ** 4
    color = [a * (1 - t) + b * t for a, b in zip(zenith, horizon)]
    # Clouds: soft noise, thinning toward the zenith.
    u = math.atan2(x, -z) / (2 * math.pi) + 0.5
    v = math.acos(max(-1.0, min(1.0, y))) / math.pi
    n = (value_noise(u * 24, v * 10, 1) * 0.6 + value_noise(u * 60, v * 24, 2) * 0.3 +
         value_noise(u * 140, v * 50, 3) * 0.1)
    cover = max(0.0, min(1.0, (n - 0.52) * 4.0)) * min(1.0, y * 6.0) * (1.0 - y * 0.5)
    color = [c * (1 - cover) + 0.95 * cover for c in color]
    # The sun: a bright disc with a warm glow around it.
    cosine = x * SUN[0] + y * SUN[1] + z * SUN[2]
    glow = max(0.0, cosine) ** 64 * 1.5
    disc = 40.0 if cosine > 0.9996 else 0.0
    color = [c + (glow + disc) * w for c, w in zip(color, (1.0, 0.9, 0.7))]
    return tuple(color)


def rgbe(color) -> bytes:
    peak = max(color)
    if peak < 1e-32:
        return bytes((0, 0, 0, 0))
    mantissa, exponent = math.frexp(peak)
    scale = mantissa * 256.0 / peak
    return bytes((min(255, int(color[0] * scale)), min(255, int(color[1] * scale)),
                  min(255, int(color[2] * scale)), exponent + 128))


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for row in range(HEIGHT):
        theta = (row + 0.5) / HEIGHT * math.pi
        line = bytearray()
        for column in range(WIDTH):
            phi = ((column + 0.5) / WIDTH - 0.5) * 2 * math.pi
            direction = (math.sin(theta) * math.sin(phi), math.cos(theta),
                         -math.sin(theta) * math.cos(phi))
            line += rgbe(sky(direction))
        rows.append(bytes(line))
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {HEIGHT} +X {WIDTH}\n".encode()
    OUTPUT.write_bytes(header + b"".join(rows))
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
