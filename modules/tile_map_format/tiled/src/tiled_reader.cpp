// Reads maps and tilesets saved by Tiled (https://www.mapeditor.org/) in its JSON format, .tmj and
// .tsj, with yyjson (ADR 0050).

#include "dream/core/module_registry.h"
#include "dream/tile_map_format/tile_map_format.h"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace dream::modules {
namespace {

using namespace tile_map_format;

core::Error parse_error(std::string_view path, std::string_view message) {
    return core::Error{core::ErrorCode::ParseError, std::format("'{}': {}", path, message)};
}

// The parsed document, freed when it goes.
struct Document {
    yyjson_doc* doc = nullptr;
    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document() {
        yyjson_doc_free(doc);
    }
};

core::Result<yyjson_val*> parse(Document& document, std::span<const std::byte> file,
                                std::string_view path) {
    yyjson_read_err error{};
    document.doc = yyjson_read_opts(const_cast<char*>(reinterpret_cast<const char*>(file.data())),
                                    file.size(), YYJSON_READ_NOFLAG, nullptr, &error);
    if (document.doc == nullptr) {
        return parse_error(path, std::format("not JSON, at byte {}: {}", error.pos,
                                             error.msg != nullptr ? error.msg : "?"));
    }
    yyjson_val* const root = yyjson_doc_get_root(document.doc);
    if (!yyjson_is_obj(root)) {
        return parse_error(path, "not a Tiled JSON file");
    }
    return root;
}

double number(yyjson_val* object, const char* key, double fallback = 0.0) {
    yyjson_val* const value = yyjson_obj_get(object, key);
    return yyjson_is_num(value) ? yyjson_get_num(value) : fallback;
}

std::uint32_t whole(yyjson_val* object, const char* key, std::uint32_t fallback = 0) {
    const double value = number(object, key, fallback);
    return value >= 0.0 && value < 4294967296.0 ? static_cast<std::uint32_t>(value) : fallback;
}

float decimal(yyjson_val* object, const char* key, float fallback = 0.0f) {
    return static_cast<float>(number(object, key, fallback));
}

std::string text(yyjson_val* object, const char* key) {
    yyjson_val* const value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) ? std::string(yyjson_get_str(value), yyjson_get_len(value))
                                : std::string();
}

bool flag(yyjson_val* object, const char* key, bool fallback) {
    yyjson_val* const value = yyjson_obj_get(object, key);
    return yyjson_is_bool(value) ? yyjson_get_bool(value) : fallback;
}

// Any value as text, numbers without a trailing ".0" when they are whole, and objects, such as
// class properties' values, as their JSON.
std::string as_text(yyjson_val* value) {
    if (yyjson_is_str(value)) {
        return std::string(yyjson_get_str(value), yyjson_get_len(value));
    }
    if (yyjson_is_bool(value)) {
        return yyjson_get_bool(value) ? "true" : "false";
    }
    if (yyjson_is_int(value)) {
        return std::to_string(yyjson_get_sint(value));
    }
    if (yyjson_is_num(value)) {
        return std::format("{}", yyjson_get_num(value));
    }
    if (value == nullptr || yyjson_is_null(value)) {
        return {};
    }
    std::size_t length = 0;
    char* const json = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &length);
    std::string written = json != nullptr ? std::string(json, length) : std::string();
    std::free(json); // NOLINT: yyjson's default allocator is malloc.
    return written;
}

std::vector<Property> properties(yyjson_val* object) {
    std::vector<Property> read;
    yyjson_val* const list = yyjson_obj_get(object, "properties");
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* entry = nullptr;
    yyjson_arr_foreach(list, index, count, entry) {
        std::string type = text(entry, "type");
        read.push_back(Property{.name = text(entry, "name"),
                                .type = type.empty() ? "string" : std::move(type),
                                .value = as_text(yyjson_obj_get(entry, "value"))});
    }
    return read;
}

// "#rrggbb" or "#aarrggbb" as red, green and blue from 0 to 1, and opacity.
bool read_color(std::string_view color, std::array<float, 4>& into) {
    if (color.starts_with('#')) {
        color.remove_prefix(1);
    }
    if (color.size() != 6 && color.size() != 8) {
        return false;
    }
    std::array<unsigned, 4> parts{255, 0, 0, 0};
    const std::size_t first = color.size() == 8 ? 0 : 1;
    for (std::size_t part = first; part < 4; ++part) {
        const std::string_view digits = color.substr((part - first) * 2, 2);
        unsigned value = 0;
        for (const char digit : digits) {
            value *= 16;
            if (digit >= '0' && digit <= '9') {
                value += static_cast<unsigned>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value += static_cast<unsigned>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value += static_cast<unsigned>(digit - 'A' + 10);
            } else {
                return false;
            }
        }
        parts[part] = value;
    }
    into = {static_cast<float>(parts[1]) / 255.0f, static_cast<float>(parts[2]) / 255.0f,
            static_cast<float>(parts[3]) / 255.0f, static_cast<float>(parts[0]) / 255.0f};
    return true;
}

