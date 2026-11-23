#pragma once

#include "dream/core/result.h"
#include "dream/particles/particles.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace dream::runtime {

class App;

// Gives off particles from its entity, placed and turned by the entity's transform (ADR 0038).
// Particles::update moves them on; the renderer draws them. Change `desc` at any time: the emitter
// starts again with the new settings.
struct ParticleEmitter {
    particles::EmitterDesc desc;
    // sRGB colors with straight alpha, multiplied by each particle's color. Without one, particles
    // are soft round dots.
    AssetRef<TextureAsset> texture;
    renderer::ParticleBlend blend = renderer::ParticleBlend::Alpha;
    // Whether new particles are born. Those alive carry on either way.
    bool emitting = true;
    // How fast time passes for it: 0 holds its particles still.
    float time_scale = 1.0f;

    // Its live particles in world space, as the last update left them: what the renderer draws.
    std::vector<renderer::ParticleQuad> particles;
};

// Moves particle emitters on with the project's particles module, chosen by its `particles` list
// in dream.toml, such as `particles = ["cpu", "null"]` (ADR 0038). Games call update once per
// frame, after the world's transforms and before rendering:
//
//     on_start:   renderer.initialize(app);           // First: it loads particle textures.
//                 particles.initialize(app);
//     on_update:  world->update_transforms();
//                 particles.update(*world, time.delta_seconds);
//
// initialize lets scene files use the component "particle_emitter"; docs/guides/particles.md lists
// its settings and explains how to make effects:
//
//     [entities.campfire.particle_emitter]
//     texture = "assets:/textures/flame.png"   # Optional: soft dots without one.
//     blend = "additive"                       # Or "alpha", the default.
//     rate = 40.0
//     lifetime = [0.6, 1.0]                    # A number, or [min, max] for each particle.
//     shape = "cone"
//     speed = [0.8, 1.4]
//     size = [0.3, 0.5]
//     end_size = 0.2
//     start_color = [3.0, 1.2, 0.3]
//     end_color = [1.0, 0.2, 0.05]
//     end_alpha = 0.0
class Particles {
public:
    Particles();
    ~Particles();

    Particles(const Particles&) = delete;
    Particles& operator=(const Particles&) = delete;

    // Creates the configured particles module. Game::on_start is the place for it, after
    // Renderer::initialize and before any scene with particle emitters loads.
    [[nodiscard]] core::Result<void> initialize(App& app);

    // The same with a given module, for tests or a game that makes its own.
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<particles::ParticleSystem> system);

    // Moves every emitter of the world on by `seconds`, each by its time scale.
    void update(World& world, double seconds);

    // The same in steps of 1/60 s, for effects that should already be under way when a scene
    // appears, and for screenshot checks, which need the same particles every run.
    void warm_up(World& world, double seconds);

    // Gives off `count` particles at once from the entity's emitter, as an explosion or a hit
    // does. Nothing happens if it has none, or before its first update.
    void burst(World& world, Entity entity, std::uint32_t count);

    // Removes the entity's particles and starts its emitter's first cycle again.
    void restart(World& world, Entity entity);

    // Whether the entity's emitter doesn't loop, has ended, and its last particle has died: a
    // finished one-off effect, whose entity the game can now remove.
    [[nodiscard]] bool finished(const World& world, Entity entity) const;

    // Null until initialized.
    [[nodiscard]] particles::ParticleSystem* system() noexcept {
        return m_system.get();
    }

private:
    // One emitter of the module, kept while its entity is seen each update.
    struct Live {
        particles::EmitterHandle emitter;
        // The settings it was made with, to see when they change.
        particles::EmitterDesc desc;
        std::uint64_t seen = 0;
    };
    // By World::id and entity.
    using Key = std::pair<std::uint64_t, std::uint32_t>;

    [[nodiscard]] Live* find(const World& world, Entity entity);
    [[nodiscard]] const Live* find(const World& world, Entity entity) const;
    void release(Live& live);

    std::unique_ptr<particles::ParticleSystem> m_system;
    std::map<Key, Live> m_live;
    WorldLifetimes m_worlds;
    std::uint64_t m_update = 0;
};

} // namespace dream::runtime
