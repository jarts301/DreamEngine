#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/platform/web_canvas.h"
#include "dream/rhi/rhi.h"
#include "dream/rhi/software.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace rhi;

struct Buffer {
    std::vector<std::byte> contents;
};

struct Texture {
    TextureDesc desc;
    std::vector<std::byte> pixels;
};

struct Shader {};

// The targets a pipeline was made for, which the pass it is used in must have.
struct PipelineInfo {
    std::optional<TextureFormat> color;
    std::optional<TextureFormat> depth;
};

core::Error invalid(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::byte linear_to_srgb_byte(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float encoded =
        value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::byte>(static_cast<int>(encoded * 255.0f + 0.5f));
}

// Appends the rest of an RGBA8 texture's mipmap chain to `pixels`, each level the average of 2x2
// pixels of the one before, averaged in linear light for sRGB textures.
void generate_mipmaps(Texture& texture) {
    const bool srgb = is_srgb(texture.desc.format);
    std::array<float, 256> to_linear{};
    for (std::size_t value = 0; value < to_linear.size(); ++value) {
        const float unit = static_cast<float>(value) / 255.0f;
        to_linear[value] = srgb ? srgb_to_linear(unit) : unit;
    }
    const std::uint32_t levels = full_mip_chain(texture.desc.width, texture.desc.height);
    texture.pixels.resize(texture_bytes(TextureDesc{.width = texture.desc.width,
                                                    .height = texture.desc.height,
                                                    .format = texture.desc.format,
                                                    .mip_levels = levels}));
    std::size_t source_offset = 0;
    for (std::uint32_t level = 1; level < levels; ++level) {
        const std::uint32_t source_width = mip_size(texture.desc.width, level - 1);
        const std::uint32_t source_height = mip_size(texture.desc.height, level - 1);
        const std::uint32_t width = mip_size(texture.desc.width, level);
        const std::uint32_t height = mip_size(texture.desc.height, level);
        const std::size_t offset =
            source_offset + texture_level_bytes(texture.desc.format, source_width, source_height);
        const std::byte* const source = texture.pixels.data() + source_offset;
        std::byte* const target = texture.pixels.data() + offset;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::array<float, 4> sum{};
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = std::min(x * 2 + dx, source_width - 1);
                        const std::uint32_t sy = std::min(y * 2 + dy, source_height - 1);
                        const std::byte* const pixel =
                            source + (std::size_t{sy} * source_width + sx) * 4;
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            sum[channel] += to_linear[static_cast<std::size_t>(pixel[channel])];
                        }
                        sum[3] += static_cast<float>(pixel[3]) / 255.0f;
                    }
                }
                std::byte* const out = target + (std::size_t{y} * width + x) * 4;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float average = sum[channel] * 0.25f;
                    out[channel] = srgb ? linear_to_srgb_byte(average)
                                        : static_cast<std::byte>(
                                              static_cast<int>(average * 255.0f + 0.5f));
                }
                out[3] = static_cast<std::byte>(static_cast<int>(sum[3] * 0.25f * 255.0f + 0.5f));
            }
        }
        source_offset = offset;
    }
    texture.desc.mip_levels = levels;
}

// A device that keeps every resource in memory and draws on the CPU, for machines with no GPU
// (ADR 0039). It can't run shaders: a renderer made for it, such as the software renderer, reads
// resources through software_access() and writes the window's pixels itself. Draw commands are
// checked against the interface's rules, as the null device checks them, and otherwise ignored,
// so programs that draw with their own shaders show only their clear color.
//
// Finished frames go to the window's WebCanvas, if it has one, and can be captured for screenshot
// tests.
class SoftwareDevice final : public Device, private CommandList, private SoftwareAccess {
public:
    core::Result<void> attach_surface(const platform::NativeWindow& window,
                                      platform::SurfaceSize size) override {
        m_canvas = nullptr;
        if (const auto* const web = std::get_if<platform::WebWindow>(&window)) {
            m_canvas = static_cast<platform::WebCanvas*>(web->canvas);
        }
        m_has_surface = true;
        m_was_attached = true;
        resize_surface(size);
        return {};
    }

    void detach_surface() override {
        m_has_surface = false;
        m_canvas = nullptr;
    }

