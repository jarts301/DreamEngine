# 0039. 2D sprites and tile maps

- Status: Accepted
- Date: 2026-11-20

## Context

2D physics has been in the engine since Phase 7, but a 2D game had nothing to draw with: the
renderer drew 3D models, and 2D scenes were drawn as flat 3D meshes. 2D games need sprites, sprite
sheets with animation, tile maps for levels, a 2D camera, and pixel art that stays crisp. They are
the most common kind of small game on phones.

## Decision

### Sprites as renderer data

`renderer::SpriteQuad` is one rectangle of a texture: position, size, pivot and rotation in the 2D
world, the texture rectangle it shows, and a color. `renderer::SpriteBatch` is a run of them with one
texture, its size in pixels, and a filter. `Frame::sprite_view` is the 2D camera, from the 2D world to
clip space, and `Frame::sprites` the batches in drawing order. The renderer draws batch after batch,
in order, without depth: later sprites cover earlier ones.

The 2D world is the XY plane, +Y up, in world units: the plane 2D physics already uses, so a body
and its sprite share an entity and its transform.

### Drawing them in `forward` and `forward_plus`

`sprites.slang` draws one rectangle per instance, 64 bytes each, from the same four corners as
particles. Sprites are drawn into the window after tone mapping, over the 3D scene if there is one,
and under debug lines and the interface: tone mapping would change an artist's colors, and 2D art
is painted for the screen. Without a 3D view the HDR pass is skipped, so a 2D game's clear color
also comes out as given. Debug lines, such as 2D physics' shapes, use the 2D camera when there is
no 3D one.

Two filters: `Pixel`, for pixel art, samples the middle of the nearest texel at the full-size level,
which a linear sampler returns unfiltered, so no change to the graphics interface's samplers was
needed; `Smooth` filters with mipmaps, for painted art.

Drawing sprites is part of the renderer modules, not a module of its own: sprites need the
renderer's passes and the window's pass, and the renderer module is what a project swaps to draw
differently.

### The runtime's side

Components, which Renderer::initialize lets scene files use:

| Component | Scene name | What it does |
|---|---|---|
| `Camera2D` | `camera_2d` | Sees `height` world units from the bottom of the window to the top, at its entity |
| `SpriteRenderer` | `sprite` | A texture, or one frame of it as a sprite sheet, sized from its pixels or given a size, with a pivot, color, flips, layer and order |
| `SpriteAnimator` | `sprite_animator` | Named clips of frames at a rate, looping or not, played with `play` |
| `TileMap` | `tile_map` | A grid of tiles from a tileset, written as rows of characters with a legend, or as numbers |

The renderer gathers sprites and tile maps each frame, sorts them by layer, then order, then a
stable order of its own, and merges neighbors with the same texture into batches. Tile maps give only
the tiles in view. `update_sprite_animators` moves animators on. `make_tile_colliders` turns a tile
map's solid tiles into static 2D physics boxes, merging neighbors into larger boxes so characters
don't catch on the seams between tiles. `screen_to_world_2d` turns a pointer into a world point.

Textures are `TextureAsset`s (ADR 0038). Images in a `textures` folder are cooked to compressed
formats, whose small color shifts show in pixel art on phones, so pixel art is kept elsewhere, such
as a `sprites` folder, and loads losslessly from its PNG. `Camera2D::min_width` keeps a view wide
enough on a phone held upright.

Scene component tables can now hold tables of their own where the component names them, such as
an animator's clips and a tile map's legend, and `ComponentData` reads lists of strings and numbers.

### A sample

`samples/platformer` is a small 2D platformer: a tile map level with its colliders, clouds in a tile
map behind it, an animated hero moved by Box2D, coins, a camera that follows, and keyboard, gamepad
and touch controls. Its pixel art is drawn by `tools/make_sprite_art.py`. Its checks compare a
picture on the five PC graphics backends, and a scripted run with real physics runs along the
level and collects a coin.

## Consequences

- 2D games can be made from scene files and a little code, on every graphics backend, and on
  phones.
- Sprites aren't lit, cast no shadows, and are drawn over the 3D scene, not among it: a sprite
  standing in a 3D world needs to be a particle or a mesh.
- Sprites are sorted on the CPU each frame. Tens of thousands are fine; static tile maps are
  rebuilt from their cells every frame, which costs a little time per visible tile.
- Not yet: tile maps from Tiled's files, which need a JSON reader; animated tiles; parallax
  scrolling; 9-slice sprites; texture atlases packed at build time; sorting by Y for top-down
  games; and a renderer module made for 2D alone. Each is a later step on the roadmap.
