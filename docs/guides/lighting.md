# Lighting: the sun, point lights and spot lights

This guide explains the lights DreamEngine draws, how to place them, how bright to make them, and
which renderer to choose for many of them. The design is in
[ADR 0037](../adr/0037-point-and-spot-lights.md); shadows are in
[ADR 0029](../adr/0029-directional-shadows.md), and how surfaces answer light is in the
[materials guide](materials.md).

## In short

- **The sun** (`directional_light`): one per scene, lights everything from one direction, and
  casts soft shadows.
- **Point lights** (`point_light`): shine every way from a point, out to a range.
- **Spot lights** (`spot_light`): shine in a cone along their entity's -Z axis.
- Point and spot lights cast **no shadows** yet.
- `renderer = ["forward_plus", "forward", "null"]` lets a scene have up to **256** point and spot
  lights; `forward` draws the **16** nearest.

## Choosing a renderer

| Renderer | Point and spot lights | Cost | For |
|---|---|---|---|
| `forward_plus` | Up to 256 in view | Sorting lights into 16 x 9 x 24 clusters on the CPU each frame; each pixel loops over its cluster's lights only | Night scenes, interiors, many lamps and effects |
| `forward` | The 16 nearest in view | Each pixel loops over all of them | Daylight scenes with a few lights, the cheapest on phones |

List them in order of preference, as any module:

```toml
[modules.windows]
renderer = ["forward_plus", "forward", "null"]

[modules.android]
renderer = ["forward_plus", "forward", "null"]

[renderer]
max_lights = 64   # Optional: fewer than the renderer's limit, for weaker phones.
```

With more lights in view than the limit, the nearest are drawn and the rest left out;
`Renderer::last_light_count()` and `last_dropped_light_count()` tell how many were drawn and left
out, and the profiler shows a "Lights" value.

## Placing lights

In a scene file:

```toml
[entities.lamp.transform]
position = [2.0, 1.5, 0.0]

[entities.lamp.point_light]
color = [1.0, 0.6, 0.3]   # Linear. All optional.
intensity = 3.0
range = 6.0               # Meters; the light fades to nothing here.

[entities.torch.transform]
position = [0.0, 3.0, 2.0]
rotation = [-45.0, 0.0, 0.0]   # Spots shine along -Z: this tilts it 45 degrees down.

[entities.torch.spot_light]
color = [1.0, 1.0, 1.0]
intensity = 15.0
range = 10.0
inner_angle = 15.0        # Degrees from the middle: full brightness inside.
outer_angle = 30.0        # Fades to nothing here. Below 90.
```

In code, add `runtime::PointLight` or `runtime::SpotLight` to an entity with a transform. Lights
move with their entity, so a light parented to a moving entity follows it, and lights can change
color, intensity and range every frame.

```cpp
const runtime::Entity lamp = world.create_entity();
world.registry().get<runtime::Transform>(lamp).position = core::Vec3{2.0f, 1.5f, 0.0f};
world.registry().emplace<runtime::PointLight>(
    lamp, runtime::PointLight{.color = {1.0f, 0.6f, 0.3f}, .intensity = 3.0f, .range = 6.0f});
```

## How bright

All lights share one unit: what a white, fully rough, non-metal surface **facing the light**
reflects. For the sun that is `color × intensity` everywhere; the default sun is 1.3. For a point
or spot light it is `color × intensity` at **1 m**, falling off with the square of the distance:

| Distance from the light | Fraction of `intensity` |
|---|---|
| 1 m | 1 |
| 2 m | 1/4 |
| 3 m | 1/9 |
| 5 m | 1/25 |

So to light a surface 3 m away as brightly as a sun of 1.3, an intensity of about 12 is needed.
The light also fades smoothly to nothing at its `range`, so keep the range a little past where
it matters: about where `intensity / distance²` falls below 0.01 to 0.05.

Starting points, with a sun of 1.3 and ambient 0.3:

| Light | Intensity | Range |
|---|---|---|
| Candle, small glow | 0.2–0.5 | 1–2 m |
| Lamp lighting a desk | 2–4 | 3–5 m |
| Street lamp | 20–40 | 10–15 m |
| Torch or flashlight (spot) | 10–30 | 10–20 m |
| Stage spot (spot) | 50–100 | 20–30 m |

At night, lower the sun's intensity and ambient (an ambient of 0.03 to 0.08 reads as night), or
use a dark environment (see the [environment guide](environment.md)), so the lights stand out;
lights against a bright sun barely show.

**Light very close to a surface** makes a hot spot, since brightness grows as the distance
shrinks. Keep lights at least 0.3–0.5 m from the surfaces they light, or lower their intensity.

## Using lights well

- **Keep ranges tight.** A light costs time in every cluster its range reaches, and on every pixel
  of `forward`. A range twice as large covers about eight times as many clusters.
- **Glowing objects:** give a lamp's bulb an emissive material (see the materials guide) and put a
  point light inside it: the emission makes it glow, and the light lights what is around it.
- **Phones:** each light in reach costs a few texture reads and the lighting math per pixel. On a
  mid-range phone, keep the lights covering most of the screen to a handful; many small lights in
  separate places are cheap with `forward_plus`.
- **No shadows yet:** a point light behind a wall lights the far side of the wall too. Place lights
  where that doesn't show, or keep their range from reaching through walls.

## Checking

`samples/render_demo` lights its yard with a lamp, a blue spot on the brick wall, and a ring of
eight small colored lights around the tentacle, drawn with `forward_plus`.

| You see | Likely cause | Fix |
|---|---|---|
| A light does nothing | Its intensity is too low against the sun, or its entity has no transform | Raise the intensity, or dim the sun |
| A bright spot on the floor under a light | The light is very close to the surface | Raise it, or lower its intensity |
| A light's edge cuts off sharply | Its range ends where it is still bright | Raise the range |
| Lights pop in and out as the camera moves | More lights in view than the renderer takes | Use `forward_plus`, raise `max_lights`, or use fewer lights |
| Light leaks through a wall | Point and spot lights cast no shadows | Shorten the range, or move the light |
| A spot shines the wrong way | It points along its entity's -Z axis | Rotate the entity; `rotation = [-90, 0, 0]` points straight down |

## Limits

- Point and spot lights cast no shadows.
- At most 256 lights in view with `forward_plus`, and 16 with `forward`.
- Lights are sorted into clusters on the CPU; there are no compute shaders yet.
- No area lights, light cookies or volumetric light.

## How it works inside

1. **`runtime::Renderer`** gathers `PointLight` and `SpotLight` entities into `Frame::lights`,
   placed and pointed by their world transforms.
2. **The `forward` and `forward_plus` modules**, one source built twice, pick the nearest lights
   whose range is in view, find the clusters each reaches from its bounding sphere's corners on
   screen and its depth, and write the lights, each cluster's list, and a header into a 256 x 32
   float texture, updated before the frame's first pass.
3. **`forward.slang`** finds each pixel's cluster from its world position and depth, and adds the
   light of each light in its list with the same GGX lighting as the sun's, falling off with
   distance and, for spots, across the cone's edge.
