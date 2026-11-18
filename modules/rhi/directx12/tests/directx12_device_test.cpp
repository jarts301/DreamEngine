// Runs the backend on WARP, Windows' software rasterizer, drawing to a hidden window, so it needs
// neither a GPU nor a display. The shaders are compiled here from HLSL, because a game's shaders
// are compiled by its own build.

#include "dream/core/config.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <doctest/doctest.h>

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

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
[[nodiscard]] core::Result<void> register_rhi_directx12(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::rhi;

namespace {

constexpr platform::SurfaceSize k_size{640, 480};

// Follows the register layout of ADR 0013.
constexpr std::string_view k_shaders = R"hlsl(
cbuffer Frame : register(b0) { float4 tint; };
cbuffer Draw : register(b1) { float4 offset; };
Texture2D base_color : register(t0);
SamplerState base_sampler : register(s0);

struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };

Output vertex_main(Input input) {
    Output output;
    output.position = float4(input.position, 1.0) + offset;
    output.uv = input.uv;
    return output;
}

float4 fragment_main(Output input) : SV_Target {
    return base_color.Sample(base_sampler, input.uv) * tint;
}
)hlsl";

constexpr std::string_view k_misplaced_shader = R"hlsl(
cbuffer Elsewhere : register(b2) { float4 value; };
float4 vertex_main(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0) + value;
}
)hlsl";

struct Vertex {
    float position[3];
    float uv[2];
};

constexpr std::array<Vertex, 3> k_triangle{{
    {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f}},
    {{0.0f, 0.5f, 0.5f}, {0.5f, 1.0f}},
}};
constexpr std::array<std::uint16_t, 3> k_indices{0, 1, 2};
constexpr std::array<VertexAttribute, 2> k_attributes{
    VertexAttribute{0, VertexFormat::Float3, offsetof(Vertex, position)},
    VertexAttribute{1, VertexFormat::Float2, offsetof(Vertex, uv)},
};
constexpr std::array<std::byte, 16> k_constants{};

std::vector<std::byte> compile(std::string_view source, const char* entry, const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result =
        D3DCompile(source.data(), source.size(), "test.hlsl", nullptr, nullptr, entry, profile,
                   D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
        const std::string message(errors ? static_cast<const char*>(errors->GetBufferPointer())
                                         : "no details");
        FAIL("Compiling the test shader failed: " << message);
        return {};
    }
    const auto* const bytes = static_cast<const std::byte*>(code->GetBufferPointer());
    return std::vector<std::byte>(bytes, bytes + code->GetBufferSize());
}

std::unique_ptr<Device> make_device() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_rhi_directx12(registry).has_value());

    const core::Config config = core::Config::parse("[rhi.directx12]\nsoftware = true\n").value();
    const std::vector<std::string> preference{"directx12"};
    auto created = registry.create<Device>(preference, config);
    REQUIRE_MESSAGE(created.has_value(), created.error().message);
    return std::move(created.value().instance);
}

// A window that is never shown, which DXGI can still present to. A build server may run tests
// where no window can exist at all, and then the tests that need one are skipped.
class HiddenWindow {
public:
    HiddenWindow() {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"DreamDirectX12Test";
        RegisterClassW(&window_class); // Fails harmlessly once the class exists.
        m_window = CreateWindowExW(0, window_class.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(k_size.width),
                                   static_cast<int>(k_size.height), nullptr, nullptr,
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

Shaders make_shaders(Device& device) {
    const auto vertex =
        device.create_shader(ShaderStage::Vertex, compile(k_shaders, "vertex_main", "vs_5_0"));
    REQUIRE_MESSAGE(vertex.has_value(), vertex.error().message);
    const auto fragment =
        device.create_shader(ShaderStage::Fragment, compile(k_shaders, "fragment_main", "ps_5_0"));
    REQUIRE_MESSAGE(fragment.has_value(), fragment.error().message);
    return Shaders{vertex.value(), fragment.value()};
}

PipelineHandle make_pipeline(Device& device, const Shaders& shaders,
                             std::span<const VertexAttribute> attributes = k_attributes) {
    const auto pipeline = device.create_pipeline(PipelineDesc{
        .vertex_shader = shaders.vertex,
        .fragment_shader = shaders.fragment,
        .vertex_stride = sizeof(Vertex),
        .attributes = attributes,
    });
    REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().message);
    return pipeline.value();
}

} // namespace

TEST_CASE("Direct3D 12 device starts on WARP and loads DXBC shaders") {
    const auto device = make_device();
    CHECK(device->shader_file_suffix() == ".dxbc");

    const auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame.value() == nullptr);
}

