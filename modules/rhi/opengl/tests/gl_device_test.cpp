// Runs the 'opengl' or 'opengles' backend, whichever DREAM_GL_ES picks, drawing to a hidden window.
// It needs a GPU driver with OpenGL 4.3, or with OpenGL ES 3.1 contexts. Without one, such as on a
// build server, the device can't start, and each test says so and passes. The shaders are written
// here in GLSL, laid out as DreamCrossCompileShader.cmake writes a game's.

#include "dream/core/config.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream> // Needed for doctest to print std::string_view.
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
#if DREAM_GL_ES
[[nodiscard]] core::Result<void> register_rhi_opengles(core::ModuleRegistry& registry);
#else
[[nodiscard]] core::Result<void> register_rhi_opengl(core::ModuleRegistry& registry);
#endif
} // namespace dream::modules

using namespace dream;
using namespace dream::rhi;

namespace {

constexpr platform::SurfaceSize k_size{640, 480};

#if DREAM_GL_ES
constexpr std::string_view k_backend = "opengles";
constexpr std::string_view k_suffix = ".essl";
#define DREAM_TEST_VERSION "#version 310 es\n"
#else
constexpr std::string_view k_backend = "opengl";
constexpr std::string_view k_suffix = ".glsl";
#define DREAM_TEST_VERSION "#version 430\n"
#endif

// Follows the bindings of ADR 0026: uniforms at block 0, the texture at 1, push constants at
// block 2.
constexpr std::string_view k_vertex_shader = DREAM_TEST_VERSION R"glsl(
layout(binding = 0, std140) uniform Frame { vec4 tint; } frame;
layout(binding = 2, std140) uniform Draw { vec4 offset; } draw;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 v_uv;
void main() {
    gl_Position = vec4(position, 1.0) + draw.offset;
    v_uv = uv;
}
)glsl";

constexpr std::string_view k_fragment_shader = DREAM_TEST_VERSION R"glsl(
precision mediump float;
layout(binding = 0, std140) uniform Frame { vec4 tint; } frame;
layout(binding = 1) uniform sampler2D base_color;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;
void main() {
    color = texture(base_color, v_uv) * frame.tint;
}
)glsl";

constexpr std::string_view k_misplaced_shader = DREAM_TEST_VERSION R"glsl(
layout(binding = 3, std140) uniform Elsewhere { vec4 value; } elsewhere;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 v_uv;
void main() {
    gl_Position = vec4(position, 1.0) + elsewhere.value;
    v_uv = uv;
}
)glsl";

constexpr std::string_view k_broken_shader = DREAM_TEST_VERSION R"glsl(
void main() { this is not GLSL }
)glsl";

struct Vertex {
    float position[3];
    float uv[2];
};

constexpr std::array<Vertex, 3> k_triangle{{
    {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f}},
    {{0.0f, 0.5f, 0.5f}, {0.5f, 1.0f}},
}};
// Covers the whole surface.
constexpr std::array<Vertex, 3> k_full_screen{{
    {{-1.0f, -1.0f, 0.5f}, {0.0f, 0.0f}},
    {{3.0f, -1.0f, 0.5f}, {2.0f, 0.0f}},
    {{-1.0f, 3.0f, 0.5f}, {0.0f, 2.0f}},
}};
constexpr std::array<std::uint16_t, 3> k_indices{0, 1, 2};
constexpr std::array<VertexAttribute, 2> k_attributes{
    VertexAttribute{0, VertexFormat::Float3, offsetof(Vertex, position)},
    VertexAttribute{1, VertexFormat::Float2, offsetof(Vertex, uv)},
};
constexpr std::array<std::byte, 16> k_constants{};

std::span<const std::byte> text(std::string_view source) {
    return std::as_bytes(std::span(source.data(), source.size()));
}