    void resize_surface(platform::SurfaceSize size) override {
        m_size = size;
        m_frame.assign(std::size_t{size.width} * size.height * 4, std::byte{0});
    }

    // No shaders are compiled for this device; any file is taken, and none is run.
    std::string_view shader_file_suffix() const override {
        return ".spv";
    }

    core::Result<BufferHandle> create_buffer(BufferUsage /*usage*/,
                                             std::span<const std::byte> contents) override {
        if (contents.empty()) {
            return invalid("A buffer can't be empty");
        }
        return handle_cast<BufferTag>(
            m_buffers.create(Buffer{std::vector<std::byte>(contents.begin(), contents.end())}));
    }

    core::Result<TextureHandle> create_texture(const TextureDesc& desc,
                                               std::span<const std::byte> pixels) override {
        if (const auto problem = texture_problem(desc, pixels.size())) {
            return invalid(*problem);
        }
        if (!supports_texture_format(desc.format) && !desc.render_target) {
            return invalid("The software device takes uncompressed textures only");
        }
        Texture texture{.desc = desc};
        if (desc.render_target) {
            texture.pixels.assign(texture_bytes(desc), std::byte{0});
        } else {
            texture.pixels.assign(pixels.begin(), pixels.end());
            if (generates_mipmaps(desc)) {
                generate_mipmaps(texture);
            }
        }
        return handle_cast<TextureTag>(m_textures.create(std::move(texture)));
    }

    bool supports_render_target(TextureFormat format) const override {
        return !is_compressed(format);
    }

    core::Result<ShaderHandle> create_shader(ShaderStage /*stage*/,
                                             std::span<const std::byte> code) override {
        if (code.empty()) {
            return invalid("A shader can't be empty");
        }
        return handle_cast<ShaderTag>(m_shaders.create());
    }

    core::Result<PipelineHandle> create_pipeline(const PipelineDesc& desc) override {
        if (!m_was_attached) {
            return invalid("A pipeline needs a surface to have been attached");
        }
        if (const auto problem = pipeline_problem(desc)) {
            return invalid(*problem);
        }
        if (!m_shaders.contains(handle_cast<Shader>(desc.vertex_shader)) ||
            (desc.fragment_shader.is_valid() &&
             !m_shaders.contains(handle_cast<Shader>(desc.fragment_shader)))) {
            return invalid("A pipeline needs valid shaders");
        }
        return handle_cast<PipelineTag>(
            m_pipelines.create(PipelineInfo{desc.color_target, desc.depth_target}));
    }

    void destroy_buffer(BufferHandle buffer) override {
        m_buffers.destroy(handle_cast<Buffer>(buffer));
    }

    void destroy_texture(TextureHandle texture) override {
        m_textures.destroy(handle_cast<Texture>(texture));
    }

    void destroy_shader(ShaderHandle shader) override {
        m_shaders.destroy(handle_cast<Shader>(shader));
    }

    void destroy_pipeline(PipelineHandle pipeline) override {
        m_pipelines.destroy(handle_cast<PipelineInfo>(pipeline));
    }

    core::Result<CommandList*> begin_frame() override {
        DREAM_VERIFY(!m_in_frame, "begin_frame called twice");
        if (!m_has_surface || m_size.width == 0 || m_size.height == 0) {
            return static_cast<CommandList*>(nullptr);
        }
        m_in_frame = true;
        m_frame_vertex_bytes = 0;
        return static_cast<CommandList*>(this);
    }

    core::Result<void> end_frame() override {
        DREAM_VERIFY(m_in_frame, "end_frame called without begin_frame");
        DREAM_VERIFY(!m_in_pass, "The render pass was not ended");
        m_in_frame = false;
        // The screen shows no transparency.
        for (std::size_t pixel = 3; pixel < m_frame.size(); pixel += 4) {
            m_frame[pixel] = std::byte{255};
        }
        if (m_canvas != nullptr) {
            m_canvas->present(m_size, m_frame);
        }
        if (m_capture_next) {
            m_capture_next = false;
            m_captured = CapturedFrame{.width = m_size.width, .height = m_size.height,
                                       .pixels = m_frame};
        }
        return {};
    }