TEST_CASE("Direct3D 12 device warns when it closes with objects never destroyed") {
    struct Warnings final : core::LogSink {
        void write(const core::LogMessage& message) override {
            if (message.level == core::LogLevel::Warning) {
                texts.emplace_back(message.text);
            }
        }
        std::vector<std::string> texts;
    };
    constexpr std::array k_pixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    Warnings warnings;
    core::add_log_sink(warnings);

    SUBCASE("left behind") {
        auto device = make_device();
        REQUIRE(device->create_texture(TextureDesc{.width = 1, .height = 1}, k_pixel).has_value());
        REQUIRE(device->create_buffer(BufferUsage::Vertex, k_pixel).has_value());
        device.reset();
        const bool warned = std::ranges::any_of(warnings.texts, [](const std::string& text) {
            return text.find("never destroyed: 1 textures, 1 buffers, 0 shaders and 0 pipelines") !=
                   std::string::npos;
        });
        CHECK(warned);
    }
    SUBCASE("all destroyed") {
        auto device = make_device();
        const auto texture = device->create_texture(TextureDesc{.width = 1, .height = 1}, k_pixel);
        REQUIRE(texture.has_value());
        device->destroy_texture(texture.value());
        device.reset();
        const bool warned = std::ranges::any_of(warnings.texts, [](const std::string& text) {
            return text.find("never destroyed") != std::string::npos;
        });
        CHECK_FALSE(warned);
    }
    core::remove_log_sink(warnings);
}

TEST_CASE("Direct3D 12 device rejects invalid resources") {
    const auto device = make_device();

    CHECK_FALSE(device->create_buffer(BufferUsage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, k_constants).has_value());
    CHECK_FALSE(
        device->create_texture(TextureDesc{.width = 1, .height = 1}, k_constants).has_value());
    CHECK_FALSE(device->create_texture(TextureDesc{.width = 0, .height = 1}, {}).has_value());

    SUBCASE("a pipeline before any surface") {
        const Shaders shaders = make_shaders(*device);
        PipelineDesc desc;
        desc.vertex_shader = shaders.vertex;
        desc.fragment_shader = shaders.fragment;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
    SUBCASE("a pipeline with a destroyed shader") {
        const HiddenWindow window;
        if (!window.exists()) {
            return;
        }
        REQUIRE(device->attach_surface(window.native(), k_size).has_value());
        const Shaders shaders = make_shaders(*device);
        device->destroy_shader(shaders.vertex);
        PipelineDesc desc;
        desc.vertex_shader = shaders.vertex;
        desc.fragment_shader = shaders.fragment;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
}

TEST_CASE("Direct3D 12 device refuses shaders that read other registers") {
    const auto device = make_device();
    const auto shader = device->create_shader(ShaderStage::Vertex,
                                              compile(k_misplaced_shader, "vertex_main", "vs_5_0"));
    REQUIRE_FALSE(shader.has_value());
    CHECK(shader.error().message.find("register(b1)") != std::string::npos);
}

TEST_CASE("Direct3D 12 device matches vertex attributes to the shader's inputs in order") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    const Shaders shaders = make_shaders(*device);

    SUBCASE("a missing input is an error") {
        const std::array one{k_attributes[0]};
        const auto pipeline = device->create_pipeline(PipelineDesc{
            .vertex_shader = shaders.vertex,
            .fragment_shader = shaders.fragment,
            .vertex_stride = sizeof(Vertex),
            .attributes = one,
        });
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(pipeline.error().message.find("TEXCOORD0") != std::string::npos);
    }
    SUBCASE("an attribute the shader doesn't read is ignored") {
        const std::array three{k_attributes[0], k_attributes[1],
                               VertexAttribute{2, VertexFormat::Float2, offsetof(Vertex, uv)}};
        make_pipeline(*device, shaders, three);
    }
}

TEST_CASE("Direct3D 12 device draws a frame to a hidden window") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());

    const Shaders shaders = make_shaders(*device);
    const PipelineHandle pipeline = make_pipeline(*device, shaders);
    const BufferHandle vertices =
        device->create_buffer(BufferUsage::Vertex, std::as_bytes(std::span(k_triangle))).value();
    const BufferHandle indices =
        device->create_buffer(BufferUsage::Index, std::as_bytes(std::span(k_indices))).value();
    constexpr std::array<std::byte, 16> k_pixels{};
    const TextureHandle texture =
        device->create_texture(TextureDesc{.width = 2, .height = 2}, k_pixels).value();
    const std::array<std::uint32_t, 3> frame_indices{0, 1, 2};

    for (int frame = 0; frame < 3; ++frame) {
        CommandList* const commands = device->begin_frame().value();
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
        REQUIRE(device->end_frame().has_value());
    }

    // Frames stop while the window has no size, and resume when it has one again.
    device->resize_surface({0, 0});
    CHECK(device->begin_frame().value() == nullptr);
    device->resize_surface({320, 200});
    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->end_render_pass();
    REQUIRE(device->end_frame().has_value());

    device->detach_surface();
    CHECK(device->begin_frame().value() == nullptr);

    device->destroy_texture(texture);
    device->destroy_buffer(indices);
    device->destroy_buffer(vertices);
    device->destroy_pipeline(pipeline);
    device->destroy_shader(shaders.fragment);
    device->destroy_shader(shaders.vertex);
    device->destroy_buffer(vertices);
}