// Null, with a message, where the driver can't run the backend.
std::unique_ptr<Device> make_device() {
    core::ModuleRegistry registry;
#if DREAM_GL_ES
    REQUIRE(modules::register_rhi_opengles(registry).has_value());
#else
    REQUIRE(modules::register_rhi_opengl(registry).has_value());
#endif
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{std::string(k_backend)};
    auto created = registry.create<Device>(preference, config);
    if (!created.has_value()) {
        MESSAGE("The device can't start here, so this test is skipped: "
                << created.error().message);
        return nullptr;
    }
    return std::move(created.value().instance);
}

// A window that is never shown, with a client area of k_size.
class HiddenWindow {
public:
    HiddenWindow() {
        WNDCLASSW window_class{};
        window_class.style = CS_OWNDC;
        window_class.lpfnWndProc = &DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"DreamOpenGLTest";
        RegisterClassW(&window_class); // Fails harmlessly once the class exists.
        RECT rect{0, 0, static_cast<LONG>(k_size.width), static_cast<LONG>(k_size.height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        m_window = CreateWindowExW(0, window_class.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                   rect.bottom - rect.top, nullptr, nullptr,
                                   window_class.hInstance, nullptr);
        if (m_window == nullptr) {
            MESSAGE("No window can be created here, so this test is skipped");
        }
    }

    ~HiddenWindow() {
        if (m_window != nullptr) {
            DestroyWindow(m_window);
        }
    }

    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;

    [[nodiscard]] bool exists() const {
        return m_window != nullptr;
    }

    [[nodiscard]] platform::NativeWindow native() const {
        return platform::Win32Window{GetModuleHandleW(nullptr), m_window};
    }

private:
    HWND m_window = nullptr;
};

struct Shaders {
    ShaderHandle vertex;
    ShaderHandle fragment;
};

Shaders make_shaders(Device& device, std::string_view vertex_source = k_vertex_shader) {
    const auto vertex = device.create_shader(ShaderStage::Vertex, text(vertex_source));
    REQUIRE_MESSAGE(vertex.has_value(), vertex.error().message);
    const auto fragment = device.create_shader(ShaderStage::Fragment, text(k_fragment_shader));
    REQUIRE_MESSAGE(fragment.has_value(), fragment.error().message);
    return Shaders{vertex.value(), fragment.value()};
}

core::Result<PipelineHandle> try_pipeline(Device& device, const Shaders& shaders,
                                          std::span<const VertexAttribute> attributes) {
    return device.create_pipeline(PipelineDesc{
        .vertex_shader = shaders.vertex,
        .fragment_shader = shaders.fragment,
        .vertex_stride = sizeof(Vertex),
        .attributes = attributes,
        .depth_test = false,
        .cull_back_faces = false,
    });
}

PipelineHandle make_pipeline(Device& device, const Shaders& shaders,
                             std::span<const VertexAttribute> attributes = k_attributes) {
    const auto pipeline = try_pipeline(device, shaders, attributes);
    REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().message);
    return pipeline.value();
}

// A device and a window it draws to, or neither where that can't be.
struct Drawing {
    std::unique_ptr<Device> device = make_device();
    HiddenWindow window;

    [[nodiscard]] bool ready() {
        if (device == nullptr || !window.exists()) {
            return false;
        }
        const auto attached = device->attach_surface(window.native(), k_size);
        REQUIRE_MESSAGE(attached.has_value(), attached.error().message);
        return true;
    }
};

std::array<std::byte, 4> pixel_at(const CapturedFrame& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * frame.width + x) * 4;
    return {frame.pixels[at], frame.pixels[at + 1], frame.pixels[at + 2], frame.pixels[at + 3]};
}

} // namespace

TEST_CASE("OpenGL device starts and loads its own kind of GLSL") {
    const auto device = make_device();
    if (device == nullptr) {
        return;
    }
    CHECK(device->shader_file_suffix() == k_suffix);
    const auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame.value() == nullptr);
}

