#include "dream/runtime/program.h"

#include "dream/core/log.h"
#include "dream/core/module_project.h"
#include "dream/platform/platform.h"
#include "dream/rhi/rhi.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"

#include <memory>
#include <utility>

namespace dream::runtime {

int run_program() {
    const core::Result<core::ModuleProject> project =
        core::ModuleProject::load(core::generated_module_project());
    if (!project.has_value()) {
        DREAM_LOG_ERROR("{}", project.error().message);
        return 1;
    }

    core::Result<core::CreatedModule<platform::Platform>> created =
        project.value().create<platform::Platform>();
    if (!created.has_value()) {
        DREAM_LOG_ERROR("{}", created.error().message);
        return 1;
    }

    std::unique_ptr<rhi::Device> device;
    if (project.value().is_configured<rhi::Device>()) {
        core::Result<core::CreatedModule<rhi::Device>> created_device =
            project.value().create<rhi::Device>();
        if (!created_device.has_value()) {
            DREAM_LOG_ERROR("{}", created_device.error().message);
            return 1;
        }
        device = std::move(created_device.value().instance);
    }

    App app(std::move(created.value().instance), project.value().config(), std::move(device),
            &project.value());

    // Created after the App so it is destroyed first, while the GPU resources it holds are valid.
    const std::unique_ptr<Game> game = create_game();
    if (game == nullptr) {
        DREAM_LOG_ERROR("create_game returned null");
        return 1;
    }

    if (const core::Result<void> result = app.run(*game); !result.has_value()) {
        DREAM_LOG_ERROR("{}", result.error().message);
        return 1;
    }
    return 0;
}

} // namespace dream::runtime