TEST_CASE("Direct3D 12 device captures a presented frame as RGBA") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    CHECK_FALSE(device->take_captured_frame().has_value());

    REQUIRE(device->capture_next_frame());
    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    // Pure red, which is the same before and after sRGB encoding, and shows if red and blue swap.
    commands->begin_render_pass(ClearColor{1.0f, 0.0f, 0.0f, 1.0f});
    commands->end_render_pass();
    REQUIRE(device->end_frame().has_value());

    const std::optional<CapturedFrame> frame = device->take_captured_frame();
    REQUIRE(frame.has_value());
    CHECK(frame->width == k_size.width);
    CHECK(frame->height == k_size.height);
    REQUIRE(frame->pixels.size() == std::size_t{k_size.width} * k_size.height * 4);
    const std::size_t middle =
        (std::size_t{k_size.height} / 2 * k_size.width + k_size.width / 2) * 4;
    CHECK(frame->pixels[middle + 0] == std::byte{255});
    CHECK(frame->pixels[middle + 1] == std::byte{0});
    CHECK(frame->pixels[middle + 2] == std::byte{0});
    CHECK(frame->pixels[middle + 3] == std::byte{255});
    // Given once.
    CHECK_FALSE(device->take_captured_frame().has_value());
}

TEST_CASE("Direct3D 12 device limits the vertex data drawn in one frame") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device, make_shaders(*device));
    const std::vector<std::byte> half(k_max_frame_vertex_bytes / 2);
    constexpr std::uint32_t k_vertices_in_half =
        static_cast<std::uint32_t>(k_max_frame_vertex_bytes / 2 / sizeof(Vertex));

    for (int frame = 0; frame < 2; ++frame) {
        CommandList* const commands = device->begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{});
        commands->bind_pipeline(pipeline);
        CHECK(commands->draw_vertices(std::span(half).first(k_vertices_in_half * sizeof(Vertex)),
                                      k_vertices_in_half));
        CHECK(commands->draw_vertices(std::span(half).first(k_vertices_in_half * sizeof(Vertex)),
                                      k_vertices_in_half));
        CHECK_FALSE(commands->draw_vertices(std::as_bytes(std::span(k_triangle)), 3));
        commands->end_render_pass();
        REQUIRE(device->end_frame().has_value());
    }
}