TEST_CASE("OpenGL device warns when it closes with objects never destroyed") {
    struct Warnings final : core::LogSink {
        void write(const core::LogMessage& message) override {
            if (message.level == core::LogLevel::Warning) {
                texts.emplace_back(message.text);
            }
        }
        std::vector<std::string> texts;
    };
    constexpr std::array k_pixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    auto device = make_device();
    if (device == nullptr) {
        return;
    }
    Warnings warnings;
    core::add_log_sink(warnings);
    REQUIRE(device->create_texture(TextureDesc{.width = 1, .height = 1}, k_pixel).has_value());
    REQUIRE(device->create_buffer(BufferUsage::Vertex, k_pixel).has_value());
    device.reset();
    core::remove_log_sink(warnings);
    const bool warned = std::ranges::any_of(warnings.texts, [](const std::string& message) {
        return message.find("never destroyed: 1 textures, 1 buffers, 0 shaders and 0 pipelines") !=
               std::string::npos;
    });
    CHECK(warned);
}

TEST_CASE("OpenGL device rejects invalid resources") {
    const auto device = make_device();
    if (device == nullptr) {
        return;
    }
    CHECK_FALSE(device->create_buffer(BufferUsage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, k_constants).has_value());
    CHECK_FALSE(
        device->create_texture(TextureDesc{.width = 1, .height = 1}, k_constants).has_value());
    CHECK_FALSE(device->create_texture(TextureDesc{.width = 0, .height = 1}, {}).has_value());

    const auto broken = device->create_shader(ShaderStage::Vertex, text(k_broken_shader));
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().message.find("Compiling a vertex shader failed") != std::string::npos);

    // The other kind of GLSL is refused by its first line.
#if DREAM_GL_ES
    constexpr std::string_view k_other = "#version 430\nvoid main() {}\n";
#else
    constexpr std::string_view k_other = "#version 310 es\nvoid main() {}\n";
#endif
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, text(k_other)).has_value());

    const Shaders shaders = make_shaders(*device);
    SUBCASE("a pipeline before any surface") {
        CHECK_FALSE(try_pipeline(*device, shaders, k_attributes).has_value());
    }
    SUBCASE("a pipeline with a destroyed shader") {
        const HiddenWindow window;
        if (!window.exists()) {
            return;
        }
        REQUIRE(device->attach_surface(window.native(), k_size).has_value());
        device->destroy_shader(shaders.vertex);
        CHECK_FALSE(try_pipeline(*device, shaders, k_attributes).has_value());
    }
    device->destroy_shader(shaders.vertex);
    device->destroy_shader(shaders.fragment);
}

TEST_CASE("OpenGL device refuses shaders that read other bindings") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    const Shaders shaders = make_shaders(*drawing.device, k_misplaced_shader);
    const auto pipeline = try_pipeline(*drawing.device, shaders, k_attributes);
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().message.find("is at binding 3") != std::string::npos);
    drawing.device->destroy_shader(shaders.vertex);
    drawing.device->destroy_shader(shaders.fragment);
}

TEST_CASE("OpenGL device matches vertex attributes to the shader's inputs by location") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    const Shaders shaders = make_shaders(*drawing.device);

    SUBCASE("a missing input is an error") {
        const std::array one{k_attributes[0]};
        const auto pipeline = try_pipeline(*drawing.device, shaders, one);
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(pipeline.error().message.find("input 1 (uv)") != std::string::npos);
    }
    SUBCASE("an attribute the shader doesn't read is ignored") {
        const std::array three{k_attributes[0], k_attributes[1],
                               VertexAttribute{2, VertexFormat::Float2, offsetof(Vertex, uv)}};
        drawing.device->destroy_pipeline(make_pipeline(*drawing.device, shaders, three));
    }
    drawing.device->destroy_shader(shaders.vertex);
    drawing.device->destroy_shader(shaders.fragment);
}

