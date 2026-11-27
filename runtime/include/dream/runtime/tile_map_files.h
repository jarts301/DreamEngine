#pragma once

#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "dream/tile_map_format/tile_map_format.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// Tile maps made in a map editor, such as Tiled, read by the game's tile_map_format module and
// turned into the 2D world's tile maps, sprites and entities (ADR 0050). docs/guides/sprites.md
// explains how to make them.

// A map file with the textures it draws: its tilesets' images and its image layers'. Paths in
// `map` are virtual paths, such as "assets:/sprites/tiles.png", and tilesets kept in files of their
// own are read into it.
struct TileMapFileAsset {
    tile_map_format::Map map;
    // One per tileset of `map`, in order.
    std::vector<AssetRef<TextureAsset>> tilesets;
    // One per layer of `map`, in order; empty but for image layers.
    std::vector<AssetRef<TextureAsset>> images;
};

// A thing from one of a map's object layers, at its entity, which is named as the object is: its
// middle for areas, its foot for tile objects, its place for points. Games find spawn points,
// doors and triggers by name, or by `type`.
struct MapObject {
    // The class, or type, given in the map editor.
    std::string type;
    // In world units, along the entity's own axes.
    core::Vec2 size{};
    tile_map_format::ObjectShape shape = tile_map_format::ObjectShape::Rectangle;
    std::vector<tile_map_format::Property> properties;

    // A custom property's value as text, or `fallback` without it.
    [[nodiscard]] std::string_view property(std::string_view name,
                                            std::string_view fallback = {}) const;
    [[nodiscard]] float number(std::string_view name, float fallback = 0.0f) const;
    [[nodiscard]] bool flag(std::string_view name, bool fallback = false) const;
};

// How a map file becomes part of the world.
struct TileMapFileOptions {
    // Each tile's size in the world. Objects and images take the same scale from the map's pixels.
    core::Vec2 tile_size{1.0f, 1.0f};
    // The drawing layer of the map's first layer; each next one is a layer higher, unless it has an
    // integer property `layer` of its own.
    std::int32_t layer = 0;
    // The bool property that makes a tile solid, or a whole tile layer solid. Tiles with collision
    // shapes drawn in the editor are solid too.
    std::string solid = "solid";
    renderer::SpriteFilter filter = renderer::SpriteFilter::Pixel;
};

// On the entity a map file was added to: what it made, as its children.
struct TileMapFile {
    std::vector<Entity> layers;
    std::vector<Entity> objects;
};

// Adds a map file's layers and objects to the world, as children of `parent`, whose X and Y are the
// map's bottom left corner, and puts a TileMapFile on `parent`. Each tile layer becomes a TileMap,
// one per tileset it draws from; each image layer a sprite; and each object an entity with a
// MapObject, and a sprite for a tile object. Layers' offsets, opacity, tint, parallax and
// visibility, and tiles' flips and animations, come along.
[[nodiscard]] core::Result<void> add_tile_map_file(World& world, Entity parent,
                                                   const TileMapFileAsset& file,
                                                   const TileMapFileOptions& options = {});

class SceneLoader;

// For Renderer: loads map files through `format` as TileMapFileAssets, and lets scene files use
// "tile_map_file". Without a format, a scene naming a map file fails, saying which module to list.
void register_tile_map_files(SceneLoader& scenes, AssetManager& assets,
                             tile_map_format::TileMapFormat* format);

// `relative`, which a file at `path` names, as a path of its own: "../sprites/a.png" from
// "assets:/maps/level.tmj" is "assets:/sprites/a.png". Fails for a path that leaves the root.
[[nodiscard]] core::Result<std::string> path_beside(std::string_view path,
                                                    std::string_view relative);

} // namespace dream::runtime
