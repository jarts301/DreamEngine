#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
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

// Where every shader finds its inputs, the same registers as the Direct3D 11 backend (ADR 0013), so
// both load the same DXBC files. The root signature puts the uniform block in a root constant
// buffer view, the push constants in root constants, as Vulkan's push constants are, and the
// texture of each slot in a one-descriptor table, with the samplers fixed in the root signature.
// Slot n reads register tn with sampler sn (ADR 0027).
constexpr UINT k_uniform_register = 0;
constexpr UINT k_push_constant_register = 1;
// The block of set_draw_uniforms (ADR 0036).
constexpr UINT k_draw_uniform_register = 2;
constexpr UINT k_texture_register = 0;
constexpr UINT k_sampler_register = 0;

constexpr UINT k_root_uniforms = 0;
constexpr UINT k_root_push_constants = 1;
constexpr UINT k_root_texture = 2; // Slot n's table is k_root_texture + n.
constexpr UINT k_root_draw_uniforms = k_root_texture + k_texture_slots;

// Textures not being drawn into wait in this state, ready for any shader to read.
constexpr D3D12_RESOURCE_STATES k_shader_read =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

static_assert(k_max_push_constant_size % 4 == 0);
constexpr UINT k_push_constant_values = static_cast<UINT>(k_max_push_constant_size / 4);

// The window is drawn through an sRGB view of a plain back buffer, which is how the flip
// presentation model wants sRGB output, as in the Direct3D 11 backend.
constexpr DXGI_FORMAT k_back_buffer_format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT k_render_target_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr DXGI_FORMAT k_depth_format = DXGI_FORMAT_D32_FLOAT;
constexpr UINT k_back_buffer_count = 2;

// Frames the CPU may record while the GPU still draws earlier ones.
constexpr std::size_t k_frames_in_flight = 2;

// Textures the shader-visible descriptor heap holds at once, the white texture included.
constexpr UINT k_max_textures = 4096;

// Uniform blocks are written to upload memory in chunks of this size, and a frame takes more chunks
// when it sets more uniforms than one holds.
constexpr UINT64 k_constant_chunk_bytes = 1024 * 1024;
// Each block is the largest uniform block, rounded up to where constant buffers can start.
constexpr UINT64 k_constant_block_bytes =
    (k_max_uniform_size + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) /
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(k_max_uniform_size <= k_constant_block_bytes);
// The per-draw block's space, likewise: shaders may read all of it.
constexpr UINT64 k_draw_block_bytes =
    (k_max_draw_uniform_size + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) /
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(k_draw_block_bytes <= k_constant_chunk_bytes);

// How many measured frames are kept for take_gpu_frame_seconds before the oldest are dropped.
constexpr std::size_t k_kept_gpu_frame_times = 16;

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
    case TextureFormat::Rgba32Float:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
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
// in whether ID3D12InfoQueue's method of that name was declared with the macro in effect. This
// calls whichever name the header ended up with.
#pragma push_macro("GetMessage")
#undef GetMessage
template <typename Queue>
HRESULT get_debug_message(Queue& queue, UINT64 index, D3D12_MESSAGE* message, SIZE_T* length) {
    if constexpr (requires { queue.GetMessage(index, message, length); }) {
        return queue.GetMessage(index, message, length);
    } else if constexpr (requires { queue.GetMessageA(index, message, length); }) {
        return queue.GetMessageA(index, message, length);
    } else {
        return queue.GetMessageW(index, message, length);
    }
}
#pragma pop_macro("GetMessage")

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

// sRGB encoding, for averaging colors in linear light when making mipmaps, as the GPU does.
float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    return value <= 0.0031308f ? value * 12.92f
                               : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// One level of a mipmap chain, four bytes per pixel.
struct MipLevel {
    UINT width = 0;
    UINT height = 0;
    std::vector<std::byte> pixels;
};

// Direct3D 12 has no GenerateMips, so the chain is made here, each level a 2x2 box filter of the
// one above, with colors averaged in linear light for sRGB textures. Alpha is never sRGB.
std::vector<MipLevel> make_mipmaps(UINT width, UINT height, std::span<const std::byte> pixels,
                                   bool srgb) {
    std::array<float, 256> to_linear{};
    for (std::size_t value = 0; value < to_linear.size(); ++value) {
        const float unit = static_cast<float>(value) / 255.0f;
        to_linear[value] = srgb ? srgb_to_linear(unit) : unit;
    }
    const auto encode = [srgb](float value) {
        const float encoded = srgb ? linear_to_srgb(value) : value;
        return static_cast<std::byte>(
            std::clamp(static_cast<int>(std::lround(encoded * 255.0f)), 0, 255));
    };

    std::vector<MipLevel> levels;
    levels.push_back(MipLevel{width, height, {pixels.begin(), pixels.end()}});
    while (levels.back().width > 1 || levels.back().height > 1) {
        const MipLevel& above = levels.back();
        MipLevel level{std::max(above.width / 2, 1u), std::max(above.height / 2, 1u), {}};
        level.pixels.resize(std::size_t{level.width} * level.height * 4);
        for (UINT y = 0; y < level.height; ++y) {
            for (UINT x = 0; x < level.width; ++x) {
                std::array<float, 4> sum{};
                for (UINT dy = 0; dy < 2; ++dy) {
                    for (UINT dx = 0; dx < 2; ++dx) {
                        // An odd size repeats the last row or column.
                        const UINT sx = std::min(x * 2 + dx, above.width - 1);
                        const UINT sy = std::min(y * 2 + dy, above.height - 1);
                        const std::byte* const source =
                            above.pixels.data() + (std::size_t{sy} * above.width + sx) * 4;
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            sum[channel] += to_linear[std::to_integer<std::size_t>(source[channel])];
                        }
                        sum[3] += static_cast<float>(std::to_integer<int>(source[3])) / 255.0f;
                    }
                }
                std::byte* const target = level.pixels.data() + (std::size_t{y} * level.width + x) * 4;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    target[channel] = encode(sum[channel] / 4.0f);
                }
                target[3] = static_cast<std::byte>(
                    std::clamp(static_cast<int>(std::lround(sum[3] / 4.0f * 255.0f)), 0, 255));
            }
        }
        levels.push_back(std::move(level));
    }
    return levels;
}

struct BufferResource {
    ComPtr<ID3D12Resource> buffer;
    UINT size = 0;
};

struct ShaderResource {
    ShaderStage stage = ShaderStage::Vertex;
    // Kept, because pipelines are built from the code itself.
    std::vector<std::byte> code;
};

// A pipeline state object holds its compiled shaders, so a pipeline keeps working after its shaders
// are destroyed, as it does on Vulkan.
struct PipelineResource {
    ComPtr<ID3D12PipelineState> state;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT vertex_stride = 0;
    UINT instance_stride = 0;
    std::optional<TextureFormat> color_target;
    std::optional<TextureFormat> depth_target;
};

struct TextureResource {
    ComPtr<ID3D12Resource> texture;
    // The texture's place in the shader-visible descriptor heap.
    UINT descriptor = 0;
    // For render targets, a heap of their own with the view a pass draws through.
    ComPtr<ID3D12DescriptorHeap> target_view;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    platform::SurfaceSize size;
    bool render_target = false;
    // Its rows can be replaced with update_texture (ADR 0037).
    bool dynamic = false;
};

// Memory the CPU writes and the GPU reads, mapped for as long as it lives.
struct UploadBuffer {
    ComPtr<ID3D12Resource> resource;
    std::byte* mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    UINT64 size = 0;
};

// What one frame in flight owns. It is reused once the GPU has finished the frame that last used it.
struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value = 0;
    // Uniform blocks, in as many chunks as the frame needed; kept for later frames.
    std::vector<UploadBuffer> constant_chunks;
    std::size_t constant_chunk = 0;
    UINT64 constant_offset = 0;
    // draw_vertices and bind_frame_geometry.
    UploadBuffer vertices;
};

// Objects the GPU may still use, released once the fence passes `fence_value`.
struct Retired {
    UINT64 fence_value = 0;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12PipelineState> pipeline;
    std::optional<UINT> descriptor;
    ComPtr<ID3D12DescriptorHeap> heap;
};

// A graphics device on Direct3D 12, feature level 11.0, presenting with the DXGI flip model. Unlike
// Direct3D 11, it leaves memory, synchronization and mipmaps to the program, which is why the
// backend tracks frames in flight with a fence and retires objects only once the GPU is done.
class DirectX12Device final : public Device, private CommandList {
public:
    DirectX12Device() = default;
    DirectX12Device(const DirectX12Device&) = delete;
    DirectX12Device& operator=(const DirectX12Device&) = delete;

