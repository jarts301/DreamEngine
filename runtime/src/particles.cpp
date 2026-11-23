#include "dream/runtime/particles.h"

#include "dream/core/log.h"
#include "dream/core/math/vector.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dream::runtime {
namespace {

// A setting that is one number for every particle, or [min, max] to pick from.
core::Result<particles::Range> read_range(const ComponentData& data, std::string_view key,
                                          particles::Range fallback, float scale = 1.0f) {
    if (!data.has(key)) {
        return fallback;
    }
    if (const core::Result<float> single = data.number(key, 0.0f); single.has_value()) {
        return particles::Range{single.value() * scale, single.value() * scale};
    }
    const core::Result<core::Vec2> pair = data.vec2(key, {});
    if (!pair.has_value()) {
        return scene_error(std::format("'{}' must be a number or [min, max]", key));
    }
    return particles::Range{pair.value().x * scale, pair.value().y * scale};
}

core::Result<particles::EmitterShape> read_shape(std::string_view name) {
    if (name == "point") {
        return particles::EmitterShape::Point;
    }
    if (name == "sphere") {
        return particles::EmitterShape::Sphere;
    }
    if (name == "box") {
        return particles::EmitterShape::Box;
    }
    if (name == "cone") {
        return particles::EmitterShape::Cone;
    }
    return scene_error(std::format(
        "A particle emitter's shape is \"point\", \"sphere\", \"box\" or \"cone\", not \"{}\"", name));
}

core::Result<void> read_particle_emitter(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (core::Result<void> known = data.check_keys(
            {"texture",     "blend",       "emitting",    "time_scale", "rate",      "burst",
             "duration",    "loop",        "max_particles", "shape",    "extents",   "radius",
             "cone_angle",  "lifetime",    "speed",       "size",       "end_size",  "rotation",
             "spin",        "start_color", "start_alpha", "end_color",  "end_alpha", "gravity",
             "drag",        "local_space", "seed"});
        !known.has_value()) {
        return known;
    }
    ParticleEmitter emitter;
    particles::EmitterDesc& desc = emitter.desc;

    SettingReader read(data);

    std::string texture;
    std::string blend = "alpha";
    std::string shape = "point";
    read.text("texture", texture);
    read.text("blend", blend);
    read.text("shape", shape);
    read.flag("emitting", emitter.emitting);
    read.number("time_scale", emitter.time_scale);
    read.number("rate", desc.rate);
    float burst = static_cast<float>(desc.burst);
    float max_particles = static_cast<float>(desc.max_particles);
    float seed = static_cast<float>(desc.seed);
    read.number("burst", burst);
    read.number("max_particles", max_particles);
    read.number("seed", seed);
    read.number("duration", desc.duration);
    read.flag("loop", desc.loop);
    read.vec3("extents", desc.extents);
    float radius = desc.extents.x;
    read.number("radius", radius);
    float cone_degrees = desc.cone_angle * 180.0f / core::k_pi;
    read.number("cone_angle", cone_degrees);
    read.take([&] { return read_range(data, "lifetime", desc.lifetime); }, desc.lifetime);
    read.take([&] { return read_range(data, "speed", desc.speed); }, desc.speed);
    read.take([&] { return read_range(data, "size", desc.size); }, desc.size);
    read.number("end_size", desc.end_size);
    // Degrees in the file, radians in the settings.
    read.take([&] { return read_range(data, "rotation", {}, core::k_pi / 180.0f); }, desc.rotation);
    read.take([&] { return read_range(data, "spin", {}, core::k_pi / 180.0f); }, desc.spin);
    core::Vec3 start_color{desc.start_color.x, desc.start_color.y, desc.start_color.z};
    core::Vec3 end_color{desc.end_color.x, desc.end_color.y, desc.end_color.z};
    read.vec3("start_color", start_color);
    read.vec3("end_color", end_color);
    read.number("start_alpha", desc.start_color.w);
    read.number("end_alpha", desc.end_color.w);
    read.vec3("gravity", desc.gravity);
    read.number("drag", desc.drag);
    read.flag("local_space", desc.local_space);
    if (core::Result<void> done = read.result(); !done.has_value()) {
        return done;
    }

    for (const float count : {burst, max_particles, seed}) {
        if (!(count >= 0.0f && count <= 4294967295.0f) || count != std::floor(count)) {
            return scene_error("A particle emitter's burst, max_particles and seed are whole "
                               "numbers, 0 or more");
        }
    }
    desc.burst = static_cast<std::uint32_t>(burst);
    desc.max_particles = static_cast<std::uint32_t>(max_particles);
    desc.seed = static_cast<std::uint32_t>(seed);
    if (data.has("radius")) {
        desc.extents.x = radius;
    }
    desc.cone_angle = cone_degrees * core::k_pi / 180.0f;
    desc.start_color = core::Vec4{start_color.x, start_color.y, start_color.z, desc.start_color.w};
    desc.end_color = core::Vec4{end_color.x, end_color.y, end_color.z, desc.end_color.w};
    if (!(emitter.time_scale >= 0.0f)) {
        return scene_error("A particle emitter's time_scale must be 0 or more");
    }

    const core::Result<particles::EmitterShape> shape_value = read_shape(shape);
    if (!shape_value.has_value()) {
        return shape_value.error();
    }
    desc.shape = shape_value.value();
    if (blend == "alpha") {
        emitter.blend = renderer::ParticleBlend::Alpha;
    } else if (blend == "additive") {
        emitter.blend = renderer::ParticleBlend::Additive;
    } else {
        return scene_error(std::format(
            "A particle emitter's blend is \"alpha\" or \"additive\", not \"{}\"", blend));
    }

    if (!texture.empty()) {
        core::Result<AssetRef<TextureAsset>> loaded = context.assets.load<TextureAsset>(texture);
        if (!loaded.has_value()) {
            return core::Error{loaded.error().code,
                               std::format("Particle texture '{}': {} (is the Renderer initialized "
                                           "before scenes load?)",
                                           texture, loaded.error().message)};
        }
        emitter.texture = std::move(loaded).value();
    }
    context.world.registry().emplace_or_replace<ParticleEmitter>(context.entity,
                                                                std::move(emitter));
    return {};
}

} // namespace

