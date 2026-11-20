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
#include <iterator>
#include <memory>
#include <optional>
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
                    .material = MeshMaterial{
                        .base_color_texture = texture != 0
                                                  ? rhi::TextureHandle{.index = texture,
                                                                       .generation = 1}
                                                  : rhi::TextureHandle{}}};
}

// Draws, uniforms and the textures bound at slot 0, which the base color and the interface use.
Calls only_drawing(const Calls& calls) {
    Calls result;
    for (const std::string& call : calls) {
        if (call.starts_with("draw") || call == "uniforms" ||
            (call.starts_with("bind_texture") && call.find(" at ") == std::string::npos)) {
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
    CHECK(std::ranges::count(calls, "draw 0+3 x1") == 1);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws each part, with its texture") {
    start();
    const std::array meshes{mesh(0), mesh(3), mesh(0, 7)};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.draws == 3);
    CHECK(stats.draw_calls == 3);
    // Uniforms are set once for the frame, and white is bound for untextured parts. Parts are
    // drawn grouped by what they draw, not in the order given.
    CHECK(only_drawing(calls) == Calls{"uniforms", "bind_texture 0", "draw 0+3 x1",
                                       "bind_texture 7", "draw 0+3 x1", "bind_texture 0",
                                       "draw 3+3 x1"});
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

namespace {

// A part one unit across around its origin, moved to `position`.
MeshDraw box_part(Vec3 position) {
    MeshDraw part = mesh(0);
    part.model = core::translation(position);
    part.bounds.add(Vec3{-0.5f, -0.5f, -0.5f});
    part.bounds.add(Vec3{0.5f, 0.5f, 0.5f});
    return part;
}

} // namespace

TEST_CASE_FIXTURE(Fixture, "The forward renderer skips parts out of the camera's view") {
    start();
    // In view, behind the camera, and far off to the side.
    const std::array meshes{box_part(Vec3{0.0f, 0.0f, 0.0f}), box_part(Vec3{0.0f, 0.0f, 20.0f}),
                            box_part(Vec3{60.0f, 0.0f, 0.0f}), mesh(3)};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    // The part without bounds is always drawn.
    CHECK(stats.draws == 2);
    CHECK(stats.culled == 2);
    CHECK(std::ranges::count(calls, "draw 0+3 x1") == 1);
    CHECK(std::ranges::count(calls, "draw 3+3 x1") == 1);
    CHECK(contains(std::string(renderer->describe()), "parts out of view are skipped"));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws everything with culling off") {
    start("[renderer]\nculling = false");
    const std::array meshes{box_part(Vec3{0.0f, 0.0f, 0.0f}), box_part(Vec3{0.0f, 0.0f, 20.0f})};
    FrameStats stats;
    draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.draws == 2);
    CHECK(stats.culled == 0);
    CHECK(contains(std::string(renderer->describe()), "culling is off"));
    CHECK(contains(described(state, "[renderer]\nculling = \"yes\""), "error:"));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer skips casters outside each shadow cascade") {
    state.render_target_formats = {rhi::TextureFormat::Depth32Float};
    start("[renderer.shadows]\nmode = \"on\"\ncascades = 4\nresolution = 1024");
    // One in front of the camera, and one far beyond where shadows reach.
    const std::array meshes{box_part(Vec3{0.0f, 0.0f, 0.0f}), box_part(Vec3{500.0f, 0.0f, 0.0f})};
    FrameStats stats;
    draw(Frame{.size = k_size, .view = view_from({0, 3, 8}), .sun = Sunlight{}, .meshes = meshes},
         &stats);

    // The far one is in no cascade, and the near one in at least one: each is either drawn or
    // skipped in every cascade.
    CHECK(stats.shadow_casters >= 1);
    CHECK(stats.shadow_culled >= 4);
    CHECK(stats.shadow_casters + stats.shadow_culled == 2 * 4);
    CHECK(stats.culled == 1);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws parts that share a mesh as instances of one draw") {
    start();
    std::array meshes{box_part(Vec3{-1.0f, 0.0f, 0.0f}), mesh(3), box_part(Vec3{1.0f, 0.0f, 0.0f})};
    // Other factors don't split a group: they are each instance's.
    meshes[2].material.base_color = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
    meshes[2].material.metallic = 1.0f;
    meshes[2].material.roughness = 0.2f;
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.draws == 3);
    CHECK(stats.draw_calls == 2);
    CHECK(std::ranges::count(calls, "draw 0+3 x2") == 1);
    CHECK(std::ranges::count(calls, "draw 3+3 x1") == 1);

    // Each instance's model matrix, normals and material, 160 bytes, in the order given. The last
    // instances bound are the one part drawn alone.
    const auto group = std::ranges::find(calls, "draw 0+3 x2");
    REQUIRE(group != calls.begin());
    CHECK(*std::prev(group) == "instances 320");
    // That part's material factors, after its matrices and color: a default material's.
    REQUIRE(state.last_instances.size() == 160);
    std::array<float, 4> factors{};
    std::memcpy(factors.data(), state.last_instances.data() + 128, sizeof(factors));
    CHECK(factors == std::array{0.0f, 0.5f, 1.0f, 1.0f});
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer tells the shader which textures a part has") {
    start();
    const auto features_of = [&](const MeshMaterial& material) {
        std::array meshes{mesh(0)};
        meshes[0].material = material;
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes});
        REQUIRE(state.last_instances.size() == 160);
        float features = 0.0f;
        std::memcpy(&features, state.last_instances.data() + 156, sizeof(features));
        return features;
    };
    const rhi::TextureHandle texture{.index = 40, .generation = 1};

    CHECK(features_of(MeshMaterial{}) == 0.0f);
    CHECK(features_of(MeshMaterial{.normal_texture = texture}) == 1.0f);
    CHECK(features_of(MeshMaterial{.metallic_roughness_texture = texture,
                                   .occlusion_texture = texture}) == 6.0f);
    CHECK(features_of(MeshMaterial{.emissive = Vec3{1.0f, 0.0f, 0.0f}}) == 8.0f);
    // A texture with nothing to show is skipped too.
    CHECK(features_of(MeshMaterial{.normal_scale = 0.0f, .normal_texture = texture}) == 0.0f);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer binds a material's textures, or a flat normal map") {
    start();
    std::array meshes{mesh(0), mesh(0)};
    meshes[1].material.normal_texture = rhi::TextureHandle{.index = 40, .generation = 1};
    meshes[1].material.metallic_roughness_texture = rhi::TextureHandle{.index = 41, .generation = 1};
    meshes[1].material.occlusion_texture = rhi::TextureHandle{.index = 42, .generation = 1};
    meshes[1].material.emissive_texture = rhi::TextureHandle{.index = 43, .generation = 1};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    // Different textures make different draws.
    CHECK(stats.draw_calls == 2);
    CHECK(std::ranges::count(calls, "bind_texture 40 at 3") == 1);
    CHECK(std::ranges::count(calls, "bind_texture 41 at 4") == 1);
    CHECK(std::ranges::count(calls, "bind_texture 42 at 5") == 1);
    CHECK(std::ranges::count(calls, "bind_texture 43 at 6") == 1);

    // The part without a normal map gets the renderer's flat one: one linear texel.
    const auto flat = std::ranges::find_if(state.textures, [](const rhi::TextureDesc& desc) {
        return desc.width == 1 && desc.height == 1 &&
               desc.format == rhi::TextureFormat::Rgba8Unorm;
    });
    REQUIRE(flat != state.textures.end());
    const auto flat_index = static_cast<std::size_t>(flat - state.textures.begin()) + 1;
    CHECK(std::ranges::count(calls, std::format("bind_texture {} at 3", flat_index)) == 1);
}

