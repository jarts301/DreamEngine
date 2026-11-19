#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>

namespace dream::modules {
namespace {

using namespace rhi;

struct Resource {};

struct TextureInfo {
    TextureDesc desc;
};

// The targets a pipeline was made for, which the pass it is used in must have.
struct PipelineInfo {
    std::optional<TextureFormat> color;
    std::optional<TextureFormat> depth;
};

core::Error invalid(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

// A device that draws nothing, for automated runs and machines without a GPU. It still applies the
// rules of the interface, so mistakes in engine code are caught without one.
class NullDevice final : public Device, private CommandList {
public:
    core::Result<void> attach_surface(const platform::NativeWindow& /*window*/,
                                      platform::SurfaceSize size) override {
        m_has_surface = true;
        m_was_attached = true;
        m_size = size;
        return {};
    }

    void detach_surface() override {
        m_has_surface = false;
    }

    void resize_surface(platform::SurfaceSize size) override {
        m_size = size;
    }

    std::string_view shader_file_suffix() const override {
        return ".spv";
    }

    core::Result<BufferHandle> create_buffer(BufferUsage /*usage*/,
                                             std::span<const std::byte> contents) override {
        if (contents.empty()) {
            return invalid("A buffer can't be empty");
        }
        return handle_cast<BufferTag>(m_buffers.create());
    }

    core::Result<TextureHandle> create_texture(const TextureDesc& desc,
                                               std::span<const std::byte> pixels) override {
        if (const auto problem = texture_problem(desc, pixels.size())) {
            return invalid(*problem);
        }
        return handle_cast<TextureTag>(m_textures.create(TextureInfo{desc}));
    }

    // Nothing is drawn, so every format is taken, which lets code that picks a compressed format
    // run without a GPU.
    bool supports_texture_format(TextureFormat format) const override {
        return !is_render_target_only(format);
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
        if (!m_shaders.contains(handle_cast<Resource>(desc.vertex_shader)) ||
            (desc.fragment_shader.is_valid() &&
             !m_shaders.contains(handle_cast<Resource>(desc.fragment_shader)))) {
            return invalid("A pipeline needs valid shaders");
        }
        return handle_cast<PipelineTag>(
            m_pipelines.create(PipelineInfo{desc.color_target, desc.depth_target}));
    }

    void destroy_buffer(BufferHandle buffer) override {
        m_buffers.destroy(handle_cast<Resource>(buffer));
    }

    void destroy_texture(TextureHandle texture) override {
        m_textures.destroy(handle_cast<TextureInfo>(texture));
    }

    void destroy_shader(ShaderHandle shader) override {
        m_shaders.destroy(handle_cast<Resource>(shader));
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
        return {};
    }

private:
    void begin_render_pass(const ClearColor& /*color*/) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        m_in_pass = true;
        m_pass = PipelineInfo{};
        m_pass_textures = {};
    }

    void begin_render_pass(const RenderTargets& targets) override {
        DREAM_VERIFY(m_in_frame && !m_in_pass, "Render pass started in the wrong place");
        const TextureInfo* const color = m_textures.get(handle_cast<TextureInfo>(targets.color));
        const TextureInfo* const depth = m_textures.get(handle_cast<TextureInfo>(targets.depth));
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
        DREAM_VERIFY(m_buffers.contains(handle_cast<Resource>(buffer)),
                     "Binding a buffer that doesn't exist");
    }

    void bind_index_buffer(BufferHandle buffer, IndexFormat /*format*/) override {
        DREAM_VERIFY(m_buffers.contains(handle_cast<Resource>(buffer)),
                     "Binding a buffer that doesn't exist");
    }

    void push_constants(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_push_constant_size, "Too many push constants");
    }

    void set_uniforms(std::span<const std::byte> data) override {
        DREAM_VERIFY(data.size() <= k_max_uniform_size, "Too many uniforms");
    }

    void bind_texture(std::uint32_t slot, TextureHandle texture) override {
        DREAM_VERIFY(slot < k_texture_slots, "There is no such texture slot");
        if (!texture.is_valid()) {
            return;
        }
        const TextureInfo* const info = m_textures.get(handle_cast<TextureInfo>(texture));
        DREAM_VERIFY(info != nullptr, "Binding a texture that doesn't exist");
        DREAM_VERIFY(texture != m_pass_textures[0] && texture != m_pass_textures[1],
                     "A pass can't read the textures it draws into");
        DREAM_VERIFY((slot == k_shadow_slot) == is_depth(info->desc.format),
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
        if (m_frame_vertex_bytes + vertices.size() > k_max_frame_vertex_bytes) {
            return false;
        }
        m_frame_vertex_bytes += vertices.size();
        return true;
    }

    bool bind_frame_geometry(std::span<const std::byte> vertices,
                             std::span<const std::uint32_t> indices) override {
        DREAM_VERIFY(m_in_pass, "Binding geometry outside a render pass");
        const std::size_t bytes = vertices.size() + indices.size_bytes();
        if (m_frame_vertex_bytes + bytes > k_max_frame_vertex_bytes) {
            return false;
        }
        m_frame_vertex_bytes += bytes;
        return true;
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        DREAM_VERIFY(m_in_pass, "Binding instances outside a render pass");
        if (m_frame_vertex_bytes + instances.size() > k_max_frame_vertex_bytes) {
            return false;
        }
        m_frame_vertex_bytes += instances.size();
        return true;
    }

    void draw_indexed_instanced(std::uint32_t /*first_index*/, std::uint32_t /*index_count*/,
                                std::uint32_t /*instance_count*/) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
    }

    void set_scissor(std::optional<ScissorRect> /*rect*/) override {
        DREAM_VERIFY(m_in_pass, "Setting a scissor outside a render pass");
    }

    core::HandlePool<TextureInfo> m_textures;
    core::HandlePool<Resource> m_buffers;
    core::HandlePool<Resource> m_shaders;
    core::HandlePool<PipelineInfo> m_pipelines;
    PipelineInfo m_pass;
    std::array<TextureHandle, 2> m_pass_textures{};
    platform::SurfaceSize m_size;
    bool m_has_surface = false;
    bool m_was_attached = false;
    bool m_in_frame = false;
    bool m_in_pass = false;
    bool m_pipeline_bound = false;
    std::size_t m_frame_vertex_bytes = 0;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& /*config*/) {
    return std::unique_ptr<Device>{std::make_unique<NullDevice>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "null", .create = &create});
}

} // namespace dream::modules
