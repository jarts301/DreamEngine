// Runs the backend on WARP, Windows' software rasterizer, drawing to a hidden window, so it needs
// neither a GPU nor a display. The shaders are compiled here from HLSL, because a game's shaders
// are compiled by its own build.

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <doctest/doctest.h>

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_rhi_directx(core::ModuleRegistry& registry);
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
    REQUIRE(modules::register_rhi_directx(registry).has_value());

    const core::Config config = core::Config::parse("[rhi.directx]\nsoftware = true\n").value();
    const std::vector<std::string> preference{"directx"};
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
        window_class.lpszClassName = L"DreamDirectXTest";
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

TEST_CASE("DirectX device starts on WARP and loads DXBC shaders") {
    const auto device = make_device();
    CHECK(device->shader_file_suffix() == ".dxbc");

    const auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame.value() == nullptr);
}

TEST_CASE("DirectX device rejects invalid resources") {
    const auto device = make_device();

    CHECK_FALSE(device->create_buffer(BufferUsage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, k_constants).has_value());
    CHECK_FALSE(
        device->create_texture(TextureDesc{.width = 2, .height = 2}, k_constants).has_value());
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

TEST_CASE("DirectX device refuses shaders that read other registers") {
    const auto device = make_device();
    const auto shader = device->create_shader(ShaderStage::Vertex,
                                              compile(k_misplaced_shader, "vertex_main", "vs_5_0"));
    REQUIRE_FALSE(shader.has_value());
    CHECK(shader.error().message.find("register(b1)") != std::string::npos);
}

TEST_CASE("DirectX device matches vertex attributes to the shader's inputs in order") {
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

TEST_CASE("DirectX device draws a frame to a hidden window") {
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

TEST_CASE("DirectX device limits the vertex data drawn in one frame") {
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
