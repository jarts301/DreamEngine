#!/usr/bin/env python3
"""Generates the original sounds shared by the samples, in samples/assets/sounds, as WAV files.

Usage: python tools/make_sample_sounds.py

Needs only Python's standard library. The results are committed, so this runs only to change them.
Every sound is synthesized here, so the samples carry no third-party audio.
"""

import math
import random
import struct
import wave
from pathlib import Path

OUTPUT = Path(__file__).resolve().parent.parent / "samples" / "assets" / "sounds"
RATE = 22050


def write(name, samples):
    path = OUTPUT / name
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        frames = bytearray()
        for sample in samples:
            frames += struct.pack("<h", int(max(-1.0, min(1.0, sample)) * 32000))
        file.writeframes(bytes(frames))
    print(f"{name}: {path.stat().st_size} bytes, {len(samples) / RATE:.2f} s")


def envelope(t, attack, decay):
    """Rises over `attack` seconds, then falls away with time constant `decay`."""
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def make_throw():
    # A short rising whoosh: filtered noise with a sweeping tone under it.
    rng = random.Random(7)
    samples, smoothed = [], 0.0
    count = int(0.25 * RATE)
    for i in range(count):
        t = i / RATE
        smoothed += (rng.uniform(-1, 1) - smoothed) * 0.15
        tone = math.sin(2 * math.pi * (300 + 900 * t / 0.25) * t)
        samples.append((smoothed * 0.8 + tone * 0.15) * envelope(t, 0.03, 0.08))
    write("throw.wav", samples)


def make_hit():
    # A wooden knock: two damped partials and a click of noise.
    rng = random.Random(3)
    samples = []
    for i in range(int(0.3 * RATE)):
        t = i / RATE
        body = math.sin(2 * math.pi * 180 * t) * 0.7 + math.sin(2 * math.pi * 410 * t) * 0.35
        click = rng.uniform(-1, 1) * math.exp(-t / 0.004)
        samples.append((body * math.exp(-t / 0.06) + click * 0.5) * 0.9)
    write("hit.wav", samples)


def make_click():
    # A soft interface blip.
    samples = []
    for i in range(int(0.08 * RATE)):
        t = i / RATE
        samples.append(math.sin(2 * math.pi * 1200 * t) * envelope(t, 0.002, 0.02) * 0.6)
    write("click.wav", samples)


def make_music():
    # Eight seconds that loop: an arpeggio over a slow bass, in A minor, at 120 beats per minute.
    beat = 0.5
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    length = int(8.0 * RATE)
    samples = [0.0] * length

    def note(start, duration, midi, volume, brightness):
        frequency = 440.0 * 2 ** ((midi - 69) / 12)
        first = int(start * RATE)
        for i in range(int(duration * RATE)):
            if first + i >= length:
                break
            t = i / RATE
            wave_value = math.sin(2 * math.pi * frequency * t)
            wave_value += brightness * math.sin(4 * math.pi * frequency * t)
            samples[first + i] += wave_value * envelope(t, 0.01, duration * 0.35) * volume

    for bar, chord in enumerate(chords):
        bar_start = bar * 4 * beat
        note(bar_start, 4 * beat, chord[0] - 12, 0.22, 0.1)
        for step in range(8):
            midi = chord[step % 3] + (12 if step >= 6 else 0)
            note(bar_start + step * beat / 2, beat * 0.9, midi, 0.12, 0.3)

    peak = max(abs(s) for s in samples)
    write("music.wav", [s / peak * 0.7 for s in samples])


def jingle(name, notes, step):
    # A few bright notes in a row, for winning or losing a level.
    samples = [0.0] * int((len(notes) * step + 0.6) * RATE)
    for index, midi in enumerate(notes):
        frequency = 440.0 * 2 ** ((midi - 69) / 12)
        first = int(index * step * RATE)
        for i in range(int(0.6 * RATE)):
            t = i / RATE
            tone = math.sin(2 * math.pi * frequency * t) + 0.3 * math.sin(4 * math.pi * frequency * t)
            samples[first + i] += tone * envelope(t, 0.005, 0.18) * 0.35
    write(name, samples)


def make_win():
    # Rising: C, E, G, then C an octave up.
    jingle("win.wav", [72, 76, 79, 84], 0.11)


def make_lose():
    # Falling: G, E flat, C, in a lower register.
    jingle("lose.wav", [67, 63, 60], 0.16)


if __name__ == "__main__":
    OUTPUT.mkdir(parents=True, exist_ok=True)
    make_throw()
    make_hit()
    make_click()
    make_music()
    make_win()
    make_lose()