core::Result<Tileset> read_tileset_object(yyjson_val* object, std::string_view path) {
    Tileset tileset;
    tileset.first_tile = whole(object, "firstgid", 1);
    tileset.source = text(object, "source");
    if (!tileset.source.empty()) {
        if (tileset.source.ends_with(".tsx")) {
            return parse_error(path, std::format("its tileset '{}' is in Tiled's XML format: "
                                                 "export it as JSON (.tsj) instead",
                                                 tileset.source));
        }
        return tileset;
    }
    tileset.name = text(object, "name");
    tileset.image = text(object, "image");
    if (tileset.image.empty()) {
        return parse_error(path, std::format("its tileset '{}' is a collection of images, which "
                                             "isn't read: use one image cut into tiles",
                                             tileset.name));
    }
    tileset.image_width = whole(object, "imagewidth");
    tileset.image_height = whole(object, "imageheight");
    tileset.tile_width = whole(object, "tilewidth");
    tileset.tile_height = whole(object, "tileheight");
    tileset.columns = whole(object, "columns");
    tileset.tile_count = whole(object, "tilecount");
    tileset.margin = whole(object, "margin");
    tileset.spacing = whole(object, "spacing");
    if (tileset.tile_width == 0 || tileset.tile_height == 0) {
        return parse_error(path, std::format("its tileset '{}' has no tile size", tileset.name));
    }
    if (tileset.columns == 0) {
        tileset.columns = std::max(tileset.image_width / tileset.tile_width, 1u);
    }
    tileset.properties = properties(object);

    yyjson_val* const tiles = yyjson_obj_get(object, "tiles");
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* entry = nullptr;
    yyjson_arr_foreach(tiles, index, count, entry) {
        TileInfo info{.id = whole(entry, "id")};
        yyjson_val* const frames = yyjson_obj_get(entry, "animation");
        std::size_t frame_index = 0;
        std::size_t frame_count = 0;
        yyjson_val* frame = nullptr;
        yyjson_arr_foreach(frames, frame_index, frame_count, frame) {
            info.animation.push_back(AnimationFrame{
                .tile = whole(frame, "tileid"),
                .seconds = std::max(decimal(frame, "duration", 100.0f), 1.0f) / 1000.0f});
        }
        yyjson_val* const shapes = yyjson_obj_get(yyjson_obj_get(entry, "objectgroup"), "objects");
        info.has_collision = yyjson_arr_size(shapes) > 0;
        info.properties = properties(entry);
        tileset.tiles.push_back(std::move(info));
    }
    return tileset;
}

// Base64, as Tiled writes layer data with "encoding": "base64".
bool decode_base64(std::string_view encoded, std::vector<std::uint8_t>& into) {
    const auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::uint32_t bits = 0;
    int held = 0;
    for (const char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        const int value = value_of(c);
        if (value < 0) {
            return false;
        }
        bits = (bits << 6) | static_cast<std::uint32_t>(value);
        held += 6;
        if (held >= 8) {
            held -= 8;
            into.push_back(static_cast<std::uint8_t>((bits >> held) & 0xffu));
        }
    }
    return true;
}

// What a layer inherits from the groups around it.
struct Inherited {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float parallax_x = 1.0f;
    float parallax_y = 1.0f;
};