    std::optional<MemoryUsage> memory_usage() override {
        std::uint64_t used = m_frame.size();
        m_buffers.for_each([&](core::Handle<Buffer>, const Buffer& buffer) { used += buffer.contents.size(); });
        m_textures.for_each([&](core::Handle<Texture>, const Texture& texture) { used += texture.pixels.size(); });
        return MemoryUsage{.used_bytes = used};
    }

    bool capture_next_frame() override {
        m_capture_next = true;
        return true;
    }

    std::optional<CapturedFrame> take_captured_frame() override {
        std::optional<CapturedFrame> captured = std::move(m_captured);
        m_captured.reset();
        return captured;
    }

    SoftwareAccess* software_access() override {
        return this;
    }

private:
    // SoftwareAccess.

    std::span<const std::byte> buffer_contents(BufferHandle buffer) const override {
        const Buffer* const found = m_buffers.get(handle_cast<Buffer>(buffer));
        return found != nullptr ? std::span<const std::byte>(found->contents)
                                : std::span<const std::byte>();
    }

    std::optional<SoftwareTexture> texture_contents(TextureHandle texture) const override {
        const Texture* const found = m_textures.get(handle_cast<Texture>(texture));
        if (found == nullptr) {
            return std::nullopt;
        }
        return SoftwareTexture{.desc = found->desc, .pixels = found->pixels};
    }

    SoftwareTarget window_target() override {
        DREAM_VERIFY(m_in_frame, "The window's pixels exist only during a frame");
        return SoftwareTarget{.width = m_size.width, .height = m_size.height, .pixels = m_frame};
    }

    // CommandList.

