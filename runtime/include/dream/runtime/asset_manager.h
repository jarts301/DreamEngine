#pragma once

#include "dream/core/profile.h"
#include "dream/core/result.h"
#include "dream/runtime/virtual_file_system.h"

#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::runtime {

class AssetManager;

// A shared, read-only asset. It is freed when the last reference goes away.
template <typename T> using AssetRef = std::shared_ptr<const T>;

struct AssetLoadContext {
    std::string_view path;
    VirtualFileSystem& files;

    // For loading the other assets this one depends on.
    AssetManager& assets;
};

template <typename T> using AssetLoader = std::function<core::Result<T>(AssetLoadContext&)>;

namespace detail {

// Identifies an asset type without RTTI. Not const, so a linker can't merge two of them.
template <typename T> inline char g_asset_tag = 0;

} // namespace detail

// Loads each asset once, and shares it for as long as anything is using it.
//
// Loading happens immediately, on the calling thread, which must be the main thread. Each asset
// type needs a loader, and an asset must be movable. A game's references are released when the game
// object is destroyed, which happens before the graphics device goes away.
class AssetManager {
public:
    explicit AssetManager(VirtualFileSystem& files) : m_files(files) {}

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Replaces any loader already registered for T.
    template <typename T> void register_loader(AssetLoader<T> loader) {
        m_loaders.insert_or_assign(
            &detail::g_asset_tag<T>,
            std::shared_ptr<const void>(std::make_shared<AssetLoader<T>>(std::move(loader))));
    }

    // Returns the existing asset if `path` is already loaded as a T. A failed load is not
    // remembered, so a later call tries again.
    template <typename T> [[nodiscard]] core::Result<AssetRef<T>> load(std::string_view path) {
        const void* const type = &detail::g_asset_tag<T>;
        Key key{type, std::string(path)};

        if (const std::shared_ptr<const void> cached = find_cached(key)) {
            return std::static_pointer_cast<const T>(cached);
        }

        const auto loader = m_loaders.find(type);
        if (loader == m_loaders.end()) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("No loader can read the asset '{}'", path)};
        }
        if (!begin_loading(key)) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("The asset '{}' depends on itself", path)};
        }

        // Only loads that happen are measured; finding a loaded asset is too quick to matter.
        DREAM_PROFILE_ZONE_DETAIL("Load asset", path);
        AssetLoadContext context{path, m_files, *this};
        core::Result<T> loaded = (*static_cast<const AssetLoader<T>*>(loader->second.get()))(context);
        end_loading();
        if (!loaded.has_value()) {
            return core::Error{loaded.error().code, std::format("Could not load '{}': {}", path,
                                                                loaded.error().message)};
        }

        AssetRef<T> asset = std::make_shared<const T>(std::move(loaded).value());
        store(std::move(key), asset);
        return asset;
    }

    // Whether `path` is currently in memory as a T.
    template <typename T> [[nodiscard]] bool is_loaded(std::string_view path) const {
        return find_cached(Key{&detail::g_asset_tag<T>, std::string(path)}) != nullptr;
    }

private:
    struct Key {
        const void* type = nullptr;
        std::string path;

        auto operator<=>(const Key&) const = default;
    };

    [[nodiscard]] std::shared_ptr<const void> find_cached(const Key& key) const;
    [[nodiscard]] bool begin_loading(const Key& key);
    void end_loading();
    void store(Key key, std::shared_ptr<const void> asset);

    VirtualFileSystem& m_files;
    std::map<const void*, std::shared_ptr<const void>> m_loaders;
    std::map<Key, std::weak_ptr<const void>> m_cache;
    std::vector<Key> m_loading;
};

} // namespace dream::runtime
