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
#include <vector>

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
inline constexpr std::size_t k_max_uniform_size = 512;

// The largest block accepted by CommandList::set_draw_uniforms (ADR 0036): 16 KB, the most every
// OpenGL ES 3 and Vulkan device takes in one uniform block.
inline constexpr std::size_t k_max_draw_uniform_size = 16 * 1024;

// Where shaders find the block of set_draw_uniforms.
inline constexpr std::uint32_t k_draw_uniform_binding = 8;

// How many bytes CommandList::draw_vertices, bind_frame_geometry and bind_instances can take in one
// frame, over all their calls.
inline constexpr std::size_t k_max_frame_vertex_bytes = 4 * 1024 * 1024;

enum class BufferUsage : std::uint8_t { Vertex, Index };

// Srgb is for colors, and Unorm for data such as normal maps. Rgba8 is four bytes per pixel. The
// compressed formats store each block of 4x4 pixels in 16 bytes, with blocks at the right and
// bottom edges covering pixels past them: BC7 is the PC's, and ASTC 4x4 the phone's. Only the Rgba8
// formats work everywhere; see Device::supports_texture_format.
//
// The last three are for render targets only (TextureDesc::render_target): Rgba16Float is eight
// bytes per pixel of half floats, Rg11B10Float four bytes of red, green and blue as small floats
// with no alpha, both for colors brighter than 1, and Depth32Float a depth buffer that shaders can
// read. See Device::supports_render_target.
//
// Rgba32Float is sixteen bytes per pixel of full floats, for data that shaders read texel by texel,
// such as lights (ADR 0037), never filtered: Slang's Texture2D.Load, or texelFetch. Every device
// takes it.
enum class TextureFormat : std::uint8_t {
    Rgba8Srgb,
    Rgba8Unorm,
    Bc7Srgb,
    Bc7Unorm,
    Astc4x4Srgb,
    Astc4x4Unorm,
    Rgba16Float,
    Rg11B10Float,
    Depth32Float,
    Rgba32Float,
};

[[nodiscard]] constexpr bool is_compressed(TextureFormat format) noexcept {
    return format == TextureFormat::Bc7Srgb || format == TextureFormat::Bc7Unorm ||
           format == TextureFormat::Astc4x4Srgb || format == TextureFormat::Astc4x4Unorm;
}

[[nodiscard]] constexpr bool is_depth(TextureFormat format) noexcept {
    return format == TextureFormat::Depth32Float;
}

// The formats a texture can have only as a render target, never filled with pixels.
[[nodiscard]] constexpr bool is_render_target_only(TextureFormat format) noexcept {
    return format == TextureFormat::Rgba16Float || format == TextureFormat::Rg11B10Float ||
           format == TextureFormat::Depth32Float;
}

[[nodiscard]] constexpr bool is_srgb(TextureFormat format) noexcept {
    return format == TextureFormat::Rgba8Srgb || format == TextureFormat::Bc7Srgb ||
           format == TextureFormat::Astc4x4Srgb;
}

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8Srgb;

    // How many levels the pixels hold, the full size first and each next one half as wide and
    // high, rounded down, but at least 1.
    std::uint32_t mip_levels = 1;

    // The device makes the rest of the chain itself. Only for the Rgba8 formats, with one level.
    bool generate_mipmaps = true;

    // A texture to draw into with CommandList::begin_render_pass, then read in shaders. It has one
    // level and no pixels to start with: its contents are undefined until a pass clears or draws
    // into it. A depth format makes it a depth target, and any other a color target.
    bool render_target = false;

    // A texture whose rows CommandList::update_texture replaces, as often as every frame
    // (ADR 0037). One level, no mipmaps made, and never a render target.
    bool dynamic = false;
};

// The size of one level of a texture, in pixels.
[[nodiscard]] constexpr std::uint32_t mip_size(std::uint32_t full, std::uint32_t level) noexcept {
    const std::uint32_t size = level < 32 ? full >> level : 0;
    return size > 0 ? size : 1;
}

// Levels down to 1x1.
[[nodiscard]] constexpr std::uint32_t full_mip_chain(std::uint32_t width,
                                                     std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    for (std::uint32_t size = width > height ? width : height; size > 1; size /= 2) {
        ++levels;
    }
    return levels;
}