core::Result<void> read_layers(yyjson_val* list, const Inherited& outer, Map& map,
                               std::string_view path, int depth) {
    if (depth > 32) {
        return parse_error(path, "its groups are nested too deeply");
    }
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* object = nullptr;
    yyjson_arr_foreach(list, index, count, object) {
        const std::string type = text(object, "type");
        Inherited here = outer;
        const std::string name = text(object, "name");
        here.name = outer.name.empty() ? name : outer.name + "/" + name;
        here.visible = outer.visible && flag(object, "visible", true);
        here.opacity = outer.opacity * decimal(object, "opacity", 1.0f);
        here.offset_x = outer.offset_x + decimal(object, "offsetx");
        here.offset_y = outer.offset_y + decimal(object, "offsety");
        here.parallax_x = outer.parallax_x * decimal(object, "parallaxx", 1.0f);
        here.parallax_y = outer.parallax_y * decimal(object, "parallaxy", 1.0f);
        if (const std::string tint = text(object, "tintcolor"); !tint.empty()) {
            std::array<float, 4> color{};
            if (!read_color(tint, color)) {
                return parse_error(path, std::format("layer '{}' has a tint '{}' that isn't a "
                                                     "color",
                                                     here.name, tint));
            }
            for (std::size_t channel = 0; channel < 3; ++channel) {
                here.tint[channel] *= color[channel];
            }
            here.opacity *= color[3];
        }
        if (type == "group") {
            if (auto read = read_layers(yyjson_obj_get(object, "layers"), here, map, path,
                                        depth + 1);
                !read.has_value()) {
                return read;
            }
            continue;
        }

        Layer layer{.name = here.name,
                    .visible = here.visible,
                    .opacity = here.opacity,
                    .tint = {here.tint[0], here.tint[1], here.tint[2]},
                    .offset_x = here.offset_x,
                    .offset_y = here.offset_y,
                    .parallax_x = here.parallax_x,
                    .parallax_y = here.parallax_y,
                    .properties = properties(object)};
        if (type == "tilelayer") {
            layer.kind = LayerKind::Tiles;
            if (yyjson_obj_get(object, "chunks") != nullptr) {
                return parse_error(path, "it is an infinite map, which isn't read: untick "
                                         "\"Infinite\" in the map's properties");
            }
            layer.columns = whole(object, "width");
            layer.rows = whole(object, "height");
            const std::size_t cells = std::size_t{layer.columns} * layer.rows;
            yyjson_val* const data = yyjson_obj_get(object, "data");
            if (yyjson_is_arr(data)) {
                layer.tiles.reserve(cells);
                std::size_t cell = 0;
                std::size_t cell_count = 0;
                yyjson_val* tile = nullptr;
                yyjson_arr_foreach(data, cell, cell_count, tile) {
                    const double value = yyjson_is_num(tile) ? yyjson_get_num(tile) : -1.0;
                    if (value < 0.0 || value >= 4294967296.0) {
                        return parse_error(path, std::format("layer '{}' has a cell that isn't "
                                                             "a tile",
                                                             layer.name));
                    }
                    layer.tiles.push_back(static_cast<std::uint32_t>(value));
                }
            } else if (yyjson_is_str(data)) {
                const std::string compression = text(object, "compression");
                if (text(object, "encoding") != "base64" || !compression.empty()) {
                    return parse_error(
                        path, std::format("layer '{}' is compressed with {}, which isn't read: "
                                          "save layers as CSV or uncompressed Base64",
                                          layer.name, compression.empty() ? "?" : compression));
                }
                std::vector<std::uint8_t> bytes;
                if (!decode_base64(std::string_view(yyjson_get_str(data), yyjson_get_len(data)),
                                   bytes) ||
                    bytes.size() != cells * 4) {
                    return parse_error(path, std::format("layer '{}' has broken Base64 data",
                                                         layer.name));
                }
                layer.tiles.resize(cells);
                for (std::size_t cell = 0; cell < cells; ++cell) {
                    layer.tiles[cell] = std::uint32_t{bytes[cell * 4]} |
                                        (std::uint32_t{bytes[cell * 4 + 1]} << 8) |
                                        (std::uint32_t{bytes[cell * 4 + 2]} << 16) |
                                        (std::uint32_t{bytes[cell * 4 + 3]} << 24);
                }
            }
            if (layer.tiles.size() != cells) {
                return parse_error(path, std::format("layer '{}' has {} cells, not its {} by {}",
                                                     layer.name, layer.tiles.size(),
                                                     layer.columns, layer.rows));
            }
        } else if (type == "objectgroup") {
            layer.kind = LayerKind::Objects;
            yyjson_val* const objects = yyjson_obj_get(object, "objects");
            std::size_t object_index = 0;
            std::size_t object_count = 0;
            yyjson_val* entry = nullptr;
            yyjson_arr_foreach(objects, object_index, object_count, entry) {
                std::string kind = text(entry, "class");
                if (kind.empty()) {
                    kind = text(entry, "type");
                }
                MapObject placed{.id = whole(entry, "id"),
                                 .name = text(entry, "name"),
                                 .type = std::move(kind),
                                 .x = decimal(entry, "x"),
                                 .y = decimal(entry, "y"),
                                 .width = decimal(entry, "width"),
                                 .height = decimal(entry, "height"),
                                 .rotation = decimal(entry, "rotation"),
                                 .shape = ObjectShape::Rectangle,
                                 .tile = whole(entry, "gid"),
                                 .visible = flag(entry, "visible", true),
                                 .properties = properties(entry)};
                if (flag(entry, "point", false)) {
                    placed.shape = ObjectShape::Point;
                } else if (flag(entry, "ellipse", false)) {
                    placed.shape = ObjectShape::Ellipse;
                } else if (yyjson_val* const points = yyjson_is_arr(yyjson_obj_get(entry, "polygon"))
                                                          ? yyjson_obj_get(entry, "polygon")
                                                          : yyjson_obj_get(entry, "polyline");
                           yyjson_is_arr(points)) {
                    // The rectangle around the points, which are relative to the object's place.
                    float low_x = 0.0f;
                    float low_y = 0.0f;
                    float high_x = 0.0f;
                    float high_y = 0.0f;
                    std::size_t point_index = 0;
                    std::size_t point_count = 0;
                    yyjson_val* point = nullptr;
                    yyjson_arr_foreach(points, point_index, point_count, point) {
                        const float px = decimal(point, "x");
                        const float py = decimal(point, "y");
                        low_x = std::min(low_x, px);
                        low_y = std::min(low_y, py);
                        high_x = std::max(high_x, px);
                        high_y = std::max(high_y, py);
                    }
                    placed.shape = ObjectShape::Polygon;
                    placed.x += low_x;
                    placed.y += low_y;
                    placed.width = high_x - low_x;
                    placed.height = high_y - low_y;
                }
                layer.objects.push_back(std::move(placed));
            }
        } else if (type == "imagelayer") {
            layer.kind = LayerKind::Image;
            layer.image = text(object, "image");
            layer.repeat_x = flag(object, "repeatx", false);
            layer.repeat_y = flag(object, "repeaty", false);
            if (layer.image.empty()) {
                continue;
            }
        } else {
            return parse_error(path, std::format("layer '{}' is of a kind not read: '{}'",
                                                 layer.name, type));
        }
        map.layers.push_back(std::move(layer));
    }
    return {};
}