namespace {

// A part with two simpler versions, after its full range: one barely different, one coarse.
MeshDraw part_with_versions(Vec3 position) {
    MeshDraw part = box_part(position);
    static const std::array<MeshLod, 2> versions{
        MeshLod{.first_index = 3, .index_count = 3, .error = 0.001f},
        MeshLod{.first_index = 6, .index_count = 3, .error = 0.1f}};
    part.lods = versions;
    return part;
}

} // namespace

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws simpler versions of parts small on screen") {
    start();
    // Near the camera, the coarse version would stray by pixels, and far away by less than one.
    const std::array meshes{part_with_versions(Vec3{0.0f, 0.0f, 0.0f}),
                            part_with_versions(Vec3{0.0f, 0.0f, -80.0f})};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.simplified == 2);
    CHECK(std::ranges::count(calls, "draw 3+3 x1") == 1);
    CHECK(std::ranges::count(calls, "draw 6+3 x1") == 1);
    CHECK(contains(std::string(renderer->describe()), "simpler versions of parts within 1 pixels"));
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws parts in full with lod_error 0") {
    start("[renderer]\nlod_error = 0.0");
    const std::array meshes{part_with_versions(Vec3{0.0f, 0.0f, -80.0f})};
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.simplified == 0);
    CHECK(std::ranges::count(calls, "draw 0+3 x1") == 1);
    CHECK(contains(described(state, "[renderer]\nlod_error = -1.0"), "error:"));
}

