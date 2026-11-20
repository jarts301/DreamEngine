#pragma once

#include "dream/rhi/rhi.h"

#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dream::renderer::tests {

// Records what the main loop asks of the graphics device. Lives outside the device, because the
// App takes ownership of it.
struct FakeDeviceState {
    std::vector<std::string> calls;
    int attach_failures_left = 0;
    bool frames_available = true;
    std::optional<core::Error> begin_frame_error;
    platform::NativeWindow last_window;
    std::vector<std::byte> last_uniforms;
    std::vector<std::byte> last_draw_uniforms;
    std::vector<std::byte> last_vertices;
    std::vector<std::byte> last_instances;
    // What memory_usage reports. Nothing, like a backend that can't tell, unless a test sets it.
    std::optional<rhi::MemoryUsage> memory;
    // What take_gpu_frame_seconds hands out, oldest first.
    std::deque<double> gpu_frame_seconds;
    // What a frame capture returns. Without it, the device can't capture.
    std::optional<rhi::CapturedFrame> captured;
    // Compressed formats the device takes, besides the Rgba8 ones every device does.
    std::vector<rhi::TextureFormat> compressed_formats;
    // Each texture made, as it was described.
    std::vector<rhi::TextureDesc> textures;
    // Formats the device draws into, besides the Rgba8 ones every device does.
    std::vector<rhi::TextureFormat> render_target_formats;
    // Each pipeline made, as it was described, without its spans. A pipeline's handle index is its
    // place here, and binding one records "pipeline <index>".
    std::vector<rhi::PipelineDesc> pipelines;
};

class FakeDevice final : public rhi::Device, private rhi::CommandList {
public:
    explicit FakeDevice(FakeDeviceState& state) : m_state(state) {}

    core::Result<void> attach_surface(const platform::NativeWindow& window,
                                      platform::SurfaceSize size) override {
        m_state.last_window = window;
        if (m_state.attach_failures_left > 0) {
            --m_state.attach_failures_left;
            m_state.calls.emplace_back("attach_failed");
            return core::Error{core::ErrorCode::Unknown, "surface not ready"};
        }
        m_state.calls.push_back(std::format("attach {}x{}", size.width, size.height));
        return {};
    }

    void detach_surface() override {
        m_state.calls.emplace_back("detach");
    }

    void resize_surface(platform::SurfaceSize size) override {
        m_state.calls.push_back(std::format("resize {}x{}", size.width, size.height));
    }

    std::string_view shader_file_suffix() const override {
        return ".fake";
    }

    std::optional<rhi::MemoryUsage> memory_usage() override {
        return m_state.memory;
    }

    std::optional<double> take_gpu_frame_seconds() override {
        if (m_state.gpu_frame_seconds.empty()) {
            return std::nullopt;
        }
        const double seconds = m_state.gpu_frame_seconds.front();
        m_state.gpu_frame_seconds.pop_front();
        return seconds;
    }

    core::Result<rhi::BufferHandle> create_buffer(rhi::BufferUsage,
                                                  std::span<const std::byte>) override {
        return rhi::BufferHandle{.index = 0, .generation = 1};
    }

    core::Result<rhi::TextureHandle> create_texture(const rhi::TextureDesc& desc,
                                                    std::span<const std::byte>) override {
        m_state.calls.push_back(std::format("texture {}x{}", desc.width, desc.height));
        m_state.textures.push_back(desc);
        return rhi::TextureHandle{.index = ++m_next_texture, .generation = 1};
    }

    bool supports_texture_format(rhi::TextureFormat format) const override {
        return !rhi::is_compressed(format) ||
               std::ranges::find(m_state.compressed_formats, format) !=
                   m_state.compressed_formats.end();
    }

    bool supports_render_target(rhi::TextureFormat format) const override {
        return format == rhi::TextureFormat::Rgba8Srgb || format == rhi::TextureFormat::Rgba8Unorm ||
               std::ranges::find(m_state.render_target_formats, format) !=
                   m_state.render_target_formats.end();
    }

    core::Result<rhi::ShaderHandle> create_shader(rhi::ShaderStage,
                                                  std::span<const std::byte>) override {
        return rhi::ShaderHandle{.index = 0, .generation = 1};
    }

