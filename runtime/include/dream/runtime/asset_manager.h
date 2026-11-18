#pragma once

#include "dream/core/profile.h"
#include "dream/core/result.h"
#include "dream/runtime/virtual_file_system.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::runtime {

class AssetManager;

namespace detail {

// Where every reference to one asset finds its current version, so a reload reaches them all.
template <typename T> struct AssetSlot {
    std::shared_ptr<const T> current;
    std::uint32_t version = 0;
};

} // namespace detail

// A shared, read-only asset. It is freed when the last reference goes away.
//
// When hot reloading replaces the asset, every reference sees the new one from then on, between
// frames. Code that built something of its own from an asset, such as a pipeline from a shader,
// can compare version() with the one it built from.
template <typename T> class AssetRef {
public:
    AssetRef() = default;
    AssetRef(std::nullptr_t) noexcept {} // NOLINT: an empty reference, as with a pointer.

    // An asset not loaded by an AssetManager, which never reloads.
    AssetRef(std::shared_ptr<const T> asset) // NOLINT: stands in for the pointer it wraps.
        : m_slot(asset ? std::make_shared<detail::AssetSlot<T>>(
                             detail::AssetSlot<T>{std::move(asset), 0})
                       : nullptr) {}

    [[nodiscard]] const T* get() const noexcept {
        return m_slot ? m_slot->current.get() : nullptr;
    }
    [[nodiscard]] const T& operator*() const noexcept {
        return *get();
    }
    [[nodiscard]] const T* operator->() const noexcept {
        return get();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return get() != nullptr;
    }
    [[nodiscard]] friend bool operator==(const AssetRef& ref, std::nullptr_t) noexcept {
        return ref.get() == nullptr;
    }
    [[nodiscard]] friend bool operator==(const AssetRef& a, const AssetRef& b) noexcept {
        return a.m_slot == b.m_slot;
    }

    // How many times the asset has been reloaded.
    [[nodiscard]] std::uint32_t version() const noexcept {
        return m_slot ? m_slot->version : 0;
    }

private:
    friend class AssetManager;

    explicit AssetRef(std::shared_ptr<detail::AssetSlot<T>> slot) : m_slot(std::move(slot)) {}

    std::shared_ptr<detail::AssetSlot<T>> m_slot;
};

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
    // Watches what `files` reads, to know which files each asset comes from.
    explicit AssetManager(VirtualFileSystem& files);
    ~AssetManager();

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

        if (const std::shared_ptr<void> cached = find_cached(key)) {
            return AssetRef<T>(std::static_pointer_cast<detail::AssetSlot<T>>(cached));
        }

        if (m_refusing_loads) {
            return core::Error{core::ErrorCode::OutOfMemory,
                               std::format("Not loading '{}': memory is critically low", path)};
        }

        std::vector<std::string> files;
        core::Result<T> loaded = run_loader<T>(key, files);
        if (!loaded.has_value()) {
            return loaded.error();
        }

        auto slot = std::make_shared<detail::AssetSlot<T>>(
            detail::AssetSlot<T>{std::make_shared<const T>(std::move(loaded).value()), 0});
        // Reloading runs the loader again into the same slot, if anything still uses it.
        std::function<core::Result<void>()> reload = [this, key, weak = std::weak_ptr(slot)] {
            const std::shared_ptr<detail::AssetSlot<T>> live = weak.lock();
            if (!live) {
                return core::Result<void>{};
            }
            std::vector<std::string> read;
            core::Result<T> again = run_loader<T>(key, read);
            if (!again.has_value()) {
                return core::Result<void>{again.error()};
            }
            live->current = std::make_shared<const T>(std::move(again).value());
            ++live->version;
            set_files(key, std::move(read));
            return core::Result<void>{};
        };
        store(std::move(key), slot, std::move(files), std::move(reload));
        return AssetRef<T>(std::move(slot));
    }

    // Hot reloading: loads again every asset still in use whose files changed since it was loaded,
    // as far as the platform can tell, and returns the paths of those reloaded. An asset that fails
    // to reload keeps its last version, and the failure is logged. Call it between frames.
    std::vector<std::string> reload_changed();

    // While on, loading an asset that isn't already in memory fails with ErrorCode::OutOfMemory
    // instead of allocating for it, so a load fails cleanly rather than the program running out.
    // Assets already loaded are still returned. The App turns it on while memory pressure is
    // critical (docs/adr/0018-memory-budgets.md).
    void set_refusing_loads(bool refusing) noexcept {
        m_refusing_loads = refusing;
    }

    [[nodiscard]] bool is_refusing_loads() const noexcept {
        return m_refusing_loads;
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

    // A loaded asset, the files it was read from with their versions then, and how to reload it.
    struct Entry {
        std::weak_ptr<void> slot;
        std::vector<std::pair<std::string, std::optional<std::int64_t>>> files;
        std::function<core::Result<void>()> reload;
    };

    template <typename T>
    [[nodiscard]] core::Result<T> run_loader(const Key& key, std::vector<std::string>& files) {
        const auto loader = m_loaders.find(key.type);
        if (loader == m_loaders.end()) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("No loader can read the asset '{}'", key.path)};
        }
        if (!begin_loading(key, files)) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("The asset '{}' depends on itself", key.path)};
        }

        // Only loads that happen are measured; finding a loaded asset is too quick to matter.
        DREAM_PROFILE_ZONE_DETAIL("Load asset", key.path);
        AssetLoadContext context{key.path, m_files, *this};
        core::Result<T> loaded =
            (*static_cast<const AssetLoader<T>*>(loader->second.get()))(context);
        end_loading();
        if (!loaded.has_value()) {
            return core::Error{loaded.error().code, std::format("Could not load '{}': {}",
                                                                key.path, loaded.error().message)};
        }
        return loaded;
    }

    [[nodiscard]] std::shared_ptr<void> find_cached(const Key& key) const;
    [[nodiscard]] bool begin_loading(const Key& key, std::vector<std::string>& files);
    void end_loading();
    void store(Key key, std::shared_ptr<void> slot, std::vector<std::string> files,
               std::function<core::Result<void>()> reload);
    void set_files(const Key& key, std::vector<std::string> files);

    VirtualFileSystem& m_files;
    std::map<const void*, std::shared_ptr<const void>> m_loaders;
    std::map<Key, Entry> m_cache;
    // The assets being loaded, innermost last, each with the files its own loader read.
    std::vector<std::pair<Key, std::vector<std::string>*>> m_loading;
    bool m_refusing_loads = false;
};

} // namespace dream::runtime