    ~DirectX12Device() override {
        warn_about_leftovers();
        if (m_queue && m_fence) {
            wait_for_gpu();
        }
        m_retired.clear();
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
        core::Result<TextureResource> far_depth = create_render_target(TextureDesc{
            .width = 1, .height = 1, .format = TextureFormat::Depth32Float, .render_target = true});
        if (!far_depth.has_value()) {
            return far_depth.error();
        }
        m_far_depth_texture = std::move(far_depth).value();
        ID3D12Resource* const far_resource = m_far_depth_texture.texture.Get();
        const D3D12_CPU_DESCRIPTOR_HANDLE far_view =
            m_far_depth_texture.target_view->GetCPUDescriptorHandleForHeapStart();
        auto cleared = run_upload([&](ID3D12GraphicsCommandList& list) {
            const D3D12_RESOURCE_BARRIER to_depth =
                transition(far_resource, k_shader_read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            list.ResourceBarrier(1, &to_depth);
            list.ClearDepthStencilView(far_view, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            const D3D12_RESOURCE_BARRIER to_read =
                transition(far_resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, k_shader_read);
            list.ResourceBarrier(1, &to_read);
        });
        if (!cleared.has_value()) {
            return cleared.error();
        }
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

        ComPtr<IDXGISwapChain1> swapchain;
        HRESULT result = m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &desc, nullptr,
                                                           nullptr, &swapchain);
        if (SUCCEEDED(result)) {
            result = swapchain.As(&m_swapchain);
        }
        if (FAILED(result)) {
            m_swapchain.Reset();
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
        wait_for_gpu();
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
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = to_directx(format),
                                                  .Support1 = {},
                                                  .Support2 = {}};
        return support.Format != DXGI_FORMAT_UNKNOWN &&
               SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                       sizeof(support))) &&
               (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0;
    }

    // Feature level 11.0 draws into and filters all of these, but the driver is asked anyway.
    bool supports_render_target(TextureFormat format) const override {
        if (is_compressed(format)) {
            return false;
        }
        const bool depth = is_depth(format);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{
            .Format = depth ? DXGI_FORMAT_D32_FLOAT : to_directx(format),
            .Support1 = {},
            .Support2 = {}};
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                 sizeof(support)))) {
            return false;
        }
        const D3D12_FORMAT_SUPPORT1 needed =
            depth ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
                  : D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                        D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        return (support.Support1 & needed) == needed;
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

        BufferResource resource;
        resource.size = static_cast<UINT>(contents.size());
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc = buffer_desc(contents.size());
        HRESULT result = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&resource.buffer));
        if (FAILED(result)) {
            return directx_error("Creating a buffer", result);
        }

        core::Result<UploadBuffer> staging = create_upload_buffer(contents.size());
        if (!staging.has_value()) {
            return staging.error();
        }
        std::memcpy(staging.value().mapped, contents.data(), contents.size());

        const D3D12_RESOURCE_STATES final_state = usage == BufferUsage::Vertex
                                                      ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                                                      : D3D12_RESOURCE_STATE_INDEX_BUFFER;
        auto copied = run_upload([&](ID3D12GraphicsCommandList& list) {
            const D3D12_RESOURCE_BARRIER to_copy = transition(
                resource.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list.ResourceBarrier(1, &to_copy);
            list.CopyBufferRegion(resource.buffer.Get(), 0, staging.value().resource.Get(), 0,
                                  contents.size());
            const D3D12_RESOURCE_BARRIER to_use =
                transition(resource.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, final_state);
            list.ResourceBarrier(1, &to_use);
        });
        if (!copied.has_value()) {
            return copied.error();
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

        // Direct3D 12 only reads the code when a pipeline is made, so it is checked here, and a
        // shader that reads the wrong registers would draw garbage, so it is refused up front.
        core::Result<ComPtr<ID3D11ShaderReflection>> reflection = reflect(code);
        if (!reflection.has_value()) {
            return core::Error{reflection.error().code,
                               reflection.error().message +
                                   " (is it DXBC, compiled for shader model 5.0?)"};
        }
        D3D11_SHADER_DESC shader{};
        if (const HRESULT result = reflection.value()->GetDesc(&shader); FAILED(result)) {
            return directx_error("Reading a shader's layout", result);
        }
        const UINT kind = D3D11_SHVER_GET_TYPE(shader.Version);
        const UINT expected = stage == ShaderStage::Vertex ? D3D11_SHVER_VERTEX_SHADER
                                                           : D3D11_SHVER_PIXEL_SHADER;
        if (kind != expected) {
            return invalid(stage == ShaderStage::Vertex ? "The code is not a vertex shader"
                                                        : "The code is not a pixel shader");
        }
        if (auto checked = check_bindings(*reflection.value().Get()); !checked.has_value()) {
            return checked.error();
        }

        ShaderResource resource{.stage = stage, .code = {code.begin(), code.end()}};
        return handle_cast<ShaderTag>(m_shaders.create(std::move(resource)));
    }

    core::Result<PipelineHandle> create_pipeline(const PipelineDesc& desc) override {
        // The formats don't depend on the surface, but the interface asks for one, as Vulkan
        // needs it, so every backend behaves alike.
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
        if (vertex == nullptr || vertex->stage != ShaderStage::Vertex ||
            (desc.fragment_shader.is_valid() &&
             (fragment == nullptr || fragment->stage != ShaderStage::Fragment))) {
            return invalid("A pipeline needs valid shaders, one per stage");
        }

        core::Result<InputLayout> layout =
            create_input_layout(desc.attributes, desc.instance_attributes, vertex->code);
        if (!layout.has_value()) {
            return layout.error();
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = m_root_signature.Get();
        state.VS = {vertex->code.data(), vertex->code.size()};
        if (fragment != nullptr) {
            state.PS = {fragment->code.data(), fragment->code.size()};
        }
        state.InputLayout = {layout.value().elements.data(),
                             static_cast<UINT>(layout.value().elements.size())};
        state.PrimitiveTopologyType = desc.topology == PrimitiveTopology::Lines
                                          ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                          : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.SampleMask = UINT_MAX;
        state.SampleDesc.Count = 1;
        const bool into_window = !desc.color_target.has_value() && !desc.depth_target.has_value();
        if (into_window) {
            state.NumRenderTargets = 1;
            state.RTVFormats[0] = k_render_target_format;
            state.DSVFormat = k_depth_format;
        } else {
            state.NumRenderTargets = desc.color_target.has_value() ? 1 : 0;
            if (desc.color_target.has_value()) {
                state.RTVFormats[0] = to_directx(*desc.color_target);
            }
            state.DSVFormat =
                desc.depth_target.has_value() ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        }
        const bool has_depth = into_window || desc.depth_target.has_value();

        D3D12_RASTERIZER_DESC& rasterizer = state.RasterizerState;
        rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = desc.cull == CullMode::Back    ? D3D12_CULL_MODE_BACK
                              : desc.cull == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                             : D3D12_CULL_MODE_NONE;
        rasterizer.DepthBias =
            static_cast<INT>(desc.depth_bias + (desc.depth_bias < 0 ? -0.5f : 0.5f));
        rasterizer.SlopeScaledDepthBias = desc.slope_depth_bias;
        // DirectX clip space already has +Y up, so counter-clockwise means the same as in ADR 0005.
        rasterizer.FrontCounterClockwise = TRUE;
        rasterizer.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC& depth = state.DepthStencilState;
        // A pass with no depth target has nothing to test against.
        const bool depth_test = desc.depth_test && has_depth;
        depth.DepthEnable = depth_test ? TRUE : FALSE;
        depth.DepthWriteMask = depth_test && desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                                              : D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        D3D12_RENDER_TARGET_BLEND_DESC& target = state.BlendState.RenderTarget[0];
        target.BlendEnable = desc.blend != BlendMode::Opaque ? TRUE : FALSE;
        target.SrcBlend = desc.blend == BlendMode::Alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        PipelineResource resource;
        resource.vertex_stride = desc.vertex_stride;
        resource.instance_stride = desc.instance_stride;
        resource.color_target = desc.color_target;
        resource.depth_target = desc.depth_target;
        resource.topology = desc.topology == PrimitiveTopology::Lines
                                ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        const HRESULT result =
            m_device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&resource.state));
        log_debug_messages();
        if (FAILED(result)) {
            return directx_error("Creating a pipeline (do the attribute formats match the vertex "
                                 "shader?)",
                                 result);
        }
        return handle_cast<PipelineTag>(m_pipelines.create(std::move(resource)));
    }

    // The GPU may still be drawing with these, so they are retired, and released once it is done.
    void destroy_buffer(BufferHandle buffer) override {
        if (BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(buffer))) {
            retire(Retired{.resource = std::move(resource->buffer)});
            m_buffers.destroy(handle_cast<BufferResource>(buffer));
        }
    }

    void destroy_texture(TextureHandle texture) override {
        if (TextureResource* const resource =
                m_textures.get(handle_cast<TextureResource>(texture))) {
            retire(Retired{.resource = std::move(resource->texture),
                           .descriptor = resource->descriptor,
                           .heap = std::move(resource->target_view)});
            m_textures.destroy(handle_cast<TextureResource>(texture));
        }
    }

    void destroy_shader(ShaderHandle shader) override {
        m_shaders.destroy(handle_cast<ShaderResource>(shader));
    }

    void destroy_pipeline(PipelineHandle pipeline) override {
        if (PipelineResource* const resource =
                m_pipelines.get(handle_cast<PipelineResource>(pipeline))) {
            retire(Retired{.pipeline = std::move(resource->state)});
            m_pipelines.destroy(handle_cast<PipelineResource>(pipeline));
        }
    }

    core::Result<CommandList*> begin_frame() override {
        DREAM_VERIFY(!m_in_frame, "begin_frame called twice");
        const auto no_frame = static_cast<CommandList*>(nullptr);

        release_retired();
        if (!m_swapchain) {
            return no_frame;
        }
        if (m_needs_resize) {
            if (auto resized = resize_targets(); !resized.has_value()) {
                return resized.error();
            }
        }
        // A minimized window has no size, so its frames are skipped until it gets one again.
        if (!m_back_buffers[0] || m_extent.width == 0 || m_extent.height == 0) {
            return no_frame;
        }

        FrameSlot& frame = m_frames[m_frame_index];
        wait_for_fence(frame.fence_value);
        collect_frame_timings();
        HRESULT result = frame.allocator->Reset();
        if (SUCCEEDED(result)) {
            result = m_list->Reset(frame.allocator.Get(), nullptr);
        }
        if (FAILED(result)) {
            return directx_error("Starting a frame's commands", result);
        }

        m_in_frame = true;
        m_frame_error.reset();
        frame.constant_chunk = 0;
        frame.constant_offset = 0;
        m_frame_bytes_used = 0;
        m_vertex_view.reset();
        m_instance_view.reset();
        m_vertex_view_dirty = true;
        m_bound_stride = 0;
        m_back_buffer = m_swapchain->GetCurrentBackBufferIndex();

        const D3D12_RESOURCE_BARRIER to_target =
            transition(m_back_buffers[m_back_buffer].Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_list->ResourceBarrier(1, &to_target);
        ID3D12DescriptorHeap* const heaps[] = {m_texture_heap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetGraphicsRootSignature(m_root_signature.Get());
        if (m_timing_available) {
            m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp_index(0));
        }
        return static_cast<CommandList*>(this);
    }

    core::Result<void> end_frame() override {
        DREAM_VERIFY(m_in_frame, "end_frame called without begin_frame");
        DREAM_VERIFY(!m_in_pass, "The render pass was not ended");
        m_in_frame = false;

        FrameSlot& frame = m_frames[m_frame_index];
        ID3D12Resource* const back_buffer = m_back_buffers[m_back_buffer].Get();
        // Before presenting, so waiting for the display isn't part of the GPU's time.
        if (m_timing_available) {
            m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp_index(1));
            m_list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     timestamp_index(0), 2, m_timestamp_readback.Get(),
                                     UINT64{timestamp_index(0)} * sizeof(UINT64));
        }

        const bool capturing = std::exchange(m_capture_requested, false);
        if (capturing) {
            record_capture(back_buffer);
        } else {
            const D3D12_RESOURCE_BARRIER to_present = transition(
                back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            m_list->ResourceBarrier(1, &to_present);
        }

        HRESULT result = m_list->Close();
        if (FAILED(result)) {
            log_debug_messages();
            return directx_error("Finishing a frame's commands", result);
        }
        ID3D12CommandList* const lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        result = m_swapchain->Present(1, 0);
        frame.fence_value = signal();
        if (m_timing_available) {
            m_timed_frames.push_back(TimedFrame{m_frame_index, frame.fence_value});
        }
        m_frame_index = (m_frame_index + 1) % k_frames_in_flight;
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
        if (capturing) {
            wait_for_fence(frame.fence_value);
            read_capture();
        }
        return {};
    }

