#include "dream/core/assert.h"
#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <memory>
#include <string>
#include <string_view>

namespace dream::modules {
namespace {

using namespace rhi;

struct Resource {};

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
        if (!m_shaders.contains(handle_cast<Resource>(desc.vertex_shader)) ||
            !m_shaders.contains(handle_cast<Resource>(desc.fragment_shader))) {
            return invalid("A pipeline needs valid shaders");
        }
        return handle_cast<PipelineTag>(m_pipelines.create());
    }

    void destroy_buffer(BufferHandle buffer) override {
        m_buffers.destroy(handle_cast<Resource>(buffer));
    }

    void destroy_shader(ShaderHandle shader) override {
        m_shaders.destroy(handle_cast<Resource>(shader));
    }

    void destroy_pipeline(PipelineHandle pipeline) override {
        m_pipelines.destroy(handle_cast<Resource>(pipeline));
    }

    core::Result<CommandList*> begin_frame() override {
        DREAM_VERIFY(!m_in_frame, "begin_frame called twice");
        if (!m_has_surface || m_size.width == 0 || m_size.height == 0) {
            return static_cast<CommandList*>(nullptr);
        }
        m_in_frame = true;
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
    }

    void end_render_pass() override {
        DREAM_VERIFY(m_in_pass, "No render pass to end");
        m_in_pass = false;
    }

    void bind_pipeline(PipelineHandle pipeline) override {
        DREAM_VERIFY(m_pipelines.contains(handle_cast<Resource>(pipeline)),
                     "Binding a pipeline that doesn't exist");
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

    void draw_indexed(std::uint32_t /*index_count*/) override {
        DREAM_VERIFY(m_in_pass && m_pipeline_bound, "Drawing without a pass and pipeline");
    }

    core::HandlePool<Resource> m_buffers;
    core::HandlePool<Resource> m_shaders;
    core::HandlePool<Resource> m_pipelines;
    platform::SurfaceSize m_size;
    bool m_has_surface = false;
    bool m_was_attached = false;
    bool m_in_frame = false;
    bool m_in_pass = false;
    bool m_pipeline_bound = false;
};

core::Result<std::unique_ptr<Device>> create(const core::Config& /*config*/) {
    return std::unique_ptr<Device>{std::make_unique<NullDevice>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_rhi_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Device>{.name = "null", .create = &create});
}

} // namespace dream::modules
