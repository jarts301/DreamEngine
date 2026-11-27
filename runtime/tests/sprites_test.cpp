#include "dream/runtime/sprites.h"

#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/renderer.h"
#include "fake_device.h"
#include "fake_platform.h"
#include "fake_renderer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <variant>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec2;
using core::Vec3;
using doctest::Approx;

namespace {

constexpr platform::SurfaceSize k_size{800, 400};

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

struct SpriteFixture {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;

    SpriteFixture() {
        state.assets["shaders/fake_renderer.txt"] = std::vector<std::byte>(4);
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
            void on_render(App& app, rhi::CommandList& commands) override {
                (*run)(app, renderer, commands);
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

Entity add_camera(World& world, Vec2 position, float height) {
    const Entity camera = world.create_entity();
    world.registry().get<Transform>(camera).position = Vec3{position.x, position.y, 0.0f};
    world.registry().emplace<Camera2D>(camera, Camera2D{.height = height});
    return camera;
}

} // namespace

TEST_CASE("A 2D camera sees its height of the world, and as much across as the window's shape") {
    World world;
    add_camera(world, Vec2{10.0f, 5.0f}, 4.0f);
    world.update_transforms();
    const auto view = camera_2d_view(world, k_size);
    REQUIRE(view.has_value());
    // The camera's middle is the window's middle; 4 units up, 8 across.
    const Vec3 middle = core::transform_point(*view, Vec3{10.0f, 5.0f, 0.0f});
    CHECK(middle.x == Approx(0.0f));
    CHECK(middle.y == Approx(0.0f));
    const Vec3 corner = core::transform_point(*view, Vec3{14.0f, 7.0f, 0.0f});
    CHECK(corner.x == Approx(1.0f));
    CHECK(corner.y == Approx(1.0f));

    const auto top_left = screen_to_world_2d(world, k_size, Vec2{0.0f, 0.0f});
    REQUIRE(top_left.has_value());
    CHECK(top_left->x == Approx(6.0f));
    CHECK(top_left->y == Approx(7.0f));
    CHECK_FALSE(camera_2d_view(World{}, k_size).has_value());

    // On an upright window, min_width keeps 8 units across, and the view grows taller.
    world.registry().get<Camera2D>(world.registry().view<Camera2D>().front()).min_width = 8.0f;
    const auto upright = camera_2d_view(world, platform::SurfaceSize{400, 800});
    REQUIRE(upright.has_value());
    const Vec3 edge = core::transform_point(*upright, Vec3{14.0f, 13.0f, 0.0f});
    CHECK(edge.x == Approx(1.0f));
    CHECK(edge.y == Approx(1.0f));
}

TEST_CASE("Sprites are gathered in order of layer and order, with their frame and flips") {
    World world;
    add_camera(world, Vec2{}, 10.0f);
    const Entity back = world.create_entity();
    world.registry().emplace<SpriteRenderer>(back, SpriteRenderer{.layer = -1});
    const Entity hero = world.create_entity();
    world.registry().get<Transform>(hero).position = Vec3{2.0f, 1.0f, 0.0f};
    world.registry().get<Transform>(hero).scale = Vec3{2.0f, 1.0f, 1.0f};
    world.registry().emplace<SpriteRenderer>(hero, SpriteRenderer{.columns = 4,
                                                                  .rows = 2,
                                                                  .frame = 5,
                                                                  .size = {1.0f, 1.5f},
                                                                  .flip_x = true,
                                                                  .layer = 1});
    const Entity hidden = world.create_entity();
    world.registry().emplace<SpriteRenderer>(hidden, SpriteRenderer{.visible = false});
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 2);
    // Both without a texture, so one batch.
    REQUIRE(batches.size() == 1);
    CHECK(batches[0].sprites.size() == 2);
    const renderer::SpriteQuad& drawn = quads[1];
    CHECK(drawn.position.x == Approx(2.0f));
    CHECK(drawn.size.x == Approx(2.0f));
    CHECK(drawn.size.y == Approx(1.5f));
    // Frame 5 of a 4 x 2 sheet: column 1, row 1, flipped across.
    CHECK(drawn.uv.x == Approx(0.5f));
    CHECK(drawn.uv.z == Approx(0.25f));
    CHECK(drawn.uv.y == Approx(0.5f));
    CHECK(drawn.uv.w == Approx(1.0f));
}

TEST_CASE("Tile maps give their tiles in view, from their bottom left corner") {
    World world;
    add_camera(world, Vec2{1.0f, 1.0f}, 2.0f);
    const Entity level = world.create_entity();
    world.registry().get<Transform>(level).position = Vec3{0.0f, 0.0f, 0.0f};
    TileMap map{.columns = 20, .rows = 2};
    map.tiles.assign(40, -1);
    map.set_tile(0, 1, 3); // Bottom left.
    map.set_tile(1, 0, 4); // Above and right of it.
    map.set_tile(19, 1, 5); // Far out of view.
    world.registry().emplace<TileMap>(level, map);
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 2);
    CHECK(quads[0].position.x == Approx(0.0f));
    CHECK(quads[0].position.y == Approx(0.0f));
    CHECK(quads[0].pivot.x == Approx(0.0f));
    CHECK(quads[1].position.x == Approx(1.0f));
    CHECK(quads[1].position.y == Approx(1.0f));

