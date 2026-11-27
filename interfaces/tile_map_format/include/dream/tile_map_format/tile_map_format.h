#pragma once

#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::tile_map_format {

// Reads tile maps made in a map editor, such as Tiled (ADR 0050). The runtime turns what a module
// reads into tile maps, sprites and entities of the 2D world; a module only reads files, so it
// knows nothing of the world, the GPU or other files. Sizes and places are in the map's pixels,
// with Y down, as map editors give them.

// A tile in a layer or a tile object: its number among all of the map's tilesets, with these flips
// in its top bits, as Tiled writes them. 0 is no tile.
inline constexpr std::uint32_t k_flipped_horizontally = 0x80000000u;
inline constexpr std::uint32_t k_flipped_vertically = 0x40000000u;
// Mirrored across the diagonal from the top left to the bottom right, before the other flips.
inline constexpr std::uint32_t k_flipped_diagonally = 0x20000000u;
inline constexpr std::uint32_t k_tile_number = 0x0fffffffu;

// A custom property, with its value as text: "true", "3", "0.5", "#ff8000" or any string.
struct Property {
    std::string name;
    // "string", "int", "float", "bool", "color", "file" or "object".
    std::string type;
    std::string value;
};

struct AnimationFrame {
    // The tile shown, by its number within its tileset.
    std::uint32_t tile = 0;
    float seconds = 0.1f;
};

// What a tileset says about one of its tiles.
struct TileInfo {
    // Within its tileset, from 0.
    std::uint32_t id = 0;
    std::vector<AnimationFrame> animation;
    // Whether collision shapes were drawn on it in the map editor.
    bool has_collision = false;
    std::vector<Property> properties;
};

// An image cut into equal tiles.
struct Tileset {
    // The tileset's own file, relative to the map's, when the map only names it: the runtime reads
    // that file with read_tileset, and fills in the rest from it.
    std::string source;
    // The number of its first tile in the map's layers.
    std::uint32_t first_tile = 1;
    std::string name;
    // Relative to the file that names it.
    std::string image;
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
    std::uint32_t tile_width = 0;
    std::uint32_t tile_height = 0;
    std::uint32_t columns = 0;
    std::uint32_t tile_count = 0;
    // Pixels around the image's edge, and between tiles.
    std::uint32_t margin = 0;
    std::uint32_t spacing = 0;
    // Only the tiles it says something about.
    std::vector<TileInfo> tiles;
    std::vector<Property> properties;
};

enum class ObjectShape : std::uint8_t {
    Rectangle,
    Ellipse,
    Point,
    // Polygons and polylines, read as the rectangle around them.
    Polygon,
};

// A thing placed on an object layer: a spawn point, a trigger area, a door, or a tile placed
// freely, such as a tree.
struct MapObject {
    std::uint32_t id = 0;
    std::string name;
    // The class, or type, given in the map editor.
    std::string type;
    // The top left corner, except for tile objects, which are placed by their bottom left corner,
    // as map editors do.
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // In degrees, clockwise, around (x, y).
    float rotation = 0.0f;
    ObjectShape shape = ObjectShape::Rectangle;
    // A tile with its flips, for a tile object; otherwise 0.
    std::uint32_t tile = 0;
    bool visible = true;
    std::vector<Property> properties;
};

enum class LayerKind : std::uint8_t { Tiles, Objects, Image };

// One layer, in drawing order. Groups are flattened into their layers: their offsets, parallax,
// opacity, tint and visibility applied, and their names joined with '/'.
struct Layer {
    LayerKind kind = LayerKind::Tiles;
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    // Multiplies the layer's colors: red, green, blue, from 0 to 1, as sRGB.
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    // How far the layer moves as the camera moves: 1 as the map does.
    float parallax_x = 1.0f;
    float parallax_y = 1.0f;
    // For image layers: repeated across, or up and down.
    bool repeat_x = false;
    bool repeat_y = false;
    // Tiles: `columns` by `rows` of them, row by row from the top.
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::vector<std::uint32_t> tiles;
    // Objects.
    std::vector<MapObject> objects;
    // An image layer's image, relative to the map's file.
    std::string image;
    std::vector<Property> properties;
};

// An orthogonal map of equal tiles.
struct Map {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t tile_width = 0;
    std::uint32_t tile_height = 0;
    std::vector<Tileset> tilesets;
    std::vector<Layer> layers;
    std::vector<Property> properties;
};

class TileMapFormat {
public:
    static constexpr std::string_view k_module_interface = "tile_map_format";

    virtual ~TileMapFormat() = default;

    // Whether it reads the map file at `path`, by its name.
    [[nodiscard]] virtual bool reads(std::string_view path) const = 0;

    // Fails, saying why, for a file it can't read or a map the runtime can't draw, such as an
    // isometric one. `path` is for messages.
    [[nodiscard]] virtual core::Result<Map> read_map(std::span<const std::byte> file,
                                                     std::string_view path) = 0;

    // A tileset in a file of its own, which a map names in Tileset::source.
    [[nodiscard]] virtual core::Result<Tileset> read_tileset(std::span<const std::byte> file,
                                                             std::string_view path) = 0;
};

} // namespace dream::tile_map_format
