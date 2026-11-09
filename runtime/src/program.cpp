#include "dream/runtime/program.h"

#include "dream/core/log.h"
#include "dream/core/module_project.h"
#include "dream/platform/platform.h"
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

    const std::unique_ptr<Game> game = create_game();
    if (game == nullptr) {
        DREAM_LOG_ERROR("create_game returned null");
        return 1;
    }

    App app(std::move(created.value().instance), project.value().config());
    if (const core::Result<void> result = app.run(*game); !result.has_value()) {
        DREAM_LOG_ERROR("{}", result.error().message);
        return 1;
    }
    return 0;
}

} // namespace dream::runtime