// Bytes in one level of a texture, rows packed with nothing between them.
[[nodiscard]] constexpr std::size_t texture_level_bytes(TextureFormat format, std::uint32_t width,
                                                        std::uint32_t height) noexcept {
    if (is_compressed(format)) {
        return std::size_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
    }
    const std::size_t pixel_bytes = format == TextureFormat::Rgba32Float   ? 16
                                    : format == TextureFormat::Rgba16Float ? 8
                                                                           : 4;
    return std::size_t{width} * height * pixel_bytes;
}

// The bytes the pixels of a texture with `desc` take, every level after the one before.
[[nodiscard]] constexpr std::size_t texture_bytes(const TextureDesc& desc) noexcept {
    std::size_t total = 0;
    for (std::uint32_t level = 0; level < desc.mip_levels; ++level) {
        total += texture_level_bytes(desc.format, mip_size(desc.width, level),
                                     mip_size(desc.height, level));
    }
    return total;
}

// What is wrong with a texture's description and pixels, if anything, as every backend checks it.
// Compressed textures must be a whole number of blocks wide and high, which Direct3D requires.
[[nodiscard]] constexpr std::optional<std::string_view>
texture_problem(const TextureDesc& desc, std::size_t pixel_bytes) noexcept {
    if (desc.width == 0 || desc.height == 0) {
        return "A texture can't be empty";
    }
    if (desc.render_target) {
        if (is_compressed(desc.format)) {
            return "A render target can't be compressed";
        }
        if (desc.mip_levels != 1) {
            return "A render target has one mip level";
        }
        if (pixel_bytes != 0) {
            return "A render target starts with no pixels";
        }
        return std::nullopt;
    }
    if (is_render_target_only(desc.format)) {
        return "This format is only for render targets";
    }
    if (desc.dynamic && (desc.mip_levels != 1 || is_compressed(desc.format))) {
        return "A dynamic texture has one level and isn't compressed";
    }
    if (desc.mip_levels == 0 || desc.mip_levels > full_mip_chain(desc.width, desc.height)) {
        return "A texture's mip levels must be from 1 down to 1x1 at most";
    }
    if (is_compressed(desc.format) && (desc.width % 4 != 0 || desc.height % 4 != 0)) {
        return "A compressed texture's width and height must be multiples of 4";
    }
    if (pixel_bytes != texture_bytes(desc)) {
        return "The pixels don't match the texture's size";
    }
    return std::nullopt;
}

// Whether the device makes a texture's mipmaps itself.
[[nodiscard]] constexpr bool generates_mipmaps(const TextureDesc& desc) noexcept {
    return desc.generate_mipmaps && !desc.render_target && !desc.dynamic && desc.mip_levels == 1 &&
           !is_compressed(desc.format) && !is_render_target_only(desc.format) &&
           desc.format != TextureFormat::Rgba32Float && (desc.width > 1 || desc.height > 1);
}

enum class ShaderStage : std::uint8_t { Vertex, Fragment };
enum class IndexFormat : std::uint8_t { UInt16, UInt32 };
// UByte4Norm is four bytes read as numbers from 0 to 1, such as an RGBA color.
enum class VertexFormat : std::uint8_t { Float2, Float3, Float4, UByte4Norm };

// Lines draws each pair of vertices as a line one pixel wide.
enum class PrimitiveTopology : std::uint8_t { Triangles, Lines };

// Which faces aren't drawn. Front faces are counter-clockwise, as seen in the engine's +Y-up clip
// space. Shadow maps often cull the front faces, so the back ones cast the shadow.
enum class CullMode : std::uint8_t { None, Back, Front };

// How many textures a draw can read, each at a slot with its own sampler (ADR 0027, ADR 0033):
//
// | Slot | Sampler | Vulkan | DirectX | OpenGL |
// |---|---|---|---|---|
// | 0 | linear, mipmaps, repeating | set 0, binding 1 | t0, s0 | texture unit 1 |
// | 1 | linear, clamped to the edge | set 0, binding 2 | t1, s1 | texture unit 2 |
// | 2 | depth comparison, less or equal, linear, clamped | set 0, binding 3 | t2, s2 | texture unit 3 |
// | 3 to 6 | linear, mipmaps, repeating | set 0, bindings 4 to 7 | t3 to t6, s3 to s6 | texture units 4 to 7 |
//
// Slot 2 takes a Depth32Float texture, read with a shader's shadow sampler: Sampler2DShadow in
// Slang. Slots 3 to 6 are for a material's other textures, such as its normal map.
inline constexpr std::uint32_t k_texture_slots = 7;
inline constexpr std::uint32_t k_shadow_slot = 2;

