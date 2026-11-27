#include "dream/runtime/sprites.h"

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/profile.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <tuple>
#include <utility>

namespace dream::runtime {
namespace {

using core::Mat4;
using core::Vec2;
using core::Vec3;
using core::Vec4;

// The shared settings, plus the ones of sprites and tile maps.
class Reader : public SettingReader {
public:
    using SettingReader::SettingReader;

    // Color as [r, g, b] and opacity as `alpha`.
    void color(Vec4& into) {
        Vec3 rgb{into.x, into.y, into.z};
        vec3("color", rgb);
        number("alpha", into.w);
        into = Vec4{rgb.x, rgb.y, rgb.z, into.w};
    }
    void filter(renderer::SpriteFilter& into) {
        std::string name = "pixel";
        text("filter", name);
        if (failed()) {
            return;
        }
        if (name == "pixel") {
            into = renderer::SpriteFilter::Pixel;
        } else if (name == "smooth") {
            into = renderer::SpriteFilter::Smooth;
        } else {
            fail(std::format("'filter' is \"pixel\" or \"smooth\", not \"{}\"", name));
        }
    }
    void texture(std::string_view key, AssetManager& assets, AssetRef<TextureAsset>& into) {
        std::string path;
        text(key, path);
        if (failed() || path.empty()) {
            return;
        }
        take([&]() -> core::Result<AssetRef<TextureAsset>> {
            core::Result<AssetRef<TextureAsset>> loaded = assets.load<TextureAsset>(path);
            if (!loaded.has_value()) {
                return core::Error{loaded.error().code,
                                   std::format("'{}': {}", path, loaded.error().message)};
            }
            return loaded;
        }, into);
    }
};

// Whole numbers from a list, or an error naming the setting.
core::Result<std::vector<std::int32_t>> whole_numbers(const ComponentData& data,
                                                      std::string_view key) {
    core::Result<std::vector<float>> numbers = data.numbers(key);
    if (!numbers.has_value()) {
        return numbers.error();
    }
    std::vector<std::int32_t> wholes;
    wholes.reserve(numbers.value().size());
    for (const float number : numbers.value()) {
        if (number != std::floor(number) || std::abs(number) > 1.0e9f) {
            return scene_error(std::format("'{}' holds whole numbers, not {}", key, number));
        }
        wholes.push_back(static_cast<std::int32_t>(number));
    }
    return wholes;
}

core::Result<void> read_camera_2d(ComponentContext& context) {
    if (auto known = context.data.check_keys({"height", "min_width", "y_sort_layers"});
        !known.has_value()) {
        return known;
    }
    Camera2D camera;
    Reader reader(context.data);
    reader.number("height", camera.height);
    reader.number("min_width", camera.min_width);
    if (context.data.has("y_sort_layers")) {
        reader.take([&] { return whole_numbers(context.data, "y_sort_layers"); },
                    camera.y_sort_layers);
    }
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    if (!(camera.height > 0.0f)) {
        return scene_error("A 2D camera's height must be above 0");
    }
    if (!(camera.min_width >= 0.0f)) {
        return scene_error("A 2D camera's min_width must be 0 or more");
    }
    context.world.registry().emplace_or_replace<Camera2D>(context.entity, camera);
    return {};
}

core::Result<void> read_sprite(ComponentContext& context) {
    if (auto known = context.data.check_keys(
            {"texture", "columns", "rows", "frame", "size", "pixels_per_unit", "pivot", "slice",
             "color", "alpha", "flip_x", "flip_y", "layer", "order", "filter", "visible"});
        !known.has_value()) {
        return known;
    }
    SpriteRenderer sprite;
    Reader reader(context.data);
    reader.texture("texture", context.assets, sprite.texture);
    reader.whole("columns", sprite.columns);
    reader.whole("rows", sprite.rows);
    reader.whole("frame", sprite.frame);
    reader.vec2("size", sprite.size);
    reader.number("pixels_per_unit", sprite.pixels_per_unit);
    reader.vec2("pivot", sprite.pivot);
    if (context.data.has("slice")) {
        // One number for every side, or four: left, right, top, bottom.
        std::vector<float> slice;
        reader.take([&]() -> core::Result<std::vector<float>> {
            if (const core::Result<float> all = context.data.number("slice", 0.0f);
                all.has_value()) {
                return std::vector<float>{all.value()};
            }
            return context.data.numbers("slice");
        }, slice);
        if (!reader.failed()) {
            if (slice.size() == 1) {
                sprite.slice = Vec4{slice[0], slice[0], slice[0], slice[0]};
            } else if (slice.size() == 4) {
                sprite.slice = Vec4{slice[0], slice[1], slice[2], slice[3]};
            } else {
                reader.fail("A sprite's slice is one number, or four: left, right, top, bottom");
            }
            if (!(sprite.slice.x >= 0.0f && sprite.slice.y >= 0.0f && sprite.slice.z >= 0.0f &&
                  sprite.slice.w >= 0.0f)) {
                reader.fail("A sprite's slice borders must be 0 or more");
            }
        }
    }
    reader.color(sprite.color);
    reader.flag("flip_x", sprite.flip_x);
    reader.flag("flip_y", sprite.flip_y);
    reader.integer("layer", sprite.layer);
    reader.integer("order", sprite.order);
    reader.filter(sprite.filter);
    reader.flag("visible", sprite.visible);
    if (sprite.columns == 0 || sprite.rows == 0) {
        reader.fail("A sprite's columns and rows must be 1 or more");
    }
    if (!(sprite.pixels_per_unit > 0.0f)) {
        reader.fail("A sprite's pixels_per_unit must be above 0");
    }
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    context.world.registry().emplace_or_replace<SpriteRenderer>(context.entity, std::move(sprite));
    return {};
}

core::Result<void> read_sprite_animator(ComponentContext& context) {
    if (auto known = context.data.check_keys({"clip", "speed", "playing", "time", "clips"});
        !known.has_value()) {
        return known;
    }
    SpriteAnimator animator;
    Reader reader(context.data);
    reader.take([&] { return context.data.text("clip", ""); }, animator.clip);
    reader.number("speed", animator.speed);
    reader.flag("playing", animator.playing);
    reader.number("time", animator.time);
    const ComponentData clips = context.data.child("clips");
    for (const std::string& name : clips.tables()) {
        const ComponentData data = clips.child(name);
        if (auto known = data.check_keys({"frames", "fps", "loop"}); !known.has_value()) {
            return core::Error{known.error().code,
                               std::format("clip '{}': {}", name, known.error().message)};
        }
        SpriteClip clip{.name = name, .frames = {}, .fps = 10.0f, .loop = true};
        Reader clip_reader(data);
        std::vector<float> frames;
        clip_reader.take([&] { return data.numbers("frames"); }, frames);
        clip_reader.number("fps", clip.fps);
        clip_reader.flag("loop", clip.loop);
        for (const float frame : frames) {
            if (frame < 0.0f || frame != std::floor(frame)) {
                clip_reader.fail("frames are whole numbers, 0 or more");
                break;
            }
            clip.frames.push_back(static_cast<std::uint32_t>(frame));
        }
        if (clip.frames.empty()) {
            clip_reader.fail("a clip needs frames");
        }
        if (!(clip.fps > 0.0f)) {
            clip_reader.fail("fps must be above 0");
        }
        if (auto read = clip_reader.result(); !read.has_value()) {
            return core::Error{read.error().code,
                               std::format("clip '{}': {}", name, read.error().message)};
        }
        animator.clips.push_back(std::move(clip));
    }
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    context.world.registry().emplace_or_replace<SpriteAnimator>(context.entity,
                                                                std::move(animator));
    return {};
}

// A tile map's [animations] tables, each named by a tile's number, or by its character in the
// legend, with `frames` and `fps`.
core::Result<void> read_tile_animations(const ComponentData& data, TileMap& map) {
    const ComponentData animations = data.child("animations");
    const ComponentData legend = data.child("legend");
    for (const std::string& name : animations.tables()) {
        const auto failed = [&](std::string_view message) {
            return scene_error(std::format("A tile map's animation '{}': {}", name, message));
        };
        TileAnimation animation;
        if (name.size() == 1 && legend.has(name)) {
            const core::Result<float> tile = legend.number(name, -1.0f);
            if (!tile.has_value()) {
                return tile.error();
            }
            animation.tile = static_cast<std::int32_t>(tile.value());
        } else {
            const char* const end = name.data() + name.size();
            const auto [last, error] = std::from_chars(name.data(), end, animation.tile);
            if (error != std::errc{} || last != end) {
                return failed("name it by a tile's number, or by its character in the legend");
            }
        }
        const ComponentData entry = animations.child(name);
        if (auto known = entry.check_keys({"frames", "fps"}); !known.has_value()) {
            return failed(known.error().message);
        }
        core::Result<std::vector<std::int32_t>> frames = whole_numbers(entry, "frames");
        if (!frames.has_value()) {
            return failed(frames.error().message);
        }
        animation.frames = std::move(frames).value();
        const core::Result<float> fps = entry.number("fps", animation.fps);
        if (!fps.has_value()) {
            return failed(fps.error().message);
        }
        animation.fps = fps.value();
        if (animation.frames.empty()) {
            return failed("it needs frames");
        }
        if (!(animation.fps > 0.0f)) {
            return failed("fps must be above 0");
        }
        map.animations.push_back(std::move(animation));
    }
    return {};
}

core::Result<void> read_parallax(ComponentContext& context) {
    if (auto known = context.data.check_keys({"factor", "repeat_x", "repeat_y"});
        !known.has_value()) {
        return known;
    }
    Parallax parallax;
    Reader reader(context.data);
    reader.vec2("factor", parallax.factor);
    reader.flag("repeat_x", parallax.repeat_x);
    reader.flag("repeat_y", parallax.repeat_y);
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    context.world.registry().emplace_or_replace<Parallax>(context.entity, parallax);
    return {};
}

core::Result<void> read_tile_map(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (auto known = data.check_keys({"tileset", "tile_pixels", "tile_size", "rows", "tiles",
                                      "columns", "solid", "color", "alpha", "layer", "order",
                                      "filter", "visible", "legend", "animations",
                                      "animation_speed"});
        !known.has_value()) {
        return known;
    }
    TileMap map;
    Reader reader(data);
    reader.texture("tileset", context.assets, map.tileset);
    Vec2 tile_pixels{16.0f, 16.0f};
    reader.vec2("tile_pixels", tile_pixels);
    reader.vec2("tile_size", map.tile_size);
    reader.color(map.color);
    reader.integer("layer", map.layer);
    reader.integer("order", map.order);
    reader.filter(map.filter);
    reader.flag("visible", map.visible);
    reader.number("animation_speed", map.animation_speed);
    std::vector<float> solid;
    if (data.has("solid")) {
        reader.take([&] { return data.numbers("solid"); }, solid);
    }
    if (auto read = reader.result(); !read.has_value()) {
        return read;
    }
    if (!(tile_pixels.x >= 1.0f && tile_pixels.y >= 1.0f)) {
        return scene_error("A tile map's tile_pixels must be 1 or more");
    }
    if (!(map.tile_size.x > 0.0f && map.tile_size.y > 0.0f)) {
        return scene_error("A tile map's tile_size must be above 0");
    }
    map.tile_width = static_cast<std::uint32_t>(tile_pixels.x);
    map.tile_height = static_cast<std::uint32_t>(tile_pixels.y);
    for (const float tile : solid) {
        map.solid.push_back(static_cast<std::int32_t>(tile));
    }

    if (data.has("rows")) {
        // Text rows, top first, each character a tile through the legend; '.' and ' ' are empty.
        core::Result<std::vector<std::string>> rows = data.strings("rows");
        if (!rows.has_value()) {
            return rows.error();
        }
        const ComponentData legend = data.child("legend");
        std::array<std::int32_t, 256> tiles{};
        tiles.fill(std::numeric_limits<std::int32_t>::min());
        tiles[static_cast<unsigned char>('.')] = -1;
        tiles[static_cast<unsigned char>(' ')] = -1;
        for (const std::string& key : legend.keys()) {
            if (key.size() != 1) {
                return scene_error(std::format(
                    "A tile map's legend maps single characters to tiles, not '{}'", key));
            }
            const core::Result<float> tile = legend.number(key, -1.0f);
            if (!tile.has_value()) {
                return tile.error();
            }
            tiles[static_cast<unsigned char>(key[0])] = static_cast<std::int32_t>(tile.value());
        }
        map.rows = static_cast<std::uint32_t>(rows.value().size());
        for (const std::string& row : rows.value()) {
            map.columns = std::max(map.columns, static_cast<std::uint32_t>(row.size()));
        }
        map.tiles.assign(std::size_t{map.columns} * map.rows, -1);
        for (std::uint32_t row = 0; row < map.rows; ++row) {
            const std::string& text = rows.value()[row];
            for (std::uint32_t column = 0; column < text.size(); ++column) {
                const std::int32_t tile = tiles[static_cast<unsigned char>(text[column])];
                if (tile == std::numeric_limits<std::int32_t>::min()) {
                    return scene_error(std::format(
                        "A tile map's row {} has '{}', which its legend doesn't list", row + 1,
                        text[column]));
                }
                map.tiles[std::size_t{row} * map.columns + column] = tile;
            }
        }
    } else if (data.has("tiles")) {
        core::Result<std::vector<float>> tiles = data.numbers("tiles");
        float columns = 0.0f;
        if (!tiles.has_value()) {
            return tiles.error();
        }
        if (auto read = data.number("columns", 0.0f); read.has_value()) {
            columns = read.value();
        } else {
            return read.error();
        }
        if (!(columns >= 1.0f) || columns != std::floor(columns) ||
            tiles.value().size() % static_cast<std::size_t>(columns) != 0) {
            return scene_error("A tile map's tiles must fill whole rows of its columns");
        }
        map.columns = static_cast<std::uint32_t>(columns);
        map.rows = static_cast<std::uint32_t>(tiles.value().size() / map.columns);
        for (const float tile : tiles.value()) {
            map.tiles.push_back(static_cast<std::int32_t>(tile));
        }
    } else {
        return scene_error("A tile map needs `rows` with a `legend`, or `tiles` with `columns`");
    }
    if (auto read = read_tile_animations(data, map); !read.has_value()) {
        return read;
    }
    context.world.registry().emplace_or_replace<TileMap>(context.entity, std::move(map));
    return {};
}

const SpriteClip* find_clip(const SpriteAnimator& animator, std::string_view name) {
    const auto found = std::ranges::find(animator.clips, name, &SpriteClip::name);
    return found != animator.clips.end() ? &*found : nullptr;
}

// Where an entity is in the 2D world, its angle around Z, and its scale in X and Y.
struct Placement {
    Vec2 position;
    float angle = 0.0f;
    Vec2 scale{1.0f, 1.0f};
};

Placement placement(const Mat4& matrix) {
    const Vec3 origin = core::transform_point(matrix, Vec3{});
    const Vec2 x_axis{matrix.columns[0].x, matrix.columns[0].y};
    const Vec2 y_axis{matrix.columns[1].x, matrix.columns[1].y};
    return Placement{.position = Vec2{origin.x, origin.y},
                     .angle = std::atan2(x_axis.y, x_axis.x),
                     .scale = Vec2{core::length(x_axis), core::length(y_axis)}};
}

} // namespace

void SpriteAnimator::play(std::string_view name) {
    if (clip == name && playing) {
        return;
    }
    clip = std::string(name);
    time = 0.0f;
    playing = true;
}

float SpriteAnimator::duration(std::string_view name) const {
    const SpriteClip* const found = find_clip(*this, name);
    return found != nullptr ? static_cast<float>(found->frames.size()) / found->fps : 0.0f;
}

std::int32_t TileMap::tile(std::uint32_t column, std::uint32_t row) const noexcept {
    if (column >= columns || row >= rows || tiles.size() < std::size_t{columns} * rows) {
        return -1;
    }
    return tiles[std::size_t{row} * columns + column];
}

void TileMap::set_tile(std::uint32_t column, std::uint32_t row, std::int32_t tile) noexcept {
    if (column < columns && row < rows && tiles.size() >= std::size_t{columns} * rows) {
        tiles[std::size_t{row} * columns + column] = tile;
    }
}

std::optional<std::array<std::uint32_t, 2>> TileMap::cell_at(Vec2 origin, Vec2 point) const {
    const float column = std::floor((point.x - origin.x) / tile_size.x);
    const float from_bottom = std::floor((point.y - origin.y) / tile_size.y);
    if (column < 0.0f || from_bottom < 0.0f || column >= static_cast<float>(columns) ||
        from_bottom >= static_cast<float>(rows)) {
        return std::nullopt;
    }
    return std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(column),
                                        rows - 1 - static_cast<std::uint32_t>(from_bottom)};
}