class TiledReader final : public TileMapFormat {
public:
    [[nodiscard]] bool reads(std::string_view path) const override {
        return path.ends_with(".tmj") || path.ends_with(".json");
    }

    core::Result<Map> read_map(std::span<const std::byte> file, std::string_view path) override {
        Document document;
        const core::Result<yyjson_val*> root = parse(document, file, path);
        if (!root.has_value()) {
            return root.error();
        }
        yyjson_val* const object = root.value();
        if (text(object, "type") != "map") {
            return parse_error(path, "not a Tiled map: save it from Tiled as JSON (.tmj)");
        }
        if (const std::string orientation = text(object, "orientation");
            orientation != "orthogonal") {
            return parse_error(path, std::format("its orientation is '{}': only orthogonal maps "
                                                 "are read",
                                                 orientation));
        }
        if (flag(object, "infinite", false)) {
            return parse_error(path, "it is an infinite map, which isn't read: untick "
                                     "\"Infinite\" in the map's properties");
        }
        Map map{.columns = whole(object, "width"),
                .rows = whole(object, "height"),
                .tile_width = whole(object, "tilewidth"),
                .tile_height = whole(object, "tileheight"),
                .properties = properties(object)};
        if (map.columns == 0 || map.rows == 0 || map.tile_width == 0 || map.tile_height == 0) {
            return parse_error(path, "it has no size, or no tile size");
        }
        yyjson_val* const tilesets = yyjson_obj_get(object, "tilesets");
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* entry = nullptr;
        yyjson_arr_foreach(tilesets, index, count, entry) {
            core::Result<Tileset> tileset = read_tileset_object(entry, path);
            if (!tileset.has_value()) {
                return tileset.error();
            }
            map.tilesets.push_back(std::move(tileset).value());
        }
        std::ranges::sort(map.tilesets, {}, &Tileset::first_tile);
        if (auto read = read_layers(yyjson_obj_get(object, "layers"), Inherited{}, map, path, 0);
            !read.has_value()) {
            return read.error();
        }
        return map;
    }

    core::Result<Tileset> read_tileset(std::span<const std::byte> file,
                                       std::string_view path) override {
        Document document;
        const core::Result<yyjson_val*> root = parse(document, file, path);
        if (!root.has_value()) {
            return root.error();
        }
        if (const std::string type = text(root.value(), "type"); !type.empty() && type != "tileset") {
            return parse_error(path, "not a Tiled tileset: save it from Tiled as JSON (.tsj)");
        }
        return read_tileset_object(root.value(), path);
    }
};

core::Result<std::unique_ptr<TileMapFormat>> create(const core::Config& /*config*/) {
    return std::unique_ptr<TileMapFormat>{std::make_unique<TiledReader>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_tile_map_format_tiled(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<tile_map_format::TileMapFormat>{.name = "tiled", .create = &create});
}

} // namespace dream::modules