    const TileMap& placed = world.registry().get<TileMap>(level);
    CHECK(placed.tile(1, 0) == 4);
    CHECK(placed.tile(25, 0) == -1);
    const auto cell = placed.cell_at(Vec2{}, Vec2{1.5f, 1.5f});
    REQUIRE(cell.has_value());
    CHECK((*cell)[0] == 1);
    CHECK((*cell)[1] == 0);
    CHECK_FALSE(placed.cell_at(Vec2{}, Vec2{-0.5f, 0.5f}).has_value());
}

TEST_CASE("Parallax moves sprites and tile maps by part of the camera's move, and repeats them") {
    World world;
    // Sees from 0 to 20 across, and -5 to 5 up.
    add_camera(world, Vec2{10.0f, 0.0f}, 10.0f);
    const Entity hills = world.create_entity();
    world.registry().emplace<SpriteRenderer>(hills, SpriteRenderer{.size = {4.0f, 2.0f}});
    world.registry().emplace<Parallax>(hills, Parallax{.factor = {0.5f, 1.0f}});
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 1);
    // Half of the camera's 10 across.
    CHECK(quads[0].position.x == Approx(5.0f));
    CHECK(quads[0].position.y == Approx(0.0f));

    // Repeated every 4 units, from [3, 7] at 5: the copies that reach into 0 to 20.
    world.registry().get<Parallax>(hills).repeat_x = true;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 6);
    CHECK(quads.front().position.x == Approx(1.0f));
    CHECK(quads.back().position.x == Approx(21.0f));

    // A tile map 2 tiles across, fixed on screen and repeated across it.
    world.registry().remove<SpriteRenderer>(hills);
    TileMap strip{.columns = 2, .rows = 1};
    strip.tiles = {0, 1};
    world.registry().emplace<TileMap>(hills, strip);
    world.registry().get<Parallax>(hills).factor = Vec2{0.0f, 0.0f};
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    // Starting at 10, where the camera is, and every 2 units to either side: 10 copies of 2 tiles.
    REQUIRE(quads.size() == 20);
    CHECK(quads[0].position.x == Approx(0.0f));
    CHECK(quads[0].position.y == Approx(0.0f));
    CHECK(quads[19].position.x == Approx(19.0f));
}