    core::Result<rhi::PipelineHandle> create_pipeline(const rhi::PipelineDesc& desc) override {
        rhi::PipelineDesc kept = desc;
        kept.attributes = {};
        kept.instance_attributes = {};
        m_state.pipelines.push_back(kept);
        return rhi::PipelineHandle{.index = static_cast<std::uint32_t>(m_state.pipelines.size() - 1),
                                   .generation = 1};
    }

    void destroy_texture(rhi::TextureHandle) override {}
    void destroy_buffer(rhi::BufferHandle) override {}
    void destroy_shader(rhi::ShaderHandle) override {}
    void destroy_pipeline(rhi::PipelineHandle) override {}

    core::Result<rhi::CommandList*> begin_frame() override {
        if (m_state.begin_frame_error.has_value()) {
            return *m_state.begin_frame_error;
        }
        m_state.calls.emplace_back("begin_frame");
        return m_state.frames_available ? static_cast<rhi::CommandList*>(this) : nullptr;
    }

    core::Result<void> end_frame() override {
        m_state.calls.emplace_back("end_frame");
        if (m_capture_requested) {
            m_capture_requested = false;
            m_captured = m_state.captured;
        }
        return {};
    }

    bool capture_next_frame() override {
        m_state.calls.emplace_back("capture");
        m_capture_requested = m_state.captured.has_value();
        return m_capture_requested;
    }

    std::optional<rhi::CapturedFrame> take_captured_frame() override {
        return std::exchange(m_captured, std::nullopt);
    }

private:
    void begin_render_pass(const rhi::ClearColor&) override {}
    void begin_render_pass(const rhi::RenderTargets&) override {
        m_state.calls.emplace_back("render_targets");
    }
    void end_render_pass() override {}
    void bind_pipeline(rhi::PipelineHandle pipeline) override {
        m_state.calls.push_back(std::format("pipeline {}", pipeline.index));
    }
    void bind_vertex_buffer(rhi::BufferHandle) override {}
    void bind_index_buffer(rhi::BufferHandle, rhi::IndexFormat) override {}
    void push_constants(std::span<const std::byte>) override {}

    void set_uniforms(std::span<const std::byte> data) override {
        m_state.last_uniforms.assign(data.begin(), data.end());
        m_state.calls.emplace_back("uniforms");
    }

    void set_draw_uniforms(std::span<const std::byte> data) override {
        m_state.last_draw_uniforms.assign(data.begin(), data.end());
        m_state.calls.push_back(std::format("draw_uniforms {}", data.size()));
    }

    void bind_texture(std::uint32_t slot, rhi::TextureHandle texture) override {
        const std::uint32_t index = texture.is_valid() ? texture.index : 0;
        m_state.calls.push_back(slot == 0 ? std::format("bind_texture {}", index)
                                          : std::format("bind_texture {} at {}", index, slot));
    }

    void draw_indexed(std::uint32_t count) override {
        m_state.calls.push_back(std::format("draw {}", count));
    }

    void draw_indexed_range(std::uint32_t first, std::uint32_t count) override {
        m_state.calls.push_back(std::format("draw {}+{}", first, count));
    }

    bool draw_vertices(std::span<const std::byte> vertices, std::uint32_t count) override {
        m_state.calls.push_back(std::format("vertices {}", count));
        m_state.last_vertices.assign(vertices.begin(), vertices.end());
        return true;
    }

    bool bind_frame_geometry(std::span<const std::byte> vertices,
                             std::span<const std::uint32_t> indices) override {
        m_state.calls.push_back(std::format("geometry {} {}", vertices.size(), indices.size()));
        return true;
    }

    bool bind_instances(std::span<const std::byte> instances) override {
        m_state.calls.push_back(std::format("instances {}", instances.size()));
        m_state.last_instances.assign(instances.begin(), instances.end());
        return true;
    }

    void draw_indexed_instanced(std::uint32_t first, std::uint32_t count,
                                std::uint32_t instances) override {
        m_state.calls.push_back(std::format("draw {}+{} x{}", first, count, instances));
    }

    void set_scissor(std::optional<rhi::ScissorRect> rect) override {
        m_state.calls.push_back(rect.has_value() ? std::format("scissor {} {} {} {}", rect->x,
                                                               rect->y, rect->width, rect->height)
                                                 : std::string("scissor off"));
    }

    FakeDeviceState& m_state;
    std::uint32_t m_next_texture = 0;
    std::optional<rhi::CapturedFrame> m_captured;
    bool m_capture_requested = false;
};

} // namespace dream::renderer::tests
