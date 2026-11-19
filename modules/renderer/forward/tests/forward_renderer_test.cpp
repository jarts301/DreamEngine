#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"
#include "fake_device.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <set>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_renderer_forward(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::renderer;
using namespace dream::renderer::tests;
using core::Vec3;
using core::Vec4;
using doctest::Approx;

namespace {

using Calls = std::vector<std::string>;

constexpr platform::SurfaceSize k_size{800, 600};

// The renderer's shader files, as the build makes them for the fake device, less any in `missing`.
struct ShaderFiles {
    std::set<std::string> missing;
    int reads = 0;

    FileReader reader() {
        return [this](std::string_view path) -> core::Result<std::vector<std::byte>> {
            ++reads;
            if (!path.starts_with("assets:/shaders/") || !path.ends_with(".fake") ||
                missing.contains(std::string(path))) {
                return core::Error{core::ErrorCode::NotFound,
                                   std::format("'{}' does not exist", path)};
            }
            return std::vector<std::byte>(4);
        };
    }
};

core::Result<std::unique_ptr<SceneRenderer>> make(std::string_view config_text) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_renderer_forward(registry).has_value());
    const core::Config config = core::Config::parse(config_text).value();
    const std::vector<std::string> preference{"forward"};
    auto created = registry.create<SceneRenderer>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

// A started forward renderer with its shaders, on a device that records what it is told.
struct Fixture {
    FakeDeviceState state;
    FakeDevice device{state};
    ShaderFiles files;
    std::unique_ptr<SceneRenderer> renderer;

    void start(std::string_view config = "") {
        renderer = make(config).value();
        REQUIRE(renderer->start(device).has_value());
        REQUIRE(renderer->load_shaders(files.reader()).has_value());
    }

    // Records one frame and returns the calls it made.
    Calls draw(const Frame& frame, FrameStats* stats = nullptr) {
        state.calls.clear();
        rhi::CommandList* const commands = device.begin_frame().value();
        REQUIRE(commands != nullptr);
        const FrameStats drawn = renderer->render(*commands, frame);
        REQUIRE(device.end_frame().has_value());
        if (stats != nullptr) {
            *stats = drawn;
        }
        return state.calls;
    }
};

// A camera at `eye` looking at the origin.
View view_from(Vec3 eye) {
    const core::Mat4 projection = core::perspective(core::radians(60.0f), 800.0f / 600.0f, 0.1f,
                                                    100.0f);
    return View{.view_projection = projection * core::look_at(eye, Vec3{}, Vec3{0.0f, 1.0f, 0.0f}),
                .eye = eye,
                .z_near = 0.1f,
                .z_far = 100.0f};
}

MeshDraw mesh(std::uint32_t first_index, std::uint32_t texture = 0) {
    return MeshDraw{.vertices = rhi::BufferHandle{.index = 1, .generation = 1},
                    .indices = rhi::BufferHandle{.index = 2, .generation = 1},
                    .first_index = first_index,
                    .index_count = 3,
                    .texture = texture != 0 ? rhi::TextureHandle{.index = texture, .generation = 1}
                                            : rhi::TextureHandle{}};
}

Calls only_drawing(const Calls& calls) {
    Calls result;
    for (const std::string& call : calls) {
        if (call.starts_with("draw") || call.starts_with("bind_texture") || call == "uniforms") {
            result.push_back(call);
        }
    }
    return result;
}

std::ptrdiff_t first(const Calls& calls, std::string_view prefix) {
    return std::ranges::find_if(calls, [&](const std::string& c) { return c.starts_with(prefix); }) -
           calls.begin();
}

// One quad per command.
ui::DrawList quads(std::vector<ui::DrawCommand> commands) {
    ui::DrawList list;
    for (ui::DrawCommand& command : commands) {
        const auto base = static_cast<std::uint32_t>(list.vertices.size());
        list.vertices.resize(list.vertices.size() + 4);
        command.first_index = static_cast<std::uint32_t>(list.indices.size());
        command.index_count = 6;
        for (const std::uint32_t index : {0u, 1u, 2u, 2u, 1u, 3u}) {
            list.indices.push_back(base + index);
        }
        list.commands.push_back(command);
    }
    return list;
}

