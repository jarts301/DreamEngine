#include "dream/runtime/asset_manager.h"

#include <algorithm>

namespace dream::runtime {

std::shared_ptr<const void> AssetManager::find_cached(const Key& key) const {
    const auto entry = m_cache.find(key);
    return entry == m_cache.end() ? nullptr : entry->second.lock();
}

bool AssetManager::begin_loading(const Key& key) {
    if (std::ranges::find(m_loading, key) != m_loading.end()) {
        return false;
    }
    m_loading.push_back(key);
    return true;
}

void AssetManager::end_loading() {
    m_loading.pop_back();
}

void AssetManager::store(Key key, std::shared_ptr<const void> asset) {
    std::erase_if(m_cache, [](const auto& entry) { return entry.second.expired(); });
    m_cache.insert_or_assign(std::move(key), std::move(asset));
}

} // namespace dream::runtime