TEST_CASE("Direct3D 12 device measures how long the GPU takes for each frame") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    CHECK_FALSE(device->take_gpu_frame_seconds().has_value());

    // The GPU runs behind the CPU, so the first frames' times arrive while later ones are drawn.
    // The frame limit only keeps a broken measurement from looping forever.
    std::vector<double> seconds;
    for (int frame = 0; frame < 300 && seconds.size() < 5; ++frame) {
        CommandList* const commands = device->begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{0.1f, 0.2f, 0.3f, 1.0f});
        commands->end_render_pass();
        REQUIRE(device->end_frame().has_value());
        while (const std::optional<double> taken = device->take_gpu_frame_seconds()) {
            seconds.push_back(*taken);
        }
    }

    REQUIRE(seconds.size() >= 5);
    for (const double time : seconds) {
        CHECK(time >= 0.0);
        CHECK(time < 1.0);
    }
    CHECK(std::ranges::any_of(seconds, [](double time) { return time > 0.0; }));
}

TEST_CASE("Direct3D 12 device draws with its uniforms, push constants and texture") {
    struct Errors final : core::LogSink {
        void write(const core::LogMessage& message) override {
            if (message.level >= core::LogLevel::Error) {
                texts.emplace_back(message.text);
            }
        }
        std::vector<std::string> texts;
    };
    Errors errors;
    core::add_log_sink(errors);
    {
        const auto device = make_device();
        const HiddenWindow window;
        if (!window.exists()) {
            core::remove_log_sink(errors);
            return;
        }
        REQUIRE(device->attach_surface(window.native(), k_size).has_value());
        const Shaders shaders = make_shaders(*device);
        const PipelineHandle pipeline = make_pipeline(*device, shaders);
        const BufferHandle vertices =
            device->create_buffer(BufferUsage::Vertex, std::as_bytes(std::span(k_triangle))).value();
        const BufferHandle indices =
            device->create_buffer(BufferUsage::Index, std::as_bytes(std::span(k_indices))).value();
        // Green, with a mipmap chain, so the uploads of several levels are exercised.
        std::vector<std::byte> green(8 * 8 * 4);
        for (std::size_t pixel = 0; pixel < green.size(); pixel += 4) {
            green[pixel + 1] = std::byte{255};
            green[pixel + 3] = std::byte{255};
        }
        const TextureHandle texture =
            device
                ->create_texture(TextureDesc{.width = 8, .height = 8,
                                             .format = TextureFormat::Rgba8Unorm},
                                 green)
                .value();

        // Draws the triangle over the middle of the window, moved by `offset_x` in clip space,
        // tinted by `tint`, and reads back the middle pixel.
        const auto draw = [&](float offset_x, std::array<float, 4> tint) {
            const std::array<float, 4> offset{offset_x, 0.0f, 0.0f, 0.0f};
            REQUIRE(device->capture_next_frame());
            CommandList* const commands = device->begin_frame().value();
            REQUIRE(commands != nullptr);
            commands->begin_render_pass(ClearColor{0.0f, 0.0f, 1.0f, 1.0f});
            commands->bind_pipeline(pipeline);
            commands->set_uniforms(std::as_bytes(std::span(tint)));
            commands->push_constants(std::as_bytes(std::span(offset)));
            commands->bind_texture(texture);
            commands->bind_vertex_buffer(vertices);
            commands->bind_index_buffer(indices, IndexFormat::UInt16);
            commands->draw_indexed(3);
            commands->end_render_pass();
            REQUIRE(device->end_frame().has_value());
            const std::optional<CapturedFrame> frame = device->take_captured_frame();
            REQUIRE(frame.has_value());
            const std::size_t middle =
                (std::size_t{k_size.height} / 2 * k_size.width + k_size.width / 2) * 4;
            return std::array{std::to_integer<int>(frame->pixels[middle + 0]),
                              std::to_integer<int>(frame->pixels[middle + 1]),
                              std::to_integer<int>(frame->pixels[middle + 2])};
        };

        CHECK(draw(0.0f, {1.0f, 1.0f, 1.0f, 1.0f}) == std::array{0, 255, 0});
        // The tint halves green, which the sRGB window shows brighter than half.
        const std::array tinted = draw(0.0f, {1.0f, 0.5f, 1.0f, 1.0f});
        CHECK(tinted[0] == 0);
        CHECK(tinted[1] > 128);
        CHECK(tinted[1] < 255);
        // The push constants move the triangle away, leaving the clear color.
        CHECK(draw(2.0f, {1.0f, 1.0f, 1.0f, 1.0f}) == std::array{0, 0, 255});

        device->destroy_texture(texture);
        device->destroy_buffer(indices);
        device->destroy_buffer(vertices);
        device->destroy_pipeline(pipeline);
        device->destroy_shader(shaders.fragment);
        device->destroy_shader(shaders.vertex);
    }
    core::remove_log_sink(errors);
    CHECK(errors.texts.empty());
    for (const std::string& text : errors.texts) {
        MESSAGE(text);
    }
}

