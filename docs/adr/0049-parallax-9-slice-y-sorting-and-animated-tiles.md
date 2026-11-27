# 0049. Parallax, 9-slice sprites, sorting by height and animated tiles

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0039 gave 2D games sprites, sprite sheets and tile maps, and left a list for later. Four of
its items are small additions to what sprites and tile maps already are, and most 2D games need at
least one of them:

- **Parallax:** backgrounds that move slower than the level, and repeat, so a level seems deep.
- **9-slice sprites:** panels, frames, speech bubbles and health bars that stretch without
  stretching their corners.
- **Sorting by height:** in a top-down game, what stands lower on screen is nearer, and covers what
  stands higher. Layers and orders fixed in a scene can't say that for things that move.
- **Animated tiles:** water, torches, lava and flowers in the wind, without an entity per tile.

The rest of ADR 0039's list, Tiled maps, atlases packed at build time and a renderer module for 2D
alone, are steps of their own, each with an ADR.

## Decision

All four are data on ADR 0039's components and one new component, which `gather_sprites` reads
as it turns sprites and tiles into rectangles. Nothing changes in the renderer interface, in the
renderer modules or in `sprites.slang`: each feature ends as ordinary `SpriteQuad`s, so every
graphics backend, phones included, draws them as it already draws sprites.

They stay in the runtime with the rest of ADR 0039's 2D components, which the runtime's renderer
glue gathers into a frame; they are settings of those components, not features a project would
swap.

### Parallax

A `Parallax` component, `parallax` in scenes, on an entity with a sprite or a tile map:

| Setting | Default | What it does |
|---|---|---|
| `factor` | [1, 1] | How far it moves across the screen as the camera moves: 1 as the world, 0.3 a third as far, 0 fixed on screen |
| `repeat_x`, `repeat_y` | false | Repeated side by side, or above and below, every its own size, to fill the view |

It is drawn moved by the middle of the view times `1 - factor`, so where the camera is at the
world's origin, it is drawn where its entity is. Repeats are the copies that reach into the view,
at most 256 along each axis; a repeated sprite repeats as though it weren't turned, and a repeated
tile map's copies each give only their tiles in view.

### 9-slice sprites

`SpriteRenderer::slice`, `slice` in scenes, gives borders in the frame's pixels: one number for every
side, or four for left, right, top and bottom. A sliced sprite becomes up to nine rectangles:
corners that keep their size in pixels over `pixels_per_unit`, edges stretched along their length,
and the middle filling the rest, all turned around the sprite's pivot together, and flipped
together. A sprite smaller than its borders shrinks them evenly, and leaves out the middle row or
column that has no room.

### Sorting by height

`Camera2D::y_sort_layers`, `y_sort_layers` in scenes, names the layers sorted by height. In those
layers, things are drawn from the highest to the lowest before `order` is looked at; `order` still
decides between things at the same height. A sprite stands at its entity, so its pivot belongs at
its feet. A tile map in such a layer is gathered as one item per row in view, standing at the
bottom of its row, so a character can walk in front of one row of a map and behind the next.

The setting is on the camera, because sorting is about how the view is drawn, and a game has one
2D camera.

### Animated tiles

`TileMap::animations`, `[tile_map.animations.<tile>]` in scenes, maps a tile to `frames` and `fps`.
A table is named by a tile's number, or by its character in the map's legend. Cells keep the tile
itself, so collisions and `tile()` don't change as the frames do; only the drawn tile does, and a
frame of -1 draws nothing, for blinking. `update_sprite_animators`, which games already call each
frame, moves `animation_time` on by `animation_speed`, which a game can set to 0 to hold them, as the
checks do.

### The sample

The platformer gains distant hills, repeated across the view at a third of the level's speed,
clouds at 0.6, swaying flowers, water in its pits, and a sign stretched from a 16-pixel 9-slice
board. Its checks hold the animated tiles on their second frame, and one new reference picture
matches on the five PC graphics backends.

## Consequences

- Each feature costs only the rectangles it adds: a repeat or a 9-slice sprite are a few more
  instances in a batch that was drawn anyway.
- Sorting by height splits a tile map in a sorted layer into rows, which can break batches where
  rows and sprites with other textures interleave. Characters and the tiles they walk among drawn
  from one tileset, or from one atlas once those exist, keep that to a draw per row at worst.
- Tall things made of several tiles, such as a tree two tiles high, sort each row by its own bottom,
  so a character just above the trunk is drawn over the crown. Such things belong in sprites with
  their pivot at their foot, or in a Tiled object layer once maps come from Tiled.
- 9-slice borders and `y_sort_layers` have no field kind in the editor's inspector, which has no
  4-number or list kinds yet; they are written in scene files.
