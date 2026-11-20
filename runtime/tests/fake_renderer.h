#pragma once

#include "dream/renderer/scene_renderer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dream::runtime::tests {

// One interface layer as the renderer was given it, with each command's texture looked up.
struct FakeUiLayer {
    core::Vec2 offset;
    bool premultiplied = false;
    std::vector<ui::DrawCommand> commands;
    std::vector<rhi::TextureHandle> textures;
};

// A copy of one frame, which only lives during render.
struct FakeFrame {
    platform::SurfaceSize size;
    rhi::ClearColor clear_color;
    std::optional<renderer::View> view;
    std::optional<renderer::Sunlight> sun;
    std::vector<renderer::MeshDraw> meshes;
    std::vector<renderer::Light> lights;
    std::vector<renderer::LineVertex> lines;
    std::vector<FakeUiLayer> ui_layers;
};

struct FakeRendererState {
    bool started = false;
    int shader_loads = 0;
    // Read through the reader load_shaders is given, and fails the load when missing.
    std::string shader_file = "assets:/shaders/fake_renderer.txt";
    std::vector<FakeFrame> frames;
};

// A renderer module that records what the runtime gives it, and only clears the window.
class FakeRenderer final : public renderer::SceneRenderer {
public:
    explicit FakeRenderer(FakeRendererState& state) : m_state(state) {}

    core::Result<void> start(rhi::Device&) override {
        m_state.started = true;
        return {};
    }

    core::Result<void> load_shaders(const renderer::FileReader& read_file) override {
        const auto file = read_file(m_state.shader_file);
        if (!file.has_value()) {
            return file.error();
        }
        ++m_state.shader_loads;
        return {};
    }

    renderer::FrameStats render(rhi::CommandList& commands,
                                const renderer::Frame& frame) override {
        FakeFrame copy{.size = frame.size,
                       .clear_color = frame.clear_color,
                       .view = frame.view,
                       .sun = frame.sun,
                       .meshes = {frame.meshes.begin(), frame.meshes.end()},
                       .lights = {frame.lights.begin(), frame.lights.end()},
                       .lines = {frame.lines.begin(), frame.lines.end()},
                       .ui_layers = {}};
        renderer::FrameStats stats;
        stats.draws = static_cast<std::uint32_t>(frame.meshes.size());
        stats.lines = static_cast<std::uint32_t>(frame.lines.size() / 2);
        if (frame.sun.has_value() && frame.sun->casts_shadows) {
            for (const renderer::MeshDraw& mesh : frame.meshes) {
                stats.shadow_casters += mesh.casts_shadows ? 1 : 0;
            }
        }
        for (const renderer::UiLayer& layer : frame.ui_layers) {
            FakeUiLayer& recorded = copy.ui_layers.emplace_back();
            recorded.offset = layer.offset;
            if (layer.list == nullptr) {
                continue;
            }
            recorded.premultiplied = layer.list->premultiplied_alpha;
            recorded.commands = layer.list->commands;
            for (const ui::DrawCommand& command : layer.list->commands) {
                recorded.textures.push_back(frame.ui_texture(command.texture));
            }
            stats.ui_commands += static_cast<std::uint32_t>(layer.list->commands.size());
        }
        m_state.frames.push_back(std::move(copy));

        commands.begin_render_pass(frame.clear_color);
        commands.end_render_pass();
        return stats;
    }

    std::string_view describe() const override {
        return "The fake renderer records frames";
    }

private:
    FakeRendererState& m_state;
};

} // namespace dream::runtime::tests