TEST_CASE("9-slice sprites keep their corners' size and stretch the rest") {
    World world;
    add_camera(world, Vec2{}, 10.0f);
    const Entity panel = world.create_entity();
    world.registry().get<Transform>(panel).position = Vec3{1.0f, 1.0f, 0.0f};
    // A 16 x 16 texture with 4-pixel borders, at 16 pixels per unit: corners of a quarter unit.
    // An invalid handle, which the fake device is never asked to destroy.
    FakeDeviceState device_state;
    FakeDevice device(device_state);
    const AssetRef<TextureAsset> texture(std::make_shared<const TextureAsset>(TextureAsset{
        .texture = GpuTexture(device, rhi::TextureHandle{}), .width = 16, .height = 16}));
    world.registry().emplace<SpriteRenderer>(panel,
                                             SpriteRenderer{.texture = texture,
                                                            .size = {4.0f, 2.0f},
                                                            .pivot = {0.0f, 0.0f},
                                                            .slice = {4.0f, 4.0f, 4.0f, 4.0f}});
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 9);
    // Bottom left corner, bottom row first: the texture's bottom left 4 x 4 pixels.
    CHECK(quads[0].position.x == Approx(1.0f));
    CHECK(quads[0].position.y == Approx(1.0f));
    CHECK(quads[0].size.x == Approx(0.25f));
    CHECK(quads[0].size.y == Approx(0.25f));
    CHECK(quads[0].uv.x == Approx(0.0f));
    CHECK(quads[0].uv.z == Approx(0.25f));
    CHECK(quads[0].uv.y == Approx(0.75f));
    CHECK(quads[0].uv.w == Approx(1.0f));
    // The middle stretches over the rest, from the texture's middle 8 x 8 pixels.
    CHECK(quads[4].position.x == Approx(1.25f));
    CHECK(quads[4].position.y == Approx(1.25f));
    CHECK(quads[4].size.x == Approx(3.5f));
    CHECK(quads[4].size.y == Approx(1.5f));
    CHECK(quads[4].uv.x == Approx(0.25f));
    CHECK(quads[4].uv.z == Approx(0.75f));
    // Top right corner.
    CHECK(quads[8].position.x == Approx(4.75f));
    CHECK(quads[8].position.y == Approx(2.75f));
    CHECK(quads[8].uv.x == Approx(0.75f));
    CHECK(quads[8].uv.y == Approx(0.0f));

    // Turned a quarter turn around its pivot: the top right corner goes up and to the left.
    world.registry().get<Transform>(panel).rotation =
        core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 1.5707963f);
    world.update_transforms();
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 9);
    CHECK(quads[8].position.x == Approx(1.0f - 1.75f));
    CHECK(quads[8].position.y == Approx(1.0f + 3.75f));
    CHECK(quads[8].rotation == Approx(1.5707963f));

    // Narrower than its borders: the corners shrink to fit, and the middle column goes.
    world.registry().get<Transform>(panel).rotation = core::Quat{};
    world.registry().get<SpriteRenderer>(panel).size = Vec2{0.25f, 2.0f};
    world.update_transforms();
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 6);
    CHECK(quads[0].size.x == Approx(0.125f));
}

TEST_CASE("Layers sorted by height draw what stands lower over what stands higher") {
    World world;
    const Entity camera = add_camera(world, Vec2{}, 10.0f);
    world.registry().get<Camera2D>(camera).y_sort_layers = {1};
    const auto add_sprite = [&](float y, std::int32_t layer, std::int32_t order) {
        const Entity entity = world.create_entity();
        world.registry().get<Transform>(entity).position = Vec3{0.0f, y, 0.0f};
        world.registry().emplace<SpriteRenderer>(
            entity, SpriteRenderer{.size = {1.0f, 1.0f}, .layer = layer, .order = order});
        return entity;
    };
    add_sprite(-1.0f, 1, 0);  // In front: lowest.
    add_sprite(2.0f, 1, 5);   // Behind, despite its order.
    add_sprite(0.5f, 0, 9);   // A layer below, drawn first.
    add_sprite(-3.0f, 2, -9); // A layer above, not sorted by height.
    // A tile map in the sorted layer: its rows stand among the sprites, at their bottoms.
    const Entity trees = world.create_entity();
    world.registry().get<Transform>(trees).position = Vec3{2.0f, -2.0f, 0.0f};
    TileMap map{.columns = 1, .rows = 3, .layer = 1};
    map.tiles = {7, -1, 8}; // From the top: a row at y 0, an empty one, and one at y -2.
    world.registry().emplace<TileMap>(trees, map);
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    std::vector<float> heights;
    for (const renderer::SpriteQuad& quad : quads) {
        heights.push_back(quad.position.y);
    }
    // Layer 0; then layer 1 from the highest down: the sprite at 2, the tiles' row at 0, the
    // sprite at -1 and the row at -2; then layer 2.
    CHECK(heights == std::vector<float>{0.5f, 2.0f, 0.0f, -1.0f, -2.0f, -3.0f});
}