namespace {

// The index of the first lit pipeline made with these settings, as the fake device numbers them.
std::optional<std::uint32_t> pipeline_with(const FakeDeviceState& state, rhi::BlendMode blend,
                                           rhi::CullMode cull, bool fragment_shader = true) {
    for (std::size_t index = 0; index < state.pipelines.size(); ++index) {
        const rhi::PipelineDesc& desc = state.pipelines[index];
        // Lit pipelines draw models, with no depth bias, which the shadow ones have.
        if (desc.blend == blend && desc.cull == cull &&
            desc.fragment_shader.is_valid() == fragment_shader && desc.depth_bias == 0.0f &&
            desc.vertex_stride == sizeof(MeshVertex)) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return std::nullopt;
}

MeshDraw part_with(std::uint32_t first_index, AlphaMode mode, Vec3 position) {
    MeshDraw part = box_part(position);
    part.first_index = first_index;
    part.material.alpha_mode = mode;
    return part;
}

} // namespace

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws opaque, then cut-out, then blended parts, farthest first") {
    start();
    const std::array meshes{
        part_with(9, AlphaMode::Blend, Vec3{0.0f, 0.0f, 2.0f}),
        part_with(6, AlphaMode::Blend, Vec3{0.0f, 0.0f, -2.0f}),
        part_with(3, AlphaMode::Mask, Vec3{1.0f, 0.0f, 0.0f}),
        part_with(0, AlphaMode::Opaque, Vec3{-1.0f, 0.0f, 0.0f}),
    };
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    CHECK(stats.draws == 4);
    CHECK(stats.blended == 2);
    std::vector<std::string> draws;
    for (const std::string& call : calls) {
        if (call.starts_with("draw ") && call.ends_with(" x1")) {
            draws.push_back(call);
        }
    }
    // The blended part at z = -2 is farther from the camera at z = 5 than the one at z = 2.
    CHECK(draws == std::vector<std::string>{"draw 0+3 x1", "draw 3+3 x1", "draw 6+3 x1",
                                            "draw 9+3 x1"});

    // Blended parts use a premultiplied pipeline that keeps depth as it is.
    const auto blended = pipeline_with(state, rhi::BlendMode::PremultipliedAlpha, rhi::CullMode::Back);
    REQUIRE(blended.has_value());
    CHECK_FALSE(state.pipelines[*blended].depth_write);
    const auto bound = std::ranges::find(calls, std::format("pipeline {}", *blended));
    REQUIRE(bound != calls.end());
    CHECK(bound < std::ranges::find(calls, "draw 6+3 x1"));
    CHECK(std::ranges::find(calls, "draw 3+3 x1") < bound);
}