TEST_CASE("OpenGL device draws frames to a hidden window") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    Device& device = *drawing.device;
    const Shaders shaders = make_shaders(device);
    const PipelineHandle pipeline = make_pipeline(device, shaders);
    const BufferHandle vertices =
        device.create_buffer(BufferUsage::Vertex, std::as_bytes(std::span(k_triangle))).value();
    const BufferHandle indices =
        device.create_buffer(BufferUsage::Index, std::as_bytes(std::span(k_indices))).value();
    constexpr std::array<std::byte, 16> k_pixels{};
    const TextureHandle texture =
        device.create_texture(TextureDesc{.width = 2, .height = 2}, k_pixels).value();
    const std::array<std::uint32_t, 3> frame_indices{0, 1, 2};

    for (int frame = 0; frame < 3; ++frame) {
        CommandList* const commands = device.begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{0.1f, 0.2f, 0.3f, 1.0f});
        commands->bind_pipeline(pipeline);
        commands->set_uniforms(k_constants);
        commands->push_constants(k_constants);
        commands->bind_texture(texture);
        commands->bind_vertex_buffer(vertices);
        commands->bind_index_buffer(indices, IndexFormat::UInt16);
        commands->draw_indexed(3);

        commands->set_scissor(ScissorRect{.x = 10, .y = 20, .width = 300, .height = 200});
        commands->bind_texture(TextureHandle{});
        CHECK(commands->draw_vertices(std::as_bytes(std::span(k_triangle)), 3));
        CHECK(commands->bind_frame_geometry(std::as_bytes(std::span(k_triangle)), frame_indices));
        commands->draw_indexed_range(0, 3);
        commands->set_scissor(std::nullopt);
        commands->end_render_pass();
        REQUIRE(device.end_frame().has_value());
    }

    // Frames stop while the window has no size, and resume when it has one again.
    device.resize_surface({0, 0});
    CHECK(device.begin_frame().value() == nullptr);
    device.resize_surface(k_size);
    CommandList* const commands = device.begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->end_render_pass();
    REQUIRE(device.end_frame().has_value());

    // Resources outlive the window, and a window can be attached again.
    device.detach_surface();
    CHECK(device.begin_frame().value() == nullptr);
    REQUIRE(device.attach_surface(drawing.window.native(), k_size).has_value());
    CHECK(device.begin_frame().value() != nullptr);
    REQUIRE(device.end_frame().has_value());

    device.destroy_texture(texture);
    device.destroy_buffer(indices);
    device.destroy_buffer(vertices);
    device.destroy_pipeline(pipeline);
    device.destroy_shader(shaders.fragment);
    device.destroy_shader(shaders.vertex);
}

TEST_CASE("OpenGL device captures a frame as RGBA, top row first, with sRGB colors") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    Device& device = *drawing.device;
    CHECK_FALSE(device.take_captured_frame().has_value());
    const Shaders shaders = make_shaders(device);
    const PipelineHandle pipeline = make_pipeline(device, shaders);

    REQUIRE(device.capture_next_frame());
    CommandList* const commands = device.begin_frame().value();
    REQUIRE(commands != nullptr);
    // Red, then the top half, by the interface's top-left rectangles, in a linear 0.2 gray, which
    // sRGB encoding stores as about 124.
    commands->begin_render_pass(ClearColor{1.0f, 0.0f, 0.0f, 1.0f});
    commands->bind_pipeline(pipeline);
    const std::array<float, 4> gray{0.2f, 0.2f, 0.2f, 1.0f};
    commands->set_uniforms(std::as_bytes(std::span(gray)));
    commands->set_scissor(ScissorRect{.x = 0, .y = 0, .width = k_size.width,
                                      .height = k_size.height / 2});
    CHECK(commands->draw_vertices(std::as_bytes(std::span(k_full_screen)), 3));
    commands->end_render_pass();
    REQUIRE(device.end_frame().has_value());

    const std::optional<CapturedFrame> frame = device.take_captured_frame();
    REQUIRE(frame.has_value());
    CHECK(frame->width == k_size.width);
    CHECK(frame->height == k_size.height);
    REQUIRE(frame->pixels.size() == std::size_t{k_size.width} * k_size.height * 4);

    const auto top = pixel_at(*frame, k_size.width / 2, k_size.height / 4);
    CHECK(std::to_integer<int>(top[0]) >= 118);
    CHECK(std::to_integer<int>(top[0]) <= 130);
    CHECK(top[0] == top[1]);
    CHECK(top[1] == top[2]);
    CHECK(top[3] == std::byte{255});

    const auto bottom = pixel_at(*frame, k_size.width / 2, k_size.height * 3 / 4);
    CHECK(bottom[0] == std::byte{255});
    CHECK(bottom[1] == std::byte{0});
    CHECK(bottom[2] == std::byte{0});
    // Given once.
    CHECK_FALSE(device.take_captured_frame().has_value());

    device.destroy_pipeline(pipeline);
    device.destroy_shader(shaders.fragment);
    device.destroy_shader(shaders.vertex);
}

