"""Task 01, Phase 1: measure how different "harmless" and "broken" pictures look to FLIP.

Needs NVIDIA's FLIP for Python, NumPy and Pillow (pip install flip-evaluator numpy pillow).
Run from the DreamEngine folder:
    ~/flip-venv/bin/python tools/analyse_references.py --maps ~/Desktop/flip-maps

For each saved reference picture it makes four changed copies, scores each copy against the
original with NVIDIA's FLIP, and prints a table. It also says whether today's rule (no more than
1% of pixels off by more than 24) would pass each copy.
"""
import argparse
import glob
import os

import flip_evaluator as flip
import numpy as np
from PIL import Image


def srgb_to_linear(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(v):
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def changed_copies(reference, rng):
    """Two changes a correct picture can have, and two that mean something is broken."""
    ref = reference.astype(np.int32)
    copies = {}
    noisy = ref.copy()                                   # every colour off by -1, 0 or +1
    noisy[..., :3] += rng.integers(-1, 2, size=noisy[..., :3].shape)
    copies["±1 everywhere (allow)"] = noisy
    sparse = ref.copy()                                  # 1% of pixels off by 24
    picked = rng.random(ref.shape[:2]) < 0.01
    sparse[..., :3][picked] += rng.choice([-24, 24], size=(int(picked.sum()), 3))
    copies["±24 on 1% (allow)"] = sparse
    shifted = ref.copy()                                 # the picture moved 1 pixel right
    shifted[:, 1:] = ref[:, :-1]
    copies["1-px shift (catch)"] = shifted
    brighter = ref.astype(np.float64)                    # 2% more light
    brighter[..., :3] = np.round(
        linear_to_srgb(np.clip(srgb_to_linear(ref[..., :3] / 255.0) * 1.02, 0, 1)) * 255)
    copies["+2% exposure (catch)"] = brighter
    return {name: np.clip(c, 0, 255).astype(np.uint8) for name, c in copies.items()}


def flip_scores(changed, reference):
    """Mean and 99th percentile of the FLIP map, plus the map itself in colour."""
    ref = (reference[..., :3] / 255.0).astype(np.float32)
    test = (changed[..., :3] / 255.0).astype(np.float32)
    error_map, mean, _ = flip.evaluate(ref, test, "LDR", applyMagma=False,
                                       parameters={"ppd": 67.0206})
    values = np.sort(np.squeeze(error_map).ravel())
    p99 = values[int(0.99 * values.size)]
    coloured, _, _ = flip.evaluate(ref, test, "LDR", applyMagma=True,
                                   parameters={"ppd": 67.0206}, computeMeanError=False)
    return mean, p99, coloured


def old_rule_passes(changed, reference):
    worst = np.abs(changed[..., :3].astype(int) - reference[..., :3].astype(int)).max(axis=-1)
    return (worst > 24).mean() <= 0.01


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--maps", help="folder to save the colour error maps of the render demo")
    args = parser.parse_args()
    rng = np.random.default_rng(7)
    rows = []
    for path in sorted(glob.glob("samples/*/tests/reference/screenshots/*.png")):
        reference = np.asarray(Image.open(path).convert("RGBA"))
        name = os.path.basename(path)
        cells = {}
        for change, copy in changed_copies(reference, rng).items():
            mean, p99, coloured = flip_scores(copy, reference)
            cells[change] = (mean, p99, old_rule_passes(copy, reference))
            if args.maps and name == "render_demo.png":
                os.makedirs(args.maps, exist_ok=True)
                stem = {"±1 everywhere (allow)": "1_noise_everywhere", "±24 on 1% (allow)": "2_noise_on_1pct",
                        "1-px shift (catch)": "3_shift_1px", "+2% exposure (catch)": "4_exposure_2pct"}[change]
                Image.fromarray(copy[..., :3]).save(os.path.join(args.maps, f"{stem}_picture.png"))
                Image.fromarray((np.squeeze(coloured) * 255).astype(np.uint8)).save(
                    os.path.join(args.maps, f"{stem}_flip_map.png"))
        rows.append((name, cells))

    changes = list(rows[0][1])
    print("FLIP score of each changed copy: mean / 99th percentile\n")
    print(f"{'reference':20s}" + "".join(f"{c:>24s}" for c in changes))
    for name, cells in rows:
        print(f"{name:20s}" + "".join(f"{cells[c][0]:>14.4f} / {cells[c][1]:.3f}" for c in changes))
    print("\nWould today's rule (at most 1% of pixels off by more than 24) pass the copy?\n")
    print(f"{'reference':20s}" + "".join(f"{c:>24s}" for c in changes))
    for name, cells in rows:
        print(f"{name:20s}" + "".join(f"{'passes' if cells[c][2] else 'FAILS':>24s}" for c in changes))

    def most(index, word):
        return max(cells[c][index] for _, cells in rows for c in changes if word in c)

    def least(index, word):
        return min(cells[c][index] for _, cells in rows for c in changes if word in c)

    print("\nWhere to draw the lines:")
    print(f"  harmless changes:  mean at most {most(0, 'allow'):.4f}, "
          f"99th percentile at most {most(1, 'allow'):.4f}")
    print(f"  1-pixel shifts:    99th percentile at least {least(1, 'shift'):.4f} "
          f"(mean as low as {least(0, 'shift'):.4f})")
    print(f"  +2% exposure:      mean at least {least(0, 'exposure'):.4f} "
          f"(99th percentile as low as {least(1, 'exposure'):.4f})")


if __name__ == "__main__":
    main()
