# Report: performance and heat on an older Android phone

## Summary

On a Samsung Galaxy A52s 5G, a 2021 mid-range phone, release builds of Toppler, the arcade and the
render demo all run at the screen's full 120 FPS. Eleven minutes of Toppler back to back changed
nothing: the frame times of the eighth run match the first, the CPU never slowed down for heat,
and memory stayed level.

| | Frame time mean / p99 / max (ms) | CPU work mean (ms) | GPU time mean / p99 (ms) | Resident memory (MB) |
|---|---|---|---|---|
| Toppler, all 10 levels | 8.29 / 12.0 / 27.7 | 0.52 | 1.42 / 2.58 | 86 – 101 |
| Arcade, scripted | 8.26 / 10.2 / 17.2 | 0.69 | 1.36 / 2.55 | 92 – 93 |
| Render demo, 7200 frames | 8.28 / 11.1 / 27.6 | 0.20 | 1.10 / 1.15 | 64 – 82 |

The phone could do far more work: the GPU is busy about 1.4 ms of each 8.3 ms frame, and the game's
own CPU work takes about 0.6 ms. The frame rate is set by the screen.

## How it was measured

- **Phone.** Galaxy A52s 5G (SM-A528B), Snapdragon 778G (SM7325), Adreno 642L GPU, 6 GB RAM, Android
  14, screen at 1080 × 2400 and 120 Hz. Plugged in to USB and charging from 25% battery, which warms
  it on its own.
- **Builds.** `android-arm64-release`, Vulkan, Jolt, miniaudio, RmlUi, with `profiler =
  ["timeline"]` added to `[modules.android]` and the samples' scripted play turned on
  (`[demo] scripted = true`, or `quit_after_frames = 7200` for the render demo). The values are the
  profiler's summary in the log; see [reading-profiles.md](reading-profiles.md).
- **Heat.** Every 5 seconds the script read `dumpsys thermalservice` (thermal status and the AP,
  skin and battery sensors) and the clocks of the big and prime CPU cores. The GPU clock can't be
  read without root.

## Sustained play

Toppler's scripted run, all ten levels at 9238 frames, played eight times back to back, 664 seconds
in all:

| Run | Frame time mean / p99 / max (ms) | GPU time mean (ms) | Resident memory max (MB) |
|---|---|---|---|
| 1 | 8.29 / 9.79 / 45.0 | 1.42 | 99 |
| 2 | 8.29 / 9.83 / 18.6 | 1.42 | 99 |
| 3 | 8.29 / 9.85 / 19.1 | 1.42 | 106 |
| 4 | 8.29 / 10.1 / 17.3 | 1.42 | 104 |
| 5 | 8.29 / 9.83 / 18.1 | 1.43 | 105 |
| 6 | 8.29 / 10.1 / 19.8 | 1.43 | 103 |
| 7 | 8.29 / 9.80 / 19.1 | 1.42 | 101 |
| 8 | 8.29 / 10.1 / 23.9 | 1.43 | 106 |

- **Temperatures.** Skin 37.9 – 40.8 °C, the processor (AP sensor) 43 – 51 °C, with no upward trend
  across the eleven minutes. The battery rose from 33.1 to 35.8 °C, from charging.
- **Thermal status** moved between 1 (light) and 2 (moderate) the whole time, and was already
  there before the games started, so it came from charging rather than from the game. It never
  reached 3 (severe), where Android expects games to cut their work.
- **Clocks.** The big cores stayed at 1.9 GHz, apart from one sample at 1.5 GHz. The prime core was
  mostly at 806 MHz while playing and reached 2.4 GHz, its highest, between runs at minute 10, so
  the low clock looks like the scheduler leaving it idle for a light load rather than a heat limit.
- **No crashes,** no errors in the log, and every run cleared all ten levels with 28 stars.

## What it found

- **About 13 heap allocations every frame** in Toppler and the arcade, and 6 in the render demo,
  where [reading-profiles.md](reading-profiles.md) expects close to zero. They are steady, not
  growing, so they aren't a leak, but they are worth finding with a debug build's trace.
- **One frame of 27 – 45 ms** in a few runs, at a level load or at the start. The p99 stays near
  10 ms, so players won't see stutter in normal play.
- **The game runs at 120 FPS whenever the screen can,** which costs battery and heat for no visible
  gain in a puzzle game. A frame-rate cap, such as 60 FPS on battery, would be a cheap setting to add.

## What wasn't checked

- A truly low-end phone, with 3 – 4 GB of RAM and a Mali or PowerVR GPU from an older generation.
  The A52s is older than the Galaxy A57 used for the other reports, but still mid-range.
- The phone on battery, without charging, and in a warm room. Charging kept it at "moderate" from
  the start.
- Longer sessions than eleven minutes, and battery drain per hour.
- Debug builds, the second backends (`impulse`, `markup`) and the null audio on this phone.