TEST_CASE("Animated tiles show their frames in turn, as the map's animation time goes on") {
    World world;
    add_camera(world, Vec2{}, 10.0f);
    const Entity water = world.create_entity();
    TileMap map{.columns = 2, .rows = 1};
    map.tiles = {4, 1};
    map.animations.push_back(TileAnimation{.tile = 4, .frames = {4, 5, 6}, .fps = 2.0f});
    world.registry().emplace<TileMap>(water, map);
    world.update_transforms();

    // Without a tileset, the tileset is one tile across, 16 texture heights per tile: each tile's
    // number is its top edge over 16.
    const auto shown = [&] {
        std::vector<renderer::SpriteQuad> quads;
        std::vector<renderer::SpriteBatch> batches;
        SpriteScratch scratch;
        gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
        REQUIRE(quads.size() == 2);
        return std::pair(quads[0].uv.y / 16.0f, quads[1].uv.y / 16.0f);
    };
    CHECK(shown().first == Approx(4.0f).epsilon(0.01));
    update_sprite_animators(world, 0.6);
    CHECK(shown().first == Approx(5.0f).epsilon(0.01));
    update_sprite_animators(world, 1.0);
    // 1.6 s: the fourth frame, which wraps to the first; the other tile never changes.
    CHECK(shown().first == Approx(4.0f).epsilon(0.01));
    CHECK(shown().second == Approx(1.0f).epsilon(0.01));
    CHECK(world.registry().get<TileMap>(water).tiles[0] == 4);

    // Held still.
    world.registry().get<TileMap>(water).animation_speed = 0.0f;
    update_sprite_animators(world, 0.5);
    CHECK(shown().first == Approx(4.0f).epsilon(0.01));
}

TEST_CASE("Tile maps mirror and turn their cells, and read tilesets with margins and spacing") {
    World world;
    add_camera(world, Vec2{}, 10.0f);
    // A 12 x 12 tileset of 4 x 4 tiles, 1 pixel in from its edge and 2 apart: 2 tiles across.
    FakeDeviceState device_state;
    FakeDevice device(device_state);
    const AssetRef<TextureAsset> tileset(std::make_shared<const TextureAsset>(TextureAsset{
        .texture = GpuTexture(device, rhi::TextureHandle{}), .width = 12, .height = 12}));
    const Entity level = world.create_entity();
    TileMap map{.tileset = tileset,
                .tile_width = 4,
                .tile_height = 4,
                .tile_margin = 1,
                .tile_spacing = 2,
                .columns = 4,
                .rows = 1};
    map.tiles = {3, 3, 3, 3};
    map.flips = {0, k_tile_flip_x, k_tile_flip_diagonal,
                 static_cast<std::uint8_t>(k_tile_flip_diagonal | k_tile_flip_x)};
    world.registry().emplace<TileMap>(level, map);
    world.update_transforms();

    std::vector<renderer::SpriteQuad> quads;
    std::vector<renderer::SpriteBatch> batches;
    SpriteScratch scratch;
    gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
    REQUIRE(quads.size() == 4);
    // Tile 3: the second across, second down, from (7, 7) to (11, 11) in pixels.
    CHECK(quads[0].uv.x == Approx(7.0f / 12.0f).epsilon(0.002));
    CHECK(quads[0].uv.y == Approx(7.0f / 12.0f).epsilon(0.002));
    CHECK(quads[0].uv.z == Approx(11.0f / 12.0f).epsilon(0.002));
    CHECK(quads[0].rotation == Approx(0.0f));
    // Mirrored across: left and right swap.
    CHECK(quads[1].uv.x == Approx(11.0f / 12.0f).epsilon(0.002));
    CHECK(quads[1].uv.z == Approx(7.0f / 12.0f).epsilon(0.002));
    // Across the diagonal: turned a quarter around its middle, and mirrored across.
    CHECK(quads[2].rotation == Approx(1.5707963f));
    CHECK(quads[2].pivot.x == Approx(0.5f));
    CHECK(quads[2].position.x == Approx(2.5f));
    CHECK(quads[2].position.y == Approx(0.5f));
    CHECK(quads[2].uv.x == Approx(11.0f / 12.0f).epsilon(0.002));
    CHECK(quads[2].uv.y == Approx(7.0f / 12.0f).epsilon(0.002));
    // And mirrored across as well: a quarter turn to the right, which mirrors it up and down.
    CHECK(quads[3].uv.x == Approx(11.0f / 12.0f).epsilon(0.002));
    CHECK(quads[3].uv.y == Approx(11.0f / 12.0f).epsilon(0.002));
    CHECK(quads[3].uv.w == Approx(7.0f / 12.0f).epsilon(0.002));
}