TEST_CASE("OpenGL device limits the vertex data drawn in one frame") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    Device& device = *drawing.device;
    const Shaders shaders = make_shaders(device);
    const PipelineHandle pipeline = make_pipeline(device, shaders);
    const std::vector<std::byte> half(k_max_frame_vertex_bytes / 2);
    constexpr std::uint32_t k_vertices_in_half =
        static_cast<std::uint32_t>(k_max_frame_vertex_bytes / 2 / sizeof(Vertex));

    for (int frame = 0; frame < 2; ++frame) {
        CommandList* const commands = device.begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{});
        commands->bind_pipeline(pipeline);
        CHECK(commands->draw_vertices(std::span(half).first(k_vertices_in_half * sizeof(Vertex)),
                                      k_vertices_in_half));
        CHECK(commands->draw_vertices(std::span(half).first(k_vertices_in_half * sizeof(Vertex)),
                                      k_vertices_in_half));
        CHECK_FALSE(commands->draw_vertices(std::as_bytes(std::span(k_triangle)), 3));
        commands->end_render_pass();
        REQUIRE(device.end_frame().has_value());
    }
    device.destroy_pipeline(pipeline);
    device.destroy_shader(shaders.fragment);
    device.destroy_shader(shaders.vertex);
}

TEST_CASE("OpenGL device measures how long the GPU takes for each frame") {
    Drawing drawing;
    if (!drawing.ready()) {
        return;
    }
    Device& device = *drawing.device;
    CHECK_FALSE(device.take_gpu_frame_seconds().has_value());

    std::vector<double> seconds;
    for (int frame = 0; frame < 300 && seconds.size() < 5; ++frame) {
        CommandList* const commands = device.begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{0.1f, 0.2f, 0.3f, 1.0f});
        commands->end_render_pass();
        REQUIRE(device.end_frame().has_value());
        while (const std::optional<double> taken = device.take_gpu_frame_seconds()) {
            seconds.push_back(*taken);
        }
    }
    REQUIRE(seconds.size() >= 5);
    for (const double time : seconds) {
        CHECK(time >= 0.0);
        CHECK(time < 1.0);
    }
}

TEST_CASE("OpenGL device takes the compressed formats it says it supports") {
    const auto device = make_device();
    if (device == nullptr) {
        return;
    }
    // 8x8 with every level: 4 blocks, then 1, 1 and 1.
    const std::vector<std::byte> blocks(7 * 16);
    for (const TextureFormat format : {TextureFormat::Bc7Srgb, TextureFormat::Astc4x4Srgb}) {
        const auto texture = device->create_texture(
            TextureDesc{.width = 8, .height = 8, .format = format, .mip_levels = 4}, blocks);
        CHECK(texture.has_value() == device->supports_texture_format(format));
        if (texture.has_value()) {
            device->destroy_texture(texture.value());
        }
    }
#if !DREAM_GL_ES
    // BC7 is core in OpenGL 4.2.
    CHECK(device->supports_texture_format(TextureFormat::Bc7Srgb));
#endif
}
