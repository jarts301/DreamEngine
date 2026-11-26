#pragma once

#include "dream/core/asset_database.h"
#include "dream/core/result.h"
#include "dream/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace dream::asset_source {

// Where a game's assets come from (ADR 0047): every "assets:/" read goes through the source its
// project lists, and a game without one reads them through the platform, as before.
//
//     [modules.windows]
//     asset_source = ["bundles"]
//
// Bundles are groups of assets a project names in [bundles], which the build ships apart: on PC, in
// a <bundle>.pak of its own beside assets.pak. A game mounts a bundle before it loads anything in
// it, and unmounts it when it no longer needs it. The base, what no bundle holds, is always there.
class AssetSource {
public:
    static constexpr std::string_view k_module_interface = "asset_source";

    virtual ~AssetSource() = default;

    // Call once, before any read. `platform` and `database` must outlive this; `database` is the
    // build's, empty when the build made none.
    [[nodiscard]] virtual core::Result<void> initialize(platform::Platform& platform,
                                                        const core::AssetDatabase& database) = 0;

    // A file under assets:/, as "models/crate.glb". Fails for one in a bundle that isn't mounted,
    // saying which.
    [[nodiscard]] virtual core::Result<std::vector<std::byte>> read(std::string_view path) = 0;
    // As platform::Platform::file_version, for hot reload.
    [[nodiscard]] virtual std::optional<std::int64_t> version(std::string_view path) = 0;

    // Fails for a bundle the database doesn't name, or whose files can't be opened.
    [[nodiscard]] virtual core::Result<void> mount(std::string_view bundle) = 0;
    // Its files can't be read until it is mounted again. What was already loaded from it stays.
    virtual void unmount(std::string_view bundle) = 0;
    // The base always is.
    [[nodiscard]] virtual bool mounted(std::string_view bundle) const = 0;
};

} // namespace dream::asset_source
