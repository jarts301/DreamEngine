#pragma once

#include "dream/rhi/rhi.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace dream::runtime::tests {

// Records what the main loop asks of the graphics device. Lives outside the device, because the
// App takes ownership of it.
struct FakeDeviceState {
    std::vector<std::string> calls;
    int attach_failures_left = 0;
    bool frames_available = true;
    std::optional<core::Error> begin_frame_error;
    platform::NativeWindow last_window;
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

    core::Result<rhi::BufferHandle> create_buffer(rhi::BufferUsage,
                                                  std::span<const std::byte>) override {
        return rhi::BufferHandle{};
    }

    core::Result<rhi::ShaderHandle> create_shader(rhi::ShaderStage,
                                                  std::span<const std::byte>) override {
        return rhi::ShaderHandle{};
    }

    core::Result<rhi::PipelineHandle> create_pipeline(const rhi::PipelineDesc&) override {
        return rhi::PipelineHandle{};
    }

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
        return {};
    }

private:
    void begin_render_pass(const rhi::ClearColor&) override {}
    void end_render_pass() override {}
    void bind_pipeline(rhi::PipelineHandle) override {}
    void bind_vertex_buffer(rhi::BufferHandle) override {}
    void bind_index_buffer(rhi::BufferHandle, rhi::IndexFormat) override {}
    void push_constants(std::span<const std::byte>) override {}
    void draw_indexed(std::uint32_t) override {}

    FakeDeviceState& m_state;
};

} // namespace dream::runtime::tests
