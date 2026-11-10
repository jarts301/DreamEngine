#pragma once

#include "dream/core/config.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/virtual_file_system.h"
#include "dream/runtime/world.h"

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace dream::runtime {

// One component's table in a scene file. Each getter fails if the value has the wrong type, so a
// typo in a scene is reported instead of quietly using the fallback.
class ComponentData {
public:
    ComponentData(const core::Config& config, std::string prefix)
        : m_config(config), m_prefix(std::move(prefix)) {}

    [[nodiscard]] bool has(std::string_view key) const {
        return m_config.contains(full_key(key));
    }

    [[nodiscard]] core::Result<float> number(std::string_view key, float fallback) const;
    [[nodiscard]] core::Result<bool> flag(std::string_view key, bool fallback) const;
    [[nodiscard]] core::Result<core::Vec2> vec2(std::string_view key, core::Vec2 fallback) const;
    [[nodiscard]] core::Result<core::Vec3> vec3(std::string_view key, core::Vec3 fallback) const;

    // The value must be present.
    [[nodiscard]] core::Result<std::string> text(std::string_view key) const;

    // The same, with a fallback for when it is absent.
    [[nodiscard]] core::Result<std::string> text(std::string_view key,
                                                 std::string_view fallback) const;

    // Fails if the table has a setting not in `known`, which is usually a typo.
    [[nodiscard]] core::Result<void>
    check_keys(std::initializer_list<std::string_view> known) const;

private:
    [[nodiscard]] std::string full_key(std::string_view key) const;

    const core::Config& m_config;
    std::string m_prefix;
};

struct ComponentContext {
    World& world;
    Entity entity;
    const ComponentData& data;
    AssetManager& assets;
};

using ComponentReader = std::function<core::Result<void>(ComponentContext&)>;

// Builds a World from a TOML scene file:
//
//     [entities.wheel]
//     parent = "car"                # Optional.
//
//     [entities.wheel.transform]
//     position = [1.0, 0.0, 2.0]
//     rotation = [0.0, 90.0, 0.0]   # Degrees around X, Y and Z, applied Y, then X, then Z.
//     scale = [1.0, 1.0, 1.0]
//
//     [entities.wheel.spin]         # A component the game registered as "spin".
//     speed = 2.0
//
// "transform" is built in. A scene fails to load as a whole, leaving nothing half-built, if it has
// a component nobody registered or any other mistake.
class SceneLoader {
public:
    SceneLoader(VirtualFileSystem& files, AssetManager& assets);

    SceneLoader(const SceneLoader&) = delete;
    SceneLoader& operator=(const SceneLoader&) = delete;

    void register_component(std::string name, ComponentReader reader);

    // `path` is a virtual path such as "assets:/scenes/level1.toml".
    [[nodiscard]] core::Result<std::unique_ptr<World>> load(std::string_view path);

    // The same from scene text already in memory.
    [[nodiscard]] core::Result<std::unique_ptr<World>> parse(std::string_view text);

private:
    VirtualFileSystem& m_files;
    AssetManager& m_assets;
    std::map<std::string, ComponentReader, std::less<>> m_readers;
};

} // namespace dream::runtime