TEST_CASE_FIXTURE(Fixture, "Blended parts cast no shadow, and cut-out ones cast theirs with their texture") {
    state.render_target_formats = {rhi::TextureFormat::Depth32Float};
    start("[renderer.shadows]\nmode = \"on\"\ncascades = 1\nresolution = 1024");
    std::array meshes{
        part_with(0, AlphaMode::Opaque, Vec3{-1.0f, 0.0f, 0.0f}),
        part_with(3, AlphaMode::Mask, Vec3{1.0f, 0.0f, 0.0f}),
        part_with(6, AlphaMode::Blend, Vec3{0.0f, 0.0f, 1.0f}),
    };
    meshes[1].material.base_color_texture = rhi::TextureHandle{.index = 50, .generation = 1};
    FrameStats stats;
    const Calls calls = draw(Frame{.size = k_size,
                                   .view = view_from({0, 0, 5}),
                                   .sun = Sunlight{},
                                   .meshes = meshes},
                             &stats);

    CHECK(stats.shadow_casters == 2);
    // The cut-out caster's texture is bound before it is drawn into the shadow map.
    const auto texture = std::ranges::find(calls, "bind_texture 50");
    REQUIRE(texture != calls.end());
    CHECK(texture < std::ranges::find(calls, "draw 3+3 x1"));
}

TEST_CASE_FIXTURE(Fixture, "Double-sided parts are drawn without culling, and say so to the shader") {
    start();
    std::array meshes{part_with(0, AlphaMode::Opaque, Vec3{}), part_with(3, AlphaMode::Opaque, Vec3{})};
    meshes[1].material.double_sided = true;
    const Calls calls = draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes});

    const auto double_sided = pipeline_with(state, rhi::BlendMode::Opaque, rhi::CullMode::None);
    REQUIRE(double_sided.has_value());
    const auto bound = std::ranges::find(calls, std::format("pipeline {}", *double_sided));
    REQUIRE(bound != calls.end());
    CHECK(bound < std::ranges::find(calls, "draw 3+3 x1"));
    CHECK(std::ranges::find(calls, "draw 0+3 x1") < bound);

    // The last part drawn's features: double-sided (16). Its alpha cutoff rides in the normals.
    REQUIRE(state.last_instances.size() == 160);
    float features = 0.0f;
    float cutoff = 0.0f;
    std::memcpy(&features, state.last_instances.data() + 156, sizeof(features));
    std::memcpy(&cutoff, state.last_instances.data() + 76, sizeof(cutoff));
    CHECK(features == 16.0f);
    CHECK(cutoff == 0.5f);
}

TEST_CASE_FIXTURE(Fixture, "The forward renderer draws skinned parts one each, with their joints") {
    start();
    // Two joints: one in place, one moved by (1, 2, 3).
    const std::array joints{core::Mat4{}, core::translation(Vec3{1.0f, 2.0f, 3.0f})};
    std::array meshes{mesh(0), mesh(0), mesh(3)};
    meshes[0].joint_matrices = joints;
    meshes[1].joint_matrices = joints;
    FrameStats stats;
    const Calls calls =
        draw(Frame{.size = k_size, .view = view_from({0, 0, 5}), .meshes = meshes}, &stats);

    // The skinned parts share a mesh, but each has its own pose, so each is its own draw.
    CHECK(stats.draws == 3);
    CHECK(stats.skinned == 2);
    CHECK(stats.draw_calls == 3);
    // Two joints of three rows of four floats each, set before each skinned draw.
    CHECK(std::ranges::count(calls, "draw_uniforms 96") == 2);
    const auto uploaded = std::ranges::find(calls, "draw_uniforms 96");
    CHECK(uploaded < std::ranges::find(calls, "draw 0+3 x1"));

    // The second joint's rows end in its translation.
    REQUIRE(state.last_draw_uniforms.size() == 96);
    std::array<float, 24> rows{};
    std::memcpy(rows.data(), state.last_draw_uniforms.data(), sizeof(rows));
    CHECK(rows[12] == 1.0f); // Joint 1, row x: (1, 0, 0, 1).
    CHECK(rows[15] == 1.0f);
    CHECK(rows[19] == 2.0f);
    CHECK(rows[23] == 3.0f);
}