TEST_CASE("Animated tiles can give each frame a length of its own") {
    World world;
    add_camera(world, Vec2{}, 10.0f);
    const Entity torch = world.create_entity();
    TileMap map{.columns = 1, .rows = 1};
    map.tiles = {2};
    map.animations.push_back(
        TileAnimation{.tile = 2, .frames = {2, 3}, .fps = 1.0f, .durations = {0.1f, 0.5f}});
    world.registry().emplace<TileMap>(torch, map);
    world.update_transforms();
    const auto shown = [&] {
        std::vector<renderer::SpriteQuad> quads;
        std::vector<renderer::SpriteBatch> batches;
        SpriteScratch scratch;
        gather_sprites(world, *camera_2d_view(world, k_size), quads, batches, scratch);
        REQUIRE(quads.size() == 1);
        return quads[0].uv.y / 16.0f;
    };
    CHECK(shown() == Approx(2.0f).epsilon(0.01));
    update_sprite_animators(world, 0.2);
    CHECK(shown() == Approx(3.0f).epsilon(0.01));
    update_sprite_animators(world, 0.45);
    // 0.65 s wraps to 0.05 s.
    CHECK(shown() == Approx(2.0f).epsilon(0.01));
}

TEST_CASE("Sprite animators step through their clip's frames") {
    World world;
    const Entity hero = world.create_entity();
    world.registry().emplace<SpriteRenderer>(hero);
    SpriteAnimator animator;
    animator.clips.push_back(SpriteClip{.name = "run", .frames = {4, 5, 6}, .fps = 10.0f});
    animator.clips.push_back(
        SpriteClip{.name = "jump", .frames = {8, 9}, .fps = 10.0f, .loop = false});
    animator.play("run");
    world.registry().emplace<SpriteAnimator>(hero, animator);

    update_sprite_animators(world, 0.15);
    CHECK(world.registry().get<SpriteRenderer>(hero).frame == 5);
    update_sprite_animators(world, 0.2);
    // 0.35 s wraps to 0.05 s: the first frame again.
    CHECK(world.registry().get<SpriteRenderer>(hero).frame == 4);

    SpriteAnimator& playing = world.registry().get<SpriteAnimator>(hero);
    playing.play("jump");
    update_sprite_animators(world, 1.0);
    CHECK(world.registry().get<SpriteRenderer>(hero).frame == 9);
    CHECK_FALSE(playing.playing);
    CHECK(playing.duration("run") == Approx(0.3f));
}

