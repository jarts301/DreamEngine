#include "dream/runtime/app.h"
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
#include <string_view>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec2;
using core::Vec4;
using doctest::Approx;

namespace {

constexpr platform::SurfaceSize k_size{800, 400};

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

// A 4 x 2 white PNG.
constexpr unsigned char k_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7f,
    0xa8, 0x7d, 0x63, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0x8f, 0x06, 0x18, 0xd0, 0x05, 0x00, 0x1e, 0x20, 0x1f, 0xe1, 0x16, 0x9d, 0x85, 0x07, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

// As the atlas cook tool writes it: a coin on the left, and a two-frame sheet on the right.
constexpr std::string_view k_atlas = R"(# Made by dream_atlas_cook.
image = "things.png"
width = 4
height = 2

[regions]
"coin" = [0, 0, 2, 2]
"items/spark" = [2, 0, 2, 2]
)";

struct AtlasFixture {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;

    AtlasFixture() {
        state.assets["shaders/fake_renderer.txt"] = std::vector<std::byte>(4);
        std::vector<std::byte> png;
        for (const unsigned char value : k_png) {
            png.push_back(static_cast<std::byte>(value));
        }
        state.assets["atlases/things.png"] = png;
        state.assets["atlases/things.atlas"] = bytes_of(k_atlas);
        state.assets["atlases/outside.atlas"] =
            bytes_of("image = \"things.png\"\n[regions]\n\"big\" = [2, 0, 4, 2]\n");
        state.assets["atlases/empty.atlas"] = bytes_of("image = \"things.png\"\n");
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

TEST_CASE_FIXTURE(AtlasFixture, "Atlases give their texture and named regions to sprites") {
    std::vector<std::string> names;
    Vec4 spark;
    bool same_texture = false;
    Vec4 region;
    std::string missing;
    in_frame([&](App& app) {
        auto atlas = app.assets().load<SpriteAtlasAsset>("assets:/atlases/things.atlas");
        REQUIRE(atlas.has_value());
        names = atlas.value()->names;
        spark = atlas.value()->region("items/spark").value_or(Vec4{});
        CHECK_FALSE(atlas.value()->region("nothing").has_value());

        SpriteRenderer sprite;
        REQUIRE(use_atlas_region(sprite, atlas.value(), "coin").has_value());
        same_texture = sprite.texture.get() == atlas.value()->texture.get();
        region = sprite.region;
        const core::Result<void> wrong = use_atlas_region(sprite, atlas.value(), "con");
        REQUIRE_FALSE(wrong.has_value());
        missing = wrong.error().message;
    });
    CHECK(names == std::vector<std::string>{"coin", "items/spark"});
    CHECK(spark.x == 2.0f);
    CHECK(spark.z == 2.0f);
    CHECK(same_texture);
    CHECK(region.z == 2.0f);
    CHECK(region.w == 2.0f);
    // A misspelling shows the names there are.
    CHECK(missing.find("\"con\"") != std::string::npos);
    CHECK(missing.find("\"coin\"") != std::string::npos);
}

TEST_CASE_FIXTURE(AtlasFixture, "Atlases with regions outside their image, or none, don't load") {
    std::string outside;
    std::string empty;
    in_frame([&](App& app) {
        auto bad = app.assets().load<SpriteAtlasAsset>("assets:/atlases/outside.atlas");
        REQUIRE_FALSE(bad.has_value());
        outside = bad.error().message;
        auto none = app.assets().load<SpriteAtlasAsset>("assets:/atlases/empty.atlas");
        REQUIRE_FALSE(none.has_value());
        empty = none.error().message;
    });
    CHECK(outside.find("'big' isn't inside its 4 x 2 image") != std::string::npos);
    CHECK(empty.find("no regions") != std::string::npos);
}

TEST_CASE_FIXTURE(AtlasFixture, "Scene files cut sprites from atlases, and draw their region") {
    Vec4 coin_uv;
    Vec2 coin_size;
    Vec4 spark_uv;
    std::size_t batches_count = 0;
    std::string failure;
    in_frame([&](App& app) {
        auto world = app.scenes().parse(R"(
[entities.camera.camera_2d]
height = 10.0

[entities.coin.sprite]
atlas = "assets:/atlases/things.atlas"
region = "coin"
pixels_per_unit = 2.0

[entities.spark.sprite]
atlas = "assets:/atlases/things.atlas"
region = "items/spark"
columns = 2
frame = 1
)");
        if (!world.has_value()) {
            failure = world.error().message;
            return;
        }
        world.value()->update_transforms();
        std::vector<renderer::SpriteQuad> quads;
        std::vector<renderer::SpriteBatch> batches;
        SpriteScratch scratch;
        gather_sprites(*world.value(), *camera_2d_view(*world.value(), k_size), quads, batches,
                       scratch);
        REQUIRE(quads.size() == 2);
        coin_uv = quads[0].uv;
        coin_size = quads[0].size;
        spark_uv = quads[1].uv;
        // One texture, so one batch.
        batches_count = batches.size();
    });
    REQUIRE(failure.empty());
    // The coin is the left half of the 4 x 2 image, 2 pixels at 2 per unit.
    CHECK(coin_uv.x == Approx(0.0f));
    CHECK(coin_uv.z == Approx(0.5f));
    CHECK(coin_uv.y == Approx(0.0f));
    CHECK(coin_uv.w == Approx(1.0f));
    CHECK(coin_size.x == Approx(1.0f));
    CHECK(coin_size.y == Approx(1.0f));
    // The spark's second frame: the right quarter, as its region is split in two columns.
    CHECK(spark_uv.x == Approx(0.75f));
    CHECK(spark_uv.z == Approx(1.0f));
    CHECK(batches_count == 1);
}

TEST_CASE_FIXTURE(AtlasFixture, "Scene files with wrong atlas settings don't load") {
    std::vector<std::string> errors;
    in_frame([&](App& app) {
        for (const char* text : {
                 "[entities.a.sprite]\natlas = \"assets:/atlases/things.atlas\"\n",
                 "[entities.a.sprite]\nregion = \"coin\"\n",
                 "[entities.a.sprite]\ntexture = \"assets:/atlases/things.png\"\n"
                 "atlas = \"assets:/atlases/things.atlas\"\nregion = \"coin\"\n",
                 "[entities.a.sprite]\natlas = \"assets:/atlases/things.atlas\"\nregion = \"gem\"\n",
                 "[entities.a.sprite]\natlas = \"assets:/atlases/missing.atlas\"\nregion = \"a\"\n",
             }) {
            auto world = app.scenes().parse(text);
            errors.push_back(world.has_value() ? std::string() : world.error().message);
        }
    });
    REQUIRE(errors.size() == 5);
    CHECK(errors[0].find("both `atlas` and `region`") != std::string::npos);
    CHECK(errors[1].find("both `atlas` and `region`") != std::string::npos);
    CHECK(errors[2].find("not both") != std::string::npos);
    CHECK(errors[3].find("no region \"gem\"") != std::string::npos);
    CHECK_FALSE(errors[4].empty());
}
