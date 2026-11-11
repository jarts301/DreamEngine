#pragma once

#include "dream/core/handle.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/platform/native_window.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace dream::rhi {

// The graphics API, reduced to what the engine needs. It is deliberately small, and grows only when
// a feature has a user and can be implemented on every backend.

struct BufferTag;
struct ShaderTag;
struct PipelineTag;
struct TextureTag;

using BufferHandle = core::Handle<BufferTag>;
using ShaderHandle = core::Handle<ShaderTag>;
using PipelineHandle = core::Handle<PipelineTag>;
using TextureHandle = core::Handle<TextureTag>;

// For backends, which keep their own resource types in a core::HandlePool.
template <typename To, typename From>
[[nodiscard]] constexpr core::Handle<To> handle_cast(core::Handle<From> handle) noexcept {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

// The guaranteed minimum on all supported hardware.
inline constexpr std::size_t k_max_push_constant_size = 128;

// The largest block accepted by CommandList::set_uniforms.
inline constexpr std::size_t k_max_uniform_size = 256;

// How many bytes CommandList::draw_vertices can take in one frame, over all its calls.
inline constexpr std::size_t k_max_frame_vertex_bytes = 2 * 1024 * 1024;

enum class BufferUsage : std::uint8_t { Vertex, Index };

// Both are four bytes per pixel. Srgb is for colors, and Unorm is for data such as normal maps.
enum class TextureFormat : std::uint8_t { Rgba8Srgb, Rgba8Unorm };

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    bool generate_mipmaps = true;
};

enum class ShaderStage : std::uint8_t { Vertex, Fragment };
enum class IndexFormat : std::uint8_t { UInt16, UInt32 };
// UByte4Norm is four bytes read as numbers from 0 to 1, such as an RGBA color.
enum class VertexFormat : std::uint8_t { Float2, Float3, Float4, UByte4Norm };

// Lines draws each pair of vertices as a line one pixel wide.
enum class PrimitiveTopology : std::uint8_t { Triangles, Lines };

// How drawn colors combine with what is already there. Alpha uses the color's alpha as its
// opacity; PremultipliedAlpha expects colors already multiplied by their alpha.
enum class BlendMode : std::uint8_t { Opaque, Alpha, PremultipliedAlpha };

// In pixels from the top-left corner of the surface.
struct ScissorRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Matched to the vertex shader's inputs by location. DirectX has no locations, so there the n-th
// input the shader declares is location n (ADR 0013).
struct VertexAttribute {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::Float3;
    std::uint32_t offset = 0;
};

struct PipelineDesc {
    ShaderHandle vertex_shader;
    ShaderHandle fragment_shader;
    std::uint32_t vertex_stride = 0;
    std::span<const VertexAttribute> attributes;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    BlendMode blend = BlendMode::Opaque;
    bool depth_test = true;

    // Whether drawing updates the depth buffer. Ignored without depth_test.
    bool depth_write = true;

    // Front faces are counter-clockwise, as seen in the engine's +Y-up clip space.
    bool cull_back_faces = true;
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// Records the drawing for one frame. Valid only between Device::begin_frame and end_frame.
class CommandList {
public:
    // Starts drawing to the window, clearing it to `color` and clearing depth.
    virtual void begin_render_pass(const ClearColor& color) = 0;
    virtual void end_render_pass() = 0;

    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void bind_vertex_buffer(BufferHandle buffer) = 0;
    virtual void bind_index_buffer(BufferHandle buffer, IndexFormat format) = 0;

    // At most k_max_push_constant_size bytes, visible to every shader stage. Shaders read them
    // from the push constant block on Vulkan and register b1 on DirectX (ADR 0013).
    virtual void push_constants(std::span<const std::byte> data) = 0;

    // Sets the block that shaders read at binding 0 of set 0, or register b0, for this and later
    // draws. The data is copied, and it can change any number of times per frame. At most
    // k_max_uniform_size bytes.
    virtual void set_uniforms(std::span<const std::byte> data) = 0;