TEST_CASE("Tile colliders cover solid tiles with merged static boxes") {
    World world;
    const Entity level = world.create_entity();
    world.registry().get<Transform>(level).position = Vec3{10.0f, 0.0f, 0.0f};
    TileMap map{.tile_size = {1.0f, 1.0f}, .columns = 4, .rows = 3};
    // Top to bottom:  .  .  .  #
    //                 #  #  .  #
    //                 #  #  .  .
    map.tiles = {-1, -1, -1, 1, 1, 1, -1, 1, 1, 2, -1, -1};
    map.solid = {1, 2};
    world.registry().emplace<TileMap>(level, map);
    world.update_transforms();

    const auto made = make_tile_colliders(world, level);
    REQUIRE(made.has_value());
    REQUIRE(made.value().size() == 2);
    std::vector<std::pair<Vec3, Vec2>> boxes;
    for (const Entity collider : made.value()) {
        const RigidBody2D& body = world.registry().get<RigidBody2D>(collider);
        CHECK(body.desc.motion == physics2d::MotionType::Static);
        const auto* shape = std::get_if<physics2d::BoxShape>(&body.desc.shape);
        REQUIRE(shape != nullptr);
        boxes.emplace_back(world.registry().get<Transform>(collider).position,
                           shape->half_extents);
    }
    std::ranges::sort(boxes, {}, [](const auto& box) { return box.first.x; });
    // The 2 x 2 block at the bottom left.
    CHECK(boxes[0].first.x == Approx(11.0f));
    CHECK(boxes[0].first.y == Approx(1.0f));
    CHECK(boxes[0].second.x == Approx(1.0f));
    CHECK(boxes[0].second.y == Approx(1.0f));
    // The column of two at the right.
    CHECK(boxes[1].first.x == Approx(13.5f));
    CHECK(boxes[1].first.y == Approx(2.0f));
    CHECK(boxes[1].second.y == Approx(1.0f));

    CHECK_FALSE(make_tile_colliders(world, world.create_entity()).has_value());
}

TEST_CASE_FIXTURE(SpriteFixture, "Scene files set up 2D cameras, sprites, animators and tile maps") {
    const std::string_view scene = R"(
[entities.camera.camera_2d]
height = 12.0

[entities.hero.sprite]
columns = 4
rows = 2
frame = 3
pixels_per_unit = 32.0
pivot = [0.5, 0.0]
color = [1.0, 0.5, 0.5]
alpha = 0.75
flip_x = true
layer = 2
filter = "smooth"

[entities.hero.sprite_animator]
clip = "run"

[entities.hero.sprite_animator.clips.run]
frames = [0, 1, 2, 3]
fps = 12.0

[entities.hero.sprite_animator.clips.jump]
frames = [4]
loop = false

[entities.level.tile_map]
tile_pixels = [8, 8]
tile_size = [0.5, 0.5]
solid = [1]
rows = [
    "..#",
    "##=",
]

[entities.level.tile_map.legend]
"#" = 1
"=" = 7

[entities.level.tile_map.animations."="]
frames = [7, 8]
fps = 3.0

[entities.level.tile_map.animations.12]
frames = [12, -1]

[entities.camera.parallax]
factor = [0.25, 1.0]
repeat_x = true

[entities.panel.sprite]
slice = [3, 4, 5, 6]

[entities.button.sprite]
slice = 2
)";
    Camera2D camera;
    SpriteRenderer sprite;
    SpriteAnimator animator;
    TileMap map;
    Parallax parallax;
    core::Vec4 panel_slice;
    core::Vec4 button_slice;
    in_frame([&](App& app, Renderer&, rhi::CommandList&) {
        auto world = app.scenes().parse(scene);
        if (!world.has_value()) {
            FAIL(world.error().message);
        }
        Registry& registry = world.value()->registry();
        camera = registry.get<Camera2D>(world.value()->find("camera"));
        sprite = registry.get<SpriteRenderer>(world.value()->find("hero"));
        animator = registry.get<SpriteAnimator>(world.value()->find("hero"));
        map = registry.get<TileMap>(world.value()->find("level"));
        parallax = registry.get<Parallax>(world.value()->find("camera"));
        panel_slice = registry.get<SpriteRenderer>(world.value()->find("panel")).slice;
        button_slice = registry.get<SpriteRenderer>(world.value()->find("button")).slice;
    });
    CHECK(parallax.factor.x == Approx(0.25f));
    CHECK(parallax.repeat_x);
    CHECK_FALSE(parallax.repeat_y);
    CHECK(panel_slice == core::Vec4{3.0f, 4.0f, 5.0f, 6.0f});
    CHECK(button_slice == core::Vec4{2.0f, 2.0f, 2.0f, 2.0f});
    REQUIRE(map.animations.size() == 2);
    const auto water = std::ranges::find(map.animations, 7, &TileAnimation::tile);
    REQUIRE(water != map.animations.end());
    CHECK(water->frames == std::vector<std::int32_t>{7, 8});
    CHECK(water->fps == Approx(3.0f));
    const auto blink = std::ranges::find(map.animations, 12, &TileAnimation::tile);
    REQUIRE(blink != map.animations.end());
    CHECK(blink->frames == std::vector<std::int32_t>{12, -1});
    CHECK(camera.height == Approx(12.0f));
    CHECK(sprite.columns == 4);
    CHECK(sprite.frame == 3);
    CHECK(sprite.pixels_per_unit == Approx(32.0f));
    CHECK(sprite.pivot.y == Approx(0.0f));
    CHECK(sprite.color.y == Approx(0.5f));
    CHECK(sprite.color.w == Approx(0.75f));
    CHECK(sprite.flip_x);
    CHECK(sprite.layer == 2);
    CHECK(sprite.filter == renderer::SpriteFilter::Smooth);
    CHECK(animator.clip == "run");
    REQUIRE(animator.clips.size() == 2);
    const auto run = std::ranges::find(animator.clips, "run", &SpriteClip::name);
    REQUIRE(run != animator.clips.end());
    CHECK(run->frames == std::vector<std::uint32_t>{0, 1, 2, 3});
    CHECK(run->fps == Approx(12.0f));
    CHECK(map.columns == 3);
    CHECK(map.rows == 2);
    CHECK(map.tile_width == 8);
    CHECK(map.tile_size.x == Approx(0.5f));
    CHECK(map.tiles == std::vector<std::int32_t>{-1, -1, 1, 1, 1, 7});
    CHECK(map.solid == std::vector<std::int32_t>{1});
}

