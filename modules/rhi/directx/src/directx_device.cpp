#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace rhi;
using Microsoft::WRL::ComPtr;

// Where every shader finds its inputs on DirectX (ADR 0013). They mirror the Vulkan layout: the
// uniform block of CommandList::set_uniforms, the texture of bind_texture with its sampler, and the
// push constants, which DirectX has no special kind of buffer for.
constexpr UINT k_uniform_register = 0;
constexpr UINT k_push_constant_register = 1;
// Slot n of bind_texture reads register tn with sampler sn (ADR 0027).
constexpr UINT k_texture_register = 0;
constexpr UINT k_sampler_register = 0;

// Constant buffers are sized in multiples of 16 bytes.
static_assert(k_max_uniform_size % 16 == 0);
static_assert(k_max_push_constant_size % 16 == 0);

// The window is drawn through an sRGB view of a plain back buffer, which is how the flip
// presentation model wants sRGB output. The result is the same as Vulkan's sRGB surface.
constexpr DXGI_FORMAT k_back_buffer_format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT k_render_target_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr DXGI_FORMAT k_depth_format = DXGI_FORMAT_D32_FLOAT;
constexpr UINT k_back_buffer_count = 2;

#if defined(NDEBUG)
constexpr bool k_debug_layer_by_default = false;
#else
constexpr bool k_debug_layer_by_default = true;
#endif

core::Error directx_error(std::string_view action, HRESULT result) {
    return core::Error{core::ErrorCode::Unknown,
                       std::format("{} failed with DirectX error 0x{:08X}", action,
                                   static_cast<std::uint32_t>(result))};
}

