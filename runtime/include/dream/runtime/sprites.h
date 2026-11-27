#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// 2D sprites and tile maps (ADR 0039). The 2D world is the XY plane, +Y up, in world units, which
// 2D physics uses too. Renderer draws every SpriteRenderer and TileMap through the world's first
// Camera2D, over the 3D scene if there is one, and under the interface. Renderer::initialize lets
// scene files use these components; docs/guides/sprites.md explains them.

// Looks at the 2D world from its entity's X and Y, turned by its rotation around Z.
struct Camera2D {
    // How much of the world is seen from the bottom of the window to the top, in world units. The
    // width follows the window's shape.
    float height = 10.0f;
    // At least this much is seen across, in world units: on a narrow window, such as a phone held
    // upright, the view grows taller to keep it. 0 for none.
    float min_width = 0.0f;
    // Layers drawn by height, as top-down games need: within them, what stands lower in the world
    // covers what stands higher, before `order` is looked at. A sprite stands at its entity, so
    // its pivot belongs at its feet; a tile map's tiles stand at the bottom of their row.
    std::vector<std::int32_t> y_sort_layers;
};

// Draws a rectangle of a texture at its entity: its X and Y, its rotation around Z, and its scale
// in X and Y.
struct SpriteRenderer {
    // sRGB colors with straight alpha. Without one, a white rectangle tinted by `color`.
    AssetRef<TextureAsset> texture;
    // The texture as a grid of equal frames, numbered from the top left, row by row: a sprite
    // sheet. `frame` picks one; SpriteAnimator changes it.
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    std::uint32_t frame = 0;
    // In world units. Zero takes the frame's size in pixels over `pixels_per_unit`.
    core::Vec2 size{};
    float pixels_per_unit = 16.0f;
    // The point placed at the entity, from {0, 0} at the bottom left to {1, 1} at the top right.
    core::Vec2 pivot{0.5f, 0.5f};
    // 9-slice borders in the frame's pixels: left, right, top and bottom. When any is above 0, the
    // corners keep their size in pixels over `pixels_per_unit` however large the sprite is, the
    // edges stretch along their length, and the middle fills the rest: panels, frames and bars.
    core::Vec4 slice{};
    // Linear, multiplying the texture, with its opacity in w.
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    bool flip_x = false;
    bool flip_y = false;
    // Drawn in order of layer, then of order within a layer: later ones cover earlier ones. Tile
    // maps share the same order. Equal ones keep an order of their own from frame to frame.
    std::int32_t layer = 0;
    std::int32_t order = 0;
    renderer::SpriteFilter filter = renderer::SpriteFilter::Pixel;
    bool visible = true;
};

// A named run of a sprite sheet's frames.
struct SpriteClip {
    std::string name;
    std::vector<std::uint32_t> frames;
    // Frames per second.
    float fps = 10.0f;
    bool loop = true;
};

// Plays its clips on the entity's SpriteRenderer, as update_sprite_animators moves them on.
struct SpriteAnimator {
    std::vector<SpriteClip> clips;
    // The playing clip's name. Empty, or a name it doesn't have, leaves the frame alone.
    std::string clip;
    // Seconds since the clip started.
    float time = 0.0f;
    float speed = 1.0f;
    // False once a clip that doesn't loop has shown its last frame.
    bool playing = true;

    // Starts `name` from its first frame, unless it is already playing.
    void play(std::string_view name);
    // The clip's length in seconds, or 0 if it has no such clip.
    [[nodiscard]] float duration(std::string_view name) const;
};

// Moves its entity's SpriteRenderer or TileMap with the camera by less, or more, than the rest of
// the world, and can repeat it across the view: backgrounds that seem far away. Where the camera
// is at the world's origin, it is drawn where its entity is.
struct Parallax {
    // How far it moves across the screen as the camera moves, in X and Y: {1, 1} as the world does,
    // {0.25, 1} a quarter as far across, as distant hills, and {0, 0} not at all, fixed on screen.
    core::Vec2 factor{1.0f, 1.0f};
    // Repeated side by side, or above and below, every its own width or height, to fill the view.
    // A repeated sprite repeats as though it weren't turned.
    bool repeat_x = false;
    bool repeat_y = false;
};

// A tile shown as a run of other tiles, in turn: water, torches, flowers in the wind.
struct TileAnimation {
    std::int32_t tile = 0;
    std::vector<std::int32_t> frames;
    // Frames per second.
    float fps = 4.0f;
    // Each frame's own length in seconds, in place of `fps`, as map editors give them. Empty for
    // equal frames.
    std::vector<float> durations;
};

// How a tile map's cell is mirrored, in TileMap::flips.
enum TileFlip : std::uint8_t {
    k_tile_flip_x = 1,
    k_tile_flip_y = 2,
    // Mirrored across the diagonal from the top left to the bottom right, before the other two: with
    // them, the quarter turns.
    k_tile_flip_diagonal = 4,
};