TEST_CASE_FIXTURE(SpriteFixture, "Scene files with wrong sprite settings don't load") {
    int failures = 0;
    in_frame([&](App& app, Renderer&, rhi::CommandList&) {
        for (const std::string_view scene : {
                 "[entities.a.sprite]\nfilter = \"blurry\"\n",
                 "[entities.a.sprite]\ncolumns = 0\n",
                 "[entities.a.sprite]\ntexture = \"assets:/missing.png\"\n",
                 "[entities.a.camera_2d]\nheight = 0.0\n",
                 "[entities.a.sprite_animator.clips.run]\nfps = 10.0\n",
                 "[entities.a.tile_map]\nrows = [\"ab\"]\n[entities.a.tile_map.legend]\na = 1\n",
                 "[entities.a.tile_map]\ntiles = [1, 2, 3]\ncolumns = 2\n",
                 "[entities.a.tile_map]\ntile_size = [1.0, 1.0]\n",
                 "[entities.a.sprite]\nslice = [1, 2]\n",
                 "[entities.a.sprite]\nslice = -1\n",
                 "[entities.a.camera_2d]\ny_sort_layers = [1.5]\n",
                 "[entities.a.parallax]\nspeed = 1.0\n",
                 "[entities.a.tile_map]\ntiles = [1]\ncolumns = 1\n"
                 "[entities.a.tile_map.animations.water]\nframes = [1, 2]\n",
                 "[entities.a.tile_map]\ntiles = [1]\ncolumns = 1\n"
                 "[entities.a.tile_map.animations.1]\nfps = 2.0\n",
             }) {
            if (!app.scenes().parse(scene).has_value()) {
                ++failures;
            }
        }
    });
    CHECK(failures == 14);
}

TEST_CASE_FIXTURE(SpriteFixture, "Renderer gives sprites through the 2D camera, and lines with it") {
    World world;
    in_frame([&](App&, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world, Vec2{}, 10.0f);
        const Entity hero = world.create_entity();
        world.registry().emplace<SpriteRenderer>(hero);
        world.update_transforms();
        renderer.debug_lines().line(Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, core::Vec4{1, 1, 1, 1});
        renderer.render(world, commands, k_size);
        world.registry().clear();
    });
    REQUIRE_FALSE(renderer_state.frames.empty());
    const FakeFrame& frame = renderer_state.frames.back();
    CHECK_FALSE(frame.view.has_value());
    CHECK(frame.sprite_view.has_value());
    REQUIRE(frame.sprites.size() == 1);
    CHECK(frame.sprites[0].size() == 1);
    CHECK(frame.lines.size() == 2);
}