std::optional<Mat4> camera_2d_view(const World& world, platform::SurfaceSize size) {
    if (size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    const auto cameras = world.registry().view<const Camera2D, const WorldTransform>();
    const Entity entity = cameras.front();
    if (entity == k_no_entity) {
        return std::nullopt;
    }
    const Camera2D& camera = cameras.get<const Camera2D>(entity);
    const Placement where = placement(cameras.get<const WorldTransform>(entity).matrix);
    const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
    const float half_height = std::max(camera.height, camera.min_width / aspect) * 0.5f;
    const float half_width = half_height * aspect;
    const Mat4 view =
        core::rotation(core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, -where.angle)) *
        core::translation(Vec3{-where.position.x, -where.position.y, 0.0f});
    return core::orthographic(-half_width, half_width, -half_height, half_height, -1.0f, 1.0f) *
           view;
}

std::optional<Vec2> screen_to_world_2d(const World& world, platform::SurfaceSize size, Vec2 pixel) {
    const std::optional<Mat4> view = camera_2d_view(world, size);
    if (!view.has_value()) {
        return std::nullopt;
    }
    const std::optional<Mat4> inverse = core::inverse(*view);
    if (!inverse.has_value()) {
        return std::nullopt;
    }
    const Vec3 clip{2.0f * pixel.x / static_cast<float>(size.width) - 1.0f,
                    1.0f - 2.0f * pixel.y / static_cast<float>(size.height), 0.5f};
    const Vec3 point = core::transform_point(*inverse, clip);
    return Vec2{point.x, point.y};
}