TEST_CASE("Direct3D 12 device takes BC7 textures with their levels, but not ASTC") {
    const auto device = make_device();
    CHECK(device->supports_texture_format(TextureFormat::Bc7Srgb));
    CHECK(device->supports_texture_format(TextureFormat::Bc7Unorm));
    CHECK_FALSE(device->supports_texture_format(TextureFormat::Astc4x4Srgb));

    // 8x8 with every level: 4 blocks, then 1, 1 and 1. Mode 6 blocks of one opaque color.
    std::vector<std::byte> blocks(7 * 16);
    for (std::size_t block = 0; block < 7; ++block) {
        blocks[block * 16] = std::byte{0x40};
    }
    const auto texture = device->create_texture(
        TextureDesc{.width = 8, .height = 8, .format = TextureFormat::Bc7Srgb, .mip_levels = 4},
        blocks);
    REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
    device->destroy_texture(texture.value());

    CHECK_FALSE(device
                    ->create_texture(TextureDesc{.width = 4, .height = 4,
                                                 .format = TextureFormat::Astc4x4Srgb},
                                     std::span(blocks).first(16))
                    .has_value());
}

namespace {

constexpr std::string_view k_target_shaders = R"hlsl(
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };

Output target_vertex(Input input) {
    Output output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}

float4 green_fragment(Output input) : SV_Target {
    return float4(0.0, 1.0, 0.0, 1.0);
}

// Shows slot 1's texture, and darkens what slot 2's depth puts in shadow at depth 0.5.
Texture2D scene : register(t1);
SamplerState clamped : register(s1);
Texture2D<float> shadow : register(t2);
SamplerComparisonState shadow_compare : register(s2);

float4 show_fragment(Output input) : SV_Target {
    const float lit = shadow.SampleCmpLevelZero(shadow_compare, input.uv, 0.5);
    return float4(scene.Sample(clamped, input.uv).rgb * (0.25 + 0.75 * lit), 1.0);
}
)hlsl";

// The whole surface, with texture coordinates from the top left.
constexpr std::array<Vertex, 3> k_textured_screen{{
    {{-1.0f, -1.0f, 0.5f}, {0.0f, 1.0f}},
    {{3.0f, -1.0f, 0.5f}, {2.0f, 1.0f}},
    {{-1.0f, 3.0f, 0.5f}, {0.0f, -1.0f}},
}};
// The left half of the surface, nearer than 0.5.
constexpr std::array<Vertex, 6> k_left_half{{
    {{-1.0f, -1.0f, 0.25f}, {0.0f, 0.0f}},
    {{0.0f, -1.0f, 0.25f}, {0.0f, 0.0f}},
    {{0.0f, 1.0f, 0.25f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, 0.25f}, {0.0f, 0.0f}},
    {{0.0f, 1.0f, 0.25f}, {0.0f, 0.0f}},
    {{-1.0f, 1.0f, 0.25f}, {0.0f, 0.0f}},
}};

ShaderHandle make_target_shader(Device& device, ShaderStage stage, const char* entry) {
    const char* const profile = stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0";
    const auto shader = device.create_shader(stage, compile(k_target_shaders, entry, profile));
    REQUIRE_MESSAGE(shader.has_value(), shader.error().message);
    return shader.value();
}

