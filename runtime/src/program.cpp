#include "dream/runtime/program.h"

#include "dream/core/crash_handler.h"
#include "dream/core/log.h"
#include "dream/core/module_project.h"
#include "dream/platform/platform.h"
#include "dream/rhi/rhi.h"
#include "dream/runtime/app.h"
#include "dream/runtime/crash_reports.h"
#include "dream/runtime/game.h"
#include "dream/runtime/profiling.h"

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace dream::runtime {

int run_program() {
    const core::Result<core::ModuleProject> project =
        core::ModuleProject::load(core::generated_module_project());
    if (!project.has_value()) {
        DREAM_LOG_ERROR("{}", project.error().message);
        return 1;
    }

    // Installed first and removed last, so a crash anywhere in between is reported, including while
    // starting up and shutting down.
    const core::Result<CrashReportConfig> crash_config =
        CrashReportConfig::from_config(project.value().config());
    if (!crash_config.has_value()) {
        DREAM_LOG_ERROR("{}", crash_config.error().message);
        return 1;
    }
    std::unique_ptr<core::CrashHandler> crash_handler;
    if (crash_config.value().enabled) {
        core::Result<std::unique_ptr<core::CrashHandler>> installed =
            core::CrashHandler::install(core::CrashReportSettings{
                .directory = crash_config.value().directory,
                .application = std::string(
                    project.value().config().get_string("app.title").value_or("Dream Engine")),
                .minidump = crash_config.value().minidump,
            });
        if (installed.has_value()) {
            crash_handler = std::move(installed).value();
        } else {
            // A game without crash reports still runs.
            DREAM_LOG_WARNING("Crash reports are off: {}", installed.error().message);
        }
    }
    core::set_crash_detail("Module lists", std::format("[modules.{}]", project.value().platform()));

    // Started before everything else and ended after it, so it sees every zone, including those of
    // threads that other systems start and stop. Ending it saves the profile.
    const core::Result<ProfilerSession> profiling = ProfilerSession::start(project.value());
    if (!profiling.has_value()) {
        DREAM_LOG_ERROR("{}", profiling.error().message);
        return 1;
    }

    core::Result<core::CreatedModule<platform::Platform>> created =
        project.value().create<platform::Platform>();
    if (!created.has_value()) {
        DREAM_LOG_ERROR("{}", created.error().message);
        return 1;
    }
    core::set_crash_detail("platform module", created.value().name);

    std::unique_ptr<rhi::Device> device;
    if (project.value().is_configured<rhi::Device>()) {
        core::Result<core::CreatedModule<rhi::Device>> created_device =
            project.value().create<rhi::Device>();
        if (!created_device.has_value()) {
            DREAM_LOG_ERROR("{}", created_device.error().message);
            return 1;
        }
        core::set_crash_detail("rhi module", created_device.value().name);
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
