// A renderer that draws nothing but the clear color, for games without a 3D scene, servers, and
// machines where the forward renderer can't start.

#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace renderer;

class NullRenderer final : public SceneRenderer {
public:
    core::Result<void> start(rhi::Device& /*device*/) override {
        return {};
    }

    core::Result<void> load_shaders(const FileReader& /*read_file*/) override {
        return {};
    }

    FrameStats render(rhi::CommandList& commands, const Frame& frame) override {
        commands.begin_render_pass(frame.clear_color);
        commands.end_render_pass();
        return {};
    }

    std::string_view describe() const override {
        return "Nothing is drawn but the clear color";
    }
};

core::Result<std::unique_ptr<SceneRenderer>> create(const core::Config& /*config*/) {
    return std::unique_ptr<SceneRenderer>{std::make_unique<NullRenderer>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_renderer_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<SceneRenderer>{.name = "null", .create = &create});
}

} // namespace dream::modules
