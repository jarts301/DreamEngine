#include "dream/runtime/asset_manager.h"

#include "dream/core/log.h"

#include <algorithm>

namespace dream::runtime {

AssetManager::AssetManager(VirtualFileSystem& files) : m_files(files) {
    // Every file read while an asset loads is one it may come from, even a read that failed, since
    // the file may appear later.
    m_files.set_read_observer([this](std::string_view path) {
        if (!m_loading.empty()) {
            std::vector<std::string>& read = *m_loading.back().second;
            if (std::ranges::find(read, path) == read.end()) {
                read.emplace_back(path);
            }
        }
    });
}

AssetManager::~AssetManager() {
    m_files.set_read_observer({});
}

std::shared_ptr<void> AssetManager::find_cached(const Key& key) const {
    const auto entry = m_cache.find(key);
    return entry == m_cache.end() ? nullptr : entry->second.slot.lock();
}

bool AssetManager::begin_loading(const Key& key, std::vector<std::string>& files) {
    if (std::ranges::any_of(m_loading, [&](const auto& loading) { return loading.first == key; })) {
        return false;
    }
    m_loading.emplace_back(key, &files);
    return true;
}

void AssetManager::end_loading() {
    m_loading.pop_back();
}

void AssetManager::store(Key key, std::shared_ptr<void> slot, std::vector<std::string> files,
                         std::function<core::Result<void>()> reload) {
    std::erase_if(m_cache, [](const auto& entry) { return entry.second.slot.expired(); });
    m_cache.insert_or_assign(key, Entry{std::move(slot), {}, std::move(reload)});
    set_files(key, std::move(files));
}

void AssetManager::set_files(const Key& key, std::vector<std::string> files) {
    const auto entry = m_cache.find(key);
    if (entry == m_cache.end()) {
        return;
    }
    entry->second.files.clear();
    for (std::string& file : files) {
        const std::optional<std::int64_t> version = m_files.version(file);
        entry->second.files.emplace_back(std::move(file), version);
    }
}

std::vector<std::string> AssetManager::reload_changed() {
    std::erase_if(m_cache, [](const auto& entry) { return entry.second.slot.expired(); });

    // Found first and reloaded after, since reloading can load other assets into the cache.
    std::vector<Key> changed;
    for (const auto& [key, entry] : m_cache) {
        const bool any_changed = std::ranges::any_of(entry.files, [&](const auto& file) {
            return file.second.has_value() && m_files.version(file.first) != file.second;
        });
        if (any_changed) {
            changed.push_back(key);
        }
    }

    std::vector<std::string> reloaded;
    for (const Key& key : changed) {
        const auto entry = m_cache.find(key);
        if (entry == m_cache.end()) {
            continue;
        }
        // Copied, since the reload replaces the entry's files.
        const std::function<core::Result<void>()> reload = entry->second.reload;
        if (const core::Result<void> result = reload(); result.has_value()) {
            DREAM_LOG_INFO("Reloaded '{}'", key.path);
            reloaded.push_back(key.path);
        } else {
            DREAM_LOG_ERROR("Could not reload '{}', so the last version stays: {}", key.path,
                            result.error().message);
            // Not tried again until a file changes once more.
            set_files(key, [&] {
                std::vector<std::string> files;
                for (const auto& file : entry->second.files) {
                    files.push_back(file.first);
                }
                return files;
            }());
        }
    }
    return reloaded;
}

} // namespace dream::runtime
