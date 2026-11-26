// Ships assets in bundles (ADR 0047): a file in a bundle can be read only while its bundle is
// mounted, as the database the build made says, so a game that forgets to mount one fails as it
// would when packaged, during development too. Where the build packed a bundle into its own
// <bundle>.pak beside the program, as a PC package has, mounting opens it and unmounting closes it;
// elsewhere, as in a build folder or an APK, its files are read through the platform.

#include "dream/asset_source/asset_source.h"
#include "dream/core/archive.h"
#include "dream/core/module_registry.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace dream::modules {
namespace {

using namespace asset_source;

constexpr std::string_view k_base = "base";

class BundleSource final : public AssetSource {
public:
    core::Result<void> initialize(platform::Platform& platform,
                                  const core::AssetDatabase& database) override {
        m_platform = &platform;
        m_database = &database;
        return {};
    }

    core::Result<std::vector<std::byte>> read(std::string_view path) override {
        const std::string_view bundle = m_database->bundle_of(path);
        if (bundle == k_base) {
            return m_platform->read_file(platform::FileRoot::Assets, path);
        }
        const auto found = m_mounted.find(bundle);
        if (found == m_mounted.end()) {
            return core::Error{core::ErrorCode::NotFound,
                               std::format("assets:/{} is in the bundle '{}', which isn't mounted",
                                           path, bundle)};
        }
        if (found->second.has_value()) {
            return found->second->read(path);
        }
        return m_platform->read_file(platform::FileRoot::Assets, path);
    }

    std::optional<std::int64_t> version(std::string_view path) override {
        const std::string_view bundle = m_database->bundle_of(path);
        if (bundle != k_base) {
            const auto found = m_mounted.find(bundle);
            // Files in an archive, or a bundle that isn't there, don't change.
            if (found == m_mounted.end() || found->second.has_value()) {
                return std::nullopt;
            }
        }
        return m_platform->file_version(platform::FileRoot::Assets, path);
    }

    core::Result<void> mount(std::string_view bundle) override {
        if (bundle == k_base || m_mounted.contains(bundle)) {
            return {};
        }
        const std::vector<std::string>& names = m_database->bundles();
        if (std::ranges::find(names, bundle) == names.end()) {
            std::string listed;
            for (const std::string& name : names) {
                listed += listed.empty() ? name : ", " + name;
            }
            return core::Error{core::ErrorCode::NotFound,
                               std::format("There is no bundle '{}'; the project's are: {}", bundle,
                                           listed.empty() ? std::string("none") : listed)};
        }
        std::optional<core::ArchiveReader> archive;
        if (const std::optional<std::string> folder = m_platform->program_folder()) {
            core::Result<core::ArchiveReader> opened =
                core::ArchiveReader::open(std::format("{}{}.pak", *folder, bundle));
            if (opened.has_value()) {
                archive.emplace(std::move(opened).value());
            } else if (opened.error().code != core::ErrorCode::NotFound) {
                return opened.error();
            }
        }
        m_mounted.emplace(std::string(bundle), std::move(archive));
        return {};
    }

    void unmount(std::string_view bundle) override {
        if (const auto found = m_mounted.find(bundle); found != m_mounted.end()) {
            m_mounted.erase(found);
        }
    }

    bool mounted(std::string_view bundle) const override {
        return bundle == k_base || m_mounted.contains(bundle);
    }

private:
    platform::Platform* m_platform = nullptr;
    const core::AssetDatabase* m_database = nullptr;
    // Each mounted bundle, with its archive where the build packed one.
    std::map<std::string, std::optional<core::ArchiveReader>, std::less<>> m_mounted;
};

core::Result<std::unique_ptr<AssetSource>> create(const core::Config& /*config*/) {
    return std::unique_ptr<AssetSource>{std::make_unique<BundleSource>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_asset_source_bundles(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<AssetSource>{.name = "bundles", .create = &create});
}

} // namespace dream::modules