void update_sprite_animators(World& world, double seconds) {
    for (const auto [entity, animator, sprite] :
         world.registry().view<SpriteAnimator, SpriteRenderer>().each()) {
        const SpriteClip* const clip = find_clip(animator, animator.clip);
        if (clip == nullptr || clip->frames.empty()) {
            continue;
        }
        const float length = static_cast<float>(clip->frames.size()) / clip->fps;
        if (animator.playing) {
            animator.time += static_cast<float>(seconds) * animator.speed;
        }
        if (clip->loop) {
            animator.time = std::fmod(animator.time, length);
            if (animator.time < 0.0f) {
                animator.time += length;
            }
        } else if (animator.time >= length || animator.time < 0.0f) {
            animator.time = std::clamp(animator.time, 0.0f, length);
            animator.playing = false;
        }
        const auto index = std::min(static_cast<std::size_t>(animator.time * clip->fps),
                                    clip->frames.size() - 1);
        sprite.frame = clip->frames[index];
    }
    for (const auto [entity, map] : world.registry().view<TileMap>().each()) {
        if (!map.animations.empty()) {
            map.animation_time =
                std::max(0.0, map.animation_time + seconds * static_cast<double>(map.animation_speed));
        }
    }
}

core::Result<std::vector<Entity>> make_tile_colliders(World& world, Entity map_entity) {
    const TileMap* const map = world.registry().try_get<TileMap>(map_entity);
    const WorldTransform* const transform = world.registry().try_get<WorldTransform>(map_entity);
    if (map == nullptr || transform == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "make_tile_colliders needs an entity with a TileMap"};
    }
    const auto is_solid = [&](std::uint32_t column, std::uint32_t row) {
        const std::int32_t tile = map->tile(column, row);
        return tile >= 0 && std::ranges::find(map->solid, tile) != map->solid.end();
    };
    // Runs of solid tiles along each row, then runs that line up in the rows below merged.
    struct Box {
        std::uint32_t first_column = 0;
        std::uint32_t last_column = 0;
        std::uint32_t top_row = 0;
        std::uint32_t bottom_row = 0;
    };
    std::vector<Box> done;
    std::vector<Box> open;
    for (std::uint32_t row = 0; row < map->rows; ++row) {
        std::vector<Box> runs;
        for (std::uint32_t column = 0; column < map->columns;) {
            if (!is_solid(column, row)) {
                ++column;
                continue;
            }
            const std::uint32_t first = column;
            while (column < map->columns && is_solid(column, row)) {
                ++column;
            }
            runs.push_back(Box{first, column - 1, row, row});
        }
        std::vector<Box> next;
        for (Box& run : runs) {
            const auto above = std::ranges::find_if(open, [&](const Box& box) {
                return box.first_column == run.first_column && box.last_column == run.last_column;
            });
            if (above != open.end()) {
                run.top_row = above->top_row;
                open.erase(above);
            }
            next.push_back(run);
        }
        done.insert(done.end(), open.begin(), open.end());
        open = std::move(next);
    }
    done.insert(done.end(), open.begin(), open.end());

    const Placement where = placement(transform->matrix);
    std::vector<Entity> made;
    made.reserve(done.size());
    for (const Box& box : done) {
        const float width = static_cast<float>(box.last_column - box.first_column + 1) *
                            map->tile_size.x;
        const float height = static_cast<float>(box.bottom_row - box.top_row + 1) *
                             map->tile_size.y;
        const float left = static_cast<float>(box.first_column) * map->tile_size.x;
        const float bottom = static_cast<float>(map->rows - 1 - box.bottom_row) * map->tile_size.y;
        const Entity collider = world.create_entity();
        world.registry().get<Transform>(collider).position =
            Vec3{where.position.x + left + width * 0.5f, where.position.y + bottom + height * 0.5f,
                 0.0f};
        RigidBody2D body;
        body.desc.motion = physics2d::MotionType::Static;
        body.desc.shape = physics2d::BoxShape{.half_extents = Vec2{width * 0.5f, height * 0.5f}};
        world.registry().emplace<RigidBody2D>(collider, body);
        made.push_back(collider);
    }
    world.update_transforms();
    return made;
}

