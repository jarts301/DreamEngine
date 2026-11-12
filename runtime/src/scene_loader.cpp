#include "dream/runtime/scene_loader.h"

#include "dream/core/math/quaternion.h"
#include "dream/core/profile.h"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace dream::runtime {
namespace {

core::Error scene_error(std::string_view message) {
    return core::Error{core::ErrorCode::ParseError, std::string(message)};
}

core::Quat quat_from_euler_degrees(core::Vec3 degrees) {
    const core::Quat x =
        core::quat_from_axis_angle(core::Vec3{1.0f, 0.0f, 0.0f}, core::radians(degrees.x));
    const core::Quat y =
        core::quat_from_axis_angle(core::Vec3{0.0f, 1.0f, 0.0f}, core::radians(degrees.y));
    const core::Quat z =
        core::quat_from_axis_angle(core::Vec3{0.0f, 0.0f, 1.0f}, core::radians(degrees.z));
    return core::normalized(y * x * z);
}

core::Result<void> read_transform(ComponentContext& context) {
    const core::Result<core::Vec3> position = context.data.vec3("position", core::Vec3{});
    if (!position.has_value()) {
        return position.error();
    }
    const core::Result<core::Vec3> rotation = context.data.vec3("rotation", core::Vec3{});
    if (!rotation.has_value()) {
        return rotation.error();
    }
    const core::Result<core::Vec3> scale =
        context.data.vec3("scale", core::Vec3{1.0f, 1.0f, 1.0f});
    if (!scale.has_value()) {
        return scale.error();
    }

    context.world.registry().replace<Transform>(context.entity, position.value(),
                                                quat_from_euler_degrees(rotation.value()),
                                                scale.value());
    return {};
}

} // namespace

std::string ComponentData::full_key(std::string_view key) const {
    return std::format("{}.{}", m_prefix, key);
}

core::Result<float> ComponentData::number(std::string_view key, float fallback) const {
    const std::string name = full_key(key);
    if (!m_config.contains(name)) {
        return fallback;
    }
    const std::optional<double> value = m_config.get_float(name);
    if (!value.has_value()) {
        return scene_error(std::format("'{}' must be a number", key));
    }
    return static_cast<float>(*value);
}

core::Result<bool> ComponentData::flag(std::string_view key, bool fallback) const {
    const std::string name = full_key(key);
    if (!m_config.contains(name)) {
        return fallback;
    }
    const std::optional<bool> value = m_config.get_bool(name);
    if (!value.has_value()) {
        return scene_error(std::format("'{}' must be true or false", key));
    }
    return *value;
}

core::Result<core::Vec2> ComponentData::vec2(std::string_view key, core::Vec2 fallback) const {
    const std::string name = full_key(key);
    if (!m_config.contains(name)) {
        return fallback;
    }
    const auto values = m_config.get_float_list(name);
    if (!values.has_value() || values->size() != 2) {
        return scene_error(std::format("'{}' must be a list of two numbers", key));
    }
    return core::Vec2{static_cast<float>((*values)[0]), static_cast<float>((*values)[1])};
}

core::Result<core::Vec3> ComponentData::vec3(std::string_view key, core::Vec3 fallback) const {
    const std::string name = full_key(key);
    if (!m_config.contains(name)) {
        return fallback;
    }
    const auto values = m_config.get_float_list(name);
    if (!values.has_value() || values->size() != 3) {
        return scene_error(std::format("'{}' must be a list of three numbers", key));
    }
    return core::Vec3{static_cast<float>((*values)[0]), static_cast<float>((*values)[1]),
                      static_cast<float>((*values)[2])};
}

core::Result<std::string> ComponentData::text(std::string_view key) const {
    const std::optional<std::string_view> value = m_config.get_string(full_key(key));
    if (!value.has_value()) {
        return scene_error(std::format("'{}' must be a string", key));
    }
    return std::string(*value);
}

core::Result<std::string> ComponentData::text(std::string_view key,
                                              std::string_view fallback) const {
    if (!has(key)) {
        return std::string(fallback);
    }
    return text(key);
}

core::Result<void>
ComponentData::check_keys(std::initializer_list<std::string_view> known) const {
    for (const std::string_view key : m_config.keys_in(m_prefix)) {
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            return scene_error(std::format("unknown setting '{}'", key));
        }
    }
    if (const std::vector<std::string_view> tables = m_config.tables_in(m_prefix); !tables.empty()) {
        return scene_error(std::format("unexpected table '{}'", tables.front()));
    }
    return {};
}

SceneLoader::SceneLoader(VirtualFileSystem& files, AssetManager& assets)
    : m_files(files), m_assets(assets) {
    register_component("transform", &read_transform);
}

void SceneLoader::register_component(std::string name, ComponentReader reader) {
    m_readers.insert_or_assign(std::move(name), std::move(reader));
}

core::Result<std::unique_ptr<World>> SceneLoader::load(std::string_view path) {
    DREAM_PROFILE_ZONE_DETAIL("Load scene", path);
    const auto bytes = m_files.read(path);
    if (!bytes.has_value()) {
        return bytes.error();
    }

    core::Result<std::unique_ptr<World>> world = parse(
        std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    if (!world.has_value()) {
        return core::Error{world.error().code,
                           std::format("Scene '{}': {}", path, world.error().message)};
    }
    return world;
}

core::Result<std::unique_ptr<World>> SceneLoader::parse(std::string_view text) {
    const core::Result<core::Config> parsed = core::Config::parse(text);
    if (!parsed.has_value()) {
        return parsed.error();
    }
    const core::Config& config = parsed.value();

    auto world = std::make_unique<World>();
    const std::vector<std::string_view> names = config.tables_in("entities");

    // Every entity exists before any parent link, so the order in the file doesn't matter.
    for (const std::string_view name : names) {
        world->create_entity(name);
    }

    for (const std::string_view name : names) {
        const std::string prefix = std::format("entities.{}", name);
        const Entity entity = world->find(name);

        for (const std::string_view key : config.keys_in(prefix)) {
            if (key != "parent") {
                return scene_error(std::format("Entity '{}': unknown setting '{}'", name, key));
            }
            const std::optional<std::string_view> parent_name =
                config.get_string(std::format("{}.parent", prefix));
            if (!parent_name.has_value()) {
                return scene_error(std::format("Entity '{}': 'parent' must be a string", name));
            }
            const Entity parent = world->find(*parent_name);
            if (parent == k_no_entity) {
                return scene_error(
                    std::format("Entity '{}': no entity named '{}'", name, *parent_name));
            }
            if (const core::Result<void> linked = world->set_parent(entity, parent);
                !linked.has_value()) {
                return scene_error(
                    std::format("Entity '{}': {}", name, linked.error().message));
            }
        }

        for (const std::string_view component : config.tables_in(prefix)) {
            const auto reader = m_readers.find(component);
            if (reader == m_readers.end()) {
                return scene_error(
                    std::format("Entity '{}': unknown component '{}'", name, component));
            }

            const ComponentData data(config, std::format("{}.{}", prefix, component));
            ComponentContext context{*world, entity, data, m_assets};
            if (const core::Result<void> read = reader->second(context); !read.has_value()) {
                return core::Error{read.error().code,
                                   std::format("Entity '{}', component '{}': {}", name, component,
                                               read.error().message)};
            }
        }
    }

    world->update_transforms();
    return world;
}

} // namespace dream::runtime
