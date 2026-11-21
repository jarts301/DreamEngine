# 0024. Cooked textures and packaged assets

- Status: Accepted
- Date: 2026-11-17

## Context

Games shipped their textures as PNG files, decoded on the CPU when loaded and sent to the GPU as
four bytes per pixel, with mipmaps made by the device. Phones and PCs each read a compressed format
directly, at a quarter of the memory or less: ASTC on phones and BC7 on PCs, and neither runs on the
other. A PC game also shipped its assets as a folder of loose files.

## Decision

### One cooked file for every GPU

The build cooks each texture once into a KTX2 file of UASTC data, Basis Universal's
high-quality mode, with every mip level made and the whole compressed with zstd. When a game loads
it, libktx transcodes it to the best format the device takes: BC7, then ASTC 4x4, then RGBA8.
Transcoding UASTC to BC7 or ASTC is fast and keeps nearly all of the quality, so one file serves
every platform, and a new GPU format needs only a new transcode target.

Cooking runs `ktx create` from KTX-Software, the Khronos tools, which vcpkg builds for the build
machine like `slangc`, so Android builds cook too. The runtime links libktx. Both are Apache 2.0.

### What gets cooked

PNG files in a folder named `textures`, anywhere in a game's asset folders, all as sRGB colors:

```
assets/models/crate.glb
assets/models/textures/crate.png   ->   crate.ktx2 in the game's assets, and no crate.png
```

The game's assets are staged into one folder at build time: the source folders, without the PNGs
that were cooked, then the compiled shaders and cooked textures. PC builds stage next to the
program, and the APK packages the staged folder. `-DDREAM_COOK_TEXTURES=OFF` ships the PNGs as
they are, to tell whether a problem comes from cooking.

Models refer to their textures by file name, as glTF allows, so the sample models now keep their
textures in `models/textures` rather than inside the `.glb`. When a model names `x.png` and
`x.ktx2` exists beside it, the loader takes the cooked one. Models that use the
`KHR_texture_basisu` extension, or embed KTX2 images, load the same way. Embedded PNG and JPEG
images still load as before.

### The graphics interface

`rhi::TextureFormat` gains `Bc7Srgb`, `Bc7Unorm`, `Astc4x4Srgb` and `Astc4x4Unorm`, and
`TextureDesc::mip_levels` says how many levels the pixels hold. `Device::supports_texture_format`
says which formats a device takes. Vulkan turns on BC or ASTC where the GPU has them, Direct3D 11
and 12 take BC7, and the null device takes everything. Every backend checks a texture with the
same `rhi::texture_problem`; compressed textures must be a whole number of 4x4 blocks wide and
high, which Direct3D requires, and other sizes are transcoded to RGBA8.

### Packaging

On PC, each game has a `<name>_package` target that puts the program and `assets.pak` in a
folder of their own, ready to hand out. `assets.pak` holds every staged asset uncompressed, since
the textures already are, with an index read when the first asset is. The SDL platform reads assets
from it when it is next to the program, and from the assets folder otherwise. `dream_pack` writes
it, and `core::ArchiveReader` and `ArchiveWriter` hold the format. On Android, the APK is the
package.

ADR 0043 makes this one of several texture cookers, `ktx2_uastc`, the default, which a project
chooses per platform in its `[cook]` table.

## Consequences

- The samples' textures load as BC7 on an RTX 5080, with all three graphics APIs, and as ASTC 4x4
  on the Galaxy A57; the logs say which at startup. The screenshot references still match.
- On the GPU, a 64x64 texture with all its mipmaps takes 5.5 KB as BC7 or ASTC, against 21 KB as
  RGBA8. Cooked, the sample crate's file is 2.7 KB.
- Textures that aren't colors, such as normal maps, would need a linear cook; none exist yet.
- Images an interface document loads, through RmlUi or markup, aren't cooked: they are read as PNG.
- Every game links libktx, with its transcoder, which makes it larger.