Particles::Particles() = default;

Particles::~Particles() {
    for (auto& [key, live] : m_live) {
        release(live);
    }
}

core::Result<void> Particles::initialize(App& app) {
    core::Result<std::unique_ptr<particles::ParticleSystem>> system =
        app.create_module<particles::ParticleSystem>();
    if (!system.has_value()) {
        return system.error();
    }
    return initialize(app, std::move(system).value());
}

core::Result<void> Particles::initialize(App& app,
                                         std::unique_ptr<particles::ParticleSystem> system) {
    if (system == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "Particles needs a module"};
    }
    for (auto& [key, live] : m_live) {
        release(live);
    }
    m_live.clear();
    m_system = std::move(system);
    app.scenes().register_component("particle_emitter", &read_particle_emitter);
    return {};
}

void Particles::update(World& world, double seconds) {
    if (m_system == nullptr) {
        return;
    }
    DREAM_PROFILE_ZONE("Particles");
    ++m_update;
    const std::uint64_t world_id = world.id();
    std::uint32_t alive = 0;
    for (const auto [entity, transform, emitter] :
         world.registry().view<const WorldTransform, ParticleEmitter>().each()) {
        Live& live = m_live[{world_id, static_cast<std::uint32_t>(entt::to_integral(entity))}];
        const bool fresh = live.seen == 0;
        live.seen = m_update;
        // New, or with changed settings: a new emitter of the module.
        if (fresh || live.desc != emitter.desc) {
            release(live);
            live.desc = emitter.desc;
            core::Result<particles::EmitterHandle> made = m_system->create_emitter(emitter.desc);
            if (made.has_value()) {
                live.emitter = made.value();
            } else {
                DREAM_LOG_ERROR("A particle emitter can't be made: {}", made.error().message);
            }
        }
        emitter.particles.clear();
        if (!live.emitter.is_valid()) {
            continue;
        }
        m_system->update(live.emitter, transform.matrix,
                         static_cast<float>(seconds) * emitter.time_scale, emitter.emitting);
        for (const particles::Particle& particle : m_system->particles(live.emitter)) {
            emitter.particles.push_back(renderer::ParticleQuad{.position = particle.position,
                                                               .size = particle.size,
                                                               .color = particle.color,
                                                               .rotation = particle.rotation});
        }
        alive += static_cast<std::uint32_t>(emitter.particles.size());
    }
    // Emitters whose entities are gone, or lost their emitter, from this world, and those of
    // destroyed worlds.
    m_worlds.note(world);
    for (auto it = m_live.begin(); it != m_live.end();) {
        const bool gone = it->first.first == world_id ? it->second.seen != m_update
                                                      : m_worlds.destroyed(it->first.first);
        if (gone) {
            release(it->second);
            it = m_live.erase(it);
        } else {
            ++it;
        }
    }
    m_worlds.prune();
    DREAM_PROFILE_VALUE("Particles alive", alive);
}

void Particles::warm_up(World& world, double seconds) {
    constexpr double k_step = 1.0 / 60.0;
    for (double done = 0.0; done + k_step * 0.5 < seconds; done += k_step) {
        update(world, k_step);
    }
}

Particles::Live* Particles::find(const World& world, Entity entity) {
    const auto found = m_live.find(
        {world.id(), static_cast<std::uint32_t>(entt::to_integral(entity))});
    return found != m_live.end() && found->second.emitter.is_valid() ? &found->second : nullptr;
}

const Particles::Live* Particles::find(const World& world, Entity entity) const {
    const auto found = m_live.find(
        {world.id(), static_cast<std::uint32_t>(entt::to_integral(entity))});
    return found != m_live.end() && found->second.emitter.is_valid() ? &found->second : nullptr;
}

void Particles::burst(World& world, Entity entity, std::uint32_t count) {
    Live* const live = find(world, entity);
    const WorldTransform* const transform = world.registry().try_get<WorldTransform>(entity);
    if (live == nullptr || transform == nullptr || m_system == nullptr) {
        return;
    }
    m_system->burst(live->emitter, transform->matrix, count);
}

void Particles::restart(World& world, Entity entity) {
    if (Live* const live = find(world, entity); live != nullptr && m_system != nullptr) {
        m_system->restart(live->emitter);
        if (auto* const emitter = world.registry().try_get<ParticleEmitter>(entity)) {
            emitter->particles.clear();
        }
    }
}

bool Particles::finished(const World& world, Entity entity) const {
    const Live* const live = find(world, entity);
    return live != nullptr && m_system != nullptr && m_system->finished(live->emitter);
}

void Particles::release(Live& live) {
    if (m_system != nullptr && live.emitter.is_valid()) {
        m_system->destroy_emitter(live.emitter);
    }
    live.emitter = {};
}

} // namespace dream::runtime