core::Error invalid(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

std::string to_utf8(const WCHAR* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

// ASTC has no DirectX format.
DXGI_FORMAT to_directx(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8Srgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Bc7Srgb:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    case TextureFormat::Bc7Unorm:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::Astc4x4Srgb:
    case TextureFormat::Astc4x4Unorm:
        return DXGI_FORMAT_UNKNOWN;
    case TextureFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Rg11B10Float:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    // Typeless, so it has a depth view to draw into and a float view to read.
    case TextureFormat::Depth32Float:
        return DXGI_FORMAT_R32_TYPELESS;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT to_directx(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::UByte4Norm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// windows.h renames GetMessage to GetMessageA or GetMessageW with a macro, and SDK headers differ
// in whether ID3D11InfoQueue's method of that name was declared with the macro in effect. This
// calls whichever name the header ended up with.
#pragma push_macro("GetMessage")
#undef GetMessage
template <typename Queue>
HRESULT get_debug_message(Queue& queue, UINT64 index, D3D11_MESSAGE* message, SIZE_T* length) {
    if constexpr (requires { queue.GetMessage(index, message, length); }) {
        return queue.GetMessage(index, message, length);
    } else if constexpr (requires { queue.GetMessageA(index, message, length); }) {
        return queue.GetMessageA(index, message, length);
    } else {
        return queue.GetMessageW(index, message, length);
    }
}
#pragma pop_macro("GetMessage")

struct BufferResource {
    ComPtr<ID3D11Buffer> buffer;
};

struct ShaderResource {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    // Kept for vertex shaders, because each pipeline's input layout is checked against it.
    std::vector<std::byte> code;
};

// The shaders are held here too, so a pipeline keeps working after its shaders are destroyed, as it
// does on Vulkan.
struct PipelineResource {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    ComPtr<ID3D11BlendState> blend;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT vertex_stride = 0;
    UINT instance_stride = 0;
    std::optional<TextureFormat> color_target;
    std::optional<TextureFormat> depth_target;
};

struct TextureResource {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    // For render targets, the view a pass draws through.
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    platform::SurfaceSize size;
};

// A graphics device on Direct3D 11, feature level 11.0, presenting with the DXGI flip model that
// Windows 10 introduced. Direct3D 11 manages memory, synchronization and mipmaps itself, which is
// why the backend is short next to the Vulkan one.
// One frame's GPU timing queries.
struct FrameTimingQueries {
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> start;
    ComPtr<ID3D11Query> end;
};

// How many measured frames are kept for take_gpu_frame_seconds before the oldest are dropped.
constexpr std::size_t k_kept_gpu_frame_times = 16;

class DirectXDevice final : public Device, private CommandList {
public:
    DirectXDevice() = default;
    DirectXDevice(const DirectXDevice&) = delete;
    DirectXDevice& operator=(const DirectXDevice&) = delete;

    ~DirectXDevice() override {
        warn_about_leftovers();
        if (m_context) {
            m_context->ClearState();
            m_context->Flush();
        }
        log_debug_messages();
    }

    [[nodiscard]] core::Result<void> initialize(const core::Config& config) {
        if (auto result = create_device(config); !result.has_value()) {
            return result;
        }
        if (auto result = create_shared_state(); !result.has_value()) {
            return result;
        }
        create_timing_queries();

        constexpr std::array k_white_pixel{std::byte{255}, std::byte{255}, std::byte{255},
                                           std::byte{255}};
        core::Result<TextureResource> white = upload_texture(
            TextureDesc{.width = 1, .height = 1, .generate_mipmaps = false}, k_white_pixel);
        if (!white.has_value()) {
            return white.error();
        }
        m_white_texture = std::move(white).value();

        // The shadow slot's default: a depth at the far plane, so every comparison passes.
        core::Result<TextureResource> far_depth = upload_texture(
            TextureDesc{.width = 1, .height = 1, .format = TextureFormat::Depth32Float,
                        .render_target = true},
            {});
        if (!far_depth.has_value()) {
            return far_depth.error();
        }
        m_far_depth_texture = std::move(far_depth).value();
        m_context->ClearDepthStencilView(m_far_depth_texture.depth.Get(), D3D11_CLEAR_DEPTH, 1.0f,
                                         0);
        return {};
    }

    core::Result<void> attach_surface(const platform::NativeWindow& window,
                                      platform::SurfaceSize size) override {
        detach_surface();

        const auto* const win32 = std::get_if<platform::Win32Window>(&window);
        if (win32 == nullptr || win32->window == nullptr) {
            return invalid("DirectX needs a Windows window here");
        }
        const auto hwnd = static_cast<HWND>(win32->window);

        // A minimized window has no size yet. The swapchain is made as small as possible and
        // resized once the window has one.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = std::max(size.width, 1u);
        desc.Height = std::max(size.height, 1u);
        desc.Format = k_back_buffer_format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = k_back_buffer_count;
        desc.Scaling = DXGI_SCALING_NONE;
        // Paced to the display by Present, like Vulkan's FIFO mode.
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        const HRESULT result = m_factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc,
                                                                 nullptr, nullptr, &m_swapchain);
        if (FAILED(result)) {
            return directx_error("Creating the swapchain", result);
        }
        // The platform layer owns the window, so DXGI must not react to Alt+Enter itself.
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        m_was_attached = true;
        m_requested_size = size;
        m_needs_resize = size.width == 0 || size.height == 0;
        if (m_needs_resize) {
            return {};
        }
        if (auto targets = create_targets(); !targets.has_value()) {
            detach_surface();
            return targets;
        }
        return {};
    }

    void detach_surface() override {
        if (!m_swapchain) {
            return;
        }
        destroy_targets();
        m_swapchain.Reset();
    }

    void resize_surface(platform::SurfaceSize size) override {
        if (size != m_requested_size) {
            m_requested_size = size;
            m_needs_resize = true;
        }
    }

    std::string_view shader_file_suffix() const override {
        return ".dxbc";
    }

    // BC7 is part of feature level 11.0, so every device here has it.
    bool supports_texture_format(TextureFormat format) const override {
        if (is_render_target_only(format)) {
            return false;
        }
        const DXGI_FORMAT dxgi = to_directx(format);
        UINT support = 0;
        return dxgi != DXGI_FORMAT_UNKNOWN &&
               SUCCEEDED(m_device->CheckFormatSupport(dxgi, &support)) &&
               (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
    }

    // Feature level 11.0 draws into and filters all of these, but the driver is asked anyway.
    bool supports_render_target(TextureFormat format) const override {
        if (is_compressed(format)) {
            return false;
        }
        UINT support = 0;
        if (is_depth(format)) {
            return SUCCEEDED(m_device->CheckFormatSupport(DXGI_FORMAT_D32_FLOAT, &support)) &&
                   (support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0;
        }
        constexpr UINT k_needed = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                                  D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                                  D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        return SUCCEEDED(m_device->CheckFormatSupport(to_directx(format), &support)) &&
               (support & k_needed) == k_needed;
    }

    std::optional<double> take_gpu_frame_seconds() override {
        collect_frame_timings();
        if (m_gpu_frame_seconds.empty()) {
            return std::nullopt;
        }
        const double seconds = m_gpu_frame_seconds.front();
        m_gpu_frame_seconds.pop_front();
        return seconds;
    }

    // The system's own count of this program's dedicated video memory, and its budget, which
    // shrinks when other programs need the GPU.
    std::optional<MemoryUsage> memory_usage() override {
        if (!m_adapter) {
            return std::nullopt;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            return std::nullopt;
        }
        return MemoryUsage{.used_bytes = local.CurrentUsage, .budget_bytes = local.Budget};
    }

    core::Result<BufferHandle> create_buffer(BufferUsage usage,
                                             std::span<const std::byte> contents) override {
        if (contents.empty()) {
            return invalid("A buffer can't be empty");
        }
        if (contents.size() > std::numeric_limits<UINT>::max()) {
            return invalid("A buffer can't be larger than 4 GB");
        }

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(contents.size());
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags =
            usage == BufferUsage::Vertex ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = contents.data();

        BufferResource resource;
        if (const HRESULT result = m_device->CreateBuffer(&desc, &data, &resource.buffer);
            FAILED(result)) {
            return directx_error("Creating a buffer", result);
        }
        return handle_cast<BufferTag>(m_buffers.create(std::move(resource)));
    }

    core::Result<TextureHandle> create_texture(const TextureDesc& desc,
                                               std::span<const std::byte> pixels) override {
        core::Result<TextureResource> texture = upload_texture(desc, pixels);
        if (!texture.has_value()) {
            return texture.error();
        }
        return handle_cast<TextureTag>(m_textures.create(std::move(texture).value()));
    }

    core::Result<ShaderHandle> create_shader(ShaderStage stage,
                                             std::span<const std::byte> code) override {
        if (code.empty()) {
            return invalid("Shader code is not valid DXBC");
        }

        ShaderResource resource;
        HRESULT result = S_OK;
        if (stage == ShaderStage::Vertex) {
            result =
                m_device->CreateVertexShader(code.data(), code.size(), nullptr, &resource.vertex);
            resource.code.assign(code.begin(), code.end());
        } else {
            result =
                m_device->CreatePixelShader(code.data(), code.size(), nullptr, &resource.pixel);
        }
        if (FAILED(result)) {
            return directx_error("Creating a shader (is it DXBC, compiled for shader model 5.0?)",
                                 result);
        }

        // A shader that reads the wrong registers would draw garbage, so it is refused up front.
        core::Result<ComPtr<ID3D11ShaderReflection>> reflection = reflect(code);
        if (!reflection.has_value()) {
            return reflection.error();
        }
        if (auto checked = check_bindings(*reflection.value().Get()); !checked.has_value()) {
            return checked.error();
        }
        return handle_cast<ShaderTag>(m_shaders.create(std::move(resource)));
    }

    core::Result<PipelineHandle> create_pipeline(const PipelineDesc& desc) override {
        if (!m_was_attached) {
            return invalid("A pipeline needs a surface to have been attached");
        }
        if (const auto problem = pipeline_problem(desc)) {
            return invalid(*problem);
        }
        if (desc.color_target.has_value() && !supports_render_target(*desc.color_target)) {
            return invalid("This device can't draw into the pipeline's color format");
        }
        const ShaderResource* const vertex =
            m_shaders.get(handle_cast<ShaderResource>(desc.vertex_shader));
        const ShaderResource* const fragment =
            m_shaders.get(handle_cast<ShaderResource>(desc.fragment_shader));
        if (vertex == nullptr || !vertex->vertex ||
            (desc.fragment_shader.is_valid() && (fragment == nullptr || !fragment->pixel))) {
            return invalid("A pipeline needs valid shaders, one per stage");
        }

        PipelineResource resource;
        resource.vertex = vertex->vertex;
        if (fragment != nullptr) {
            resource.pixel = fragment->pixel;
        }
        resource.color_target = desc.color_target;
        resource.depth_target = desc.depth_target;
        resource.vertex_stride = desc.vertex_stride;
        resource.instance_stride = desc.instance_stride;
        resource.topology = desc.topology == PrimitiveTopology::Lines
                                ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

        core::Result<ComPtr<ID3D11InputLayout>> layout =
            create_input_layout(desc.attributes, desc.instance_attributes, vertex->code);
        if (!layout.has_value()) {
            return layout.error();
        }
        resource.input_layout = std::move(layout).value();

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = desc.cull == CullMode::Back    ? D3D11_CULL_BACK
                              : desc.cull == CullMode::Front ? D3D11_CULL_FRONT
                                                             : D3D11_CULL_NONE;
        rasterizer.DepthBias = static_cast<INT>(desc.depth_bias + (desc.depth_bias < 0 ? -0.5f : 0.5f));
        rasterizer.SlopeScaledDepthBias = desc.slope_depth_bias;
        // DirectX clip space already has +Y up, so counter-clockwise means the same as in ADR 0005.
        rasterizer.FrontCounterClockwise = TRUE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        HRESULT result = m_device->CreateRasterizerState(&rasterizer, &resource.rasterizer);
        if (FAILED(result)) {
            return directx_error("Creating a rasterizer state", result);
        }

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = desc.depth_test ? TRUE : FALSE;
        depth.DepthWriteMask = desc.depth_test && desc.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                                                   : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_LESS;
        result = m_device->CreateDepthStencilState(&depth, &resource.depth_stencil);
        if (FAILED(result)) {
            return directx_error("Creating a depth state", result);
        }

        D3D11_BLEND_DESC blend{};
        D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
        target.BlendEnable = desc.blend != BlendMode::Opaque ? TRUE : FALSE;
        target.SrcBlend = desc.blend == BlendMode::Alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = m_device->CreateBlendState(&blend, &resource.blend);
        if (FAILED(result)) {
            return directx_error("Creating a blend state", result);
        }

        return handle_cast<PipelineTag>(m_pipelines.create(std::move(resource)));
    }

    // Direct3D 11 keeps a resource alive until the GPU is done with it, so nothing waits here.
    void destroy_buffer(BufferHandle buffer) override {
        m_buffers.destroy(handle_cast<BufferResource>(buffer));
    }

    void destroy_texture(TextureHandle texture) override {
        m_textures.destroy(handle_cast<TextureResource>(texture));
    }

    void destroy_shader(ShaderHandle shader) override {
        m_shaders.destroy(handle_cast<ShaderResource>(shader));
    }

    void destroy_pipeline(PipelineHandle pipeline) override {
        m_pipelines.destroy(handle_cast<PipelineResource>(pipeline));
    }

    core::Result<CommandList*> begin_frame() override {
        DREAM_VERIFY(!m_in_frame, "begin_frame called twice");
        const auto no_frame = static_cast<CommandList*>(nullptr);

        if (!m_swapchain) {
            return no_frame;
        }
        if (m_needs_resize) {
            if (auto resized = resize_targets(); !resized.has_value()) {
                return resized.error();
            }
        }
        // A minimized window has no size, so its frames are skipped until it gets one again.
        if (!m_render_target || m_extent.width == 0 || m_extent.height == 0) {
            return no_frame;
        }

        m_in_frame = true;
        m_frame_error.reset();
        m_frame_bytes_used = 0;
        m_frame_buffer_fresh = true;
        m_vertex_buffer.Reset();
        m_vertex_buffer_offset = 0;
        m_vertex_buffer_dirty = true;
        m_bound_stride = 0;
        begin_frame_timing();
        return static_cast<CommandList*>(this);
    }

    core::Result<void> end_frame() override {
        DREAM_VERIFY(m_in_frame, "end_frame called without begin_frame");
        DREAM_VERIFY(!m_in_pass, "The render pass was not ended");
        m_in_frame = false;
        // Before presenting, so waiting for the display isn't part of the GPU's time.
        end_frame_timing();
        if (!m_swapchain) {
            return {};
        }

        if (m_capture_requested) {
            m_capture_requested = false;
            capture_back_buffer();
        }
        const HRESULT result = m_swapchain->Present(1, 0);
        log_debug_messages();
        if (m_frame_error.has_value()) {
            return *m_frame_error;
        }
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            return directx_error("Presenting a frame: the graphics device was lost",
                                 m_device->GetDeviceRemovedReason());
        }
        // Being hidden behind another window is reported as a status, and isn't a failure.
        if (FAILED(result)) {
            return directx_error("Presenting a frame", result);
        }
        return {};
    }

private:
    core::Result<void> create_device(const core::Config& config) {
        const bool wants_debug =
            config.get_bool("rhi.directx.debug").value_or(k_debug_layer_by_default);
        // WARP is Windows' software rasterizer, for machines without a usable GPU driver.
        const bool software = config.get_bool("rhi.directx.software").value_or(false);
        const D3D_DRIVER_TYPE driver = software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        // Nothing here needs more than feature level 11.0, the baseline of Direct3D 11 hardware.
        constexpr std::array k_levels{D3D_FEATURE_LEVEL_11_0};

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (wants_debug) {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }
        D3D_FEATURE_LEVEL level{};
        HRESULT result = D3D11CreateDevice(nullptr, driver, nullptr, flags, k_levels.data(),
                                           static_cast<UINT>(k_levels.size()), D3D11_SDK_VERSION,
                                           &m_device, &level, &m_context);
        // The debug layer is an optional Windows feature, "Graphics Tools", so its absence is not
        // an error.
        bool debugging = wants_debug;
        if (FAILED(result) && wants_debug) {
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            debugging = false;
            result = D3D11CreateDevice(nullptr, driver, nullptr, flags, k_levels.data(),
                                       static_cast<UINT>(k_levels.size()), D3D11_SDK_VERSION,
                                       &m_device, &level, &m_context);
        }
        if (FAILED(result)) {
            return directx_error("Creating a Direct3D 11 device", result);
        }
        if (debugging) {
            m_device.As(&m_info_queue);
        }
        DREAM_LOG_INFO("Direct3D debug layer is {}", m_info_queue ? "on" : "off");

        ComPtr<IDXGIDevice> dxgi_device;
        result = m_device.As(&dxgi_device);
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(result)) {
            result = dxgi_device->GetAdapter(&adapter);
        }
        if (SUCCEEDED(result)) {
            // The flip model needs the Windows 8 factory, which Windows 10 always has.
            result = adapter->GetParent(IID_PPV_ARGS(&m_factory));
        }
        if (FAILED(result)) {
            return directx_error("Finding the DXGI factory", result);
        }

        // Windows 10's adapters report the video memory the program uses, for memory_usage. Without
        // it, there is simply nothing to report.
        static_cast<void>(adapter.As(&m_adapter));

        DXGI_ADAPTER_DESC adapter_desc{};
        adapter->GetDesc(&adapter_desc);
        const auto level_bits = static_cast<unsigned>(level);
        DREAM_LOG_INFO("Direct3D device: {} (feature level {}.{})",
                       to_utf8(adapter_desc.Description), (level_bits >> 12) & 0xF,
                       (level_bits >> 8) & 0xF);
        return {};
    }

    // GPU timing, for take_gpu_frame_seconds. Each frame uses one set of queries from a ring: two
    // timestamps, and a disjoint query that gives their frequency and says whether they can be
    // trusted. The driver may queue a few frames, so a set is read back some frames later, without
    // waiting, and a frame goes untimed if every set is still waiting for the GPU.
    //
    // Every Direct3D 11 device has these queries, so failing to make them only turns timing off.
    void create_timing_queries() {
        const D3D11_QUERY_DESC disjoint{.Query = D3D11_QUERY_TIMESTAMP_DISJOINT, .MiscFlags = 0};
        const D3D11_QUERY_DESC timestamp{.Query = D3D11_QUERY_TIMESTAMP, .MiscFlags = 0};
        for (FrameTimingQueries& queries : m_timing_queries) {
            if (FAILED(m_device->CreateQuery(&disjoint, &queries.disjoint)) ||
                FAILED(m_device->CreateQuery(&timestamp, &queries.start)) ||
                FAILED(m_device->CreateQuery(&timestamp, &queries.end))) {
                DREAM_LOG_WARNING("Direct3D timestamp queries are unavailable, so GPU time "
                                  "isn't measured");
                return;
            }
        }
        m_timing_available = true;
    }

    [[nodiscard]] FrameTimingQueries& next_timing_queries() {
        return m_timing_queries[(m_timing_oldest + m_timing_pending) % m_timing_queries.size()];
    }

    void begin_frame_timing() {
        collect_frame_timings();
        m_timing_this_frame = m_timing_available && m_timing_pending < m_timing_queries.size();
        if (m_timing_this_frame) {
            FrameTimingQueries& queries = next_timing_queries();
            m_context->Begin(queries.disjoint.Get());
            m_context->End(queries.start.Get());
        }
    }

    void end_frame_timing() {
        if (!m_timing_this_frame) {
            return;
        }
        FrameTimingQueries& queries = next_timing_queries();
        m_context->End(queries.end.Get());
        m_context->End(queries.disjoint.Get());
        ++m_timing_pending;
        m_timing_this_frame = false;
    }

    // Reads the finished query sets, oldest first, and stops at the first one that isn't.
    void collect_frame_timings() {
        // Presenting already sends the queries to the GPU, so there is no need to flush, and not
        // waiting is the point.
        const auto read = [this](ID3D11Query* query, auto& data) {
            return m_context->GetData(query, &data, static_cast<UINT>(sizeof(data)),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        };
        while (m_timing_pending > 0) {
            FrameTimingQueries& queries = m_timing_queries[m_timing_oldest];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (!read(queries.disjoint.Get(), disjoint)) {
                return;
            }
            UINT64 start = 0;
            UINT64 end = 0;
            const bool timed = read(queries.start.Get(), start) && read(queries.end.Get(), end);
            // A disjoint interval, such as one where the GPU changed its clock, has no usable time.
            if (timed && !disjoint.Disjoint && disjoint.Frequency > 0 && end >= start) {
                if (m_gpu_frame_seconds.size() == k_kept_gpu_frame_times) {
                    m_gpu_frame_seconds.pop_front();
                }
                m_gpu_frame_seconds.push_back(static_cast<double>(end - start) /
                                              static_cast<double>(disjoint.Frequency));
            }
            m_timing_oldest = (m_timing_oldest + 1) % m_timing_queries.size();
            --m_timing_pending;
        }
    }

    // The sampler, constant buffers and per-frame vertex memory that every pipeline shares.
    core::Result<void> create_shared_state() {
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> repeating;
        ComPtr<ID3D11SamplerState> clamped;
        ComPtr<ID3D11SamplerState> comparing;
        HRESULT result = m_device->CreateSamplerState(&sampler, &repeating);
        if (SUCCEEDED(result)) {
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            result = m_device->CreateSamplerState(&sampler, &clamped);
        }
        if (SUCCEEDED(result)) {
            sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            result = m_device->CreateSamplerState(&sampler, &comparing);
        }
        // One sampler per slot, as ADR 0027 lists them.
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            switch (slot_sampler(slot)) {
            case SlotSampler::Repeat:
                m_samplers[slot] = repeating;
                break;
            case SlotSampler::Clamp:
                m_samplers[slot] = clamped;
                break;
            case SlotSampler::ShadowCompare:
                m_samplers[slot] = comparing;
                break;
            }
        }
        if (FAILED(result)) {
            return directx_error("Creating the samplers", result);
        }

        // Dynamic buffers are rewritten with the discard mode, and the driver hands out fresh
        // memory each time, so a small buffer can change between every draw.
        const auto create_dynamic = [&](std::size_t size, UINT bind_flags,
                                        ComPtr<ID3D11Buffer>& buffer) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(size);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = bind_flags;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return m_device->CreateBuffer(&desc, nullptr, &buffer);
        };
        result = create_dynamic(k_max_uniform_size, D3D11_BIND_CONSTANT_BUFFER, m_uniform_buffer);
        if (FAILED(result)) {
            return directx_error("Creating the uniform buffer", result);
        }
        result = create_dynamic(k_max_push_constant_size, D3D11_BIND_CONSTANT_BUFFER,
                                m_push_constant_buffer);
        if (FAILED(result)) {
            return directx_error("Creating the push constant buffer", result);
        }
        constexpr UINT k_geometry = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
        result = create_dynamic(k_max_frame_vertex_bytes, k_geometry, m_frame_buffer);
        if (FAILED(result)) {
            return directx_error("Creating the per-frame vertex buffer", result);
        }
        return {};
    }

    core::Result<ComPtr<ID3D11ShaderReflection>> reflect(std::span<const std::byte> code) {
        ComPtr<ID3D11ShaderReflection> reflection;
        if (const HRESULT result =
                D3DReflect(code.data(), code.size(), IID_ID3D11ShaderReflection, &reflection);
            FAILED(result)) {
            return directx_error("Reading a shader's layout", result);
        }
        return reflection;
    }

    // Checks that a shader's resources are at the registers this backend binds; see the constants
    // at the top of the file.
    core::Result<void> check_bindings(ID3D11ShaderReflection& reflection) {
        D3D11_SHADER_DESC shader{};
        if (const HRESULT result = reflection.GetDesc(&shader); FAILED(result)) {
            return directx_error("Reading a shader's layout", result);
        }

        for (UINT index = 0; index < shader.BoundResources; ++index) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            if (const HRESULT result = reflection.GetResourceBindingDesc(index, &binding);
                FAILED(result)) {
                return directx_error("Reading a shader's layout", result);
            }
            const std::string_view name = binding.Name != nullptr ? binding.Name : "";

            switch (binding.Type) {
            case D3D_SIT_CBUFFER: {
                D3D11_SHADER_BUFFER_DESC buffer{};
                ID3D11ShaderReflectionConstantBuffer* const block =
                    reflection.GetConstantBufferByName(binding.Name);
                if (block == nullptr || FAILED(block->GetDesc(&buffer))) {
                    return directx_error("Reading a shader's layout", E_FAIL);
                }
                if (binding.BindPoint == k_uniform_register) {
                    if (buffer.Size > k_max_uniform_size) {
                        return invalid(std::format("Uniform block '{}' is {} bytes; at most {} "
                                                   "are allowed",
                                                   name, buffer.Size, k_max_uniform_size));
                    }
                } else if (binding.BindPoint == k_push_constant_register) {
                    if (buffer.Size > k_max_push_constant_size) {
                        return invalid(std::format("Push constants '{}' are {} bytes; at most {} "
                                                   "are allowed",
                                                   name, buffer.Size, k_max_push_constant_size));
                    }
                } else {
                    return invalid(std::format(
                        "Constant buffer '{}' is at register b{}; DirectX shaders must put the "
                        "uniform block at register(b{}) and push constants at register(b{}), "
                        "see ADR 0013",
                        name, binding.BindPoint, k_uniform_register, k_push_constant_register));
                }
                break;
            }
            case D3D_SIT_TEXTURE:
                if (binding.BindPoint >= k_texture_register + k_texture_slots ||
                    binding.BindCount != 1) {
                    return invalid(std::format("Texture '{}' is at register t{}; textures go at "
                                               "t0 to t{}, see ADR 0027",
                                               name, binding.BindPoint, k_texture_slots - 1));
                }
                break;
            case D3D_SIT_SAMPLER: {
                const bool comparison = (binding.uFlags & D3D_SIF_COMPARISON_SAMPLER) != 0;
                const bool placed = binding.BindPoint < k_sampler_register + k_texture_slots &&
                                    binding.BindCount == 1 &&
                                    (binding.BindPoint == k_shadow_slot) == comparison;
                if (!placed) {
                    return invalid(std::format("Sampler '{}' is at register s{}; samplers go at "
                                               "s0 to s{}, and shadow samplers at s{} alone, see "
                                               "ADR 0027",
                                               name, binding.BindPoint, k_texture_slots - 1,
                                               k_shadow_slot));
                }
                break;
            }
            default:
                return invalid(std::format("Shader resource '{}' is of a kind the graphics "
                                           "interface doesn't offer",
                                           name));
            }
        }
        return {};
    }

    // DirectX matches vertex attributes to shader inputs by semantic name, which the interface
    // doesn't have. The vertex shader's inputs are declared in location order (ADR 0013), so the
    // n-th input that isn't a system value takes the attribute at location n. Per-instance
    // attributes are read from input slot 1.
    core::Result<ComPtr<ID3D11InputLayout>>
    create_input_layout(std::span<const VertexAttribute> attributes,
                        std::span<const VertexAttribute> instance_attributes,
                        std::span<const std::byte> vertex_code) {
        core::Result<ComPtr<ID3D11ShaderReflection>> reflected = reflect(vertex_code);
        if (!reflected.has_value()) {
            return reflected.error();
        }
        ID3D11ShaderReflection& reflection = *reflected.value().Get();

        D3D11_SHADER_DESC shader{};
        if (const HRESULT result = reflection.GetDesc(&shader); FAILED(result)) {
            return directx_error("Reading the vertex shader's inputs", result);
        }

        // The names point into the reflection object, which outlives this function's use of them.
        std::vector<D3D11_SIGNATURE_PARAMETER_DESC> inputs;
        for (UINT index = 0; index < shader.InputParameters; ++index) {
            D3D11_SIGNATURE_PARAMETER_DESC parameter{};
            if (const HRESULT result = reflection.GetInputParameterDesc(index, &parameter);
                FAILED(result)) {
                return directx_error("Reading the vertex shader's inputs", result);
            }
            if (parameter.SystemValueType == D3D_NAME_UNDEFINED) {
                inputs.push_back(parameter);
            }
        }

        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        elements.reserve(attributes.size() + instance_attributes.size());
        const auto add = [&](std::span<const VertexAttribute> from, bool per_instance) {
            for (const VertexAttribute& attribute : from) {
                // An attribute the shader doesn't read is left out, as Vulkan would ignore it.
                if (attribute.location >= inputs.size()) {
                    continue;
                }
                const D3D11_SIGNATURE_PARAMETER_DESC& input = inputs[attribute.location];

                D3D11_INPUT_ELEMENT_DESC element{};
                element.SemanticName = input.SemanticName;
                element.SemanticIndex = input.SemanticIndex;
                element.Format = to_directx(attribute.format);
                element.InputSlot = per_instance ? 1 : 0;
                element.AlignedByteOffset = attribute.offset;
                element.InputSlotClass =
                    per_instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
                element.InstanceDataStepRate = per_instance ? 1 : 0;
                elements.push_back(element);
            }
        };
        add(attributes, false);
        add(instance_attributes, true);
        for (std::size_t location = 0; location < inputs.size(); ++location) {
            const auto at = [&](const VertexAttribute& a) { return a.location == location; };
            const bool provided = std::ranges::any_of(attributes, at) ||
                                  std::ranges::any_of(instance_attributes, at);
            if (!provided) {
                return invalid(std::format("The vertex shader reads input {} ({}{}), which the "
                                           "pipeline has no attribute for",
                                           location, inputs[location].SemanticName,
                                           inputs[location].SemanticIndex));
            }
        }

        ComPtr<ID3D11InputLayout> layout;
        if (elements.empty()) {
            return layout;
        }
        if (const HRESULT result =
                m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                            vertex_code.data(), vertex_code.size(), &layout);
            FAILED(result)) {
            return directx_error("Creating a pipeline's input layout (do the attribute formats "
                                 "match the vertex shader?)",
                                 result);
        }
        return layout;
    }

    core::Result<TextureResource> upload_texture(const TextureDesc& desc,
                                                 std::span<const std::byte> pixels) {
        if (const auto problem = texture_problem(desc, pixels.size())) {
            return invalid(*problem);
        }
        if (desc.render_target) {
            return create_render_target(desc);
        }
        if (!supports_texture_format(desc.format)) {
            return invalid("This device doesn't support the texture's format");
        }

        const DXGI_FORMAT format = to_directx(desc.format);
        UINT support = 0;
        m_device->CheckFormatSupport(format, &support);
        const bool mipmaps =
            generates_mipmaps(desc) && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0;

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = desc.width;
        texture_desc.Height = desc.height;
        texture_desc.ArraySize = 1;
        texture_desc.Format = format;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        // Each level's rows, packed one after another, as the pixels hold them.
        std::vector<D3D11_SUBRESOURCE_DATA> levels(desc.mip_levels);
        std::size_t offset = 0;
        for (std::uint32_t level = 0; level < desc.mip_levels; ++level) {
            const std::uint32_t width = mip_size(desc.width, level);
            const std::uint32_t height = mip_size(desc.height, level);
            levels[level].pSysMem = pixels.data() + offset;
            levels[level].SysMemPitch = static_cast<UINT>(
                texture_level_bytes(desc.format, width, 1));
            offset += texture_level_bytes(desc.format, width, height);
        }

        TextureResource texture;
        HRESULT result = S_OK;
        if (mipmaps) {
            // Mipmaps are generated on the GPU, which needs the whole chain as a render target.
            texture_desc.MipLevels = 0;
            texture_desc.Usage = D3D11_USAGE_DEFAULT;
            texture_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            texture_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            result = m_device->CreateTexture2D(&texture_desc, nullptr, &texture.texture);
            if (SUCCEEDED(result)) {
                m_context->UpdateSubresource(texture.texture.Get(), 0, nullptr, levels[0].pSysMem,
                                             levels[0].SysMemPitch, 0);
            }
        } else {
            texture_desc.MipLevels = desc.mip_levels;
            texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
            result = m_device->CreateTexture2D(&texture_desc, levels.data(), &texture.texture);
        }
        if (FAILED(result)) {
            return directx_error("Creating a texture", result);
        }

        result = m_device->CreateShaderResourceView(texture.texture.Get(), nullptr, &texture.view);
        if (FAILED(result)) {
            return directx_error("Creating a texture view", result);
        }
        if (mipmaps) {
            m_context->GenerateMips(texture.view.Get());
        }
        texture.format = desc.format;
        texture.size = {desc.width, desc.height};
        return texture;
    }

    core::Result<TextureResource> create_render_target(const TextureDesc& desc) {
        if (!supports_render_target(desc.format)) {
            return invalid("This device can't draw into textures of this format");
        }
        const bool depth = is_depth(desc.format);
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = desc.width;
        texture_desc.Height = desc.height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = to_directx(desc.format);
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags =
            D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);

        TextureResource texture{.format = desc.format, .size = {desc.width, desc.height}};
        HRESULT result = m_device->CreateTexture2D(&texture_desc, nullptr, &texture.texture);
        if (FAILED(result)) {
            return directx_error("Creating a render target", result);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = depth ? DXGI_FORMAT_R32_FLOAT : texture_desc.Format;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        result = m_device->CreateShaderResourceView(texture.texture.Get(), &view, &texture.view);
        if (SUCCEEDED(result) && depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC target{};
            target.Format = DXGI_FORMAT_D32_FLOAT;
            target.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            result = m_device->CreateDepthStencilView(texture.texture.Get(), &target,
                                                      &texture.depth);
        } else if (SUCCEEDED(result)) {
            result = m_device->CreateRenderTargetView(texture.texture.Get(), nullptr,
                                                      &texture.target);
        }
        if (FAILED(result)) {
            return directx_error("Creating a render target's views", result);
        }
        return texture;
    }

    core::Result<void> create_targets() {
        ComPtr<ID3D11Texture2D> back_buffer;
        HRESULT result = m_swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(result)) {
            return directx_error("Getting the window's back buffer", result);
        }
        D3D11_TEXTURE2D_DESC back_buffer_desc{};
        back_buffer->GetDesc(&back_buffer_desc);
        m_extent = platform::SurfaceSize{back_buffer_desc.Width, back_buffer_desc.Height};

        D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
        view_desc.Format = k_render_target_format;
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        result = m_device->CreateRenderTargetView(back_buffer.Get(), &view_desc, &m_render_target);
        if (FAILED(result)) {
            return directx_error("Creating the window's render target", result);
        }

        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = m_extent.width;
        depth_desc.Height = m_extent.height;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = k_depth_format;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        result = m_device->CreateTexture2D(&depth_desc, nullptr, &m_depth_buffer);
        if (FAILED(result)) {
            return directx_error("Creating the depth buffer", result);
        }
        result = m_device->CreateDepthStencilView(m_depth_buffer.Get(), nullptr, &m_depth_view);
        if (FAILED(result)) {
            return directx_error("Creating the depth view", result);
        }
        return {};
    }

    void destroy_targets() {
        // The swapchain can't resize while its back buffer is bound or referenced, and the context
        // may hold a reference until its queued commands are submitted.
        if (m_context) {
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        m_depth_view.Reset();
        m_depth_buffer.Reset();
        m_render_target.Reset();
        m_extent = platform::SurfaceSize{};
        if (m_context) {
            m_context->Flush();
        }
    }

    core::Result<void> resize_targets() {
        destroy_targets();
        m_needs_resize = false;
        if (m_requested_size.width == 0 || m_requested_size.height == 0) {
            return {};
        }
        const HRESULT result = m_swapchain->ResizeBuffers(
            0, m_requested_size.width, m_requested_size.height, DXGI_FORMAT_UNKNOWN, 0);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            return directx_error("Resizing the window's buffers: the graphics device was lost",
                                 m_device->GetDeviceRemovedReason());
        }
        if (FAILED(result)) {
            return directx_error("Resizing the window's buffers", result);
        }
        return create_targets();
    }

    bool capture_next_frame() override {
        m_capture_requested = true;
        return true;
    }

    std::optional<CapturedFrame> take_captured_frame() override {
        return std::exchange(m_captured, std::nullopt);
    }

    // Copies the frame about to be presented into a texture the CPU can read, and from there into
    // m_captured, swapping blue and red, since the back buffer is BGRA.
    void capture_back_buffer() {
        ComPtr<ID3D11Texture2D> back_buffer;
        HRESULT result = m_swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(result)) {
            DREAM_LOG_WARNING("Could not capture a frame: {}", directx_error("Reading the back buffer", result).message);
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        back_buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        result = m_device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(result)) {
            DREAM_LOG_WARNING("Could not capture a frame: {}", directx_error("Making a readable copy", result).message);
            return;
        }
        m_context->CopyResource(staging.Get(), back_buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        result = m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) {
            DREAM_LOG_WARNING("Could not capture a frame: {}", directx_error("Reading the copy", result).message);
            return;
        }
        CapturedFrame frame{.width = desc.Width, .height = desc.Height};
        frame.pixels.resize(std::size_t{desc.Width} * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* const row = static_cast<const std::byte*>(mapped.pData) + std::size_t{y} * mapped.RowPitch;
            std::byte* const out = frame.pixels.data() + std::size_t{y} * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = std::byte{255};
            }
        }
        m_context->Unmap(staging.Get(), 0);
        m_captured = std::move(frame);
    }

    // Objects still alive when the device closes were never destroyed, which over a long session
    // or repeated loads is a leak. Their COM references release them afterwards.
    void warn_about_leftovers() const {
        if (m_textures.size() + m_buffers.size() + m_shaders.size() + m_pipelines.size() > 0) {
            DREAM_LOG_WARNING("The graphics device closed with objects the game never destroyed: "
                              "{} textures, {} buffers, {} shaders and {} pipelines",
                              m_textures.size(), m_buffers.size(), m_shaders.size(),
                              m_pipelines.size());
        }
    }

    // Forwards what the debug layer has to say, since it has no callback of its own.
    void log_debug_messages() {
        if (!m_info_queue) {
            return;
        }
        const UINT64 count = m_info_queue->GetNumStoredMessages();
        std::vector<std::byte> storage;
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T length = 0;
            if (FAILED(get_debug_message(*m_info_queue.Get(), index, nullptr, &length)) ||
                length == 0) {
                continue;
            }
            storage.resize(length);
            auto* const message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (FAILED(get_debug_message(*m_info_queue.Get(), index, message, &length))) {
                continue;
            }
            std::string_view text(message->pDescription, message->DescriptionByteLength);
            while (!text.empty() && text.back() == '\0') {
                text.remove_suffix(1);
            }
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                DREAM_LOG_ERROR("Direct3D: {}", text);
            } else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
                DREAM_LOG_WARNING("Direct3D: {}", text);
            }
        }
        m_info_queue->ClearStoredMessages();
    }

    // Writes a whole dynamic buffer anew, for the uniform and push constant buffers.
    void write_dynamic(ID3D11Buffer* buffer, std::span<const std::byte> data) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = m_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            record_frame_error("Writing shader constants", result);
            return;
        }
        std::memcpy(mapped.pData, data.data(), data.size());
        m_context->Unmap(buffer, 0);
    }

    // Copies data into this frame's vertex memory, four-byte aligned. Returns the offset, or
    // nothing if it doesn't fit.
    std::optional<UINT> copy_to_frame(std::span<const std::byte> data) {
        const std::size_t offset = (m_frame_bytes_used + 3) & ~std::size_t{3};
        if (offset + data.size() > k_max_frame_vertex_bytes) {
            return std::nullopt;
        }
        if (data.empty()) {
            return static_cast<UINT>(offset);
        }

        // The first write of a frame takes fresh memory; later ones append, which the driver
        // allows because nothing written earlier this frame is touched again.
        const D3D11_MAP mode =
            m_frame_buffer_fresh ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = m_context->Map(m_frame_buffer.Get(), 0, mode, 0, &mapped);
        if (FAILED(result)) {
            record_frame_error("Writing per-frame vertices", result);
            return std::nullopt;
        }
        std::memcpy(static_cast<std::byte*>(mapped.pData) + offset, data.data(), data.size());
        m_context->Unmap(m_frame_buffer.Get(), 0);

        m_frame_buffer_fresh = false;
        m_frame_bytes_used = offset + data.size();
        return static_cast<UINT>(offset);
    }

    void record_frame_error(std::string_view action, HRESULT result) {
        if (!m_frame_error.has_value()) {
            m_frame_error = directx_error(action, result);
        }
    }

    // DirectX takes the vertex stride when the buffer is bound, and the stride belongs to the
    // pipeline, so the buffer is bound just before drawing, once both are known.
    void flush_vertex_buffer() {
        if (!m_vertex_buffer_dirty) {
            return;
        }
        m_vertex_buffer_dirty = false;
        ID3D11Buffer* const buffer = m_vertex_buffer.Get();
        m_context->IASetVertexBuffers(0, 1, &buffer, &m_bound_stride, &m_vertex_buffer_offset);
    }

    // CommandList

    void begin_render_pass(const ClearColor& color) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        unbind_textures();
        ID3D11RenderTargetView* const target = m_render_target.Get();
        m_context->OMSetRenderTargets(1, &target, m_depth_view.Get());
        const float clear[4] = {color.r, color.g, color.b, color.a};
        m_context->ClearRenderTargetView(target, clear);
        m_context->ClearDepthStencilView(m_depth_view.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        start_pass(m_extent, {}, {});
    }

    void begin_render_pass(const RenderTargets& targets) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        const TextureResource* const color =
            m_textures.get(handle_cast<TextureResource>(targets.color));
        const TextureResource* const depth =
            m_textures.get(handle_cast<TextureResource>(targets.depth));
        DREAM_VERIFY(color != nullptr || depth != nullptr, "A render pass needs a target");
        DREAM_VERIFY(color == nullptr || color->target, "The color target must be a color render target");
        DREAM_VERIFY(depth == nullptr || depth->depth, "The depth target must be a depth render target");
        DREAM_VERIFY(color == nullptr || depth == nullptr || color->size == depth->size,
                     "A pass's targets must be the same size");

        // A texture can't be read and drawn into at once, so the last pass's are put down first.
        unbind_textures();
        ID3D11RenderTargetView* const target = color != nullptr ? color->target.Get() : nullptr;
        ID3D11DepthStencilView* const depth_view = depth != nullptr ? depth->depth.Get() : nullptr;
        m_context->OMSetRenderTargets(target != nullptr ? 1 : 0, target != nullptr ? &target : nullptr,
                                      depth_view);
        if (target != nullptr && targets.clear_color.has_value()) {
            const ClearColor& value = *targets.clear_color;
            const float clear[4] = {value.r, value.g, value.b, value.a};
            m_context->ClearRenderTargetView(target, clear);
        }
        if (depth_view != nullptr && targets.clear_depth.has_value()) {
            m_context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH, *targets.clear_depth, 0);
        }
        start_pass(color != nullptr ? color->size : depth->size,
                   color != nullptr ? std::optional(color->format) : std::nullopt,
                   depth != nullptr ? std::optional(depth->format) : std::nullopt);
        m_pass_views = {color != nullptr ? color->view.Get() : nullptr,
                        depth != nullptr ? depth->view.Get() : nullptr};
    }

    void start_pass(platform::SurfaceSize size, std::optional<TextureFormat> color,
                    std::optional<TextureFormat> depth) {
        m_in_pass = true;
        m_pass_size = size;
        m_pass_color = color;
        m_pass_depth = depth;
        m_pass_views = {nullptr, nullptr};

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(size.width);
        viewport.Height = static_cast<float>(size.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
        set_scissor(std::nullopt);

        const std::array<ID3D11Buffer*, 2> constants{m_uniform_buffer.Get(),
                                                     m_push_constant_buffer.Get()};
        static_assert(k_uniform_register == 0 && k_push_constant_register == 1);
        m_context->VSSetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
        m_context->PSSetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
        std::array<ID3D11SamplerState*, k_texture_slots> samplers{};
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            samplers[slot] = m_samplers[slot].Get();
        }
        m_context->VSSetSamplers(k_sampler_register, k_texture_slots, samplers.data());
        m_context->PSSetSamplers(k_sampler_register, k_texture_slots, samplers.data());
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            bind_texture(slot, TextureHandle{});
        }
    }

    void unbind_textures() {
        const std::array<ID3D11ShaderResourceView*, k_texture_slots> none{};
        m_context->VSSetShaderResources(k_texture_register, k_texture_slots, none.data());
        m_context->PSSetShaderResources(k_texture_register, k_texture_slots, none.data());
    }

    void end_render_pass() override {
        DREAM_VERIFY(m_in_pass, "No render pass to end");
        m_in_pass = false;
        // So the next pass can read what this one drew.
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void bind_pipeline(PipelineHandle handle) override {
        const PipelineResource* const resource =
            m_pipelines.get(handle_cast<PipelineResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a pipeline that doesn't exist");
        DREAM_VERIFY(resource->color_target == m_pass_color &&
                         resource->depth_target == m_pass_depth,
                     "The pipeline was made for other render targets than this pass has");

        m_context->IASetInputLayout(resource->input_layout.Get());
        m_context->IASetPrimitiveTopology(resource->topology);
        m_context->VSSetShader(resource->vertex.Get(), nullptr, 0);
        m_context->PSSetShader(resource->pixel.Get(), nullptr, 0);
        m_context->RSSetState(resource->rasterizer.Get());
        m_context->OMSetDepthStencilState(resource->depth_stencil.Get(), 0);
        m_context->OMSetBlendState(resource->blend.Get(), nullptr, 0xFFFFFFFFu);

        if (resource->vertex_stride != m_bound_stride) {
            m_bound_stride = resource->vertex_stride;
            m_vertex_buffer_dirty = true;
        }
        m_bound_instance_stride = resource->instance_stride;
    }

    void bind_vertex_buffer(BufferHandle handle) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        m_vertex_buffer = resource->buffer;
        m_vertex_buffer_offset = 0;
        m_vertex_buffer_dirty = true;
    }

    void bind_index_buffer(BufferHandle handle, IndexFormat format) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        m_context->IASetIndexBuffer(
            resource->buffer.Get(),
            format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
        write_dynamic(m_push_constant_buffer.Get(), data);
    }

    void set_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_uniform_size, "Too many uniforms");
        write_dynamic(m_uniform_buffer.Get(), data);
    }

    void bind_texture(std::uint32_t slot, TextureHandle handle) override {
        DREAM_VERIFY(slot < k_texture_slots, "There is no such texture slot");
        ID3D11ShaderResourceView* view =
            slot == k_shadow_slot ? m_far_depth_texture.view.Get() : m_white_texture.view.Get();
        if (handle.is_valid()) {
            const TextureResource* const resource =
                m_textures.get(handle_cast<TextureResource>(handle));
            DREAM_VERIFY(resource != nullptr, "Binding a texture that doesn't exist");
            DREAM_VERIFY((slot == k_shadow_slot) == is_depth(resource->format),
                         "The shadow slot takes depth textures, and only it");
            DREAM_VERIFY(resource->view.Get() != m_pass_views[0] &&
                             resource->view.Get() != m_pass_views[1],
                         "A pass can't read the textures it draws into");
            view = resource->view.Get();
        }
        m_context->VSSetShaderResources(k_texture_register + slot, 1, &view);
        m_context->PSSetShaderResources(k_texture_register + slot, 1, &view);
    }

    void draw_indexed(std::uint32_t index_count) override {
        draw_indexed_range(0, index_count);
    }

    void draw_indexed_range(std::uint32_t first_index, std::uint32_t index_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        flush_vertex_buffer();
        m_context->DrawIndexed(index_count, first_index, 0);
    }

    bool draw_vertices(std::span<const std::byte> vertices, std::uint32_t vertex_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        DREAM_VERIFY(vertex_count == 0 || vertices.size() % vertex_count == 0,
                     "The vertex data doesn't divide into vertex_count vertices");
        if (vertex_count == 0) {
            return true;
        }

        const std::optional<UINT> offset = copy_to_frame(vertices);
        if (!offset.has_value()) {
            return false;
        }
        m_vertex_buffer = m_frame_buffer;
        m_vertex_buffer_offset = *offset;
        m_vertex_buffer_dirty = true;
        flush_vertex_buffer();
        m_context->Draw(vertex_count, 0);
        return true;
    }

    bool bind_frame_geometry(std::span<const std::byte> vertices,
                             std::span<const std::uint32_t> indices) override {
        DREAM_VERIFY(m_in_pass, "Binding geometry outside a render pass");
        const std::size_t start = m_frame_bytes_used;
        const std::optional<UINT> vertex_offset = copy_to_frame(vertices);
        const std::optional<UINT> index_offset =
            vertex_offset.has_value() ? copy_to_frame(std::as_bytes(indices)) : std::nullopt;
        if (!index_offset.has_value()) {
            m_frame_bytes_used = start;
            return false;
        }
        m_vertex_buffer = m_frame_buffer;
        m_vertex_buffer_offset = *vertex_offset;
        m_vertex_buffer_dirty = true;
        m_context->IASetIndexBuffer(m_frame_buffer.Get(), DXGI_FORMAT_R32_UINT, *index_offset);
        return true;
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        DREAM_VERIFY(m_in_pass, "Binding instances outside a render pass");
        const std::optional<UINT> offset = copy_to_frame(instances);
        if (!offset.has_value()) {
            return false;
        }
        m_instance_offset = *offset;
        return true;
    }

    void draw_indexed_instanced(std::uint32_t first_index, std::uint32_t index_count,
                                std::uint32_t instance_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        DREAM_VERIFY(m_bound_instance_stride > 0, "Drawing instances with a pipeline that has none");
        flush_vertex_buffer();
        // Set with each draw, since the stride comes from the pipeline bound now.
        ID3D11Buffer* const buffer = m_frame_buffer.Get();
        m_context->IASetVertexBuffers(1, 1, &buffer, &m_bound_instance_stride, &m_instance_offset);
        m_context->DrawIndexedInstanced(index_count, instance_count, first_index, 0, 0);
    }

    void set_scissor(std::optional<ScissorRect> rect) override {
        DREAM_VERIFY(m_in_pass, "Setting a scissor outside a render pass");
        const auto width = static_cast<std::int64_t>(m_pass_size.width);
        const auto height = static_cast<std::int64_t>(m_pass_size.height);
        std::int64_t left = 0;
        std::int64_t top = 0;
        std::int64_t right = width;
        std::int64_t bottom = height;
        if (rect.has_value()) {
            // Clamped to the surface, which DirectX requires.
            left = std::clamp<std::int64_t>(rect->x, 0, width);
            top = std::clamp<std::int64_t>(rect->y, 0, height);
            right = std::clamp<std::int64_t>(std::int64_t{rect->x} + rect->width, left, width);
            bottom = std::clamp<std::int64_t>(std::int64_t{rect->y} + rect->height, top, height);
        }
        const D3D11_RECT scissor{static_cast<LONG>(left), static_cast<LONG>(top),
                                 static_cast<LONG>(right), static_cast<LONG>(bottom)};
        m_context->RSSetScissorRects(1, &scissor);
    }

    // Released in reverse order, so the device outlives everything made from it, and the adapter
    // outlives the device.
    ComPtr<IDXGIAdapter3> m_adapter;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11InfoQueue> m_info_queue;
    ComPtr<IDXGIFactory2> m_factory;
    std::array<ComPtr<ID3D11SamplerState>, k_texture_slots> m_samplers;
    ComPtr<ID3D11Buffer> m_uniform_buffer;
    ComPtr<ID3D11Buffer> m_push_constant_buffer;
    ComPtr<ID3D11Buffer> m_frame_buffer;
    TextureResource m_white_texture;
    TextureResource m_far_depth_texture;

    ComPtr<IDXGISwapChain1> m_swapchain;
    std::optional<CapturedFrame> m_captured;
    bool m_capture_requested = false;
    ComPtr<ID3D11RenderTargetView> m_render_target;
    ComPtr<ID3D11Texture2D> m_depth_buffer;
    ComPtr<ID3D11DepthStencilView> m_depth_view;
    platform::SurfaceSize m_extent;
    platform::SurfaceSize m_requested_size;
    bool m_was_attached = false;
    bool m_needs_resize = false;

    bool m_in_frame = false;
    bool m_in_pass = false;
    platform::SurfaceSize m_pass_size;
    std::optional<TextureFormat> m_pass_color;
    std::optional<TextureFormat> m_pass_depth;
    std::array<ID3D11ShaderResourceView*, 2> m_pass_views{};
    std::optional<core::Error> m_frame_error;
    std::size_t m_frame_bytes_used = 0;
    bool m_frame_buffer_fresh = true;
    ComPtr<ID3D11Buffer> m_vertex_buffer;
    UINT m_vertex_buffer_offset = 0;
    UINT m_bound_stride = 0;
    UINT m_bound_instance_stride = 0;
    UINT m_instance_offset = 0;
    bool m_vertex_buffer_dirty = true;

    core::HandlePool<TextureResource> m_textures;
    core::HandlePool<BufferResource> m_buffers;
    core::HandlePool<ShaderResource> m_shaders;
    core::HandlePool<PipelineResource> m_pipelines;

    // Enough for the three frames DXGI queues by default, and the one being recorded.
    std::array<FrameTimingQueries, 4> m_timing_queries;
    bool m_timing_available = false;
    bool m_timing_this_frame = false;
    // The ring's sets waiting for the GPU are the `m_timing_pending` from `m_timing_oldest` on.
    std::size_t m_timing_oldest = 0;
    std::size_t m_timing_pending = 0;
    std::deque<double> m_gpu_frame_seconds;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& config) {
    auto device = std::make_unique<DirectXDevice>();
    if (core::Result<void> result = device->initialize(config); !result.has_value()) {
        return result.error();
    }
    return std::unique_ptr<Device>{std::move(device)};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_directx(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "directx", .create = &create});
}

} // namespace dream::modules
