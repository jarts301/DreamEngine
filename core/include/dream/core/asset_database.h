#pragma once

#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::core {

// A game's asset database (ADR 0047): every file of its assets, as the build staged them for one
// platform, what kind it is, the files it needs, and the bundle it is shipped in. The build writes
// it as assets:/asset_database.toml; read it with parse.
//
// Paths are as in "assets:/models/crate.glb", without "assets:/". What a file needs is what it
// names, such as a scene's models or a model's textures, in the form the game reads: a texture the
// build cooked is needed as its .ktx2.
class AssetDatabase {
public:
    struct Asset {
        std::string path;
        // "scene", "model", "texture", "environment", "sound", "font", "shader", "ui", "style",
        // "lods", "data" for other TOML files, or "file".
        std::string kind;
        std::uint64_t bytes = 0;
        // The files it names, directly.
        std::vector<std::string> needs;
        // "base", or the name of the bundle it is shipped in.
        std::string bundle;
    };

    // The database's text. Fails when it isn't one, or its lists don't match.
    [[nodiscard]] static Result<AssetDatabase> parse(std::string_view text);

    [[nodiscard]] const std::vector<Asset>& assets() const noexcept {
        return m_assets;
    }
    // Null for a file that isn't among the assets. Takes a path with or without "assets:/".
    [[nodiscard]] const Asset* find(std::string_view path) const;

    // Everything `path` needs, directly or through what it needs, each once, in the order found,
    // without `path` itself: all a scene must have to load.
    [[nodiscard]] std::vector<std::string> needs_all(std::string_view path) const;
    // The files that need `path` directly, such as the scenes that use a model.
    [[nodiscard]] std::vector<std::string> needed_by(std::string_view path) const;

    // The platform the assets were built for, and the texture cooker that made their textures.
    [[nodiscard]] const std::string& platform() const noexcept {
        return m_platform;
    }
    [[nodiscard]] const std::string& texture_cooker() const noexcept {
        return m_texture_cooker;
    }
    // The bundles the project names, besides the base, in its order.
    [[nodiscard]] const std::vector<std::string>& bundles() const noexcept {
        return m_bundles;
    }
    // The bundle `path` is shipped in: "base" for one that isn't among the assets.
    [[nodiscard]] std::string_view bundle_of(std::string_view path) const;
    // The files a bundle holds.
    [[nodiscard]] std::vector<std::string> files_in(std::string_view bundle) const;

private:
    std::vector<Asset> m_assets;
    std::map<std::string, std::size_t, std::less<>> m_index;
    std::string m_platform;
    std::string m_texture_cooker;
    std::vector<std::string> m_bundles;
};

} // namespace dream::core