// How a slot's sampler reads its texture.
enum class SlotSampler : std::uint8_t { Repeat, Clamp, ShadowCompare };

[[nodiscard]] constexpr SlotSampler slot_sampler(std::uint32_t slot) noexcept {
    if (slot == k_shadow_slot) {
        return SlotSampler::ShadowCompare;
    }
    return slot == 1 ? SlotSampler::Clamp : SlotSampler::Repeat;
}

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
// input the shader declares is location n (ADR 0013). Per-instance attributes come after the
// per-vertex ones, in locations of their own.
struct VertexAttribute {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::Float3;
    std::uint32_t offset = 0;
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct PipelineDesc {
    ShaderHandle vertex_shader;
    // May be left invalid for a pipeline that only writes depth, with a depth_target and no
    // color_target.
    ShaderHandle fragment_shader;
    std::uint32_t vertex_stride = 0;
    std::span<const VertexAttribute> attributes;

    // Attributes read once per instance, from the data CommandList::bind_instances binds, for
    // draw_indexed_instanced (ADR 0032). None for a pipeline that isn't instanced.
    std::uint32_t instance_stride = 0;
    std::span<const VertexAttribute> instance_attributes;

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    BlendMode blend = BlendMode::Opaque;
    bool depth_test = true;

    // Whether drawing updates the depth buffer. Ignored without depth_test.
    bool depth_write = true;

    CullMode cull = CullMode::Back;

    // Pushes depth away from the viewer as it is written, against shadow maps shadowing their own
    // surfaces: a constant, in the smallest steps the depth format has, and an amount that grows
    // with the slope of the triangle.
    float depth_bias = 0.0f;
    float slope_depth_bias = 0.0f;

    // The formats of the render targets the pipeline draws into, in the passes it is used in. Both
    // nothing means the window's. A pipeline can only be used in passes with these targets.
    std::optional<TextureFormat> color_target;
    std::optional<TextureFormat> depth_target;
};

// What is wrong with a pipeline's targets and stages, if anything, as every backend checks it.
[[nodiscard]] constexpr std::optional<std::string_view>
pipeline_problem(const PipelineDesc& desc) noexcept {
    if (!desc.vertex_shader.is_valid()) {
        return "A pipeline needs a vertex shader";
    }
    if (desc.color_target.has_value() &&
        (is_compressed(*desc.color_target) || is_depth(*desc.color_target))) {
        return "A pipeline's color target must be a color format that can be drawn into";
    }
    if (desc.depth_target.has_value() && !is_depth(*desc.depth_target)) {
        return "A pipeline's depth target must be a depth format";
    }
    if (desc.instance_attributes.empty() != (desc.instance_stride == 0)) {
        return "A pipeline's per-instance attributes and their stride go together";
    }
    if (!desc.fragment_shader.is_valid() &&
        (desc.color_target.has_value() || !desc.depth_target.has_value())) {
        return "Only a pipeline that draws into a depth target alone can leave out its fragment "
               "shader";
    }
    return std::nullopt;
}

// The textures a render pass draws into, instead of the window. At least one, both the same size,
// and render targets of the formats their names say. A target is read as a texture only in later
// passes, never in its own.
struct RenderTargets {
    TextureHandle color;
    TextureHandle depth;

    // What the targets start the pass with: cleared to these values, or nothing to keep what they
    // had.
    std::optional<ClearColor> clear_color = ClearColor{};
    std::optional<float> clear_depth = 1.0f;
};

// Records the drawing for one frame. Valid only between Device::begin_frame and end_frame.
class CommandList {
public:
    // Starts drawing to the window, clearing it to `color` and clearing depth.
    virtual void begin_render_pass(const ClearColor& color) = 0;

    // Starts drawing into textures. Viewport and scissor cover them, and their pixels are
    // addressed from the top left, as the window's are.
    virtual void begin_render_pass(const RenderTargets& targets) = 0;
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

    // Sets a second block, for data that changes from draw to draw, such as a skinned part's joint
    // matrices (ADR 0036): binding 8 of set 0, register b2, or uniform block binding 8 in OpenGL.
    // At most k_max_draw_uniform_size bytes, copied, for this and later draws. A shader may
    // declare the block as large as k_max_draw_uniform_size; what wasn't set reads as anything.
    virtual void set_draw_uniforms(std::span<const std::byte> data) = 0;

