#include "dream/runtime/tile_map_files.h"

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/scene_loader.h"
#include "dream/runtime/sprites.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

namespace dream::runtime {
namespace {

using core::Vec2;
using core::Vec3;
using core::Vec4;
using namespace tile_map_format;

const Property* find_property(const std::vector<Property>& properties, std::string_view name) {
    const auto found = std::ranges::find(properties, name, &Property::name);
    return found != properties.end() ? &*found : nullptr;
}

bool property_flag(const std::vector<Property>& properties, std::string_view name) {
    const Property* const found = find_property(properties, name);
    return found != nullptr && found->value == "true";
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// The tileset a tile number belongs to: the last whose first tile is at or below it.
std::size_t tileset_of(const Map& map, std::uint32_t tile) {
    std::size_t found = map.tilesets.size();
    for (std::size_t index = 0; index < map.tilesets.size(); ++index) {
        if (map.tilesets[index].first_tile <= tile) {
            found = index;
        }
    }
    return found;
}

std::uint8_t flips_of(std::uint32_t tile) {
    return static_cast<std::uint8_t>(((tile & k_flipped_horizontally) != 0 ? k_tile_flip_x : 0) |
                                     ((tile & k_flipped_vertically) != 0 ? k_tile_flip_y : 0) |
                                     ((tile & k_flipped_diagonally) != 0 ? k_tile_flip_diagonal
                                                                         : 0));
}

std::vector<TileAnimation> animations_of(const Tileset& tileset) {
    std::vector<TileAnimation> animations;
    for (const TileInfo& info : tileset.tiles) {
        if (info.animation.empty()) {
            continue;
        }
        TileAnimation animation{.tile = static_cast<std::int32_t>(info.id),
                                .frames = {},
                                .fps = 1.0f / std::max(info.animation.front().seconds, 0.001f),
                                .durations = {}};
        for (const AnimationFrame& frame : info.animation) {
            animation.frames.push_back(static_cast<std::int32_t>(frame.tile));
            animation.durations.push_back(frame.seconds);
        }
        animations.push_back(std::move(animation));
    }
    return animations;
}

core::Error file_error(std::string_view path, std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::format("'{}': {}", path, message)};
}

core::Result<TileMapFileAsset> load_map_file(AssetLoadContext& context, TileMapFormat* format) {
    if (format == nullptr) {
        return file_error(context.path, "the game reads no map files; list a tile_map_format "
                                        "module, such as \"tiled\", in its TOML file");
    }
    const core::Result<std::vector<std::byte>> bytes = context.files.read(context.path);
    if (!bytes.has_value()) {
        return bytes.error();
    }
    core::Result<Map> read = format->read_map(bytes.value(), context.path);
    if (!read.has_value()) {
        return read.error();
    }
    TileMapFileAsset file{.map = std::move(read).value(), .tilesets = {}, .images = {}};

    const auto load_texture = [&](std::string_view path) -> core::Result<AssetRef<TextureAsset>> {
        core::Result<AssetRef<TextureAsset>> texture = context.assets.load<TextureAsset>(path);
        if (!texture.has_value()) {
            return file_error(context.path, std::format("its image '{}': {}", path,
                                                        texture.error().message));
        }
        return texture;
    };
    for (Tileset& tileset : file.map.tilesets) {
        std::string beside = std::string(context.path);
        if (!tileset.source.empty()) {
            core::Result<std::string> source = path_beside(context.path, tileset.source);
            if (!source.has_value()) {
                return source.error();
            }
            const core::Result<std::vector<std::byte>> own = context.files.read(source.value());
            if (!own.has_value()) {
                return file_error(context.path, std::format("its tileset '{}': {}", source.value(),
                                                            own.error().message));
            }
            core::Result<Tileset> loaded = format->read_tileset(own.value(), source.value());
            if (!loaded.has_value()) {
                return loaded.error();
            }
            const std::uint32_t first_tile = tileset.first_tile;
            tileset = std::move(loaded).value();
            tileset.first_tile = first_tile;
            tileset.source = source.value();
            beside = source.value();
        }
        core::Result<std::string> image = path_beside(beside, tileset.image);
        if (!image.has_value()) {
            return image.error();
        }
        tileset.image = image.value();
        core::Result<AssetRef<TextureAsset>> texture = load_texture(tileset.image);
        if (!texture.has_value()) {
            return texture.error();
        }
        file.tilesets.push_back(std::move(texture).value());
    }
    for (Layer& layer : file.map.layers) {
        AssetRef<TextureAsset> texture;
        if (layer.kind == LayerKind::Image) {
            core::Result<std::string> image = path_beside(context.path, layer.image);
            if (!image.has_value()) {
                return image.error();
            }
            layer.image = image.value();
            core::Result<AssetRef<TextureAsset>> loaded = load_texture(layer.image);
            if (!loaded.has_value()) {
                return loaded.error();
            }
            texture = std::move(loaded).value();
        }
        file.images.push_back(std::move(texture));
    }
    return file;
}

core::Result<void> read_tile_map_file(ComponentContext& context, TileMapFormat* format) {
    const ComponentData& data = context.data;
    if (auto known = data.check_keys({"file", "tile_size", "layer", "solid", "filter"});
        !known.has_value()) {
        return known;
    }
    TileMapFileOptions options;
    std::string path;
    std::string filter = "pixel";
    SettingReader reader(data);
    reader.take([&] { return data.text("file"); }, path);
    reader.vec2("tile_size", options.tile_size);
    reader.integer("layer", options.layer);
    reader.text("solid", options.solid);
    reader.text("filter", filter);
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    if (!(options.tile_size.x > 0.0f && options.tile_size.y > 0.0f)) {
        return scene_error("A tile map file's tile_size must be above 0");
    }
    if (filter == "pixel") {
        options.filter = renderer::SpriteFilter::Pixel;
    } else if (filter == "smooth") {
        options.filter = renderer::SpriteFilter::Smooth;
    } else {
        return scene_error(std::format("'filter' is \"pixel\" or \"smooth\", not \"{}\"", filter));
    }
    if (format == nullptr) {
        return file_error(path, "the game reads no map files; list a tile_map_format module, "
                                "such as \"tiled\", in its TOML file");
    }
    const core::Result<AssetRef<TileMapFileAsset>> file =
        context.assets.load<TileMapFileAsset>(path);
    if (!file.has_value()) {
        return file.error();
    }
    return add_tile_map_file(context.world, context.entity, *file.value(), options);
}

} // namespace

std::string_view MapObject::property(std::string_view name, std::string_view fallback) const {
    const Property* const found = find_property(properties, name);
    return found != nullptr ? std::string_view(found->value) : fallback;
}

float MapObject::number(std::string_view name, float fallback) const {
    const Property* const found = find_property(properties, name);
    if (found == nullptr || found->value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(found->value.c_str(), &end);
    return end != nullptr && *end == '\0' ? value : fallback;
}

bool MapObject::flag(std::string_view name, bool fallback) const {
    const Property* const found = find_property(properties, name);
    return found != nullptr ? found->value == "true" : fallback;
}

core::Result<std::string> path_beside(std::string_view path, std::string_view relative) {
    if (relative.empty() || relative.starts_with('/') || relative.starts_with('\\') ||
        relative.find(':') != std::string_view::npos) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("'{}' names '{}', which isn't a path relative to it", path,
                                       relative)};
    }
    std::string_view root;
    std::string_view rest = path;
    if (const std::size_t scheme = path.find(":/"); scheme != std::string_view::npos) {
        root = path.substr(0, scheme + 2);
        rest = path.substr(scheme + 2);
    }
    std::vector<std::string_view> parts;
    const auto split = [](std::string_view text, auto&& each) {
        std::size_t start = 0;
        for (std::size_t at = 0; at <= text.size(); ++at) {
            if (at == text.size() || text[at] == '/' || text[at] == '\\') {
                each(text.substr(start, at - start));
                start = at + 1;
            }
        }
    };
    split(rest, [&](std::string_view part) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    });
    if (!parts.empty()) {
        parts.pop_back(); // The file itself.
    }
    bool escaped = false;
    split(relative, [&](std::string_view part) {
        if (part.empty() || part == ".") {
            return;
        }
        if (part == "..") {
            if (parts.empty()) {
                escaped = true;
            } else {
                parts.pop_back();
            }
            return;
        }
        parts.push_back(part);
    });
    if (escaped) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("'{}' names '{}', which is outside its folders", path,
                                       relative)};
    }
    std::string joined(root);
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            joined += '/';
        }
        joined += parts[index];
    }
    return joined;
}

