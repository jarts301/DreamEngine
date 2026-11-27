# 0052. Sprite atlases packed at build time

- Status: Accepted
- Date: 2026-11-27

## Context

The renderer draws sprites with the same texture, next to each other in drawing order, in one draw
(ADR 0039). A game whose characters, props and pickups are each an image of their own breaks into a
draw for each change of texture, and more so in layers sorted by height (ADR 0049), where rows of
different things interleave. Artists keep images apart, one file per thing, and packing them by
hand into one sheet, with each sprite's place written down, is slow and breaks whenever one
changes.

## Decision

### Packing at build time

A game names folders of images to pack in its `dream.toml`:

```toml
[atlases.things]
folder = "assets:/sprites/things"
padding = 1          # Optional, 0 to 16.
max_size = 2048      # Optional, 16 to 8192.
```

`dream_add_game` packs each with **`dream_atlas_cook`**, a tool of the engine's in
`tools/atlas_cook`, into `assets:/atlases/<name>.png` and `assets:/atlases/<name>.atlas` among the
generated assets, again whenever an image or the configuration changes. The tool:

- takes every PNG under the folder, in the order of their paths, so a build always makes the same
  files, and names each region by its path there without ".png": `hero`, `trees/oak`;
- packs them with stb_rect_pack (MIT or public domain, through vcpkg's `stb`, which the runtime
  already uses) into the smallest power-of-two image, wide before tall, up to `max_size`, and fails
  naming the folder when they don't fit;
- repeats each image's edge pixels over `padding` pixels around it, so smooth filtering near an
  edge reads the same image, not its neighbor;
- writes the regions as TOML, which the runtime reads with `core::Config`.

It needs only the standard library and stb, so it is quick to build. PC builds build it; Android's,
which cross-compile, take it from the `dream-atlas-cook` vcpkg port in `cmake/ports`, as the model
cook tool (ADR 0042). Unlike levels of detail, an atlas has no fallback at run time: a game that
names atlases without the tool fails to configure, saying so.

Staging leaves the packed folders' PNGs out of the game, which reads them from the atlas; a scene
that still names one fails the build in the asset database (ADR 0047), which also learns that a
`.atlas` file needs its image.

### Using a region

- `SpriteRenderer::region` is the part of its texture a sprite draws, in pixels from the top left.
  Zero draws all of it, as before. A sprite sheet's frames are cut from the region, so a region can
  be a character's whole sheet.
- `SpriteAtlasAsset`, loaded from a `.atlas` file through the AssetManager, holds the texture and
  the named regions. The Renderer registers its loader.
- `use_atlas_region(sprite, atlas, name)` sets a sprite's texture and region; a scene's sprite does
  the same with `atlas` and `region` in place of `texture`. A name the atlas lacks fails, listing
  some it has.

The platformer's hero, coins and sign moved into `sprites/things`, packed as one atlas. Its picture
matches its reference on the five PC graphics backends, as its pixels didn't change (on Vulkan, 3
pixels in one row differ by at most 8 levels, where a texture coordinate now falls on the other
side of a texel's edge). On a Galaxy A52s it draws the same picture with Vulkan and OpenGL ES, and
on a Galaxy A57 with Vulkan.

## Consequences

- Sprites from one atlas are drawn together however they interleave, with no change to the
  renderer or its shaders.
- Art stays one file per thing; the build keeps the atlas up to date.
- The editor reads a project's folders, not its build, so a game with atlases lists its generated
  assets folder in `[editor] assets` and is built once before editing.
- Tile maps and Tiled's tilesets keep images of their own; trimming images' empty edges, rotating
  them to pack tighter, and several pages for one atlas aren't done. Each would be added to the tool
  and the `.atlas` file without changing how sprites name regions.