void register_sprite_components(SceneLoader& scenes) {
    using core::FieldInfo;
    using core::FieldKind;
    const auto vec2 = [](double x, double y) {
        return core::ConfigValue{std::vector<double>{x, y}};
    };
    scenes.register_component(
        "camera_2d", &read_camera_2d,
        {FieldInfo{.name = "height",
                   .kind = FieldKind::Number,
                   .fallback = 10.0,
                   .minimum = 0.1f,
                   .maximum = 200.0f,
                   .help = "World units seen from the bottom of the window to the top"},
         FieldInfo{.name = "min_width",
                   .kind = FieldKind::Number,
                   .fallback = 0.0,
                   .minimum = 0.0f,
                   .maximum = 200.0f,
                   .help = "World units always seen across; 0 for none"}});
    scenes.register_component(
        "sprite", &read_sprite,
        {FieldInfo{.name = "texture", .kind = FieldKind::Asset, .fallback = std::string()},
         FieldInfo{.name = "columns", .kind = FieldKind::Integer, .fallback = std::int64_t{1}},
         FieldInfo{.name = "rows", .kind = FieldKind::Integer, .fallback = std::int64_t{1}},
         FieldInfo{.name = "frame", .kind = FieldKind::Integer, .fallback = std::int64_t{0}},
         FieldInfo{.name = "size",
                   .kind = FieldKind::Vec2,
                   .fallback = vec2(0, 0),
                   .help = "World units; 0 takes the frame's pixels over pixels_per_unit"},
         FieldInfo{.name = "pixels_per_unit", .kind = FieldKind::Number, .fallback = 16.0},
         FieldInfo{.name = "pivot", .kind = FieldKind::Vec2, .fallback = vec2(0.5, 0.5)},
         FieldInfo{.name = "color",
                   .kind = FieldKind::Color,
                   .fallback = std::vector<double>{1.0, 1.0, 1.0}},
         FieldInfo{.name = "alpha",
                   .kind = FieldKind::Number,
                   .fallback = 1.0,
                   .minimum = 0.0f,
                   .maximum = 1.0f},
         FieldInfo{.name = "flip_x", .kind = FieldKind::Flag, .fallback = false},
         FieldInfo{.name = "flip_y", .kind = FieldKind::Flag, .fallback = false},
         FieldInfo{.name = "layer", .kind = FieldKind::Integer, .fallback = std::int64_t{0}},
         FieldInfo{.name = "order", .kind = FieldKind::Integer, .fallback = std::int64_t{0}},
         FieldInfo{.name = "filter",
                   .kind = FieldKind::Choice,
                   .fallback = std::string("pixel"),
                   .choices = {"pixel", "smooth"}},
         FieldInfo{.name = "visible", .kind = FieldKind::Flag, .fallback = true}});
    scenes.register_component("sprite_animator", &read_sprite_animator);
    scenes.register_component("tile_map", &read_tile_map);
    scenes.register_component(
        "parallax", &read_parallax,
        {FieldInfo{.name = "factor",
                   .kind = FieldKind::Vec2,
                   .fallback = vec2(1, 1),
                   .help = "How far it moves as the camera moves: 1 as the world, 0 not at all"},
         FieldInfo{.name = "repeat_x", .kind = FieldKind::Flag, .fallback = false},
         FieldInfo{.name = "repeat_y", .kind = FieldKind::Flag, .fallback = false}});
}