core::Result<void> add_tile_map_file(World& world, Entity parent, const TileMapFileAsset& file,
                                     const TileMapFileOptions& options) {
    const Map& map = file.map;
    Registry& registry = world.registry();
    if (!registry.valid(parent) || map.tile_width == 0 || map.tile_height == 0 ||
        file.tilesets.size() != map.tilesets.size() || file.images.size() != map.layers.size()) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "add_tile_map_file needs an entity, and a loaded map file"};
    }
    // From the map's pixels, Y down from its top, to the world, Y up from its bottom.
    const Vec2 scale{options.tile_size.x / static_cast<float>(map.tile_width),
                     options.tile_size.y / static_cast<float>(map.tile_height)};
    const float map_height = static_cast<float>(map.rows * map.tile_height);
    const auto to_world = [&](float x, float y) {
        return Vec2{x * scale.x, (map_height - y) * scale.y};
    };

    TileMapFile made;
    const auto add_child = [&](std::string_view name, Vec2 at,
                               float angle = 0.0f) -> core::Result<Entity> {
        const Entity child = world.create_entity(name);
        if (auto parented = world.set_parent(child, parent); !parented.has_value()) {
            return parented.error();
        }
        Transform& transform = registry.get<Transform>(child);
        transform.position = Vec3{at.x, at.y, 0.0f};
        transform.rotation = core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, angle);
        return child;
    };

    for (std::size_t layer_index = 0; layer_index < map.layers.size(); ++layer_index) {
        const Layer& layer = map.layers[layer_index];
        std::int32_t draw_layer = options.layer + static_cast<std::int32_t>(layer_index);
        if (const Property* const own = find_property(layer.properties, "layer")) {
            const char* const end = own->value.data() + own->value.size();
            if (std::from_chars(own->value.data(), end, draw_layer).ptr != end) {
                return file_error(layer.name, "its 'layer' property must be a whole number");
            }
        }
        const Vec4 color{srgb_to_linear(layer.tint[0]), srgb_to_linear(layer.tint[1]),
                         srgb_to_linear(layer.tint[2]), layer.opacity};
        const bool moves_apart = layer.parallax_x != 1.0f || layer.parallax_y != 1.0f ||
                                 layer.repeat_x || layer.repeat_y;
        const Parallax parallax{.factor = Vec2{layer.parallax_x, layer.parallax_y},
                                .repeat_x = layer.repeat_x,
                                .repeat_y = layer.repeat_y};
        const Vec2 offset{layer.offset_x * scale.x, -layer.offset_y * scale.y};

        if (layer.kind == LayerKind::Tiles) {
            if (layer.columns != map.columns || layer.rows != map.rows) {
                return file_error(layer.name, "a tile layer must be the size of its map");
            }
            const bool all_solid = property_flag(layer.properties, options.solid);
            // A TileMap for each tileset the layer draws from, in the tilesets' order.
            for (std::size_t set = 0; set < map.tilesets.size(); ++set) {
                const Tileset& tileset = map.tilesets[set];
                TileMap tiles{.tileset = file.tilesets[set],
                              .tile_width = tileset.tile_width,
                              .tile_height = tileset.tile_height,
                              .tile_margin = tileset.margin,
                              .tile_spacing = tileset.spacing,
                              .tile_size = options.tile_size,
                              .columns = layer.columns,
                              .rows = layer.rows,
                              .tiles = std::vector<std::int32_t>(layer.tiles.size(), -1),
                              .solid = {},
                              .color = color,
                              .layer = draw_layer,
                              .order = static_cast<std::int32_t>(set),
                              .filter = options.filter,
                              .visible = layer.visible,
                              .animations = {},
                              .animation_time = 0.0,
                              .animation_speed = 1.0f,
                              .flips = std::vector<std::uint8_t>(layer.tiles.size(), 0)};
                bool used = false;
                bool flipped = false;
                for (std::size_t cell = 0; cell < layer.tiles.size(); ++cell) {
                    const std::uint32_t number = layer.tiles[cell] & k_tile_number;
                    if (number == 0 || tileset_of(map, number) != set) {
                        continue;
                    }
                    const auto tile = static_cast<std::int32_t>(number - tileset.first_tile);
                    tiles.tiles[cell] = tile;
                    tiles.flips[cell] = flips_of(layer.tiles[cell]);
                    flipped = flipped || tiles.flips[cell] != 0;
                    used = true;
                    if (all_solid && std::ranges::find(tiles.solid, tile) == tiles.solid.end()) {
                        tiles.solid.push_back(tile);
                    }
                }
                if (!used) {
                    continue;
                }
                if (tileset.tile_width != map.tile_width || tileset.tile_height != map.tile_height) {
                    return file_error(layer.name,
                                      std::format("its tiles from '{}' are {} by {} pixels, not "
                                                  "the map's {} by {}: place larger ones as tile "
                                                  "objects",
                                                  tileset.name, tileset.tile_width,
                                                  tileset.tile_height, map.tile_width,
                                                  map.tile_height));
                }
                if (!flipped) {
                    tiles.flips.clear();
                }
                for (const TileInfo& info : tileset.tiles) {
                    if (info.has_collision || property_flag(info.properties, options.solid)) {
                        tiles.solid.push_back(static_cast<std::int32_t>(info.id));
                    }
                }
                tiles.animations = animations_of(tileset);
                const core::Result<Entity> child = add_child(layer.name, offset);
                if (!child.has_value()) {
                    return child.error();
                }
                registry.emplace<TileMap>(child.value(), std::move(tiles));
                if (moves_apart) {
                    registry.emplace<Parallax>(child.value(), parallax);
                }
                made.layers.push_back(child.value());
            }
        } else if (layer.kind == LayerKind::Image) {
            const AssetRef<TextureAsset>& texture = file.images[layer_index];
            if (!texture) {
                continue;
            }
            const core::Result<Entity> child =
                add_child(layer.name, to_world(layer.offset_x, layer.offset_y));
            if (!child.has_value()) {
                return child.error();
            }
            registry.emplace<SpriteRenderer>(
                child.value(),
                SpriteRenderer{.texture = texture,
                               .size = Vec2{static_cast<float>(texture->width) * scale.x,
                                            static_cast<float>(texture->height) * scale.y},
                               .pivot = Vec2{0.0f, 1.0f},
                               .color = color,
                               .layer = draw_layer,
                               .filter = options.filter,
                               .visible = layer.visible});
            if (moves_apart) {
                registry.emplace<Parallax>(child.value(), parallax);
            }
            made.layers.push_back(child.value());
        } else {
            for (const tile_map_format::MapObject& object : layer.objects) {
                // Map editors turn objects clockwise, with Y down, around their placed corner.
                const float angle = object.rotation * 3.14159265f / 180.0f;
                const auto turned = [&](float x, float y) {
                    return Vec2{object.x + x * std::cos(angle) - y * std::sin(angle),
                                object.y + x * std::sin(angle) + y * std::cos(angle)};
                };
                Vec2 place{object.x, object.y};
                if (object.tile != 0) {
                    place = turned(object.width * 0.5f, 0.0f);
                } else if (object.shape != ObjectShape::Point) {
                    place = turned(object.width * 0.5f, object.height * 0.5f);
                }
                const core::Result<Entity> child =
                    add_child(object.name.empty() ? layer.name : object.name,
                              to_world(place.x, place.y) + offset, -angle);
                if (!child.has_value()) {
                    return child.error();
                }
                const Vec2 size{object.width * scale.x, object.height * scale.y};
                registry.emplace<runtime::MapObject>(child.value(),
                                                     runtime::MapObject{.type = object.type,
                                                                        .size = size,
                                                                        .shape = object.shape,
                                                                        .properties =
                                                                            object.properties});
                made.objects.push_back(child.value());

                const std::uint32_t number = object.tile & k_tile_number;
                const std::size_t set = number != 0 ? tileset_of(map, number) : map.tilesets.size();
                if (set >= map.tilesets.size()) {
                    continue;
                }
                // A tile object: a sprite of its tile, standing at its foot.
                const Tileset& tileset = map.tilesets[set];
                if (tileset.margin != 0 || tileset.spacing != 0) {
                    DREAM_LOG_WARNING("'{}': tile object '{}' is from tileset '{}', whose margin "
                                      "or spacing sprites don't take, so it may show the wrong "
                                      "part of its image",
                                      layer.name, object.name, tileset.name);
                }
                const std::uint32_t columns = std::max(tileset.columns, 1u);
                const std::uint32_t rows =
                    std::max(tileset.image_height / std::max(tileset.tile_height, 1u), 1u);
                const std::uint32_t frame = number - tileset.first_tile;
                registry.emplace<SpriteRenderer>(
                    child.value(),
                    SpriteRenderer{.texture = file.tilesets[set],
                                   .columns = columns,
                                   .rows = rows,
                                   .frame = frame,
                                   .size = size,
                                   .pivot = Vec2{0.5f, 0.0f},
                                   .color = color,
                                   .flip_x = (object.tile & k_flipped_horizontally) != 0,
                                   .flip_y = (object.tile & k_flipped_vertically) != 0,
                                   .layer = draw_layer,
                                   .filter = options.filter,
                                   .visible = layer.visible && object.visible});
                for (const TileAnimation& animation : animations_of(tileset)) {
                    if (animation.tile != static_cast<std::int32_t>(frame)) {
                        continue;
                    }
                    float length = 0.0f;
                    SpriteClip clip{.name = "tile", .frames = {}, .fps = 10.0f, .loop = true};
                    for (std::size_t index = 0; index < animation.frames.size(); ++index) {
                        clip.frames.push_back(static_cast<std::uint32_t>(animation.frames[index]));
                        length += animation.durations[index];
                    }
                    clip.fps = static_cast<float>(clip.frames.size()) / std::max(length, 0.001f);
                    SpriteAnimator animator;
                    animator.clips.push_back(std::move(clip));
                    animator.clip = "tile";
                    registry.emplace<SpriteAnimator>(child.value(), std::move(animator));
                }
                if (moves_apart) {
                    registry.emplace<Parallax>(child.value(), parallax);
                }
            }
        }
    }
    registry.emplace_or_replace<TileMapFile>(parent, std::move(made));
    return {};
}

