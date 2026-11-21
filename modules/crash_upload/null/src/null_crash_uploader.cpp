// No uploads: crash reports stay on the device, where Core saved them (ADR 0043).

#include "dream/core/module_registry.h"
#include "dream/crash_upload/crash_upload.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace crash_upload;

class NullCrashUploader final : public CrashUploader {
public:
    bool sends() const override {
        return false;
    }

    core::Result<void> upload(const Report& /*report*/, const Sender& /*send*/) override {
        return {};
    }
};

core::Result<std::unique_ptr<CrashUploader>> create(const core::Config& /*config*/) {
    return std::unique_ptr<CrashUploader>{std::make_unique<NullCrashUploader>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_crash_upload_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<CrashUploader>{.name = "null", .create = &create});
}

} // namespace dream::modules