private:
    struct InputLayout {
        // The semantic names point into the reflection object, which is kept alongside.
        ComPtr<ID3D11ShaderReflection> reflection;
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    };

    struct TimedFrame {
        std::size_t slot = 0;
        UINT64 fence_value = 0;
    };

    core::Result<void> create_device(const core::Config& config) {
        const bool wants_debug =
            config.get_bool("rhi.directx12.debug").value_or(k_debug_layer_by_default);
        // WARP is Windows' software rasterizer, for machines without a usable GPU driver.
        const bool software = config.get_bool("rhi.directx12.software").value_or(false);

        // The debug layer is an optional Windows feature, "Graphics Tools", so its absence is not
        // an error.
        bool debugging = false;
        if (wants_debug) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                debugging = true;
            }
        }

        HRESULT result =
            CreateDXGIFactory2(debugging ? DXGI_CREATE_FACTORY_DEBUG : 0u, IID_PPV_ARGS(&m_factory));
        if (FAILED(result) && debugging) {
            result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
        }
        if (FAILED(result)) {
            return directx_error("Creating the DXGI factory", result);
        }

        ComPtr<IDXGIAdapter1> adapter;
        if (software) {
            result = m_factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            if (FAILED(result)) {
                return directx_error("Finding WARP", result);
            }
        } else {
            adapter = find_hardware_adapter();
            if (!adapter) {
                return core::Error{core::ErrorCode::Unknown,
                                   "No graphics card supports Direct3D 12 at feature level 11.0"};
            }
        }

        // Nothing here needs more than feature level 11.0, the baseline of Direct3D 12 hardware.
        result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        if (FAILED(result)) {
            return directx_error("Creating a Direct3D 12 device", result);
        }
        if (debugging && SUCCEEDED(m_device.As(&m_info_queue))) {
            // The window is cleared to whatever color the game asks for, never one fixed at
            // creation, so the debug layer's advice to fix one would come every frame.
            std::array<D3D12_MESSAGE_ID, 2> quiet{
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<UINT>(quiet.size());
            filter.DenyList.pIDList = quiet.data();
            m_info_queue->PushStorageFilter(&filter);
        }
        DREAM_LOG_INFO("Direct3D 12 debug layer is {}", m_info_queue ? "on" : "off");

        // Windows 10's adapters report the video memory the program uses, for memory_usage. Without
        // it, there is simply nothing to report.
        static_cast<void>(adapter.As(&m_adapter));
        DXGI_ADAPTER_DESC1 adapter_desc{};
        adapter->GetDesc1(&adapter_desc);
        DREAM_LOG_INFO("Direct3D 12 device: {}", to_utf8(adapter_desc.Description));

        const D3D12_COMMAND_QUEUE_DESC queue{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
        result = m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_queue));
        if (SUCCEEDED(result)) {
            result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        }
        if (FAILED(result)) {
            return directx_error("Creating the command queue", result);
        }
        m_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_fence_event == nullptr) {
            return directx_error("Creating the fence event", HRESULT_FROM_WIN32(GetLastError()));
        }
        return {};
    }

    // The fastest adapter that can run Direct3D 12, skipping software ones, which WARP is asked for
    // by name.
    ComPtr<IDXGIAdapter1> find_hardware_adapter() {
        ComPtr<IDXGIFactory6> by_preference;
        static_cast<void>(m_factory.As(&by_preference));
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT found =
                by_preference
                    ? by_preference->EnumAdapterByGpuPreference(
                          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                    : m_factory->EnumAdapters1(index, &adapter);
            if (found == DXGI_ERROR_NOT_FOUND) {
                return nullptr;
            }
            if (FAILED(found)) {
                continue;
            }
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                return adapter;
            }
        }
    }

    // The root signature, descriptor heaps, command list, per-frame memory and the copy commands
    // that uploads use.
    core::Result<void> create_shared_state() {
        if (auto result = create_root_signature(); !result.has_value()) {
            return result;
        }

        const D3D12_DESCRIPTOR_HEAP_DESC textures{
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            .NumDescriptors = k_max_textures,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        const D3D12_DESCRIPTOR_HEAP_DESC targets{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                 .NumDescriptors = k_back_buffer_count};
        const D3D12_DESCRIPTOR_HEAP_DESC depth{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                               .NumDescriptors = 1};
        HRESULT result = m_device->CreateDescriptorHeap(&textures, IID_PPV_ARGS(&m_texture_heap));
        if (SUCCEEDED(result)) {
            result = m_device->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&m_target_heap));
        }
        if (SUCCEEDED(result)) {
            result = m_device->CreateDescriptorHeap(&depth, IID_PPV_ARGS(&m_depth_heap));
        }
        if (FAILED(result)) {
            return directx_error("Creating descriptor heaps", result);
        }
        m_texture_descriptor_size =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_target_descriptor_size =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_free_descriptors.reserve(k_max_textures);
        for (UINT slot = k_max_textures; slot > 0; --slot) {
            m_free_descriptors.push_back(slot - 1);
        }

        for (FrameSlot& frame : m_frames) {
            result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&frame.allocator));
            if (FAILED(result)) {
                return directx_error("Creating a command allocator", result);
            }
            core::Result<UploadBuffer> vertices = create_upload_buffer(k_max_frame_vertex_bytes);
            if (!vertices.has_value()) {
                return vertices.error();
            }
            frame.vertices = std::move(vertices).value();
        }
        result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             m_frames[0].allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&m_list));
        if (SUCCEEDED(result)) {
            result = m_list->Close();
        }
        if (SUCCEEDED(result)) {
            result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&m_upload_allocator));
        }
        if (SUCCEEDED(result)) {
            result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_upload_allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&m_upload_list));
        }
        if (SUCCEEDED(result)) {
            result = m_upload_list->Close();
        }
        if (FAILED(result)) {
            return directx_error("Creating command lists", result);
        }
        return {};
    }

    core::Result<void> create_root_signature() {
        std::array<D3D12_DESCRIPTOR_RANGE, k_texture_slots> texture_ranges{};
        for (UINT slot = 0; slot < k_texture_slots; ++slot) {
            texture_ranges[slot] = D3D12_DESCRIPTOR_RANGE{
                .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                .NumDescriptors = 1,
                .BaseShaderRegister = k_texture_register + slot,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = 0};
        }

        std::array<D3D12_ROOT_PARAMETER, k_root_draw_uniforms + 1> parameters{};
        parameters[k_root_draw_uniforms].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[k_root_draw_uniforms].Descriptor = {k_draw_uniform_register, 0};
        parameters[k_root_draw_uniforms].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[k_root_uniforms].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[k_root_uniforms].Descriptor = {k_uniform_register, 0};
        parameters[k_root_uniforms].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[k_root_push_constants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[k_root_push_constants].Constants = {k_push_constant_register, 0,
                                                       k_push_constant_values};
        parameters[k_root_push_constants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT slot = 0; slot < k_texture_slots; ++slot) {
            D3D12_ROOT_PARAMETER& table = parameters[k_root_texture + slot];
            table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            table.DescriptorTable = {1, &texture_ranges[slot]};
            table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        // One sampler per slot, as ADR 0027 lists them.
        std::array<D3D12_STATIC_SAMPLER_DESC, k_texture_slots> samplers{};
        for (UINT slot = 0; slot < k_texture_slots; ++slot) {
            D3D12_STATIC_SAMPLER_DESC& sampler = samplers[slot];
            const D3D12_TEXTURE_ADDRESS_MODE address = slot_sampler(slot) == SlotSampler::Repeat
                                                           ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                                           : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.Filter = slot == k_shadow_slot
                                 ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                 : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = address;
            sampler.AddressV = address;
            sampler.AddressW = address;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = slot == k_shadow_slot ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                           : D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = k_sampler_register + slot;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        const D3D12_ROOT_SIGNATURE_DESC desc{
            .NumParameters = static_cast<UINT>(parameters.size()),
            .pParameters = parameters.data(),
            .NumStaticSamplers = static_cast<UINT>(samplers.size()),
            .pStaticSamplers = samplers.data(),
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                     &serialized, &errors);
        if (FAILED(result)) {
            if (errors) {
                DREAM_LOG_ERROR("Direct3D 12: {}",
                                std::string_view(static_cast<const char*>(errors->GetBufferPointer()),
                                                 errors->GetBufferSize()));
            }
            return directx_error("Describing the root signature", result);
        }
        result = m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                               serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&m_root_signature));
        if (FAILED(result)) {
            return directx_error("Creating the root signature", result);
        }
        return {};
    }

    core::Result<UploadBuffer> create_upload_buffer(UINT64 size) {
        UploadBuffer buffer;
        buffer.size = size;
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC desc = buffer_desc(size);
        HRESULT result = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&buffer.resource));
        if (FAILED(result)) {
            return directx_error("Creating upload memory", result);
        }
        // Never read by the CPU, so the read range is empty.
        const D3D12_RANGE nothing{0, 0};
        void* mapped = nullptr;
        result = buffer.resource->Map(0, &nothing, &mapped);
        if (FAILED(result)) {
            return directx_error("Mapping upload memory", result);
        }
        buffer.mapped = static_cast<std::byte*>(mapped);
        buffer.address = buffer.resource->GetGPUVirtualAddress();
        return buffer;
    }

    // Records copy commands, runs them, and waits for them. Uploads go on the same queue as the
    // frames, so a resource made while a frame is recorded is ready before that frame runs.
    template <typename Record>
    core::Result<void> run_upload(Record&& record) {
        HRESULT result = m_upload_allocator->Reset();
        if (SUCCEEDED(result)) {
            result = m_upload_list->Reset(m_upload_allocator.Get(), nullptr);
        }
        if (FAILED(result)) {
            return directx_error("Starting an upload", result);
        }
        record(*m_upload_list.Get());
        result = m_upload_list->Close();
        if (FAILED(result)) {
            log_debug_messages();
            return directx_error("Recording an upload", result);
        }
        ID3D12CommandList* const lists[] = {m_upload_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        wait_for_fence(signal());
        return {};
    }

    UINT64 signal() {
        const UINT64 value = ++m_fence_value;
        m_queue->Signal(m_fence.Get(), value);
        return value;
    }

    void wait_for_fence(UINT64 value) {
        if (value == 0 || m_fence->GetCompletedValue() >= value) {
            return;
        }
        if (SUCCEEDED(m_fence->SetEventOnCompletion(value, m_fence_event))) {
            WaitForSingleObject(m_fence_event, INFINITE);
        }
    }

    void wait_for_gpu() {
        wait_for_fence(signal());
        release_retired();
    }

    void retire(Retired retired) {
        retired.fence_value = m_fence_value;
        m_retired.push_back(std::move(retired));
    }

    void release_retired() {
        const UINT64 completed = m_fence->GetCompletedValue();
        while (!m_retired.empty() && m_retired.front().fence_value <= completed) {
            if (m_retired.front().descriptor.has_value()) {
                m_free_descriptors.push_back(*m_retired.front().descriptor);
            }
            m_retired.pop_front();
        }
    }

    // GPU timing, for take_gpu_frame_seconds. Each frame slot has two timestamps, resolved into
    // memory the CPU reads once the frame's fence has passed, without waiting.
    void create_timing_queries() {
        const D3D12_QUERY_HEAP_DESC heap{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                         .Count = static_cast<UINT>(k_frames_in_flight * 2)};
        const D3D12_HEAP_PROPERTIES readback{.Type = D3D12_HEAP_TYPE_READBACK};
        const D3D12_RESOURCE_DESC desc = buffer_desc(k_frames_in_flight * 2 * sizeof(UINT64));
        UINT64 frequency = 0;
        if (FAILED(m_device->CreateQueryHeap(&heap, IID_PPV_ARGS(&m_timestamps))) ||
            FAILED(m_device->CreateCommittedResource(
                &readback, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_timestamp_readback))) ||
            FAILED(m_queue->GetTimestampFrequency(&frequency)) || frequency == 0) {
            DREAM_LOG_WARNING("Direct3D 12 timestamp queries are unavailable, so GPU time "
                              "isn't measured");
            return;
        }
        m_timestamp_frequency = frequency;
        m_timing_available = true;
    }

    [[nodiscard]] UINT timestamp_index(UINT which) const {
        return static_cast<UINT>(m_frame_index * 2) + which;
    }

    // Reads the timed frames the GPU has finished, oldest first.
    void collect_frame_timings() {
        const UINT64 completed = m_fence->GetCompletedValue();
        while (!m_timed_frames.empty() && m_timed_frames.front().fence_value <= completed) {
            const std::size_t slot = m_timed_frames.front().slot;
            m_timed_frames.pop_front();

            const D3D12_RANGE range{slot * 2 * sizeof(UINT64), (slot * 2 + 2) * sizeof(UINT64)};
            void* mapped = nullptr;
            if (FAILED(m_timestamp_readback->Map(0, &range, &mapped))) {
                continue;
            }
            std::array<UINT64, 2> ticks{};
            std::memcpy(ticks.data(), static_cast<const std::byte*>(mapped) + range.Begin,
                        sizeof(ticks));
            const D3D12_RANGE nothing{0, 0};
            m_timestamp_readback->Unmap(0, &nothing);

            if (ticks[1] >= ticks[0]) {
                if (m_gpu_frame_seconds.size() == k_kept_gpu_frame_times) {
                    m_gpu_frame_seconds.pop_front();
                }
                m_gpu_frame_seconds.push_back(static_cast<double>(ticks[1] - ticks[0]) /
                                              static_cast<double>(m_timestamp_frequency));
            }
        }
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

    // Checks that a shader's resources are at the registers the root signature has; see the
    // constants at the top of the file.
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
                } else if (binding.BindPoint == k_draw_uniform_register) {
                    if (buffer.Size > k_max_draw_uniform_size) {
                        return invalid(std::format("Per-draw block '{}' is {} bytes; at most {} "
                                                   "are allowed",
                                                   name, buffer.Size, k_max_draw_uniform_size));
                    }
                } else {
                    return invalid(std::format(
                        "Constant buffer '{}' is at register b{}; DirectX shaders must put the "
                        "uniform block at register(b{}), push constants at register(b{}) and the "
                        "per-draw block at register(b{}), see ADR 0013 and 0036",
                        name, binding.BindPoint, k_uniform_register, k_push_constant_register,
                        k_draw_uniform_register));
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
    core::Result<InputLayout> create_input_layout(std::span<const VertexAttribute> attributes,
                                                  std::span<const VertexAttribute> instances,
                                                  std::span<const std::byte> vertex_code) {
        core::Result<ComPtr<ID3D11ShaderReflection>> reflected = reflect(vertex_code);
        if (!reflected.has_value()) {
            return reflected.error();
        }
        InputLayout layout{.reflection = std::move(reflected).value(), .elements = {}};
        ID3D11ShaderReflection& reflection = *layout.reflection.Get();

        D3D11_SHADER_DESC shader{};
        if (const HRESULT result = reflection.GetDesc(&shader); FAILED(result)) {
            return directx_error("Reading the vertex shader's inputs", result);
        }

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

        layout.elements.reserve(attributes.size() + instances.size());
        const auto add = [&](std::span<const VertexAttribute> from, bool per_instance) {
            for (const VertexAttribute& attribute : from) {
                // An attribute the shader doesn't read is left out, as Vulkan would ignore it.
                if (attribute.location >= inputs.size()) {
                    continue;
                }
                const D3D11_SIGNATURE_PARAMETER_DESC& input = inputs[attribute.location];
                layout.elements.push_back(D3D12_INPUT_ELEMENT_DESC{
                    .SemanticName = input.SemanticName,
                    .SemanticIndex = input.SemanticIndex,
                    .Format = to_directx(attribute.format),
                    .InputSlot = per_instance ? 1u : 0u,
                    .AlignedByteOffset = attribute.offset,
                    .InputSlotClass = per_instance
                                          ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                          : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    .InstanceDataStepRate = per_instance ? 1u : 0u});
            }
        };
        add(attributes, false);
        add(instances, true);
        for (std::size_t location = 0; location < inputs.size(); ++location) {
            const auto at = [&](const VertexAttribute& a) { return a.location == location; };
            const bool provided =
                std::ranges::any_of(attributes, at) || std::ranges::any_of(instances, at);
            if (!provided) {
                return invalid(std::format("The vertex shader reads input {} ({}{}), which the "
                                           "pipeline has no attribute for",
                                           location, inputs[location].SemanticName,
                                           inputs[location].SemanticIndex));
            }
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
        if (m_free_descriptors.empty()) {
            release_retired();
            if (m_free_descriptors.empty()) {
                return invalid(std::format("At most {} textures can exist at once",
                                           k_max_textures));
            }
        }

        // Each level's packed rows: made here, or given, one after another, in the pixels.
        std::vector<MipLevel> made;
        std::vector<std::span<const std::byte>> levels;
        if (generates_mipmaps(desc)) {
            made = make_mipmaps(desc.width, desc.height, pixels, is_srgb(desc.format));
            for (const MipLevel& level : made) {
                levels.emplace_back(level.pixels);
            }
        } else {
            std::size_t offset = 0;
            for (std::uint32_t level = 0; level < desc.mip_levels; ++level) {
                const std::size_t size = texture_level_bytes(
                    desc.format, mip_size(desc.width, level), mip_size(desc.height, level));
                levels.push_back(pixels.subspan(offset, size));
                offset += size;
            }
        }
        const auto level_count = static_cast<UINT>(levels.size());

        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = desc.width;
        texture_desc.Height = desc.height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = static_cast<UINT16>(level_count);
        texture_desc.Format = to_directx(desc.format);
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        TextureResource texture{.format = desc.format,
                                .size = {desc.width, desc.height},
                                .dynamic = desc.dynamic};
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        HRESULT result = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture.texture));
        if (FAILED(result)) {
            return directx_error("Creating a texture", result);
        }

        // The upload memory has each level's rows at the pitch the GPU wants.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(level_count);
        std::vector<UINT> rows(level_count);
        std::vector<UINT64> row_bytes(level_count);
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&texture_desc, 0, level_count, 0, footprints.data(),
                                        rows.data(), row_bytes.data(), &total);
        core::Result<UploadBuffer> staging = create_upload_buffer(total);
        if (!staging.has_value()) {
            return staging.error();
        }
        // A row here is a row of pixels, or of 4x4 blocks for a compressed format.
        for (UINT level = 0; level < level_count; ++level) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[level];
            const auto source_pitch = static_cast<std::size_t>(row_bytes[level]);
            for (UINT row = 0; row < rows[level]; ++row) {
                std::memcpy(staging.value().mapped + footprint.Offset +
                                std::size_t{row} * footprint.Footprint.RowPitch,
                            levels[level].data() + row * source_pitch, source_pitch);
            }
        }

        auto copied = run_upload([&](ID3D12GraphicsCommandList& list) {
            for (UINT level = 0; level < level_count; ++level) {
                D3D12_TEXTURE_COPY_LOCATION target{};
                target.pResource = texture.texture.Get();
                target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.SubresourceIndex = level;
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = staging.value().resource.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprints[level];
                list.CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            const D3D12_RESOURCE_BARRIER to_read =
                transition(texture.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, k_shader_read);
            list.ResourceBarrier(1, &to_read);
        });
        if (!copied.has_value()) {
            return copied.error();
        }

        texture.descriptor = m_free_descriptors.back();
        m_free_descriptors.pop_back();
        m_device->CreateShaderResourceView(texture.texture.Get(), nullptr,
                                           texture_cpu_descriptor(texture.descriptor));
        return texture;
    }

    // Waits in the shader-read state between the passes that draw into it.
    core::Result<TextureResource> create_render_target(const TextureDesc& desc) {
        if (!supports_render_target(desc.format)) {
            return invalid("This device can't draw into textures of this format");
        }
        if (m_free_descriptors.empty()) {
            release_retired();
            if (m_free_descriptors.empty()) {
                return invalid(std::format("At most {} textures can exist at once",
                                           k_max_textures));
            }
        }
        const bool depth = is_depth(desc.format);
        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = desc.width;
        texture_desc.Height = desc.height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = to_directx(desc.format);
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture_desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                   : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        TextureResource texture{.format = desc.format,
                                .size = {desc.width, desc.height},
                                .render_target = true};
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        HRESULT result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                           &texture_desc, k_shader_read, nullptr,
                                                           IID_PPV_ARGS(&texture.texture));
        if (FAILED(result)) {
            return directx_error("Creating a render target", result);
        }
        const D3D12_DESCRIPTOR_HEAP_DESC view_heap{
            .Type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            .NumDescriptors = 1};
        result = m_device->CreateDescriptorHeap(&view_heap, IID_PPV_ARGS(&texture.target_view));
        if (FAILED(result)) {
            return directx_error("Creating a render target's view", result);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE target_view =
            texture.target_view->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC shader_view{};
        shader_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shader_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shader_view.Texture2D.MipLevels = 1;
        if (depth) {
            D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
            depth_view.Format = DXGI_FORMAT_D32_FLOAT;
            depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_device->CreateDepthStencilView(texture.texture.Get(), &depth_view, target_view);
            shader_view.Format = DXGI_FORMAT_R32_FLOAT;
        } else {
            m_device->CreateRenderTargetView(texture.texture.Get(), nullptr, target_view);
            shader_view.Format = texture_desc.Format;
        }
        texture.descriptor = m_free_descriptors.back();
        m_free_descriptors.pop_back();
        m_device->CreateShaderResourceView(texture.texture.Get(), &shader_view,
                                           texture_cpu_descriptor(texture.descriptor));
        return texture;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE texture_cpu_descriptor(UINT slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_texture_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t{slot} * m_texture_descriptor_size;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_descriptor(UINT slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_texture_heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64{slot} * m_texture_descriptor_size;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE target_descriptor(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_target_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += std::size_t{index} * m_target_descriptor_size;
        return handle;
    }

    core::Result<void> create_targets() {
        for (UINT index = 0; index < k_back_buffer_count; ++index) {
            const HRESULT result =
                m_swapchain->GetBuffer(index, IID_PPV_ARGS(&m_back_buffers[index]));
            if (FAILED(result)) {
                destroy_targets();
                return directx_error("Getting the window's back buffers", result);
            }
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = k_render_target_format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(m_back_buffers[index].Get(), &view,
                                             target_descriptor(index));
        }
        const D3D12_RESOURCE_DESC back_buffer = m_back_buffers[0]->GetDesc();
        m_extent = platform::SurfaceSize{static_cast<std::uint32_t>(back_buffer.Width),
                                         back_buffer.Height};

        D3D12_RESOURCE_DESC depth{};
        depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth.Width = m_extent.width;
        depth.Height = m_extent.height;
        depth.DepthOrArraySize = 1;
        depth.MipLevels = 1;
        depth.Format = k_depth_format;
        depth.SampleDesc.Count = 1;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = k_depth_format;
        clear.DepthStencil.Depth = 1.0f;
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT result = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&m_depth_buffer));
        if (FAILED(result)) {
            destroy_targets();
            return directx_error("Creating the depth buffer", result);
        }
        m_device->CreateDepthStencilView(m_depth_buffer.Get(), nullptr,
                                         m_depth_heap->GetCPUDescriptorHandleForHeapStart());
        return {};
    }

    // Only once the GPU is done with them; callers wait first.
    void destroy_targets() {
        for (ComPtr<ID3D12Resource>& buffer : m_back_buffers) {
            buffer.Reset();
        }
        m_depth_buffer.Reset();
        m_extent = platform::SurfaceSize{};
    }

    core::Result<void> resize_targets() {
        wait_for_gpu();
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

    // Copies the frame about to be presented into memory the CPU can read; read_capture takes it
    // from there once the GPU is done.
    void record_capture(ID3D12Resource* back_buffer) {
        const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &m_capture_footprint, nullptr, nullptr,
                                        &total);
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
        const D3D12_RESOURCE_DESC buffer = buffer_desc(total);
        m_capture_buffer.Reset();
        const HRESULT result = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_capture_buffer));
        if (FAILED(result)) {
            DREAM_LOG_WARNING("Could not capture a frame: {}",
                              directx_error("Making readable memory", result).message);
            const D3D12_RESOURCE_BARRIER to_present = transition(
                back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            m_list->ResourceBarrier(1, &to_present);
            return;
        }

        const D3D12_RESOURCE_BARRIER to_copy = transition(
            back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = m_capture_buffer.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = m_capture_footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = back_buffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER to_present = transition(
            back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        m_list->ResourceBarrier(1, &to_present);
    }

    // Swaps blue and red, since the back buffer is BGRA.
    void read_capture() {
        if (!m_capture_buffer) {
            return;
        }
        const D3D12_SUBRESOURCE_FOOTPRINT& footprint = m_capture_footprint.Footprint;
        void* mapped = nullptr;
        if (const HRESULT result = m_capture_buffer->Map(0, nullptr, &mapped); FAILED(result)) {
            DREAM_LOG_WARNING("Could not capture a frame: {}",
                              directx_error("Reading the copy", result).message);
            m_capture_buffer.Reset();
            return;
        }
        CapturedFrame frame{.width = footprint.Width, .height = footprint.Height};
        frame.pixels.resize(std::size_t{footprint.Width} * footprint.Height * 4);
        for (UINT y = 0; y < footprint.Height; ++y) {
            const auto* const row = static_cast<const std::byte*>(mapped) +
                                    m_capture_footprint.Offset +
                                    std::size_t{y} * footprint.RowPitch;
            std::byte* const out = frame.pixels.data() + std::size_t{y} * footprint.Width * 4;
            for (UINT x = 0; x < footprint.Width; ++x) {
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = std::byte{255};
            }
        }
        const D3D12_RANGE nothing{0, 0};
        m_capture_buffer->Unmap(0, &nothing);
        m_capture_buffer.Reset();
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

    // Forwards what the debug layer has to say, since it has no callback of its own before
    // Windows 11.
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
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(get_debug_message(*m_info_queue.Get(), index, message, &length))) {
                continue;
            }
            std::string_view text(message->pDescription, message->DescriptionByteLength);
            while (!text.empty() && text.back() == '\0') {
                text.remove_suffix(1);
            }
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                DREAM_LOG_ERROR("Direct3D 12: {}", text);
            } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                DREAM_LOG_WARNING("Direct3D 12: {}", text);
            }
        }
        m_info_queue->ClearStoredMessages();
    }

    void record_frame_error(std::string_view action, HRESULT result) {
        if (!m_frame_error.has_value()) {
            m_frame_error = directx_error(action, result);
        }
    }

    // Writes the uniform block to this frame's upload memory and points the root signature at it.
    // Copies `data` into this frame's constant memory, keeping `reserved` bytes from where it
    // starts, and binds it at a root parameter.
    void upload_constants(std::span<const std::byte> data, UINT64 reserved, UINT root_parameter) {
        FrameSlot& frame = m_frames[m_frame_index];
        if (frame.constant_chunk < frame.constant_chunks.size() &&
            frame.constant_offset + reserved > k_constant_chunk_bytes) {
            ++frame.constant_chunk;
            frame.constant_offset = 0;
        }
        if (frame.constant_chunk == frame.constant_chunks.size()) {
            core::Result<UploadBuffer> chunk = create_upload_buffer(k_constant_chunk_bytes);
            if (!chunk.has_value()) {
                if (!m_frame_error.has_value()) {
                    m_frame_error = chunk.error();
                }
                return;
            }
            frame.constant_chunks.push_back(std::move(chunk).value());
            frame.constant_offset = 0;
        }
        const UploadBuffer& chunk = frame.constant_chunks[frame.constant_chunk];
        std::memcpy(chunk.mapped + frame.constant_offset, data.data(), data.size());
        m_list->SetGraphicsRootConstantBufferView(root_parameter,
                                                  chunk.address + frame.constant_offset);
        frame.constant_offset += reserved;
    }

    void upload_uniforms() {
        upload_constants(m_uniforms, k_constant_block_bytes, k_root_uniforms);
    }

    // Only what was last set is copied, but the block's whole size is kept, as shaders may read
    // all of it.
    void upload_draw_uniforms() {
        upload_constants(std::span(m_draw_uniforms).first(m_draw_uniform_size), k_draw_block_bytes,
                         k_root_draw_uniforms);
    }

    // Copies data into this frame's vertex memory, four-byte aligned. Returns the offset, or
    // nothing if it doesn't fit.
    std::optional<UINT> copy_to_frame(std::span<const std::byte> data) {
        const std::size_t offset = (m_frame_bytes_used + 3) & ~std::size_t{3};
        if (offset + data.size() > k_max_frame_vertex_bytes) {
            return std::nullopt;
        }
        if (!data.empty()) {
            std::memcpy(m_frames[m_frame_index].vertices.mapped + offset, data.data(), data.size());
        }
        m_frame_bytes_used = offset + data.size();
        return static_cast<UINT>(offset);
    }

    D3D12_GPU_VIRTUAL_ADDRESS frame_address(UINT offset) const {
        return m_frames[m_frame_index].vertices.address + offset;
    }

    // DirectX takes the vertex stride when the buffer is bound, and the stride belongs to the
    // pipeline, so the buffer is bound just before drawing, once both are known.
    void flush_vertex_buffer() {
        if (!m_vertex_view_dirty || !m_vertex_view.has_value()) {
            return;
        }
        m_vertex_view_dirty = false;
        D3D12_VERTEX_BUFFER_VIEW view = *m_vertex_view;
        view.StrideInBytes = m_bound_stride;
        m_list->IASetVertexBuffers(0, 1, &view);
    }

    // CommandList

    void begin_render_pass(const ClearColor& color) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        const D3D12_CPU_DESCRIPTOR_HANDLE target = target_descriptor(m_back_buffer);
        const D3D12_CPU_DESCRIPTOR_HANDLE depth =
            m_depth_heap->GetCPUDescriptorHandleForHeapStart();
        m_list->OMSetRenderTargets(1, &target, FALSE, &depth);
        const float clear[4] = {color.r, color.g, color.b, color.a};
        m_list->ClearRenderTargetView(target, clear, 0, nullptr);
        m_list->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        start_pass(m_extent, {}, {});
    }

    void begin_render_pass(const RenderTargets& targets) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        TextureResource* const color = m_textures.get(handle_cast<TextureResource>(targets.color));
        TextureResource* const depth = m_textures.get(handle_cast<TextureResource>(targets.depth));
        DREAM_VERIFY(color != nullptr || depth != nullptr, "A render pass needs a target");
        DREAM_VERIFY(color == nullptr || (color->render_target && !is_depth(color->format)),
                     "The color target must be a color render target");
        DREAM_VERIFY(depth == nullptr || (depth->render_target && is_depth(depth->format)),
                     "The depth target must be a depth render target");
        DREAM_VERIFY(color == nullptr || depth == nullptr || color->size == depth->size,
                     "A pass's targets must be the same size");

        // From waiting to be read to being drawn into, until end_render_pass turns them back.
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        UINT barrier_count = 0;
        if (color != nullptr) {
            barriers[barrier_count++] = transition(color->texture.Get(), k_shader_read,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (depth != nullptr) {
            barriers[barrier_count++] = transition(depth->texture.Get(), k_shader_read,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        m_list->ResourceBarrier(barrier_count, barriers.data());

        D3D12_CPU_DESCRIPTOR_HANDLE color_view{};
        D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
        if (color != nullptr) {
            color_view = color->target_view->GetCPUDescriptorHandleForHeapStart();
        }
        if (depth != nullptr) {
            depth_view = depth->target_view->GetCPUDescriptorHandleForHeapStart();
        }
        m_list->OMSetRenderTargets(color != nullptr ? 1 : 0,
                                   color != nullptr ? &color_view : nullptr, FALSE,
                                   depth != nullptr ? &depth_view : nullptr);
        if (color != nullptr && targets.clear_color.has_value()) {
            const ClearColor& value = *targets.clear_color;
            const float clear[4] = {value.r, value.g, value.b, value.a};
            m_list->ClearRenderTargetView(color_view, clear, 0, nullptr);
        }
        if (depth != nullptr && targets.clear_depth.has_value()) {
            m_list->ClearDepthStencilView(depth_view, D3D12_CLEAR_FLAG_DEPTH, *targets.clear_depth,
                                          0, 0, nullptr);
        }
        start_pass(color != nullptr ? color->size : depth->size,
                   color != nullptr ? std::optional(color->format) : std::nullopt,
                   depth != nullptr ? std::optional(depth->format) : std::nullopt);
        m_pass_targets = {color != nullptr ? color->texture.Get() : nullptr,
                          depth != nullptr ? depth->texture.Get() : nullptr};
    }

    void start_pass(platform::SurfaceSize size, std::optional<TextureFormat> color,
                    std::optional<TextureFormat> depth) {
        m_in_pass = true;
        m_pass_size = size;
        m_pass_color = color;
        m_pass_depth = depth;
        m_pass_targets = {nullptr, nullptr};

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(size.width);
        viewport.Height = static_cast<float>(size.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_list->RSSetViewports(1, &viewport);
        set_scissor(std::nullopt);

        // The constants keep their last values, as Direct3D 11's buffers do, so a game that set them
        // once still finds them in later passes and frames.
        upload_uniforms();
        upload_draw_uniforms();
        m_list->SetGraphicsRoot32BitConstants(k_root_push_constants, k_push_constant_values,
                                              m_push_constants.data(), 0);
        for (std::uint32_t slot = 0; slot < k_texture_slots; ++slot) {
            bind_texture(slot, TextureHandle{});
        }
    }

    void end_render_pass() override {
        DREAM_VERIFY(m_in_pass, "No render pass to end");
        m_in_pass = false;
        // Ready for later passes to read.
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        UINT barrier_count = 0;
        if (m_pass_targets[0] != nullptr) {
            barriers[barrier_count++] = transition(
                m_pass_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, k_shader_read);
        }
        if (m_pass_targets[1] != nullptr) {
            barriers[barrier_count++] = transition(m_pass_targets[1],
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, k_shader_read);
        }
        if (barrier_count > 0) {
            m_list->ResourceBarrier(barrier_count, barriers.data());
        }
        m_pass_targets = {nullptr, nullptr};
    }

    void bind_pipeline(PipelineHandle handle) override {
        const PipelineResource* const resource =
            m_pipelines.get(handle_cast<PipelineResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a pipeline that doesn't exist");
        DREAM_VERIFY(resource->color_target == m_pass_color &&
                         resource->depth_target == m_pass_depth,
                     "The pipeline was made for other render targets than this pass has");

        m_list->SetPipelineState(resource->state.Get());
        m_list->IASetPrimitiveTopology(resource->topology);
        if (resource->vertex_stride != m_bound_stride) {
            m_bound_stride = resource->vertex_stride;
            m_vertex_view_dirty = true;
        }
        m_bound_instance_stride = resource->instance_stride;
    }

    void bind_vertex_buffer(BufferHandle handle) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        m_vertex_view = D3D12_VERTEX_BUFFER_VIEW{
            .BufferLocation = resource->buffer->GetGPUVirtualAddress(),
            .SizeInBytes = resource->size,
            .StrideInBytes = 0};
        m_vertex_view_dirty = true;
    }

    void bind_index_buffer(BufferHandle handle, IndexFormat format) override {
        const BufferResource* const resource = m_buffers.get(handle_cast<BufferResource>(handle));
        DREAM_VERIFY(resource != nullptr, "Binding a buffer that doesn't exist");
        const D3D12_INDEX_BUFFER_VIEW view{
            .BufferLocation = resource->buffer->GetGPUVirtualAddress(),
            .SizeInBytes = resource->size,
            .Format = format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        m_list->IASetIndexBuffer(&view);
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
        std::memcpy(m_push_constants.data(), data.data(), data.size());
        const auto values = static_cast<UINT>((data.size() + 3) / 4);
        if (values > 0) {
            m_list->SetGraphicsRoot32BitConstants(k_root_push_constants, values,
                                                  m_push_constants.data(), 0);
        }
    }

    void set_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_uniform_size, "Too many uniforms");
        std::memcpy(m_uniforms.data(), data.data(), data.size());
        upload_uniforms();
    }

    void update_texture(TextureHandle handle, std::uint32_t first_row,
                        std::span<const std::byte> rows) override {
        TextureResource* const texture = m_textures.get(handle_cast<TextureResource>(handle));
        DREAM_VERIFY(texture != nullptr && texture->dynamic, "Updating a texture that isn't dynamic");
        const std::size_t row_bytes = texture_level_bytes(texture->format, texture->size.width, 1);
        const std::size_t row_count = rows.size() / row_bytes;
        DREAM_VERIFY(row_count * row_bytes == rows.size() &&
                         first_row + row_count <= texture->size.height,
                     "update_texture takes whole rows within the texture");
        if (row_count == 0) {
            return;
        }

        // Into this frame's upload memory, each row at the pitch copies need, from a placement
        // they accept.
        const UINT64 pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
                             D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        const UINT64 bytes = pitch * row_count;
        FrameSlot& frame = m_frames[m_frame_index];
        UINT64 offset = (frame.constant_offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        DREAM_VERIFY(bytes <= k_constant_chunk_bytes, "A texture update larger than 1 MB");
        if (frame.constant_chunk < frame.constant_chunks.size() &&
            offset + bytes > k_constant_chunk_bytes) {
            ++frame.constant_chunk;
            offset = 0;
        }
        if (frame.constant_chunk == frame.constant_chunks.size()) {
            core::Result<UploadBuffer> chunk = create_upload_buffer(k_constant_chunk_bytes);
            if (!chunk.has_value()) {
                if (!m_frame_error.has_value()) {
                    m_frame_error = chunk.error();
                }
                return;
            }
            frame.constant_chunks.push_back(std::move(chunk).value());
            offset = 0;
        }
        const UploadBuffer& chunk = frame.constant_chunks[frame.constant_chunk];
        for (std::size_t row = 0; row < row_count; ++row) {
            std::memcpy(chunk.mapped + offset + row * pitch, rows.data() + row * row_bytes,
                        row_bytes);
        }
        frame.constant_offset = offset + bytes;

        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = texture->texture.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = chunk.resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = offset;
        source.PlacedFootprint.Footprint = D3D12_SUBRESOURCE_FOOTPRINT{
            .Format = to_directx(texture->format),
            .Width = texture->size.width,
            .Height = static_cast<UINT>(row_count),
            .Depth = 1,
            .RowPitch = static_cast<UINT>(pitch)};
        const D3D12_RESOURCE_BARRIER to_copy =
            transition(texture->texture.Get(), k_shader_read, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->ResourceBarrier(1, &to_copy);
        m_list->CopyTextureRegion(&target, 0, first_row, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER to_read =
            transition(texture->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, k_shader_read);
        m_list->ResourceBarrier(1, &to_read);
    }

    void set_draw_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_draw_uniform_size, "Too many per-draw uniforms");
        std::memcpy(m_draw_uniforms.data(), data.data(), data.size());
        m_draw_uniform_size = data.size();
        upload_draw_uniforms();
    }

    void bind_texture(std::uint32_t slot, TextureHandle handle) override {
        DREAM_VERIFY(slot < k_texture_slots, "There is no such texture slot");
        UINT descriptor = slot == k_shadow_slot ? m_far_depth_texture.descriptor
                                                : m_white_texture.descriptor;
        if (handle.is_valid()) {
            const TextureResource* const resource =
                m_textures.get(handle_cast<TextureResource>(handle));
            DREAM_VERIFY(resource != nullptr, "Binding a texture that doesn't exist");
            DREAM_VERIFY((slot == k_shadow_slot) == is_depth(resource->format),
                         "The shadow slot takes depth textures, and only it");
            DREAM_VERIFY(resource->texture.Get() != m_pass_targets[0] &&
                             resource->texture.Get() != m_pass_targets[1],
                         "A pass can't read the textures it draws into");
            descriptor = resource->descriptor;
        }
        m_list->SetGraphicsRootDescriptorTable(k_root_texture + slot,
                                               texture_gpu_descriptor(descriptor));
    }

    void draw_indexed(std::uint32_t index_count) override {
        draw_indexed_range(0, index_count);
    }

    void draw_indexed_range(std::uint32_t first_index, std::uint32_t index_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        flush_vertex_buffer();
        m_list->DrawIndexedInstanced(index_count, 1, first_index, 0, 0);
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
        m_vertex_view = D3D12_VERTEX_BUFFER_VIEW{.BufferLocation = frame_address(*offset),
                                                 .SizeInBytes = static_cast<UINT>(vertices.size()),
                                                 .StrideInBytes = 0};
        m_vertex_view_dirty = true;
        flush_vertex_buffer();
        m_list->DrawInstanced(vertex_count, 1, 0, 0);
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
        m_vertex_view = D3D12_VERTEX_BUFFER_VIEW{.BufferLocation = frame_address(*vertex_offset),
                                                 .SizeInBytes = static_cast<UINT>(vertices.size()),
                                                 .StrideInBytes = 0};
        m_vertex_view_dirty = true;
        const D3D12_INDEX_BUFFER_VIEW view{
            .BufferLocation = frame_address(*index_offset),
            .SizeInBytes = static_cast<UINT>(indices.size_bytes()),
            .Format = DXGI_FORMAT_R32_UINT};
        m_list->IASetIndexBuffer(&view);
        return true;
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        DREAM_VERIFY(m_in_pass, "Binding instances outside a render pass");
        const std::optional<UINT> offset = copy_to_frame(instances);
        if (!offset.has_value()) {
            return false;
        }
        m_instance_view = D3D12_VERTEX_BUFFER_VIEW{.BufferLocation = frame_address(*offset),
                                                   .SizeInBytes = static_cast<UINT>(instances.size()),
                                                   .StrideInBytes = 0};
        return true;
    }

    void draw_indexed_instanced(std::uint32_t first_index, std::uint32_t index_count,
                                std::uint32_t instance_count) override {
        DREAM_VERIFY(m_in_pass, "Drawing outside a render pass");
        DREAM_VERIFY(m_bound_instance_stride > 0 && m_instance_view.has_value(),
                     "Drawing instances without an instanced pipeline and instances");
        flush_vertex_buffer();
        // Set with each draw, since the stride comes from the pipeline bound now.
        D3D12_VERTEX_BUFFER_VIEW view = *m_instance_view;
        view.StrideInBytes = m_bound_instance_stride;
        m_list->IASetVertexBuffers(1, 1, &view);
        m_list->DrawIndexedInstanced(index_count, instance_count, first_index, 0, 0);
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
        const D3D12_RECT scissor{static_cast<LONG>(left), static_cast<LONG>(top),
                                 static_cast<LONG>(right), static_cast<LONG>(bottom)};
        m_list->RSSetScissorRects(1, &scissor);
    }

    // Released in reverse order, so the device outlives everything made from it, and the adapter
    // outlives the device.
    ComPtr<IDXGIAdapter3> m_adapter;
    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12InfoQueue> m_info_queue;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fence_value = 0;

    // Closes the event handle last, after the members above.
    struct EventHandle {
        HANDLE handle = nullptr;
        EventHandle& operator=(HANDLE value) {
            handle = value;
            return *this;
        }
        operator HANDLE() const {
            return handle;
        }
        bool operator==(std::nullptr_t) const {
            return handle == nullptr;
        }
        EventHandle() = default;
        EventHandle(const EventHandle&) = delete;
        EventHandle& operator=(const EventHandle&) = delete;
        ~EventHandle() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    EventHandle m_fence_event;

    ComPtr<ID3D12RootSignature> m_root_signature;
    ComPtr<ID3D12DescriptorHeap> m_texture_heap;
    ComPtr<ID3D12DescriptorHeap> m_target_heap;
    ComPtr<ID3D12DescriptorHeap> m_depth_heap;
    UINT m_texture_descriptor_size = 0;
    UINT m_target_descriptor_size = 0;
    std::vector<UINT> m_free_descriptors;
    std::deque<Retired> m_retired;

    std::array<FrameSlot, k_frames_in_flight> m_frames;
    std::size_t m_frame_index = 0;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12CommandAllocator> m_upload_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_upload_list;
    TextureResource m_white_texture;
    TextureResource m_far_depth_texture;

    ComPtr<IDXGISwapChain3> m_swapchain;
    std::array<ComPtr<ID3D12Resource>, k_back_buffer_count> m_back_buffers;
    ComPtr<ID3D12Resource> m_depth_buffer;
    UINT m_back_buffer = 0;
    platform::SurfaceSize m_extent;
    platform::SurfaceSize m_requested_size;
    bool m_was_attached = false;
    bool m_needs_resize = false;

    std::optional<CapturedFrame> m_captured;
    bool m_capture_requested = false;
    ComPtr<ID3D12Resource> m_capture_buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_capture_footprint{};

    bool m_in_frame = false;
    bool m_in_pass = false;
    platform::SurfaceSize m_pass_size;
    std::optional<TextureFormat> m_pass_color;
    std::optional<TextureFormat> m_pass_depth;
    std::array<ID3D12Resource*, 2> m_pass_targets{};
    std::optional<core::Error> m_frame_error;
    std::size_t m_frame_bytes_used = 0;
    std::optional<D3D12_VERTEX_BUFFER_VIEW> m_vertex_view;
    bool m_vertex_view_dirty = true;
    UINT m_bound_stride = 0;
    UINT m_bound_instance_stride = 0;
    std::optional<D3D12_VERTEX_BUFFER_VIEW> m_instance_view;
    std::array<std::byte, k_constant_block_bytes> m_uniforms{};
    std::array<std::byte, k_max_draw_uniform_size> m_draw_uniforms{};
    std::size_t m_draw_uniform_size = 0;
    std::array<std::byte, k_max_push_constant_size> m_push_constants{};

    core::HandlePool<TextureResource> m_textures;
    core::HandlePool<BufferResource> m_buffers;
    core::HandlePool<ShaderResource> m_shaders;
    core::HandlePool<PipelineResource> m_pipelines;

    ComPtr<ID3D12QueryHeap> m_timestamps;
    ComPtr<ID3D12Resource> m_timestamp_readback;
    UINT64 m_timestamp_frequency = 0;
    bool m_timing_available = false;
    std::deque<TimedFrame> m_timed_frames;
    std::deque<double> m_gpu_frame_seconds;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& config) {
    auto device = std::make_unique<DirectX12Device>();
    if (core::Result<void> result = device->initialize(config); !result.has_value()) {
        return result.error();
    }
    return std::unique_ptr<Device>{std::move(device)};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_directx12(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "directx12", .create = &create});
}

} // namespace dream::modules
