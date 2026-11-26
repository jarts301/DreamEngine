#include "dream/core/asset_database.h"

#include "dream/core/config.h"

#include <format>
#include <set>
#include <span>
#include <utility>

namespace dream::core {
namespace {

Error database_error(std::string message) {
    return Error{ErrorCode::ParseError, std::format("Asset database: {}", message)};
}

std::string_view without_root(std::string_view path) {
    constexpr std::string_view k_root = "assets:/";
    return path.starts_with(k_root) ? path.substr(k_root.size()) : path;
}

std::vector<std::string> split(std::string_view joined) {
    std::vector<std::string> parts;
    while (!joined.empty()) {
        const std::size_t bar = joined.find('|');
        parts.emplace_back(joined.substr(0, bar));
        if (bar == std::string_view::npos) {
            break;
        }
        joined.remove_prefix(bar + 1);
    }
    return parts;
}

} // namespace

Result<AssetDatabase> AssetDatabase::parse(std::string_view text) {
    const Result<Config> parsed = Config::parse(text);
    if (!parsed.has_value()) {
        return database_error(parsed.error().message);
    }
    const Config& config = parsed.value();
    const auto strings = [&](std::string_view key) -> std::span<const std::string> {
        return config.get_string_list(key).value_or(std::span<const std::string>{});
    };
    const std::span<const std::string> paths = strings("assets.paths");
    const std::span<const std::string> kinds = strings("assets.kinds");
    const std::span<const std::string> needs = strings("assets.needs");
    const std::span<const std::string> bundles = strings("assets.bundles");
    const std::span<const double> bytes =
        config.get_float_list("assets.bytes").value_or(std::span<const double>{});
    if (kinds.size() != paths.size() || needs.size() != paths.size() ||
        bundles.size() != paths.size() || bytes.size() != paths.size()) {
        return database_error("its lists aren't all one entry per file");
    }

    AssetDatabase database;
    database.m_platform = config.get_string("platform").value_or("");
    database.m_texture_cooker = config.get_string("texture_cooker").value_or("");
    for (const std::string& name : strings("bundles")) {
        database.m_bundles.push_back(name);
    }
    database.m_assets.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (!database.m_index.emplace(paths[index], index).second) {
            return database_error(std::format("'{}' is listed twice", paths[index]));
        }
        database.m_assets.push_back(Asset{.path = paths[index],
                                          .kind = kinds[index],
                                          .bytes = static_cast<std::uint64_t>(bytes[index]),
                                          .needs = split(needs[index]),
                                          .bundle = bundles[index]});
    }
    return database;
}

const AssetDatabase::Asset* AssetDatabase::find(std::string_view path) const {
    const auto found = m_index.find(without_root(path));
    return found == m_index.end() ? nullptr : &m_assets[found->second];
}

std::vector<std::string> AssetDatabase::needs_all(std::string_view path) const {
    std::vector<std::string> all;
    std::set<std::string, std::less<>> seen{std::string(without_root(path))};
    std::vector<std::string> pending{std::string(without_root(path))};
    while (!pending.empty()) {
        const std::string next = std::move(pending.front());
        pending.erase(pending.begin());
        const Asset* asset = find(next);
        if (asset == nullptr) {
            continue;
        }
        for (const std::string& needed : asset->needs) {
            if (seen.insert(needed).second) {
                all.push_back(needed);
                pending.push_back(needed);
            }
        }
    }
    return all;
}

std::vector<std::string> AssetDatabase::needed_by(std::string_view path) const {
    const std::string_view wanted = without_root(path);
    std::vector<std::string> users;
    for (const Asset& asset : m_assets) {
        for (const std::string& needed : asset.needs) {
            if (needed == wanted) {
                users.push_back(asset.path);
                break;
            }
        }
    }
    return users;
}

std::string_view AssetDatabase::bundle_of(std::string_view path) const {
    const Asset* asset = find(path);
    return asset == nullptr ? std::string_view("base") : std::string_view(asset->bundle);
}

std::vector<std::string> AssetDatabase::files_in(std::string_view bundle) const {
    std::vector<std::string> files;
    for (const Asset& asset : m_assets) {
        if (asset.bundle == bundle) {
            files.push_back(asset.path);
        }
    }
    return files;
}

} // namespace dream::core
