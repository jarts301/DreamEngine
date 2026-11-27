# 2D games: sprites, sprite sheets, animation and tile maps

This guide explains how to make a 2D game with DreamEngine: the 2D camera, sprites and sprite sheets,
animating them, building levels from tile maps with collisions and animated tiles, parallax
backgrounds, 9-slice panels, sorting by height for top-down games, levels made in the Tiled map
editor, atlases packed at build time, and making the art. The design is in
[ADR 0039](../adr/0039-sprites-and-tile-maps.md),
[ADR 0049](../adr/0049-parallax-9-slice-y-sorting-and-animated-tiles.md),
[ADR 0050](../adr/0050-tiled-maps.md) and [ADR 0052](../adr/0052-sprite-atlases.md). [samples/platformer](../../samples/platformer) and
[samples/top_down](../../samples/top_down) are complete small games to start from.

## In short

- The 2D world is the **XY plane, +Y up**, in world units, the same plane 2D physics uses.
- A **`camera_2d`** decides what is seen: its entity's position is the middle of the window, and
  `height` is how many world units fit from bottom to top.
- A **`sprite`** draws an image, or one frame of a sprite sheet, at its entity.
- A **`sprite_animator`** plays named runs of frames.
- A **`tile_map`** draws a level from a tileset, and `make_tile_colliders` makes it solid. Its
  **`animations`** make tiles such as water move.