std::array<std::byte, 4> captured_pixel(const CapturedFrame& frame, double x, double y) {
    const auto column = static_cast<std::size_t>(x * frame.width);
    const auto row = static_cast<std::size_t>(y * frame.height);
    const std::size_t at = (row * frame.width + column) * 4;
    return {frame.pixels[at], frame.pixels[at + 1], frame.pixels[at + 2], frame.pixels[at + 3]};
}

} // namespace

TEST_CASE("Direct3D 12 device makes render targets of the formats it says it supports") {
    const auto device = make_device();
    CHECK(device->supports_render_target(TextureFormat::Rgba8Srgb));
    CHECK(device->supports_render_target(TextureFormat::Rgba16Float));
    CHECK(device->supports_render_target(TextureFormat::Rg11B10Float));
    CHECK(device->supports_render_target(TextureFormat::Depth32Float));
    CHECK_FALSE(device->supports_render_target(TextureFormat::Bc7Srgb));
    for (const TextureFormat format : {TextureFormat::Rgba8Srgb, TextureFormat::Rgba16Float,
                                       TextureFormat::Rg11B10Float, TextureFormat::Depth32Float}) {
        const auto target = device->create_texture(
            TextureDesc{.width = 64, .height = 32, .format = format, .render_target = true}, {});
        REQUIRE_MESSAGE(target.has_value(), target.error().message);
        device->destroy_texture(target.value());
    }
    constexpr std::array<std::byte, 4> k_pixel{};
    CHECK_FALSE(device
                    ->create_texture(TextureDesc{.width = 1, .height = 1, .render_target = true},
                                     k_pixel)
                    .has_value());
    CHECK_FALSE(device
                    ->create_texture(TextureDesc{.width = 1, .height = 1,
                                                 .format = TextureFormat::Depth32Float},
                                     k_pixel)
                    .has_value());
}

TEST_CASE("Direct3D 12 device refuses pipelines with the wrong targets or stages") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    const ShaderHandle vertex = make_target_shader(*device, ShaderStage::Vertex, "target_vertex");
    const ShaderHandle green = make_target_shader(*device, ShaderStage::Fragment, "green_fragment");
    CHECK_FALSE(device
                    ->create_pipeline(PipelineDesc{.vertex_shader = vertex,
                                                   .vertex_stride = sizeof(Vertex),
                                                   .attributes = k_attributes,
                                                   .color_target = TextureFormat::Rgba8Unorm})
                    .has_value());
    CHECK_FALSE(device
                    ->create_pipeline(PipelineDesc{.vertex_shader = vertex,
                                                   .fragment_shader = green,
                                                   .vertex_stride = sizeof(Vertex),
                                                   .attributes = k_attributes,
                                                   .color_target = TextureFormat::Depth32Float})
                    .has_value());
    device->destroy_shader(vertex);
    device->destroy_shader(green);
}