// A grid of tiles from a tileset: a texture cut into equal tiles, numbered from the top left, row
// by row. The map's bottom left corner is at its entity's X and Y; it isn't turned or scaled.
struct TileMap {
    AssetRef<TextureAsset> tileset;
    // Each tile's size in the tileset, in pixels.
    std::uint32_t tile_width = 16;
    std::uint32_t tile_height = 16;
    // Pixels around the tileset's edge, and between its tiles.
    std::uint32_t tile_margin = 0;
    std::uint32_t tile_spacing = 0;
    // Each tile's size in the world.
    core::Vec2 tile_size{1.0f, 1.0f};
    // The map's size in tiles.
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    // Each cell's tile, row by row from the top: rows * columns of them. Negative is empty.
    std::vector<std::int32_t> tiles;
    // The tiles that make_tile_colliders turns into static 2D physics boxes.
    std::vector<std::int32_t> solid;
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::int32_t layer = 0;
    std::int32_t order = 0;
    renderer::SpriteFilter filter = renderer::SpriteFilter::Pixel;
    bool visible = true;
    // Animated tiles: cells holding `tile` show its frames instead. Cells and collisions keep the
    // tile itself.
    std::vector<TileAnimation> animations;
    // Seconds into the animations, which update_sprite_animators moves on by `animation_speed`.
    double animation_time = 0.0;
    float animation_speed = 1.0f;
    // Each cell's TileFlip bits, in the order of `tiles`, or empty for none.
    std::vector<std::uint8_t> flips;

    // The tile at a cell, counted from the top left, or -1 outside the map.
    [[nodiscard]] std::int32_t tile(std::uint32_t column, std::uint32_t row) const noexcept;
    void set_tile(std::uint32_t column, std::uint32_t row, std::int32_t tile) noexcept;
    // The cell at a point of the world, given where the map's bottom left corner is, or nothing
    // outside the map.
    [[nodiscard]] std::optional<std::array<std::uint32_t, 2>> cell_at(core::Vec2 origin,
                                                                      core::Vec2 point) const;
};

// The world's first Camera2D, from the 2D world to clip space for a window of `size`, or nothing
// without one or with an empty window.
[[nodiscard]] std::optional<core::Mat4> camera_2d_view(const World& world,
                                                       platform::SurfaceSize size);

// Where a window pixel, from its top left, falls in the 2D world, through the first Camera2D.
[[nodiscard]] std::optional<core::Vec2> screen_to_world_2d(const World& world,
                                                           platform::SurfaceSize size,
                                                           core::Vec2 pixel);

// Moves every SpriteAnimator on by `seconds` and sets its SpriteRenderer's frame, and moves tile
// maps' animated tiles on. Games call it once per frame, before rendering.
void update_sprite_animators(World& world, double seconds);

// Adds static 2D physics boxes over the tile map's solid tiles, with neighbors merged into larger
// boxes, as entities of their own, not children of the map, which Physics2D picks up. Given an
// entity with a map file (TileMapFile), it does so for each of the file's tile layers. Call it after
// World::update_transforms, once the map is placed, and again after changing it, having removed
// the entities it returned before.
[[nodiscard]] core::Result<std::vector<Entity>> make_tile_colliders(World& world, Entity map);

class SceneLoader;

// For Renderer: lets scene files use "camera_2d", "sprite", "sprite_animator", "tile_map" and
// "parallax".
void register_sprite_components(SceneLoader& scenes);

// The lists gather_sprites works with, which its caller keeps between calls, so gathering allocates
// nothing once they have grown.
struct SpriteScratch {
    // A tile map drawn whole, rather than one of its rows.
    static constexpr std::uint32_t k_whole_map = 0xffffffffu;
    // Something drawn, in order: layer, height in layers sorted by it, order, then a sprite before
    // a map, then the entity, then a map's row.
    struct Item {
        std::int32_t layer = 0;
        // Minus the height it stands at, in a layer sorted by height; otherwise 0.
        float depth = 0.0f;
        std::int32_t order = 0;
        bool map = false;
        std::uint32_t id = 0;
        Entity entity = k_no_entity;
        // A tile map's row, counted up from its bottom, in a layer sorted by height.
        std::uint32_t row = k_whole_map;
    };
    // Quads in a row that one batch draws.
    struct Run {
        std::size_t first = 0;
        std::size_t count = 0;
        rhi::TextureHandle texture;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        renderer::SpriteFilter filter = renderer::SpriteFilter::Pixel;
    };
    std::vector<Item> items;
    std::vector<Run> runs;
};

// For Renderer: the world's sprites and the tiles of its tile maps that `view` sees, in drawing
// order, as quads and the batches that draw them, which point into `quads`.
void gather_sprites(const World& world, const core::Mat4& view,
                    std::vector<renderer::SpriteQuad>& quads,
                    std::vector<renderer::SpriteBatch>& batches, SpriteScratch& scratch);

} // namespace dream::runtime