    // Replaces rows of a dynamic texture (TextureDesc::dynamic), from `first_row` down, with
    // `rows`: whole rows, packed. Only between render passes; draws in later passes read the new
    // rows, and draws before it the old ones (ADR 0037).
    virtual void update_texture(TextureHandle texture, std::uint32_t first_row,
                                std::span<const std::byte> rows) = 0;

    // Sets the texture that shaders read at `slot`, below k_texture_slots, for this and later draws
    // in the pass. Each pass starts with defaults, which an invalid handle also selects: plain
    // white for every color slot, and a depth at the far plane for the shadow slot, so every
    // comparison passes.
    virtual void bind_texture(std::uint32_t slot, TextureHandle texture) = 0;

    // Slot 0, which most shaders read: binding 1 of set 0, or registers t0 and s0.
    void bind_texture(TextureHandle texture) {
        bind_texture(0, texture);
    }

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

    // Copies one block of per-instance data for each instance into the same per-frame memory as
    // draw_vertices, and binds it for the instanced pipeline's per-instance attributes. Returns
    // false, binding nothing, if it doesn't fit.
    virtual bool bind_instances(std::span<const std::byte> instances) = 0;

    // Draws part of the bound index buffer once for each of `instance_count` instances, which read
    // their attributes from the data bind_instances bound last.
    virtual void draw_indexed_instanced(std::uint32_t first_index, std::uint32_t index_count,
                                        std::uint32_t instance_count) = 0;

    // Limits drawing to a rectangle, or with nullopt to the whole surface, which is also how each
    // render pass starts.
    virtual void set_scissor(std::optional<ScissorRect> rect) = 0;

protected:
    ~CommandList() = default;
};

// Video memory, as a backend that can tell reports it.
struct MemoryUsage {
    // What this program uses in the GPU's own memory.
    std::uint64_t used_bytes = 0;
    // How much the system lets it use before performance suffers, as other programs need some too.
    // Zero when unknown.
    std::uint64_t budget_bytes = 0;
};

// A finished frame read back from the GPU: width * height pixels of four bytes, red, green, blue
// and alpha, row by row from the top, with colors as the screen shows them. Alpha is always 255.
struct CapturedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> pixels;
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

    // `pixels` holds each level, row by row from the top of the image, texture_bytes(desc) in all.
    // The contents are fixed at creation. A format the device doesn't support is an error.
    [[nodiscard]] virtual core::Result<TextureHandle>
    create_texture(const TextureDesc& desc, std::span<const std::byte> pixels) = 0;

    // Whether create_texture takes textures of `format`. The Rgba8 formats always work.
    [[nodiscard]] virtual bool supports_texture_format(TextureFormat format) const {
        return !is_compressed(format) && !is_render_target_only(format);
    }

    // Whether create_texture takes render targets of `format`, which shaders can also sample with
    // linear filtering, or for Depth32Float, compare. False for the compressed formats.
    [[nodiscard]] virtual bool supports_render_target(TextureFormat format) const {
        return format == TextureFormat::Rgba8Srgb || format == TextureFormat::Rgba8Unorm;
    }

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

    // An optional capability, for profiling: nothing when the backend can't tell, which is the
    // default. It may ask the driver, so call it a few times a second at most, outside a frame.
    [[nodiscard]] virtual std::optional<MemoryUsage> memory_usage() {
        return std::nullopt;
    }

    // An optional capability, for profiling: how long the GPU spent on a finished frame, from the
    // start of the frame's commands to their end, in seconds. Each frame's time is given once,
    // oldest first, and a few frames late, since the GPU runs behind the CPU. Nothing means no new
    // time yet, and always for a backend that can't measure, which is the default. Call it after
    // end_frame until it returns nothing. Backends keep only the last few times nobody took.
    [[nodiscard]] virtual std::optional<double> take_gpu_frame_seconds() {
        return std::nullopt;
    }

    // An optional capability, for screenshot tests: reads back the next frame that is drawn and
    // presented, for take_captured_frame. It waits for the GPU, so it is slow. False for a backend
    // that can't, which is the default.
    [[nodiscard]] virtual bool capture_next_frame() {
        return false;
    }

    // The frame capture_next_frame asked for, once it has been presented, given once.
    [[nodiscard]] virtual std::optional<CapturedFrame> take_captured_frame() {
        return std::nullopt;
    }
};

} // namespace dream::rhi
