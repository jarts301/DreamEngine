// Reads every asset through the platform, as a game that lists no asset source does: from the
// assets folder, assets.pak or the APK. Bundles are always mounted, so a game written for bundles
// runs with this too, reading everything from wherever the platform finds it.

#include "dream/asset_source/asset_source.h"
#include "dream/core/module_registry.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace asset_source;

class FilesSource final : public AssetSource {
public:
    core::Result<void> initialize(platform::Platform& platform,
                                  const core::AssetDatabase& /*database*/) override {
        m_platform = &platform;
        return {};
    }
    core::Result<std::vector<std::byte>> read(std::string_view path) override {
        return m_platform->read_file(platform::FileRoot::Assets, path);
    }
    std::optional<std::int64_t> version(std::string_view path) override {
        return m_platform->file_version(platform::FileRoot::Assets, path);
    }
    core::Result<void> mount(std::string_view /*bundle*/) override {
        return {};
    }
    void unmount(std::string_view /*bundle*/) override {}
    bool mounted(std::string_view /*bundle*/) const override {
        return true;
    }

private:
    platform::Platform* m_platform = nullptr;
};

core::Result<std::unique_ptr<AssetSource>> create(const core::Config& /*config*/) {
    return std::unique_ptr<AssetSource>{std::make_unique<FilesSource>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_asset_source_files(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<AssetSource>{.name = "files", .create = &create});
}

} // namespace dream::modules