    // Sets the texture that shaders read at binding 1 of set 0, or registers t0 and s0, for this
    // and later draws. Each frame starts with a plain white texture, which an invalid handle also
    // selects.
    virtual void bind_texture(TextureHandle texture) = 0;

    virtual void draw_indexed(std::uint32_t index_count) = 0;

    // Draws part of the bound index buffer.
    virtual void draw_indexed_range(std::uint32_t first_index, std::uint32_t index_count) = 0;

    // Copies `vertices` into memory that lasts until the frame ends, binds it in place of the vertex
    // buffer and draws it without indices. For data that changes every frame, such as debug lines.
    // All calls in a frame share k_max_frame_vertex_bytes; a call that would go over it draws
    // nothing and returns false.
    virtual bool draw_vertices(std::span<const std::byte> vertices, std::uint32_t vertex_count) = 0;

    // Copies vertices and 32-bit indices into the same per-frame memory as draw_vertices, and binds
    // them in place of the vertex and index buffers, for draw_indexed_range. Returns false, binding
    // nothing, if they don't fit.
    virtual bool bind_frame_geometry(std::span<const std::byte> vertices,
                                     std::span<const std::uint32_t> indices) = 0;

    // Limits drawing to a rectangle, or with nullopt to the whole surface, which is also how each
    // render pass starts.
    virtual void set_scissor(std::optional<ScissorRect> rect) = 0;

protected:
    ~CommandList() = default;
};

// A graphics device and the window surface it presents to.
//
// All functions must be called from one thread. Resources outlive the surface: only attach_surface
// and detach_surface deal with Android taking the surface away.
class Device {
public:
    static constexpr std::string_view k_module_interface = "rhi";

    virtual ~Device() = default;

    // Fails if the window can't be drawn to.
    [[nodiscard]] virtual core::Result<void> attach_surface(const platform::NativeWindow& window,
                                                            platform::SurfaceSize size) = 0;

    // Does nothing if no surface is attached.
    virtual void detach_surface() = 0;

    virtual void resize_surface(platform::SurfaceSize size) = 0;

    // Appended to a shader's asset name, because each graphics API has its own compiled format.
    [[nodiscard]] virtual std::string_view shader_file_suffix() const = 0;

    // The contents are fixed at creation.
    [[nodiscard]] virtual core::Result<BufferHandle>
    create_buffer(BufferUsage usage, std::span<const std::byte> contents) = 0;

    // `pixels` holds width * height pixels of four bytes, row by row from the top of the image.
    // The contents are fixed at creation.
    [[nodiscard]] virtual core::Result<TextureHandle>
    create_texture(const TextureDesc& desc, std::span<const std::byte> pixels) = 0;

    [[nodiscard]] virtual core::Result<ShaderHandle>
    create_shader(ShaderStage stage, std::span<const std::byte> code) = 0;

    // Needs a surface to have been attached at least once. It keeps working across later reattaches.
    [[nodiscard]] virtual core::Result<PipelineHandle>
    create_pipeline(const PipelineDesc& desc) = 0;

    // May wait for the GPU, so don't call these while a frame is being recorded. Handles that no
    // longer exist are ignored.
    virtual void destroy_buffer(BufferHandle buffer) = 0;
    virtual void destroy_texture(TextureHandle texture) = 0;
    virtual void destroy_shader(ShaderHandle shader) = 0;
    virtual void destroy_pipeline(PipelineHandle pipeline) = 0;

    // Returns the frame's command list, or null when there is nothing to draw to right now, in
    // which case skip the frame and don't call end_frame. An error means the device is unusable.
    [[nodiscard]] virtual core::Result<CommandList*> begin_frame() = 0;

    [[nodiscard]] virtual core::Result<void> end_frame() = 0;
};

} // namespace dream::rhi
