#include "dream/runtime/tile_map_files.h"

#include "dream/runtime/app.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/sprites.h"
#include "fake_device.h"
#include "fake_platform.h"
#include "fake_renderer.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec2;
using core::Vec3;
using doctest::Approx;
namespace tmf = dream::tile_map_format;

namespace {

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

// A 4 x 2 white PNG: a tileset of two 2 x 2 tiles.
constexpr unsigned char k_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7f,
    0xa8, 0x7d, 0x63, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0x8f, 0x06, 0x18, 0xd0, 0x05, 0x00, 0x1e, 0x20, 0x1f, 0xe1, 0x16, 0x9d, 0x85, 0x07, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::vector<std::byte> png() {
    std::vector<std::byte> bytes;
    for (const unsigned char value : k_png) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

// A 3 x 2 map of 2-pixel tiles: a tile layer, an image layer and an object layer, with its
// tileset in a file of its own.
tmf::Map make_map() {
    tmf::Map map{.columns = 3, .rows = 2, .tile_width = 2, .tile_height = 2};
    map.tilesets.push_back(tmf::Tileset{.source = "../tiles/ground.tsj", .first_tile = 1});
    tmf::Layer ground{.kind = tmf::LayerKind::Tiles, .name = "ground"};
    ground.columns = 3;
    ground.rows = 2;
    // Top row: empty, tile 0 mirrored across, tile 1; bottom row: tile 0 three times.
    ground.tiles = {0, 1 | tmf::k_flipped_horizontally, 2, 1, 1, 1};
    ground.offset_x = 2.0f;
    ground.properties.push_back(tmf::Property{.name = "layer", .type = "int", .value = "5"});
    map.layers.push_back(ground);
    tmf::Layer sky{.kind = tmf::LayerKind::Image, .name = "sky", .opacity = 0.5f};
    sky.image = "sky.png";
    sky.parallax_x = 0.5f;
    sky.repeat_x = true;
    map.layers.push_back(sky);
    tmf::Layer things{.kind = tmf::LayerKind::Objects, .name = "things"};
    things.objects.push_back(tmf::MapObject{.id = 1,
                                            .name = "start",
                                            .type = "spawn",
                                            .x = 1.0f,
                                            .y = 3.0f,
                                            .shape = tmf::ObjectShape::Point});
    // A 4 x 2 area at the top left, turned a quarter clockwise around its corner.
    things.objects.push_back(tmf::MapObject{
        .id = 2,
        .name = "door",
        .type = "trigger",
        .x = 0.0f,
        .y = 0.0f,
        .width = 4.0f,
        .height = 2.0f,
        .rotation = 90.0f,
        .properties = {tmf::Property{.name = "to", .type = "string", .value = "cave"},
                       tmf::Property{.name = "speed", .type = "float", .value = "2.5"},
                       tmf::Property{.name = "locked", .type = "bool", .value = "true"}}});
    // Tile 1 as a tile object, its foot at (4, 4), the map's bottom.
    things.objects.push_back(tmf::MapObject{.id = 3,
                                            .name = "rock",
                                            .x = 4.0f,
                                            .y = 4.0f,
                                            .width = 2.0f,
                                            .height = 2.0f,
                                            .tile = 2 | tmf::k_flipped_horizontally});
    map.layers.push_back(things);
    return map;
}

class FakeFormat final : public tmf::TileMapFormat {
public:
    [[nodiscard]] bool reads(std::string_view path) const override {
        return path.ends_with(".tmj");
    }
    core::Result<tmf::Map> read_map(std::span<const std::byte>, std::string_view) override {
        return make_map();
    }
    core::Result<tmf::Tileset> read_tileset(std::span<const std::byte>,
                                            std::string_view path) override {
        tilesets_read.emplace_back(path);
        tmf::Tileset tileset{.name = "ground",
                             .image = "ground.png",
                             .image_width = 4,
                             .image_height = 2,
                             .tile_width = 2,
                             .tile_height = 2,
                             .columns = 2,
                             .tile_count = 2};
        tileset.tiles.push_back(tmf::TileInfo{
            .id = 1,
            .animation = {tmf::AnimationFrame{.tile = 1, .seconds = 0.2f},
                          tmf::AnimationFrame{.tile = 0, .seconds = 0.6f}},
            .has_collision = false,
            .properties = {}});
        tileset.tiles.push_back(tmf::TileInfo{
            .id = 0,
            .animation = {},
            .has_collision = false,
            .properties = {tmf::Property{.name = "solid", .type = "bool", .value = "true"}}});
        return tileset;
    }
    std::vector<std::string> tilesets_read;
};

struct MapFixture {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;

    MapFixture() {
        state.assets["shaders/fake_renderer.txt"] = std::vector<std::byte>(4);
        state.assets["maps/level.tmj"] = std::vector<std::byte>(4);
        state.assets["tiles/ground.tsj"] = std::vector<std::byte>(4);
        state.assets["tiles/ground.png"] = png();
        state.assets["maps/sky.png"] = png();
    }

    template <typename Body> void in_frame(Body body) {
        struct TestGame final : Game {
            Renderer renderer;
            FakeRendererState* renderer_state = nullptr;
            Body* run = nullptr;
            core::Result<void> on_start(App& app) override {
                return renderer.initialize(app, std::make_unique<FakeRenderer>(*renderer_state));
            }
            void on_update(App&, const core::FrameTime&) override {}
            void on_render(App& app, rhi::CommandList&) override {
                (*run)(app);
            }
        };
        state.script = {k_end_of_frame};
        App app(std::make_unique<FakePlatform>(state), k_config,
                std::make_unique<FakeDevice>(device_state));
        TestGame game;
        game.renderer_state = &renderer_state;
        game.run = &body;
        REQUIRE(app.run(game).has_value());
    }
};

} // namespace

TEST_CASE("Paths named in a file are taken from beside it") {
    CHECK(path_beside("assets:/maps/level.tmj", "../sprites/a.png").value() ==
          "assets:/sprites/a.png");
    CHECK(path_beside("assets:/maps/level.tmj", "tiles.tsj").value() == "assets:/maps/tiles.tsj");
    CHECK(path_beside("assets:/maps/level.tmj", "./sub\\b.png").value() ==
          "assets:/maps/sub/b.png");
    CHECK(path_beside("maps/level.tmj", "a.png").value() == "maps/a.png");
    CHECK_FALSE(path_beside("assets:/level.tmj", "../a.png").has_value());
    CHECK_FALSE(path_beside("assets:/maps/level.tmj", "C:/art/a.png").has_value());
    CHECK_FALSE(path_beside("assets:/maps/level.tmj", "/a.png").has_value());
    CHECK_FALSE(path_beside("assets:/maps/level.tmj", "").has_value());
}

TEST_CASE_FIXTURE(MapFixture, "Map files become tile maps, sprites and objects in the world") {
    FakeFormat format;
    TileMap ground;
    Vec3 ground_position;
    std::size_t ground_count = 0;
    SpriteRenderer sky;
    Parallax sky_parallax;
    Vec3 sky_position;
    MapObject door;
    Vec3 door_position;
    core::Quat door_rotation;
    Vec3 start_position;
    SpriteRenderer rock;
    Vec3 rock_position;
    std::size_t colliders = 0;
    std::string failure;
    in_frame([&](App& app) {
        register_tile_map_files(app.scenes(), app.assets(), &format);
        auto world = app.scenes().parse(R"(
[entities.level.transform]
position = [10.0, 0.0, 0.0]

[entities.level.tile_map_file]
file = "assets:/maps/level.tmj"
tile_size = [0.5, 0.5]
layer = 2
)");
        if (!world.has_value()) {
            failure = world.error().message;
            return;
        }
        World& placed = *world.value();
        placed.update_transforms();
        Registry& registry = placed.registry();
        const Entity level = placed.find("level");
        const TileMapFile& file = registry.get<TileMapFile>(level);
        ground_count = file.layers.size();
        const Entity ground_entity = placed.find("ground");
        ground = registry.get<TileMap>(ground_entity);
        ground_position =
            core::transform_point(registry.get<WorldTransform>(ground_entity).matrix, Vec3{});
        const Entity sky_entity = placed.find("sky");
        sky = registry.get<SpriteRenderer>(sky_entity);
        sky_parallax = registry.get<Parallax>(sky_entity);
        sky_position =
            core::transform_point(registry.get<WorldTransform>(sky_entity).matrix, Vec3{});
        const Entity door_entity = placed.find("door");
        door = registry.get<MapObject>(door_entity);
        door_position =
            core::transform_point(registry.get<WorldTransform>(door_entity).matrix, Vec3{});
        door_rotation = registry.get<Transform>(door_entity).rotation;
        start_position = core::transform_point(
            registry.get<WorldTransform>(placed.find("start")).matrix, Vec3{});
        const Entity rock_entity = placed.find("rock");
        rock = registry.get<SpriteRenderer>(rock_entity);
        rock_position =
            core::transform_point(registry.get<WorldTransform>(rock_entity).matrix, Vec3{});
        // The copies outlive the device, which their textures belong to.
        ground.tileset = nullptr;
        sky.texture = nullptr;
        rock.texture = nullptr;
        const auto made = make_tile_colliders(placed, level);
        if (!made.has_value()) {
            failure = made.error().message;
            return;
        }
        colliders = made.value().size();
    });
    REQUIRE_MESSAGE(failure.empty(), failure);
    CHECK(format.tilesets_read == std::vector<std::string>{"assets:/tiles/ground.tsj"});

    // The tile layer, and the image layer.
    CHECK(ground_count == 2);
    CHECK(ground.columns == 3);
    CHECK(ground.rows == 2);
    CHECK(ground.tiles == std::vector<std::int32_t>{-1, 0, 1, 0, 0, 0});
    CHECK(ground.flips == std::vector<std::uint8_t>{0, k_tile_flip_x, 0, 0, 0, 0});
    CHECK(ground.tile_width == 2);
    CHECK(ground.tile_size.x == Approx(0.5f));
    CHECK(ground.layer == 5);
    CHECK(ground.solid == std::vector<std::int32_t>{0});
    REQUIRE(ground.animations.size() == 1);
    CHECK(ground.animations[0].tile == 1);
    CHECK(ground.animations[0].frames == std::vector<std::int32_t>{1, 0});
    CHECK(ground.animations[0].durations[1] == Approx(0.6f));
    // Moved 2 pixels right, a quarter unit at 2 pixels to half a unit.
    CHECK(ground_position.x == Approx(10.5f));
    CHECK(ground_position.y == Approx(0.0f));

    // The image's top left at the map's top left, 2 by 1 units, half see-through, at the layer
    // after the first.
    CHECK(sky.size.x == Approx(1.0f));
    CHECK(sky.size.y == Approx(0.5f));
    CHECK(sky.pivot.y == Approx(1.0f));
    CHECK(sky.color.w == Approx(0.5f));
    CHECK(sky.layer == 3);
    CHECK(sky_parallax.factor.x == Approx(0.5f));
    CHECK(sky_parallax.repeat_x);
    CHECK(sky_position.y == Approx(1.0f));

    // Objects: the point where it is; the area at its middle, turned with it.
    CHECK(start_position.x == Approx(10.25f));
    CHECK(start_position.y == Approx(0.25f));
    CHECK(door.type == "trigger");
    CHECK(door.size.x == Approx(1.0f));
    CHECK(door.property("to") == "cave");
    CHECK(door.number("speed") == Approx(2.5f));
    CHECK(door.flag("locked"));
    CHECK_FALSE(door.flag("open"));
    CHECK(door.property("missing", "none") == "none");
    // Turned a quarter clockwise around (0, 0), its middle is at (-1, 2) pixels, Y down.
    CHECK(door_position.x == Approx(10.0f - 0.25f));
    CHECK(door_position.y == Approx(1.0f - 0.5f));
    CHECK(door_rotation.z == Approx(-0.7071068f));

    // The tile object: tile 1 of its tileset, mirrored, standing at its foot.
    CHECK(rock.frame == 1);
    CHECK(rock.columns == 2);
    CHECK(rock.flip_x);
    CHECK(rock.pivot.y == Approx(0.0f));
    CHECK(rock_position.x == Approx(10.0f + 2.5f * 0.5f + 0.0f));
    CHECK(rock_position.y == Approx(0.0f));

    // The solid tiles: the bottom row, merged into one box, and the mirrored one above it.
    CHECK(colliders == 2);
}

TEST_CASE_FIXTURE(MapFixture, "Map files fail without a format module, or with a missing image") {
    std::vector<std::string> failures;
    in_frame([&](App& app) {
        register_tile_map_files(app.scenes(), app.assets(), nullptr);
        auto world = app.scenes().parse(
            "[entities.a.tile_map_file]\nfile = \"assets:/maps/level.tmj\"\n");
        failures.push_back(world.has_value() ? "" : world.error().message);

        FakeFormat format;
        register_tile_map_files(app.scenes(), app.assets(), &format);
        world = app.scenes().parse(
            "[entities.a.tile_map_file]\nfile = \"assets:/maps/level.tmj\"\nlayers = 2\n");
        failures.push_back(world.has_value() ? "" : world.error().message);
    });
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].find("tile_map_format") != std::string::npos);
    CHECK(failures[1].find("layers") != std::string::npos);

    state.assets.erase("maps/sky.png");
    std::string missing;
    in_frame([&](App& app) {
        FakeFormat format;
        register_tile_map_files(app.scenes(), app.assets(), &format);
        auto world = app.scenes().parse(
            "[entities.a.tile_map_file]\nfile = \"assets:/maps/level.tmj\"\n");
        missing = world.has_value() ? "" : world.error().message;
    });
    CHECK(missing.find("assets:/maps/sky.png") != std::string::npos);
}
