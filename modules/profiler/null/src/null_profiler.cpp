#include "dream/core/module_registry.h"
#include "dream/profiler/profiler.h"

#include <memory>

namespace dream::modules {
namespace {

using profiler::Profiler;

// Records nothing. For a project that keeps its profiler list but turns profiling off, such as in
// a release, and for tests.
class NullProfiler final : public Profiler {
public:
    void begin_zone(const core::ProfileSite& /*site*/, std::string_view /*detail*/) override {}
    void end_zone(const core::ProfileSite& /*site*/) override {}
    void mark_frame() override {}
    void set_thread_name(std::string_view /*name*/) override {}
    void record_value(const char* /*name*/, double /*value*/) override {}
    void record_message(std::string_view /*text*/) override {}

    core::Result<void> flush() override {
        return {};
    }
};

core::Result<std::unique_ptr<Profiler>> create(const core::Config& /*config*/) {
    return std::unique_ptr<Profiler>{std::make_unique<NullProfiler>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_profiler_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Profiler>{.name = "null", .create = &create});
}

} // namespace dream::modules