std::string described(FakeDeviceState& state, std::string_view config) {
    FakeDevice device(state);
    auto renderer = make(config).value();
    const core::Result<void> started = renderer->start(device);
    return started.has_value() ? std::string(renderer->describe())
                               : "error: " + started.error().message;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("The forward renderer picks an HDR format the device draws into") {
    FakeDeviceState state;
    const auto describe = [&](std::string_view config) { return described(state, config); };

    // Neither float format: straight to the window.
    CHECK(contains(describe(""), "without HDR"));

    state.render_target_formats = {rhi::TextureFormat::Rgba16Float,
                                   rhi::TextureFormat::Depth32Float};
    CHECK(contains(describe(""), "RGBA16 float"));

    // The smaller format wins where both work.
    state.render_target_formats.push_back(rhi::TextureFormat::Rg11B10Float);
    CHECK(contains(describe(""), "R11G11B10 float, with PBR Neutral tone mapping"));
    CHECK(contains(describe("[renderer]\nhdr = \"rgba16f\""), "RGBA16 float"));
    CHECK(contains(describe("[renderer]\nhdr = \"off\""), "without HDR"));
    CHECK(contains(describe("[renderer]\ntone_mapping = \"none\""), "with no tone mapping"));

    // Without a depth target to go with it, there is no HDR.
    state.render_target_formats = {rhi::TextureFormat::Rg11B10Float};
    CHECK(contains(describe(""), "without HDR"));

    CHECK(contains(describe("[renderer]\nhdr = \"yes\""), "error: Setting 'renderer.hdr'"));
    CHECK(contains(describe("[renderer]\ntone_mapping = \"filmic\""), "error:"));
    CHECK(contains(describe("[renderer]\nexposure = 0.0"), "error:"));
    CHECK_FALSE(contains(describe("[renderer]\nexposure = 2.5"), "error:"));
}

TEST_CASE("The forward renderer gives shadows per device, unless it can't draw depth") {
    FakeDeviceState state;
    const auto describe = [&](std::string_view config) { return described(state, config); };

    CHECK(contains(describe(""), "shadows are off"));
    CHECK(contains(describe("[renderer.shadows]\nmode = \"on\""), "shadows are off"));

    state.render_target_formats = {rhi::TextureFormat::Depth32Float};
#if defined(__ANDROID__)
    // Phones with at least 4 GB, such as every phone these tests run on.
    CHECK(contains(describe(""), "3 cascades of 1024x1024, reaching 40"));
#else
    CHECK(contains(describe(""), "4 cascades of 2048x2048, reaching 40"));
#endif
    CHECK(contains(describe("[renderer.shadows]\nresolution = 1024"), "of 1024x1024"));
    CHECK(contains(describe("[renderer.shadows]\nmode = \"off\""), "shadows are off"));
    CHECK(contains(describe("[renderer.shadows]\ncascades = 2"), "2 cascades"));
    CHECK(contains(describe("[renderer.shadows]\ndistance = 25.0"), "reaching 25"));

    CHECK(contains(describe("[renderer.shadows]\ncascades = 5"), "error:"));
    CHECK(contains(describe("[renderer.shadows]\nmode = \"maybe\""), "error:"));
    CHECK(contains(describe("[renderer.shadows]\nresolution = 100"), "error:"));
    CHECK(contains(describe("[renderer.shadows]\ndistance = 0.5"), "error:"));
}

TEST_CASE("The forward renderer explains what's wrong when its shaders are missing") {
    Fixture fixture;
    fixture.start();

    // A failed reload keeps the shaders it had, so frames still draw.
    fixture.files.missing.insert("assets:/shaders/forward.vert.fake");
    const core::Result<void> reloaded = fixture.renderer->load_shaders(fixture.files.reader());
    REQUIRE_FALSE(reloaded.has_value());
    CHECK(contains(reloaded.error().message, "forward.vert.fake"));
    CHECK(contains(reloaded.error().message, "renderer module"));

    const std::array meshes{mesh(0)};
    const Calls calls = fixture.draw(Frame{.size = k_size, .view = view_from({0, 0, 5}),
                                           .meshes = meshes});
    CHECK(std::ranges::count(calls, "draw 0+3") == 1);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws each part, with its texture") {
    start();
    const std::array meshes{mesh(0), mesh(3), mesh(0, 7)};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.draws == 3);
    // Uniforms are set once for the frame, and white is bound for untextured parts.
    CHECK(only_drawing(calls) == Calls{"uniforms", "bind_texture 0", "draw 0+3", "bind_texture 0",
                                       "draw 3+3", "bind_texture 7", "draw 0+3"});
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer only clears the screen without a view") {
    start();
    const std::array meshes{mesh(0)};
    const std::array<LineVertex, 2> lines{};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .meshes = meshes, .lines = lines}, &stats);

    CHECK(stats.draws == 0);
    CHECK(stats.lines == 0);
    CHECK(only_drawing(calls).empty());
    CHECK(first(calls, "vertices") == static_cast<std::ptrdiff_t>(calls.size()));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer sends the camera position and the way to the light") {
    start();
    const std::array meshes{mesh(0)};
    draw(Frame{.size = k_size,
               .view = view_from({1.0f, 2.0f, 3.0f}),
               .sun = Sunlight{.direction = Vec3{0.0f, -2.0f, 0.0f}, .ambient = 0.5f},
               .meshes = meshes});

    // The layout of FrameUniforms: a matrix, then three vectors.
    REQUIRE(state.last_uniforms.size() >= 112);
    std::array<float, 12> vectors{};
    std::memcpy(vectors.data(), state.last_uniforms.data() + 64, sizeof(vectors));
    CHECK(vectors[0] == Approx(1.0f));
    CHECK(vectors[2] == Approx(3.0f));
    CHECK(vectors[5] == Approx(1.0f));
    CHECK(vectors[11] == Approx(0.5f));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws the scene in HDR, then tone-maps it") {
    state.render_target_formats = {rhi::TextureFormat::Rg11B10Float,
                                   rhi::TextureFormat::Depth32Float};
    start("[renderer.shadows]\nmode = \"off\"");
    const std::array meshes{mesh(0)};
    const Calls calls = draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes});

    // A color and a depth target the size of the surface.
    std::vector<rhi::TextureFormat> targets;
    for (const rhi::TextureDesc& desc : state.textures) {
        if (desc.render_target && desc.width == k_size.width && desc.height == k_size.height) {
            targets.push_back(desc.format);
        }
    }
    CHECK(targets == std::vector{rhi::TextureFormat::Rg11B10Float,
                                 rhi::TextureFormat::Depth32Float});

    // The model is drawn in the pass into the targets, and the tone mapping reads the color target
    // in slot 1 afterwards.
    const auto pass = first(calls, "render_targets");
    const auto model = first(calls, "draw 0+3");
    REQUIRE(pass < static_cast<std::ptrdiff_t>(calls.size()));
    CHECK(pass < model);
    const auto read =
        std::ranges::find_if(calls, [](const std::string& c) { return c.ends_with(" at 1"); });
    REQUIRE(read != calls.end());
    CHECK(read - calls.begin() > model);

    // The targets are kept while the size stays.
    const auto made = state.textures.size();
    draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes});
    CHECK(state.textures.size() == made);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws shadow casters into the shadow map first") {
    state.render_target_formats = {rhi::TextureFormat::Depth32Float};
    start("[renderer.shadows]\nmode = \"on\"\ncascades = 4\nresolution = 2048");
    std::array meshes{mesh(0), mesh(3), mesh(0)};
    meshes[2].casts_shadows = false;
    FrameStats stats;
    const Calls calls = draw(
        Frame{.size = k_size, .view = view_from({0, 3, 8}), .sun = Sunlight{}, .meshes = meshes},
        &stats);

    // Both casters go into each of the four cascades, and every part is drawn.
    CHECK(stats.shadow_casters == 2 * 4);
    CHECK(stats.draws == 3);

    // The shadow pass comes first, a cell of the atlas at a time, then the scene reads the map in
    // the shadow slot.
    const auto shadow_pass = first(calls, "render_targets");
    const auto shadow_read =
        std::ranges::find_if(calls, [](const std::string& c) { return c.ends_with(" at 2"); }) -
        calls.begin();
    REQUIRE(shadow_pass < static_cast<std::ptrdiff_t>(calls.size()));
    REQUIRE(shadow_read < static_cast<std::ptrdiff_t>(calls.size()));
    CHECK(shadow_pass < shadow_read);
    CHECK(std::ranges::count(calls, "scissor 2048 2048 2048 2048") == 1);

    const auto shadow_map = std::ranges::find_if(state.textures, [](const auto& desc) {
        return desc.render_target && desc.format == rhi::TextureFormat::Depth32Float &&
               desc.width == 4096 && desc.height == 4096;
    });
    CHECK(shadow_map != state.textures.end());
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws no shadows for a light that casts none") {
    state.render_target_formats = {rhi::TextureFormat::Depth32Float};
    start();
    const std::array meshes{mesh(0)};
    FrameStats stats;
    const Calls calls = draw(Frame{.size = k_size,
                                   .view = view_from({0, 3, 8}),
                                   .sun = Sunlight{.casts_shadows = false},
                                   .meshes = meshes},
                             &stats);

    CHECK(stats.shadow_casters == 0);
    CHECK(std::ranges::find(calls, "render_targets") == calls.end());
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws debug lines once, after the models") {
    start();
    const std::array meshes{mesh(0)};
    const std::vector<LineVertex> lines(26);
    FrameStats stats;
    const Calls calls = draw(
        Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes, .lines = lines},
        &stats);

    CHECK(stats.lines == 13);
    CHECK(std::ranges::count(calls, "vertices 26") == 1);
    CHECK(first(calls, "draw 0+3") < first(calls, "vertices 26"));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws interface layers with their textures and clips") {
    start();
    const ui::DrawList list =
        quads({ui::DrawCommand{.texture = 1, .clip = ui::ClipRect{10, 20, 30, 40}},
               ui::DrawCommand{}});
    const ui::DrawList empty;
    const std::array layers{UiLayer{.list = &list, .offset = core::Vec2{20.0f, 100.0f}},
                            UiLayer{.list = &empty}};
    FrameStats stats;
    const Calls calls = draw(Frame{.size = k_size,
                                   .ui_layers = layers,
                                   .ui_texture =
                                       [](ui::TextureId id) {
                                           return id == 1 ? rhi::TextureHandle{.index = 5,
                                                                               .generation = 1}
                                                          : rhi::TextureHandle{};
                                       }},
                             &stats);

    CHECK(stats.ui_commands == 2);
    CHECK(first(calls, std::format("geometry {} 12", 8 * sizeof(ui::Vertex))) <
          static_cast<std::ptrdiff_t>(calls.size()));
    // The clip moves with the layer, and is lifted for the next command.
    CHECK(only_drawing(calls) == Calls{"bind_texture 5", "draw 0+6", "bind_texture 0", "draw 6+6"});
    CHECK(std::ranges::count(calls, "scissor 30 120 30 40") == 1);
    const auto last_scissor = std::find_if(calls.rbegin(), calls.rend(), [](const std::string& call) {
        return call.starts_with("scissor");
    });
    REQUIRE(last_scissor != calls.rend());
    CHECK(*last_scissor == "scissor off");
}