TEST_CASE("Direct3D 12 device draws into textures, top row first, and reads them in later passes") {
    const auto device = make_device();
    const HiddenWindow window;
    if (!window.exists()) {
        return;
    }
    REQUIRE(device->attach_surface(window.native(), k_size).has_value());
    const ShaderHandle target_vertex =
        make_target_shader(*device, ShaderStage::Vertex, "target_vertex");
    const ShaderHandle green = make_target_shader(*device, ShaderStage::Fragment, "green_fragment");
    const ShaderHandle show = make_target_shader(*device, ShaderStage::Fragment, "show_fragment");

    constexpr std::uint32_t k_target_size = 64;
    const TextureHandle color =
        device
            ->create_texture(TextureDesc{.width = k_target_size, .height = k_target_size,
                                         .format = TextureFormat::Rgba8Unorm,
                                         .render_target = true},
                             {})
            .value();
    const TextureHandle depth =
        device
            ->create_texture(TextureDesc{.width = k_target_size, .height = k_target_size,
                                         .format = TextureFormat::Depth32Float,
                                         .render_target = true},
                             {})
            .value();

    const auto fill = device->create_pipeline(PipelineDesc{
        .vertex_shader = target_vertex,
        .fragment_shader = green,
        .vertex_stride = sizeof(Vertex),
        .attributes = k_attributes,
        .depth_test = false,
        .cull = CullMode::Back,
        .color_target = TextureFormat::Rgba8Unorm,
    });
    REQUIRE_MESSAGE(fill.has_value(), fill.error().message);
    const auto shadow_caster = device->create_pipeline(PipelineDesc{
        .vertex_shader = target_vertex,
        .vertex_stride = sizeof(Vertex),
        .attributes = k_attributes,
        .cull = CullMode::None,
        .depth_bias = 1.0f,
        .slope_depth_bias = 1.0f,
        .depth_target = TextureFormat::Depth32Float,
    });
    REQUIRE_MESSAGE(shadow_caster.has_value(), shadow_caster.error().message);
    const auto shown = device->create_pipeline(PipelineDesc{
        .vertex_shader = target_vertex,
        .fragment_shader = show,
        .vertex_stride = sizeof(Vertex),
        .attributes = k_attributes,
        .depth_test = false,
        .cull = CullMode::None,
    });
    REQUIRE_MESSAGE(shown.has_value(), shown.error().message);

    REQUIRE(device->capture_next_frame());
    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    // Red, then green over the top half by a scissor from the top left. The full-screen triangle
    // is counter-clockwise, so culling back faces keeps it.
    commands->begin_render_pass(
        RenderTargets{.color = color, .clear_color = ClearColor{1.0f, 0.0f, 0.0f, 1.0f}});
    commands->bind_pipeline(fill.value());
    commands->set_scissor(ScissorRect{.width = k_target_size, .height = k_target_size / 2});
    CHECK(commands->draw_vertices(std::as_bytes(std::span(k_textured_screen)), 3));
    commands->end_render_pass();

    commands->begin_render_pass(RenderTargets{.depth = depth});
    commands->bind_pipeline(shadow_caster.value());
    CHECK(commands->draw_vertices(std::as_bytes(std::span(k_left_half)), 6));
    commands->end_render_pass();

    commands->begin_render_pass(ClearColor{});
    commands->bind_pipeline(shown.value());
    commands->bind_texture(1, color);
    commands->bind_texture(k_shadow_slot, depth);
    CHECK(commands->draw_vertices(std::as_bytes(std::span(k_textured_screen)), 3));
    commands->end_render_pass();
    REQUIRE(device->end_frame().has_value());

    const std::optional<CapturedFrame> frame = device->take_captured_frame();
    REQUIRE(frame.has_value());
    CHECK(captured_pixel(*frame, 0.75, 0.25)[1] == std::byte{255});
    CHECK(captured_pixel(*frame, 0.75, 0.25)[0] == std::byte{0});
    CHECK(captured_pixel(*frame, 0.75, 0.75)[0] == std::byte{255});
    CHECK(captured_pixel(*frame, 0.75, 0.75)[1] == std::byte{0});
    // In shadow, a quarter as bright, which sRGB encoding stores as about 137.
    const int shadowed_green = std::to_integer<int>(captured_pixel(*frame, 0.25, 0.25)[1]);
    CHECK(shadowed_green > 120);
    CHECK(shadowed_green < 150);
    const int shadowed_red = std::to_integer<int>(captured_pixel(*frame, 0.25, 0.75)[0]);
    CHECK(shadowed_red > 120);
    CHECK(shadowed_red < 150);

    // With nothing in the shadow slot, everything is lit.
    REQUIRE(device->capture_next_frame());
    CommandList* const again = device->begin_frame().value();
    REQUIRE(again != nullptr);
    again->begin_render_pass(ClearColor{});
    again->bind_pipeline(shown.value());
    again->bind_texture(1, color);
    CHECK(again->draw_vertices(std::as_bytes(std::span(k_textured_screen)), 3));
    again->end_render_pass();
    REQUIRE(device->end_frame().has_value());
    const std::optional<CapturedFrame> lit = device->take_captured_frame();
    REQUIRE(lit.has_value());
    CHECK(captured_pixel(*lit, 0.25, 0.25)[1] == std::byte{255});

    device->destroy_pipeline(fill.value());
    device->destroy_pipeline(shadow_caster.value());
    device->destroy_pipeline(shown.value());
    device->destroy_texture(color);
    device->destroy_texture(depth);
    device->destroy_shader(target_vertex);
    device->destroy_shader(green);
    device->destroy_shader(show);
}
