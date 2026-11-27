# 0050. Maps from the Tiled map editor

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0039's tile maps are written in scene files, as rows of characters or lists of numbers. That
suits a sample, but a real level is drawn in a map editor, and [Tiled](https://www.mapeditor.org/)
is the one most 2D games use: free, BSD-licensed, on every desktop, with layers, animated tiles,
collision shapes, objects for spawn points and triggers, and custom properties on everything. Its
maps are saved as XML (`.tmx`) or JSON (`.tmj`), and the engine had no JSON reader.

The modularity rule asks that a feature like this be a module a project chooses, and that only the
mandatory core stay out of modules.

## Decision

### A `tile_map_format` interface

`interfaces/tile_map_format` describes a map as a map editor gives it: its size in tiles, tilesets
(an image cut into tiles, what it says about its tiles, such as animations, collision shapes and
properties), and layers of tiles, objects or an image, in pixels with Y down. A module only reads
bytes: `read_map` for a map, `read_tileset` for a tileset kept in a file of its own, and `reads` to
say which files are its. It knows nothing of files, the world or the GPU.

Two modules:

| Module | What it does |
|---|---|
| `tiled` | Reads Tiled's JSON, `.tmj` and `.tsj`, with **yyjson** (MIT, plain C, no exceptions), from vcpkg. Only this module links it. |
| `null` | Reads nothing, saying which module to list. |

The `tiled` reader takes:

- orthogonal maps that aren't infinite, and fails, saying what to change in Tiled, for others;
- tile layers in CSV or uncompressed Base64, and fails for compressed ones, which would need zlib
  or zstd for little gain in a game's assets;
- groups, flattened into their layers with offsets added, opacity and tints multiplied, parallax
  multiplied, visibility combined and names joined with `/`;
- object layers: points, rectangles, ellipses, polygons and polylines as the rectangle around them,
  and tile objects, with their class, rotation and properties;
- image layers, with their repeat;
- tilesets in the map or in `.tsj` files, cut from one image, and fails for collections of images
  and for `.tsx` tilesets, saying to export JSON;
- custom properties of every kind, as text.

Tiled's XML isn't read: its JSON holds the same, and Tiled saves either.

### The runtime's side

`runtime/tile_map_files.h` turns a read map into the 2D world:

- `TileMapFileAsset` is a map file loaded through the AssetManager, with tilesets from their own
  files read in, and its images loaded as `TextureAsset`s. Paths in a map are relative to it, and
  stay within the asset roots.
- `add_tile_map_file` adds the map's layers and objects as children of an entity, whose X and Y
  are the map's bottom left corner, in drawing order:
  - each tile layer becomes a `TileMap` per tileset it draws from, with its offset, opacity, tint,
    parallax and visibility, its tiles' flips, the tileset's animations, and the tiles that are
    solid: those with a bool property `solid`, those with collision shapes, or every tile of a
    layer with `solid` set;
  - each image layer a sprite, with its parallax and repeat;
  - each object an entity named as the object, with a `MapObject` holding its class, size, shape
    and properties, at its middle, its foot for tile objects, or its point; a tile object gets a
    sprite of its tile, standing at its foot, so it sorts by height among characters.
- The drawing layer of the map's first layer is a setting, and each next one is a layer higher,
  unless a layer has an integer property `layer`. A top-down map puts its things on one layer and
  names it in `y_sort_layers` (ADR 0049).
- A scene names a map with the `tile_map_file` component, and `make_tile_colliders` on that entity
  makes the colliders of every tile layer.

The Renderer creates the `tile_map_format` module when the game lists one, as it does the texture
decoder, and registers the loader and component either way, so that a scene naming a map in a game
without one fails, saying which module to list.

`TileMap` gained what Tiled maps need, and scene files can use too: per-cell flips, including the
diagonal flip that, with the others, makes quarter turns; a tileset's margin and spacing; and
animation frames with a length each.

The asset database (ADR 0047) reads `.tmj` and `.tsj` files for the images and tilesets they name,
so a map naming a missing file fails the build.

### The sample

`samples/top_down` is a garden drawn as a Tiled map: ground, hedges and a stone wall, a pond whose
water is animated and solid, trees, stones and a lamp as tile objects from a tileset of their own,
a spawn point and an area with a property. A gardener walks among the trees, sorted by height. Its
art and first map are made by `tools/make_top_down_art.py`, which writes the map as Tiled saves it,
so it opens in Tiled to be changed there. Its checks compare a picture on the five PC graphics
backends, which match exactly, and walk it into the stone wall with real 2D physics.

## Consequences

- Levels can be made in Tiled and loaded with one component; collisions, animations, objects and
  properties come with them.
- A map whose tile layers use tiles of another size than the map's fails to load: such tiles,
  such as trees two tiles high, are placed as tile objects, which is what Tiled's own workflow
  suggests for things that stand up.
- Maps are read when a scene loads, not cooked at build time: a large map costs its JSON parse,
  which yyjson, one of the fastest JSON readers, keeps small next to loading its images.
- Reloading a map file while a game runs reloads the asset, but entities already made from it stay
  as they were, until the scene loads again.
- `.tmx`, infinite maps, compressed layers, isometric and hexagonal maps, image-collection tilesets,
  object templates and Wang sets aren't read. Each would be added to the `tiled` module, or as
  another module, without changing the interface's users.
