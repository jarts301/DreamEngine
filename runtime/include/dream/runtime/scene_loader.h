#pragma once

#include "dream/core/config.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/virtual_file_system.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // A list of strings, or of numbers, which may be empty. Fails if the value isn't one.
    [[nodiscard]] core::Result<std::vector<std::string>> strings(std::string_view key) const;
    [[nodiscard]] core::Result<std::vector<float>> numbers(std::string_view key) const;

    // The tables inside this one, by name, such as each clip of [entities.hero.animator.clips].
    [[nodiscard]] std::vector<std::string> tables() const;
    // The settings directly in this table, by name.
    [[nodiscard]] std::vector<std::string> keys() const;
    // A table inside this one, read the same way.
    [[nodiscard]] ComponentData child(std::string_view key) const {
        return ComponentData(m_config, full_key(key));
    }

    // Fails if the table has a setting or a table inside it not in `known`, which is usually a
    // typo.
    [[nodiscard]] core::Result<void>
    check_keys(std::initializer_list<std::string_view> known) const;

private:
    [[nodiscard]] std::string full_key(std::string_view key) const;

    const core::Config& m_config;
    std::string m_prefix;
};

// A mistake in a scene file, such as a setting out of range.
[[nodiscard]] core::Error scene_error(std::string message);

// Reads a component's settings in turn, keeping the first error, so a component reader can list
// them all and check once. Each value keeps what it holds when its setting is absent.
//
//     SettingReader read(context.data);
//     read.number("speed", mover.speed);
//     read.flag("loop", mover.loop);
//     if (auto done = read.result(); !done.has_value()) {
//         return done;
//     }
class SettingReader {
public:
    explicit SettingReader(const ComponentData& data) : m_data(data) {}

    // A value read some other way, such as with one of ComponentData's getters. `read` isn't
    // called once a setting has failed.
    template <typename Value, typename Read> void take(Read&& read, Value& into) {
        if (m_failed.has_value()) {
            return;
        }
        auto result = std::forward<Read>(read)();
        if (!result.has_value()) {
            m_failed = result.error();
            return;
        }
        into = std::move(result).value();
    }

    void number(std::string_view key, float& into) {
        take([&] { return m_data.number(key, into); }, into);
    }
    void flag(std::string_view key, bool& into) {
        take([&] { return m_data.flag(key, into); }, into);
    }
    void vec2(std::string_view key, core::Vec2& into) {
        take([&] { return m_data.vec2(key, into); }, into);
    }
    void vec3(std::string_view key, core::Vec3& into) {
        take([&] { return m_data.vec3(key, into); }, into);
    }
    void text(std::string_view key, std::string& into) {
        take([&] { return m_data.text(key, into); }, into);
    }
    // An angle in degrees, as radians, set only when the setting is present.
    void angle(std::string_view key, std::optional<float>& into);
    // A whole number, 0 or more, up to 2^24, which a float holds exactly.
    void whole(std::string_view key, std::uint32_t& into);
    // A whole number from -10^6 to 10^6.
    void integer(std::string_view key, std::int32_t& into);

    // Fails with a scene_error, unless a setting already has.
    void fail(std::string message);

    [[nodiscard]] bool failed() const noexcept {
        return m_failed.has_value();
    }
    [[nodiscard]] const ComponentData& data() const noexcept {
        return m_data;
    }
    [[nodiscard]] core::Result<void> result() const {
        if (m_failed.has_value()) {
            return *m_failed;
        }
        return {};
    }

private:
    const ComponentData& m_data;
    std::optional<core::Error> m_failed;
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