    void begin_render_pass(const ClearColor& color) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        m_in_pass = true;
        m_pass = PipelineInfo{};
        m_pass_textures = {};
        const std::array<std::byte, 4> clear{linear_to_srgb_byte(color.r),
                                             linear_to_srgb_byte(color.g),
                                             linear_to_srgb_byte(color.b), std::byte{255}};
        for (std::size_t pixel = 0; pixel < m_frame.size(); pixel += 4) {
            std::memcpy(&m_frame[pixel], clear.data(), clear.size());
        }
    }

    void begin_render_pass(const RenderTargets& targets) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        const Texture* const color = m_textures.get(handle_cast<Texture>(targets.color));
        const Texture* const depth = m_textures.get(handle_cast<Texture>(targets.depth));
        DREAM_VERIFY(color != nullptr || depth != nullptr, "A render pass needs a target");
        DREAM_VERIFY(color == nullptr || (color->desc.render_target && !is_depth(color->desc.format)),
                     "The color target must be a color render target");
        DREAM_VERIFY(depth == nullptr || (depth->desc.render_target && is_depth(depth->desc.format)),
                     "The depth target must be a depth render target");
        DREAM_VERIFY(color == nullptr || depth == nullptr ||
                         (color->desc.width == depth->desc.width &&
                          color->desc.height == depth->desc.height),
                     "A pass's targets must be the same size");
        m_in_pass = true;
        m_pass = PipelineInfo{};
        if (color != nullptr) {
            m_pass.color = color->desc.format;
        }
        if (depth != nullptr) {
            m_pass.depth = depth->desc.format;
        }
        m_pass_textures = {targets.color, targets.depth};
    }

    void end_render_pass() override {
        DREAM_VERIFY(m_in_pass, "No render pass to end");
        m_in_pass = false;
    }

    void bind_pipeline(PipelineHandle pipeline) override {
        const PipelineInfo* const info = m_pipelines.get(handle_cast<PipelineInfo>(pipeline));
        DREAM_VERIFY(info != nullptr, "Binding a pipeline that doesn't exist");
        DREAM_VERIFY(info->color == m_pass.color && info->depth == m_pass.depth,
                     "The pipeline was made for other render targets than this pass has");
        m_pipeline_bound = true;
    }

    void bind_vertex_buffer(BufferHandle buffer) override {
        DREAM_VERIFY(m_buffers.contains(handle_cast<Buffer>(buffer)),
                     "Binding a buffer that doesn't exist");
    }

    void bind_index_buffer(BufferHandle buffer, IndexFormat /*format*/) override {
        DREAM_VERIFY(m_buffers.contains(handle_cast<Buffer>(buffer)),
                     "Binding a buffer that doesn't exist");
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
    }

    void set_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_uniform_size, "Too many uniforms");
    }

    void set_draw_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_draw_uniform_size, "Too many per-draw uniforms");
    }

    void update_texture(TextureHandle texture, std::uint32_t first_row,
                        std::span<const std::byte> rows) override {
        DREAM_VERIFY(!m_in_pass, "Textures are updated between render passes");
        Texture* const found = m_textures.get(handle_cast<Texture>(texture));
        DREAM_VERIFY(found != nullptr && found->desc.dynamic, "Updating a texture that isn't dynamic");
        const std::size_t row_bytes = texture_level_bytes(found->desc.format, found->desc.width, 1);
        DREAM_VERIFY(rows.size() % row_bytes == 0 &&
                         first_row + rows.size() / row_bytes <= found->desc.height,
                     "update_texture takes whole rows within the texture");
        std::memcpy(found->pixels.data() + first_row * row_bytes, rows.data(), rows.size());
    }

    void bind_texture(std::uint32_t slot, TextureHandle texture) override {
        DREAM_VERIFY(slot < k_texture_slots, "There is no such texture slot");
        if (!texture.is_valid()) {
            return;
        }
        const Texture* const found = m_textures.get(handle_cast<Texture>(texture));
        DREAM_VERIFY(found != nullptr, "Binding a texture that doesn't exist");
        DREAM_VERIFY(texture != m_pass_textures[0] && texture != m_pass_textures[1],
                     "A pass can't read the textures it draws into");
        DREAM_VERIFY((slot == k_shadow_slot) == is_depth(found->desc.format),
                     "The shadow slot takes depth textures, and only it");
    }

    void draw_indexed(std::uint32_t /*index_count*/) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
    }

    void draw_indexed_range(std::uint32_t /*first_index*/, std::uint32_t /*index_count*/) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
    }

    bool draw_vertices(std::span<const std::byte> vertices, std::uint32_t vertex_count) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
        DREAM_VERIFY(vertex_count == 0 || vertices.size() % vertex_count == 0,
                     "The vertex data doesn't divide into vertex_count vertices");
        return reserve_frame_bytes(vertices.size());
    }

    bool bind_frame_geometry(std::span<const std::byte> vertices,
                             std::span<const std::uint32_t> indices) override {
        DREAM_VERIFY(m_in_pass, "Binding geometry outside a render pass");
        return reserve_frame_bytes(vertices.size() + indices.size_bytes());
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        DREAM_VERIFY(m_in_pass, "Binding instances outside a render pass");
        return reserve_frame_bytes(instances.size());
    }

    void draw_indexed_instanced(std::uint32_t /*first_index*/, std::uint32_t /*index_count*/,
                                std::uint32_t /*instance_count*/) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
    }

    void set_scissor(std::optional<ScissorRect> /*rect*/) override {
        DREAM_VERIFY(m_in_pass, "Setting a scissor outside a render pass");
    }

    bool reserve_frame_bytes(std::size_t bytes) {
        if (m_frame_vertex_bytes + bytes > k_max_frame_vertex_bytes) {
            return false;
        }
        m_frame_vertex_bytes += bytes;
        return true;
    }

    core::HandlePool<Texture> m_textures;
    core::HandlePool<Buffer> m_buffers;
    core::HandlePool<Shader> m_shaders;
    core::HandlePool<PipelineInfo> m_pipelines;
    PipelineInfo m_pass;
    std::array<TextureHandle, 2> m_pass_textures{};
    platform::SurfaceSize m_size;
    std::vector<std::byte> m_frame;
    platform::WebCanvas* m_canvas = nullptr;
    std::optional<CapturedFrame> m_captured;
    bool m_capture_next = false;
    bool m_has_surface = false;
    bool m_was_attached = false;
    bool m_in_frame = false;
    bool m_in_pass = false;
    bool m_pipeline_bound = false;
    std::size_t m_frame_vertex_bytes = 0;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& /*config*/) {
    return std::unique_ptr<Device>{std::make_unique<SoftwareDevice>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_software(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "software", .create = &create});
}

} // namespace dream::modules
