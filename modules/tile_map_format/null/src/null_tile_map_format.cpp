// Reads no tile map files, for games that don't load maps from a map editor (ADR 0050).

#include "dream/core/module_registry.h"
#include "dream/tile_map_format/tile_map_format.h"

#include <format>
#include <memory>

namespace dream::modules {
namespace {

using namespace tile_map_format;

core::Error unread(std::string_view path) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("'{}': the game reads no map files; list a tile_map_format "
                                   "module, such as \"tiled\", in its TOML file",
                                   path)};
}

class NullTileMapFormat final : public TileMapFormat {
public:
    [[nodiscard]] bool reads(std::string_view /*path*/) const override {
        return false;
    }
    core::Result<Map> read_map(std::span<const std::byte> /*file*/,
                               std::string_view path) override {
        return unread(path);
    }
    core::Result<Tileset> read_tileset(std::span<const std::byte> /*file*/,
                                       std::string_view path) override {
        return unread(path);
    }
};

core::Result<std::unique_ptr<TileMapFormat>> create(const core::Config& /*config*/) {
    return std::unique_ptr<TileMapFormat>{std::make_unique<NullTileMapFormat>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_tile_map_format_null(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<TileMapFormat>{.name = "null", .create = &create});
}

} // namespace dream::modules
