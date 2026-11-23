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
)";
    Camera2D camera;
    SpriteRenderer sprite;
    SpriteAnimator animator;
    TileMap map;
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
    });
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
             }) {
            if (!app.scenes().parse(scene).has_value()) {
                ++failures;
            }
        }
    });
    CHECK(failures == 8);
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