void register_tile_map_files(SceneLoader& scenes, AssetManager& assets, TileMapFormat* format) {
    assets.register_loader<TileMapFileAsset>(
        [format](AssetLoadContext& context) { return load_map_file(context, format); });
    using core::FieldInfo;
    using core::FieldKind;
    scenes.register_component(
        "tile_map_file",
        [format](ComponentContext& context) { return read_tile_map_file(context, format); },
        {FieldInfo{.name = "file",
                   .kind = FieldKind::Asset,
                   .fallback = std::string(),
                   .help = "A map from a map editor, such as Tiled's .tmj"},
         FieldInfo{.name = "tile_size",
                   .kind = FieldKind::Vec2,
                   .fallback = core::ConfigValue{std::vector<double>{1.0, 1.0}},
                   .help = "Each tile's size in the world"},
         FieldInfo{.name = "layer",
                   .kind = FieldKind::Integer,
                   .fallback = std::int64_t{0},
                   .help = "The drawing layer of the map's first layer"},
         FieldInfo{.name = "solid", .kind = FieldKind::Text, .fallback = std::string("solid")},
         FieldInfo{.name = "filter",
                   .kind = FieldKind::Choice,
                   .fallback = std::string("pixel"),
                   .choices = {"pixel", "smooth"}}});
}

} // namespace dream::runtime