- A **`parallax`** makes a sprite or tile map move slower than the world, and repeat: backgrounds.
- A sprite's **`slice`** stretches it without stretching its corners: panels and frames.
- A camera's **`y_sort_layers`** draw what stands lower over what stands higher, for top-down games.
- A **`tile_map_file`** loads a level made in [Tiled](https://www.mapeditor.org/): its layers,
  animated and solid tiles, and objects.
- An **atlas** packs a folder of images into one at build time, and sprites name its regions, so
  they are drawn together.
- Sprites are drawn over any 3D scene and under the interface, by layer and then order.

## Setting up

```toml
[modules.windows]
platform = ["sdl3"]
input_device = ["sdl3_gamepad", "null"]
rhi = ["vulkan", "directx12", "directx", "opengl", "opengles", "null"]
renderer = ["sprites", "null"]
physics2d = ["box2d", "chipmunk"]

[modules.android]
platform = ["sdl3"]
input_device = ["sdl3_gamepad", "null"]
rhi = ["vulkan", "opengles", "null"]
renderer = ["sprites", "null"]
physics2d = ["box2d"]
```

The `sprites` renderer draws only 2D: sprites, debug lines and the interface, straight into the
window, with three shaders instead of the 3D renderer's sixteen, and no shadow map or HDR targets
([ADR 0053](../adr/0053-sprites-renderer.md)). A 2D game that also wants particles or a 3D scene
behind its sprites lists `forward`, which draws the same sprites over it. A 2D game needs no other
3D modules. `physics2d` is only needed for collisions, and `tile_map_format = ["tiled", "null"]`
only for maps made in Tiled.

```cpp
class MyGame final : public runtime::Game {
    core::Result<void> on_start(runtime::App& app) override {
        // Before loading scenes: it lets them use the 2D components and load their textures.
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (auto ready = m_physics.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/level.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_world->update_transforms();
        // The level's solid tiles as static bodies, then physics for the world.
        if (auto made = runtime::make_tile_colliders(*m_world, m_world->find("level"));
            !made.has_value()) {
            return made.error();
        }
        m_physics.attach(m_world.get());
        return {};
    }

    void on_fixed_update(runtime::App& app, double step) override {
        m_physics.step(static_cast<float>(step));
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        m_physics.update_transforms(static_cast<float>(app.fixed_step_alpha()));
        m_world->update_transforms();
        runtime::update_sprite_animators(*m_world, time.delta_seconds);
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        // The clear color is the background, exactly as given in a 2D game.
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.36f, 0.62f, 0.92f, 1.0f});
    }

    runtime::Renderer m_renderer;
    runtime::Physics2D m_physics;
    std::unique_ptr<runtime::World> m_world;
};
```

## The camera

```toml
[entities.camera.transform]
position = [8.0, 4.5, 0.0]

[entities.camera.camera_2d]
height = 9.0      # World units from the bottom of the window to the top.
min_width = 12.0  # Optional: at least this much across, growing taller on narrow windows.
```

The width follows the window's shape: a 16:9 window with a height of 9 sees 16 units across. On a
phone held upright the same height sees only about 4 units across; `min_width` keeps at least that
much across by seeing more above and below. Move
the camera by moving its entity; turn it with the entity's rotation around Z.

**Choosing a height:** for pixel art, pick a height that makes each art pixel a whole number of
screen pixels at your main resolution. With 16-pixel tiles at one world unit each, a height of 11.25
units on a 720-pixel-tall window makes each art pixel 4 screen pixels. Other heights still look
crisp, but some art pixels come out one screen pixel wider than others.

`runtime::screen_to_world_2d(world, size, pixel)` turns a mouse or touch position into a point of
the world, for clicking on things.

## Sprites

```toml
[entities.hero.transform]
position = [3.5, 2.45, 0.0]

[entities.hero.sprite]
texture = "assets:/sprites/hero.png"
columns = 4             # A sprite sheet of 4 x 2 equal frames,
rows = 2
frame = 0               # numbered from the top left, row by row.
pixels_per_unit = 16.0  # The frame's size in the world: 16 pixels across is 1 unit.
pivot = [0.5, 0.45]     # The point at the entity: [0, 0] bottom left, [1, 1] top right.
layer = 1
```

| Setting | Default | What it does |
|---|---|---|
| `texture` | none | A PNG with straight alpha. Without one, a white rectangle in `color`. |
| `columns`, `rows` | 1, 1 | The texture as a grid of equal frames. |
| `frame` | 0 | The frame shown, from the top left, row by row. |
| `size` | from the frame | Size in world units, overriding `pixels_per_unit`. |
| `pixels_per_unit` | 16 | How many of the frame's pixels make one world unit. |
| `pivot` | [0.5, 0.5] | The point placed at the entity, and turned around. |
| `color`, `alpha` | [1, 1, 1], 1 | Multiply the texture: a red flash when hit, fading out. |
| `flip_x`, `flip_y` | false | Mirror it, as for a character facing the other way. |
| `layer`, `order` | 0, 0 | Drawing order: higher layers cover lower ones, and within a layer higher orders cover lower ones. |
| `slice` | none | 9-slice borders in the frame's pixels: one number, or `[left, right, top, bottom]`. See below. |
| `filter` | `"pixel"` | `"pixel"` keeps pixel art crisp; `"smooth"` blends, for painted or high-resolution art. |
| `visible` | true | Whether it is drawn. |

The entity's transform places it: X and Y, its rotation around Z, and its scale in X and Y, which
multiplies the size. In code the component is `runtime::SpriteRenderer`.

**Layers:** give each kind of thing its own layer and keep to them, for example: -2 far background,
-1 clouds, 0 the level, 1 characters, 2 effects in front. Use `order` within a layer for things
that overlap, such as a sword in front of a hand.

### 9-slice sprites

A panel, a frame, a speech bubble or a bar drawn from a small image, stretched to any size without
stretching its corners:

```toml
[entities.sign.sprite]
texture = "assets:/sprites/sign.png"   # 16 x 16, with 4-pixel edges.
size = [2.5, 1.25]
slice = 4                              # Or [left, right, top, bottom], in the image's pixels.
```

The image is cut into nine parts at the borders. The four corners keep their size, their pixels over
`pixels_per_unit`, so a 4-pixel corner is a quarter unit at 16 pixels per unit, however large the
sprite. The top and bottom edges stretch across, the left and right ones up, and the middle fills
the rest. Scaling the entity makes the sprite larger without making the corners larger, and turning
and flipping turn and flip it whole. A sprite narrower or shorter than its borders shrinks them to
fit. With sprite sheets, the borders are within each frame.

Draw the edges so that they can stretch: plain or striped along their length, and a middle that can
grow, such as a plain color.

## Animation

```toml
[entities.hero.sprite_animator]
clip = "idle"             # Plays at the start.

[entities.hero.sprite_animator.clips.idle]
frames = [0, 1]
fps = 2.0

[entities.hero.sprite_animator.clips.run]
frames = [2, 3, 4, 5]
fps = 10.0

[entities.hero.sprite_animator.clips.attack]
frames = [8, 9, 10]
fps = 15.0
loop = false              # Stops on its last frame.
```

```cpp
auto& animator = registry.get<runtime::SpriteAnimator>(hero);
animator.play("run");        // Starts it, unless it is already playing: safe to call every frame.
if (!animator.playing) {     // A clip that doesn't loop has finished.
    animator.play("idle");
}
animator.speed = 0.5f;       // Slow motion; 0 holds the frame.
```

Call `runtime::update_sprite_animators(world, seconds)` once per frame. Choosing the clip from what
the character does each frame, as `Platformer::animate_hero` does, keeps animation simple: on the
ground and moving is "run", in the air going up is "jump", and so on.

## Tile maps

A tile map draws a grid of tiles from a **tileset**: one image cut into equal tiles, numbered from
the top left, row by row. A 64 x 32 image of 16 x 16 tiles has tiles 0 to 3 in its top row and 4 to
7 below.

```toml
[entities.level.tile_map]
tileset = "assets:/sprites/tiles.png"
tile_pixels = [16, 16]    # Each tile's size in the tileset.
tile_size = [1.0, 1.0]    # Each tile's size in the world.
solid = [0, 1, 2, 3]      # Tiles that make_tile_colliders makes solid.
rows = [
    "................",
    "......PPP.......",
    "...f.......b....",
    "GGGGGGGGGG..GGGG",
    "DDDDDDDDDD..DDDD",
]

[entities.level.tile_map.legend]
G = 0     # Grass.
D = 1     # Dirt.
P = 3     # A platform.
b = 4     # A bush.
f = 5     # A flower.
```

- Rows are written **top first**, as they look. `.` and a space are empty; every other character
  must be in the legend.
- The map's **bottom left corner** is at its entity's X and Y, and each tile covers `tile_size`.
  Tile maps aren't turned or scaled by their transform.
- Instead of `rows` and a `legend`, `tiles = [...]` with `columns = 16` gives the tile numbers
  directly, row by row from the top, with -1 for empty: the form to generate from a level editor.
- `tile_margin` and `tile_spacing` give the pixels around a tileset's edge and between its tiles,
  for tilesets drawn with gaps.
- `layer`, `order`, `color`, `alpha`, `filter` and `visible` work as for sprites. Put decorations
  that the player walks in front of in the same map as non-solid tiles, and backgrounds in maps of
  their own on lower layers.

In code, `TileMap::tile(column, row)` and `set_tile` read and change cells, counted from the top
left, and `cell_at(origin, point)` finds the cell under a point: to dig, break bricks or place
blocks.

### Animated tiles

```toml
[entities.level.tile_map.animations.w]   # The tile that "w" stands for in the legend.
frames = [10, 11]                        # The tiles shown in turn.
fps = 2.0

[entities.level.tile_map.animations.5]   # Or name a tile by its number.
frames = [5, 8, 5, 9]
fps = 3.0
```

Every cell holding the tile shows its frames instead, all in step. Cells keep the tile itself, so
collisions and `TileMap::tile` don't change as the frames do. A frame of -1 shows nothing, for
blinking. `update_sprite_animators` moves animated tiles on along with sprite animators; set the map's
`animation_speed`, 1 by default, to 0 to hold them, or to 2 to play them twice as fast.

### Collisions

`runtime::make_tile_colliders(world, map)` adds a static 2D physics box over the map's solid tiles.
Neighboring solid tiles are merged into larger boxes, so a character sliding along the ground
doesn't catch on the seams between tiles. Call it once the map is placed, after
`update_transforms`. After changing tiles, destroy the entities it returned and call it again.

A platformer character works best as a capsule with `fixed_rotation = true` and `friction = 0.0`,
moved by setting its velocity, and with short rays cast down from its feet to know when it stands
on something, as the sample does.

## Parallax backgrounds

```toml
[entities.hills.transform]
position = [0.0, 1.5, 0.0]

[entities.hills.sprite]
texture = "assets:/sprites/hills.png"
pivot = [0.0, 0.0]
layer = -2

[entities.hills.parallax]
factor = [0.3, 1.0]   # A third as far across as the world moves, and up and down with it.
repeat_x = true       # Side by side across the whole view.
```

| Setting | Default | What it does |
|---|---|---|
| `factor` | [1, 1] | How far it moves across the screen as the camera moves, in X and Y: 1 as the world does, 0.3 a third as far, so it seems far away, 0 not at all, fixed on screen, and above 1 faster, for things passing in front |
| `repeat_x`, `repeat_y` | false | Repeated every its own width, or height, to fill the view |

A `parallax` goes on an entity with a `sprite` or a `tile_map`. Where the camera is at the world's
origin, it is drawn where its entity is; as the camera moves, it moves by `factor` of that. Give
farther things smaller factors and lower layers: hills at 0.3 on layer -2, clouds at 0.6 on -1, the
level at 1 on layer 0.

For a repeated image, draw it so its left and right edges meet without a seam. A repeated tile map
repeats its whole width, empty cells included, so a cloud map 48 tiles wide comes round every 48
units. A repeated sprite repeats as though it weren't turned.

## Top-down games: sorting by height

In a top-down game, something lower on the screen is nearer, so it should cover what is higher:
the player walking below a tree is in front of it, and walking above it, behind. List the layers to
sort that way on the camera:

```toml
[entities.camera.camera_2d]
height = 12.0
y_sort_layers = [1]   # Characters, trees and furniture.
```

In those layers, everything is drawn from the highest to the lowest, by where it stands: a sprite
at its entity, so **put its pivot at its feet**, as `pivot = [0.5, 0.0]`; `order` only decides
between things at the same height. A tile map in a sorted layer stands each row at its bottom, so
characters walk in front of one row and behind the next. Keep floors in a layer below, not sorted,
since everything stands on them.

Something tall made of several tiles, such as a tree two tiles high, sorts each row by its own
bottom, so a character standing just above the trunk is drawn over the crown. Make tall things
sprites, with their pivot at their foot.

## Maps from Tiled

[Tiled](https://www.mapeditor.org/) is a free map editor: draw the level there, and load it with one
component.

```toml
# dream.toml
[modules.windows]
tile_map_format = ["tiled", "null"]

[modules.android]
tile_map_format = ["tiled", "null"]
```

```toml
# The scene
[entities.garden.tile_map_file]
file = "assets:/maps/garden.tmj"
tile_size = [1.0, 1.0]   # Each tile's size in the world; the default.
layer = 0                # The drawing layer of the map's first layer; the default.
```

| Setting | Default | What it does |
|---|---|---|
| `file` | none | The map, saved from Tiled as JSON (`.tmj`). |
| `tile_size` | [1, 1] | Each tile's size in the world. Objects and image layers are scaled alike. |
| `layer` | 0 | The drawing layer of the map's first layer. Each next layer is one higher, unless it has an integer property `layer`. |
| `solid` | `"solid"` | The bool property that makes tiles solid. |
| `filter` | `"pixel"` | As for sprites. |

The map's bottom left corner is at its entity. Its layers and objects become the entity's children,
in Tiled's drawing order:

- **Tile layers** become tile maps, with their offset, opacity, tint, parallax, visibility, and the
  flips and rotations of their tiles. Tiles animated in Tiled's tileset editor animate, with each
  frame's own length.
- **Image layers** become sprites, with their parallax and "Repeat X" and "Repeat Y".
- **Objects** become entities named as the object, with a `runtime::MapObject` holding the object's
  class, its size in world units, its shape and its custom properties. An entity sits at the
  object's middle, or at its point. Find spawn points and doors by name:

  ```cpp
  const runtime::Entity start = world->find("start");
  const auto& door = world->registry().get<runtime::MapObject>(world->find("door"));
  const std::string_view to = door.property("to");   // A custom property, as text.
  const float speed = door.number("speed", 1.0f);
  const bool locked = door.flag("locked");
  ```

- **Tile objects**, tiles placed freely with "Insert Tile", become sprites standing at their foot,
  so in a layer sorted by height they sort by where they stand: trees, rocks, furniture. Use them
  for things taller than a tile.
- **Groups** are flattened into their layers, whose names become `group/layer`.

**Collisions:** give a tile a bool property `solid` in Tiled's tileset editor, or draw a collision
shape on it, and every cell holding it is solid; or give a whole tile layer a bool property `solid`.
Then call `runtime::make_tile_colliders(world, map_entity)` once the map is placed, which makes the
colliders of every tile layer. Objects make no colliders by themselves: add bodies for them in code,
from their `MapObject`, as the top-down sample does for tree trunks.

**Saving from Tiled:**

- Save maps as JSON (`.tmj`), and tilesets kept in files of their own as JSON too (`.tsj`): in
  Tiled's preferences, or with "Save As". XML `.tmx` and `.tsx` files aren't read.
- Keep the default tile layer format, CSV, or use uncompressed Base64. Compressed layers aren't read.
- Untick "Infinite" in the map's properties, and keep the orientation orthogonal.
- Make tilesets from one image cut into tiles, not collections of separate images.
- Tile layers take tiles the size of the map's; place larger ones, such as trees, as tile objects.
- Keep the map, its tilesets and images in the game's assets, named by relative paths, as Tiled does
  when they are saved together. The build checks that every image and tileset a map names is there.

The log says what to change when a map can't be read, such as an isometric map or a compressed layer.

## Atlases

Each texture a frame draws from breaks the sprites into another draw. An atlas packs a folder of
images into one image when the game builds, so the sprites cut from it are drawn together. Name the
folder in `dream.toml`:

```toml
[atlases.things]
folder = "assets:/sprites/things"   # Every PNG in it, and in folders inside it.
padding = 1                          # Optional: pixels repeating each image's edge, 0 to 16.
max_size = 2048                      # Optional: the largest side, in pixels.
```

The build makes `assets:/atlases/things.atlas` and its image, and leaves the folder's own images
out of the game. A sprite names the atlas and a region, the image's path in the folder without
".png":

```toml
[entities.hero.sprite]
atlas = "assets:/atlases/things.atlas"
region = "hero"            # assets:/sprites/things/hero.png; "trees/oak" for one in a folder.
columns = 4                # A region can be a sprite sheet: its frames are cut from it.
rows = 2
```

Everything else about the sprite works as with a `texture`: frames and clips, size, pivot, slices,
flips. In code, load the atlas and call `use_atlas_region`:

```cpp
auto atlas = app.assets().load<runtime::SpriteAtlasAsset>("assets:/atlases/things.atlas");
runtime::SpriteRenderer coin;
if (auto used = runtime::use_atlas_region(coin, atlas.value(), "coin"); !used.has_value()) {
    // The message lists some of the atlas's regions.
}
```

`SpriteRenderer::region` is the part of the texture drawn, in pixels, and works without an atlas
too, for any texture. The atlas is the smallest power-of-two size that holds every image, each with
`padding` pixels repeating its edge around it, so `"smooth"` sprites don't take color from their
neighbors. A name keeps to letters, digits, `_`, `-` and `/`. Tile maps keep their own tileset.

The editor reads assets from the project's folders, not from the build, so add the game's generated
assets folder to `[editor] assets`, as `samples/platformer/dream.toml` does, and build the game once
before opening it.

## Making the art

- **Pixel art:** draw at the size it is meant to be seen at, such as 16 x 16 tiles and characters,
  and let `"pixel"` scale it up. Don't scale the images up yourself.
- **Keep transparent backgrounds** with a real alpha channel. Straight alpha, as image editors save
  it, is right.
- **Sprite sheets:** every frame the same size, in a grid, with the character in the same place in
  each frame so it doesn't jitter. Leave the grid without gaps between frames.
- **Atlases:** keep each image, or sheet, as a file of its own in the packed folder, and let the
  build pack them. Group what is drawn together, such as a level's characters and props, in one
  atlas.
- **Tilesets:** tiles side by side without gaps. With `"pixel"`, neighboring tiles never bleed into
  each other. With `"smooth"` they can at the seams: leave a pixel or two of repeated border around
  each tile, or use `"pixel"`.
- **Keep pixel art in a `sprites` folder**, or anywhere but a `textures` folder: the build
  compresses images in `textures` folders, and on phones that compression shifts the colors of
  pixel art in small blocks. Elsewhere, PNGs load as they are. Painted art with `"smooth"` belongs
  in `textures`, where compression saves memory.
- **Parallax backgrounds** that repeat must meet themselves at their left and right edges. Distant
  ones can be drawn at a lower resolution than the level, as the sample's hills are, at 8 pixels per
  unit against the level's 16.
- `tools/make_sprite_art.py` draws the platformer's art in Python, as a reference for sizes and
  layout, and `tools/make_top_down_art.py` the top-down sample's, with its first Tiled map.

## Performance

- Sprites with the same texture next to each other in drawing order are drawn together, in one draw
  for up to 8,192. Many textures interleaved by layer and order mean many draws: put a character's
  frames in one sheet, a level's tiles in one tileset, and its characters and props in one atlas.
- Tile maps only give the tiles in view, so a large level costs what is on screen.
- On phones, large overlapping transparent sprites, such as full-screen backgrounds stacked three
  deep, cost the most: every covered pixel is drawn once per sprite. Parallax layers are such
  backgrounds: keep them few, and let opaque ground cover what it can.
- A 9-slice sprite is up to nine rectangles, and a repeated one a rectangle per copy: cheap, as
  they stay in their batch.
- In layers sorted by height, rows of tile maps and sprites interleave, so a batch breaks wherever
  the texture changes. Drawing characters and the tiles they walk among from one image keeps it to
  few draws.

## Checking

| You see | Likely cause | Fix |
|---|---|---|
| Nothing | No `camera_2d`, the sprites are out of its view, or `update_transforms` isn't called | Check the camera's position and height |
| Models, particles or lights are missing, and the log says the 'sprites' renderer left them out | The game lists `sprites`, which draws only 2D | List `forward` |
| A white rectangle | The sprite has no texture, or its path is wrong (the log says) | Fix `texture` |
| Blurry pixel art | `filter = "smooth"` | Use `"pixel"`, the default |
| Pixel art with blotchy colors on phones | It is in a `textures` folder, so compressed | Move it to a `sprites` folder |
| Far too zoomed in on a phone held upright | The camera's height fits a wide window | Set `min_width` |
| Some art pixels wider than others | The camera's height doesn't make whole screen pixels per art pixel | See "Choosing a height" |
| A character's frame shows part of the next | `columns` or `rows` don't match the sheet | Count the sheet's frames again |
| A tile map row is shifted | A row in `rows` is shorter than the others | Pad rows to the same length with `.` |
| Characters catch on the ground | Bodies placed per tile by hand | Use `make_tile_colliders`, which merges them |
| The wrong thing is in front | Layers and orders | Give each kind of thing its own layer |
| In a top-down game, characters cover what they stand behind | The layer isn't sorted by height, or pivots are in the middle | Add the layer to `y_sort_layers`, and put pivots at the feet |
| A parallax background ends at the screen's side | No `repeat_x` | Set `repeat_x = true` |
| A seam where a repeated background meets itself | The image's edges don't match | Draw it so its left edge continues its right |
| A 9-slice sprite's corners stretch | `slice` is missing, or larger than the image's frame | Give `slice` in the frame's pixels |
| "the game reads no map files" | No `tile_map_format` module listed for the platform | List `tile_map_format = ["tiled", "null"]` |
| A Tiled map fails to load | A format the reader doesn't take; the message says which | Save as JSON, with CSV layers, not infinite |
| Walking through hedges from a Tiled map | No solid tiles, or no `make_tile_colliders` | Give tiles or the layer a `solid` property, and call it on the map's entity |
| "atlas ... packs assets:/..., which none of ... asset folders has" | The atlas's `folder` is misspelled, or not under the game's assets | Fix `folder`, from the assets' root |
| A sprite from an atlas fails to load: "no region" | The region isn't the image's name in the folder; the message lists some | Use the file's name without ".png", with its folders |
| "assets:/sprites/things/hero.png" is missing | A sprite names an image the build packed into an atlas | Use `atlas` and `region` |
| The editor can't find an atlas | The atlas exists only in the build | Build the game, and add its generated assets folder to `[editor] assets` |
| Tiles from a Tiled tileset show the wrong part of the image | The tileset has a margin or spacing in Tiled that doesn't match the image | Set them in Tiled's tileset properties to match |

## Limits

Each of these is a later step on the roadmap:

- Tile maps and Tiled tilesets from atlases; trimming images' empty edges when packing.
- Lights and shadows for sprites.
- Particles and bloom with the `sprites` renderer; list `forward` for them.

## How it works inside

1. **`runtime::Renderer`** finds the first `Camera2D` and makes its view, then gathers every visible
   `SpriteRenderer` and `TileMap`, sorted by layer, by height in layers sorted by it, and by order,
   into rectangles: one per sprite, nine for a 9-slice sprite, one per tile in view showing its
   animation's frame, and one per copy of what parallax repeats, moved by its factor. Neighbors with
   the same texture become one `SpriteBatch`.
2. **The `forward` module** draws the batches after tone mapping, straight into the window, with
   `sprites.slang`: each rectangle placed, sized, turned and textured per instance, its texture
   read at the middle of the nearest texel for pixel art.