namespace {

// The copies of something reaching from `low` to `high` along an axis, repeated every `high - low`,
// that reach into the view from `seen_low` to `seen_high`: the first and the last, counted from the
// thing itself. Without repeating, only the thing itself.
struct Copies {
    int first = 0;
    int last = 0;
};

// The most copies drawn along an axis, however small the thing is.
constexpr int k_max_copies = 256;

Copies copies_in_view(bool repeat, float low, float high, float seen_low, float seen_high) {
    const float period = high - low;
    if (!repeat || !(period > 0.0f)) {
        return {};
    }
    const float first = std::ceil((seen_low - high) / period);
    const float last = std::floor((seen_high - low) / period);
    if (!(last >= first) || !(std::abs(first) < 1.0e6f)) {
        return Copies{1, 0};
    }
    const int start = static_cast<int>(first);
    return Copies{start, std::min(static_cast<int>(std::min(last, 1.0e6f)),
                                  start + k_max_copies - 1)};
}

// A tile map's cells in view: columns, then rows counted up from the bottom, each from the first
// to one past the last.
struct Cells {
    std::uint32_t first_column = 0;
    std::uint32_t end_column = 0;
    std::uint32_t first_up = 0;
    std::uint32_t end_up = 0;
};

Cells cells_in_view(const TileMap& map, Vec2 origin, Vec2 seen_min, Vec2 seen_max) {
    const auto clamp_cell = [](float value, std::uint32_t count) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, static_cast<float>(count)));
    };
    return Cells{
        .first_column =
            clamp_cell(std::floor((seen_min.x - origin.x) / map.tile_size.x), map.columns),
        .end_column = clamp_cell(std::ceil((seen_max.x - origin.x) / map.tile_size.x), map.columns),
        .first_up = clamp_cell(std::floor((seen_min.y - origin.y) / map.tile_size.y), map.rows),
        .end_up = clamp_cell(std::ceil((seen_max.y - origin.y) / map.tile_size.y), map.rows)};
}

bool has_cells(const TileMap& map) {
    return map.columns > 0 && map.rows > 0 &&
           map.tiles.size() >= std::size_t{map.columns} * map.rows;
}

// The tile a cell shows now, through the map's animations.
std::int32_t shown_tile(const TileMap& map, std::int32_t tile) {
    for (const TileAnimation& animation : map.animations) {
        if (animation.tile == tile && !animation.frames.empty() && animation.fps > 0.0f) {
            const auto step = static_cast<std::size_t>(map.animation_time *
                                                       static_cast<double>(animation.fps));
            return animation.frames[step % animation.frames.size()];
        }
    }
    return tile;
}

} // namespace

void gather_sprites(const World& world, const Mat4& view, std::vector<renderer::SpriteQuad>& quads,
                    std::vector<renderer::SpriteBatch>& batches, SpriteScratch& scratch) {
    DREAM_PROFILE_ZONE("Gather sprites");
    quads.clear();
    batches.clear();
    const Registry& registry = world.registry();

    // The part of the world in view, for leaving out what isn't, and the middle of the view, which
    // parallax moves things by.
    Vec2 seen_min{-1.0e30f, -1.0e30f};
    Vec2 seen_max{1.0e30f, 1.0e30f};
    Vec2 middle{};
    if (const std::optional<Mat4> inverse = core::inverse(view)) {
        seen_min = Vec2{1.0e30f, 1.0e30f};
        seen_max = Vec2{-1.0e30f, -1.0e30f};
        for (const float x : {-1.0f, 1.0f}) {
            for (const float y : {-1.0f, 1.0f}) {
                const Vec3 corner = core::transform_point(*inverse, Vec3{x, y, 0.5f});
                seen_min = Vec2{std::min(seen_min.x, corner.x), std::min(seen_min.y, corner.y)};
                seen_max = Vec2{std::max(seen_max.x, corner.x), std::max(seen_max.y, corner.y)};
            }
        }
        const Vec3 centre = core::transform_point(*inverse, Vec3{0.0f, 0.0f, 0.5f});
        middle = Vec2{centre.x, centre.y};
    }

    // The layers the camera sorts by height.
    const std::vector<std::int32_t>* y_sorted = nullptr;
    const auto cameras = registry.view<const Camera2D, const WorldTransform>();
    if (const Entity camera = cameras.front(); camera != k_no_entity) {
        y_sorted = &cameras.get<const Camera2D>(camera).y_sort_layers;
    }
    const auto sorts_by_y = [&](std::int32_t layer) {
        return y_sorted != nullptr && std::ranges::find(*y_sorted, layer) != y_sorted->end();
    };
    // How far parallax moves an entity from where it is.
    const auto shift_of = [&](const Parallax* parallax) {
        return parallax != nullptr ? Vec2{middle.x * (1.0f - parallax->factor.x),
                                          middle.y * (1.0f - parallax->factor.y)}
                                   : Vec2{};
    };
    const auto position_of = [&](Entity entity) {
        const Vec3 origin =
            core::transform_point(registry.get<const WorldTransform>(entity).matrix, Vec3{});
        return Vec2{origin.x, origin.y} + shift_of(registry.try_get<const Parallax>(entity));
    };

    using Item = SpriteScratch::Item;
    std::vector<Item>& items = scratch.items;
    items.clear();
    for (const auto [entity, sprite] : registry.view<const SpriteRenderer>().each()) {
        if (!sprite.visible || !registry.all_of<WorldTransform>(entity)) {
            continue;
        }
        items.push_back(Item{.layer = sprite.layer,
                             .depth = sorts_by_y(sprite.layer) ? -position_of(entity).y : 0.0f,
                             .order = sprite.order,
                             .map = false,
                             .id = static_cast<std::uint32_t>(entt::to_integral(entity)),
                             .entity = entity,
                             .row = SpriteScratch::k_whole_map});
    }
    for (const auto [entity, map] : registry.view<const TileMap>().each()) {
        if (!map.visible || !registry.all_of<WorldTransform>(entity) || !has_cells(map)) {
            continue;
        }
        Item item{.layer = map.layer,
                  .depth = 0.0f,
                  .order = map.order,
                  .map = true,
                  .id = static_cast<std::uint32_t>(entt::to_integral(entity)),
                  .entity = entity,
                  .row = SpriteScratch::k_whole_map};
        const Parallax* const parallax = registry.try_get<const Parallax>(entity);
        if (!sorts_by_y(map.layer) || (parallax != nullptr && parallax->repeat_y)) {
            items.push_back(item);
            continue;
        }
        // Sorted by height, each row in view stands at its bottom among the layer's sprites.
        const Vec2 origin = position_of(entity);
        const Cells cells = cells_in_view(map, origin, seen_min, seen_max);
        for (std::uint32_t up = cells.first_up; up < cells.end_up; ++up) {
            item.depth = -(origin.y + static_cast<float>(up) * map.tile_size.y);
            item.row = up;
            items.push_back(item);
        }
    }
    std::ranges::sort(items, {}, [](const Item& item) {
        return std::tuple(item.layer, item.depth, item.order, item.map, item.id, item.row);
    });

    using Run = SpriteScratch::Run;
    std::vector<Run>& runs = scratch.runs;
    runs.clear();
    const auto add = [&](const renderer::SpriteQuad& quad, const AssetRef<TextureAsset>& texture,
                         renderer::SpriteFilter filter) {
        const rhi::TextureHandle handle = texture ? texture->texture.get() : rhi::TextureHandle{};
        const std::uint32_t width = texture ? std::max(texture->width, 1u) : 1;
        const std::uint32_t height = texture ? std::max(texture->height, 1u) : 1;
        if (runs.empty() || runs.back().texture != handle || runs.back().filter != filter) {
            runs.push_back(Run{quads.size(), 0, handle, width, height, filter});
        }
        quads.push_back(quad);
        ++runs.back().count;
    };

    // A 9-slice sprite as up to nine rectangles: corners of a fixed size, edges stretched along
    // their length, and the middle filling the rest, all turned together around its pivot.
    const auto add_sliced = [&](const SpriteRenderer& sprite, Vec2 at, float angle, Vec2 drawn,
                                Vec4 frame_uv, Vec2 texture_size) {
        const Vec4 slice = sprite.slice;
        Vec4 border = slice / sprite.pixels_per_unit;
        if (border.x + border.y > drawn.x) {
            const float fit = drawn.x / (border.x + border.y);
            border.x *= fit;
            border.y *= fit;
        }
        if (border.z + border.w > drawn.y) {
            const float fit = drawn.y / (border.z + border.w);
            border.z *= fit;
            border.w *= fit;
        }
        const std::array<float, 4> xs{0.0f, border.x, drawn.x - border.y, drawn.x};
        const std::array<float, 4> ys{0.0f, border.w, drawn.y - border.z, drawn.y};
        const std::array<float, 4> us{frame_uv.x, frame_uv.x + slice.x / texture_size.x,
                                      frame_uv.z - slice.y / texture_size.x, frame_uv.z};
        // Bottom up, as ys are: from the frame's bottom edge to its top.
        const std::array<float, 4> vs{frame_uv.w, frame_uv.w - slice.w / texture_size.y,
                                      frame_uv.y + slice.z / texture_size.y, frame_uv.y};
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        for (std::size_t j = 0; j < 3; ++j) {
            for (std::size_t i = 0; i < 3; ++i) {
                float left = xs[i];
                float right = xs[i + 1];
                float bottom = ys[j];
                float top = ys[j + 1];
                if (!(right > left) || !(top > bottom)) {
                    continue;
                }
                Vec4 uv{us[i], vs[j + 1], us[i + 1], vs[j]};
                if (sprite.flip_x) {
                    std::tie(left, right) = std::pair(drawn.x - right, drawn.x - left);
                    std::swap(uv.x, uv.z);
                }
                if (sprite.flip_y) {
                    std::tie(bottom, top) = std::pair(drawn.y - top, drawn.y - bottom);
                    std::swap(uv.y, uv.w);
                }
                const Vec2 local{left - sprite.pivot.x * drawn.x, bottom - sprite.pivot.y * drawn.y};
                add(renderer::SpriteQuad{
                        .position = at + Vec2{local.x * cosine - local.y * sine,
                                              local.x * sine + local.y * cosine},
                        .size = Vec2{right - left, top - bottom},
                        .pivot = Vec2{0.0f, 0.0f},
                        .rotation = angle,
                        .uv = uv,
                        .color = sprite.color},
                    sprite.texture, sprite.filter);
            }
        }
    };

    for (const Item& item : items) {
        const Placement where = placement(registry.get<const WorldTransform>(item.entity).matrix);
        const Parallax* const parallax = registry.try_get<const Parallax>(item.entity);
        const Vec2 position = where.position + shift_of(parallax);
        const bool repeat_x = parallax != nullptr && parallax->repeat_x;
        const bool repeat_y = parallax != nullptr && parallax->repeat_y;
        if (!item.map) {
            const SpriteRenderer& sprite = registry.get<const SpriteRenderer>(item.entity);
            const std::uint32_t columns = std::max(sprite.columns, 1u);
            const std::uint32_t rows = std::max(sprite.rows, 1u);
            const Vec2 texture_size =
                sprite.texture ? Vec2{static_cast<float>(std::max(sprite.texture->width, 1u)),
                                      static_cast<float>(std::max(sprite.texture->height, 1u))}
                               : Vec2{1.0f, 1.0f};
            Vec2 size = sprite.size;
            if (size.x <= 0.0f || size.y <= 0.0f) {
                size = Vec2{texture_size.x / static_cast<float>(columns),
                            texture_size.y / static_cast<float>(rows)} /
                       sprite.pixels_per_unit;
            }
            const Vec2 drawn{size.x * where.scale.x, size.y * where.scale.y};
            const std::uint32_t column = sprite.frame % columns;
            const std::uint32_t row = (sprite.frame / columns) % rows;
            const Vec4 frame_uv{static_cast<float>(column) / static_cast<float>(columns),
                                static_cast<float>(row) / static_cast<float>(rows),
                                static_cast<float>(column + 1) / static_cast<float>(columns),
                                static_cast<float>(row + 1) / static_cast<float>(rows)};
            Vec4 uv = frame_uv;
            if (sprite.flip_x) {
                std::swap(uv.x, uv.z);
            }
            if (sprite.flip_y) {
                std::swap(uv.y, uv.w);
            }
            const bool sliced = sprite.slice.x > 0.0f || sprite.slice.y > 0.0f ||
                                sprite.slice.z > 0.0f || sprite.slice.w > 0.0f;
            // Where its rectangle reaches, unturned, for repeating it.
            const Vec2 low{position.x - sprite.pivot.x * drawn.x,
                           position.y - sprite.pivot.y * drawn.y};
            const Copies across =
                copies_in_view(repeat_x, low.x, low.x + drawn.x, seen_min.x, seen_max.x);
            const Copies up =
                copies_in_view(repeat_y, low.y, low.y + drawn.y, seen_min.y, seen_max.y);
            for (int copy_y = up.first; copy_y <= up.last; ++copy_y) {
                for (int copy_x = across.first; copy_x <= across.last; ++copy_x) {
                    const Vec2 at = position + Vec2{static_cast<float>(copy_x) * drawn.x,
                                                    static_cast<float>(copy_y) * drawn.y};
                    if (sliced) {
                        add_sliced(sprite, at, where.angle, drawn, frame_uv, texture_size);
                    } else {
                        add(renderer::SpriteQuad{.position = at,
                                                 .size = drawn,
                                                 .pivot = sprite.pivot,
                                                 .rotation = where.angle,
                                                 .uv = uv,
                                                 .color = sprite.color},
                            sprite.texture, sprite.filter);
                    }
                }
            }
            continue;
        }

        const TileMap& map = registry.get<const TileMap>(item.entity);
        const float texture_width =
            map.tileset ? static_cast<float>(std::max(map.tileset->width, 1u)) : 1.0f;
        const float texture_height =
            map.tileset ? static_cast<float>(std::max(map.tileset->height, 1u)) : 1.0f;
        const std::uint32_t tileset_columns =
            map.tileset ? std::max(map.tileset->width / std::max(map.tile_width, 1u), 1u) : 1;
        const float tile_u = static_cast<float>(map.tile_width) / texture_width;
        const float tile_v = static_cast<float>(map.tile_height) / texture_height;
        // A hundredth of a texel in from each edge, so no neighboring tile shows at the seams.
        const float inset_u = 0.01f / texture_width;
        const float inset_v = 0.01f / texture_height;
        const Vec2 extent{static_cast<float>(map.columns) * map.tile_size.x,
                          static_cast<float>(map.rows) * map.tile_size.y};
        const Copies across =
            copies_in_view(repeat_x, position.x, position.x + extent.x, seen_min.x, seen_max.x);
        const Copies up =
            copies_in_view(repeat_y, position.y, position.y + extent.y, seen_min.y, seen_max.y);
        for (int copy_y = up.first; copy_y <= up.last; ++copy_y) {
            for (int copy_x = across.first; copy_x <= across.last; ++copy_x) {
                const Vec2 origin = position + Vec2{static_cast<float>(copy_x) * extent.x,
                                                    static_cast<float>(copy_y) * extent.y};
                Cells cells = cells_in_view(map, origin, seen_min, seen_max);
                if (item.row != SpriteScratch::k_whole_map) {
                    if (item.row < cells.first_up || item.row >= cells.end_up) {
                        continue;
                    }
                    cells.first_up = item.row;
                    cells.end_up = item.row + 1;
                }
                for (std::uint32_t row_up = cells.first_up; row_up < cells.end_up; ++row_up) {
                    const std::uint32_t row = map.rows - 1 - row_up;
                    for (std::uint32_t column = cells.first_column; column < cells.end_column;
                         ++column) {
                        std::int32_t tile = map.tiles[std::size_t{row} * map.columns + column];
                        if (tile >= 0 && !map.animations.empty()) {
                            tile = shown_tile(map, tile);
                        }
                        if (tile < 0) {
                            continue;
                        }
                        const auto index = static_cast<std::uint32_t>(tile);
                        const float u = static_cast<float>(index % tileset_columns) * tile_u;
                        const float v = static_cast<float>(index / tileset_columns) * tile_v;
                        add(renderer::SpriteQuad{
                                .position =
                                    Vec2{origin.x + static_cast<float>(column) * map.tile_size.x,
                                         origin.y + static_cast<float>(row_up) * map.tile_size.y},
                                .size = map.tile_size,
                                .pivot = Vec2{0.0f, 0.0f},
                                .rotation = 0.0f,
                                .uv = Vec4{u + inset_u, v + inset_v, u + tile_u - inset_u,
                                           v + tile_v - inset_v},
                                .color = map.color},
                            map.tileset, map.filter);
                    }
                }
            }
        }
    }

    batches.reserve(runs.size());
    for (const Run& run : runs) {
        batches.push_back(renderer::SpriteBatch{
            .sprites = std::span<const renderer::SpriteQuad>(quads).subspan(run.first, run.count),
            .texture = run.texture,
            .texture_width = run.width,
            .texture_height = run.height,
            .filter = run.filter});
    }
}

} // namespace dream::runtime
