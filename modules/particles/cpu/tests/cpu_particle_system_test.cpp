#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/module_registry.h"
#include "dream/particles/particles.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_particles_cpu(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::particles;
using doctest::Approx;

namespace {

std::unique_ptr<ParticleSystem> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_particles_cpu(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"cpu"};
    auto created = registry.create<ParticleSystem>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

EmitterHandle emitter(ParticleSystem& system, const EmitterDesc& desc) {
    const core::Result<EmitterHandle> made = system.create_emitter(desc);
    REQUIRE(made.has_value());
    return made.value();
}

const core::Mat4 k_identity = core::translation(core::Vec3{});

} // namespace

TEST_CASE("Particles are born at the emitter's rate") {
    auto system = make();
    const EmitterHandle handle = emitter(*system, EmitterDesc{.rate = 10.0f,
                                                              .lifetime = {5.0f, 5.0f}});
    for (int step = 0; step < 10; ++step) {
        system->update(handle, k_identity, 0.1f, true);
    }
    CHECK(system->particles(handle).size() == 10);
}

TEST_CASE("Particles die at the end of their lifetime") {
    auto system = make();
    const EmitterHandle handle = emitter(
        *system, EmitterDesc{.rate = 0.0f, .burst = 5, .lifetime = {0.5f, 0.5f}});
    system->update(handle, k_identity, 0.1f, true);
    CHECK(system->particles(handle).size() == 5);
    for (int step = 0; step < 5; ++step) {
        system->update(handle, k_identity, 0.1f, true);
    }
    CHECK(system->particles(handle).empty());
}

TEST_CASE("No more than max_particles are alive") {
    auto system = make();
    const EmitterHandle handle = emitter(
        *system, EmitterDesc{.rate = 1000.0f, .max_particles = 50, .lifetime = {10.0f, 10.0f}});
    system->update(handle, k_identity, 0.2f, true);
    CHECK(system->particles(handle).size() == 50);
}

TEST_CASE("Gravity and speed move particles, and color and size change over their life") {
    auto system = make();
    const EmitterHandle handle = emitter(
        *system, EmitterDesc{.rate = 0.0f,
                             .burst = 1,
                             .shape = EmitterShape::Cone,
                             .cone_angle = 0.0f,
                             .lifetime = {2.0f, 2.0f},
                             .speed = {2.0f, 2.0f},
                             .size = {1.0f, 1.0f},
                             .end_size = 3.0f,
                             .start_color = {1.0f, 0.0f, 0.0f, 1.0f},
                             .end_color = {0.0f, 0.0f, 1.0f, 0.0f},
                             .gravity = {1.0f, 0.0f, 0.0f}});
    // Born at the origin, going up at 2 m/s, then pushed along +X.
    system->update(handle, core::translation(core::Vec3{0.0f, 0.0f, 5.0f}), 0.0f, true);
    for (int step = 0; step < 10; ++step) {
        system->update(handle, k_identity, 0.1f, true);
    }
    REQUIRE(system->particles(handle).size() == 1);
    const Particle& particle = system->particles(handle)[0];
    // Halfway through its life: twice as big, colors half mixed.
    CHECK(particle.size == Approx(2.0f));
    CHECK(particle.color.x == Approx(0.5f));
    CHECK(particle.color.z == Approx(0.5f));
    CHECK(particle.color.w == Approx(0.5f));
    // It stays where it was born in the world, at z = 5, after the emitter moved.
    CHECK(particle.position.z == Approx(5.0f));
    CHECK(particle.position.y == Approx(2.0f).epsilon(0.01));
    CHECK(particle.position.x > 0.3f);
}

TEST_CASE("Local-space particles follow their emitter") {
    auto system = make();
    const EmitterHandle handle = emitter(*system, EmitterDesc{.rate = 0.0f,
                                                              .burst = 1,
                                                              .speed = {0.0f, 0.0f},
                                                              .local_space = true});
    system->update(handle, k_identity, 0.0f, true);
    system->update(handle, core::translation(core::Vec3{3.0f, 0.0f, 0.0f}), 0.1f, true);
    REQUIRE(system->particles(handle).size() == 1);
    CHECK(system->particles(handle)[0].position.x == Approx(3.0f));
}

TEST_CASE("A one-off burst finishes once its particles have died") {
    auto system = make();
    const EmitterHandle handle = emitter(*system, EmitterDesc{.rate = 100.0f,
                                                              .burst = 8,
                                                              .loop = false,
                                                              .lifetime = {0.3f, 0.3f}});
    system->update(handle, k_identity, 0.1f, true);
    // Only the burst: the rate doesn't apply to a one-off.
    CHECK(system->particles(handle).size() == 8);
    CHECK_FALSE(system->finished(handle));
    for (int step = 0; step < 4; ++step) {
        system->update(handle, k_identity, 0.1f, true);
    }
    CHECK(system->finished(handle));

    system->restart(handle);
    CHECK_FALSE(system->finished(handle));
    system->update(handle, k_identity, 0.0f, true);
    CHECK(system->particles(handle).size() == 8);
}

TEST_CASE("A looping emitter bursts again each cycle") {
    auto system = make();
    const EmitterHandle handle = emitter(*system, EmitterDesc{.rate = 0.0f,
                                                              .burst = 3,
                                                              .duration = 1.0f,
                                                              .lifetime = {10.0f, 10.0f}});
    for (int step = 0; step < 25; ++step) {
        system->update(handle, k_identity, 0.1f, true);
    }
    // At the start, after 1 s and after 2 s.
    CHECK(system->particles(handle).size() == 9);
}

TEST_CASE("Nothing is born while an emitter isn't emitting") {
    auto system = make();
    const EmitterHandle handle = emitter(*system, EmitterDesc{.rate = 50.0f});
    system->update(handle, k_identity, 0.5f, false);
    CHECK(system->particles(handle).empty());
    system->burst(handle, k_identity, 4);
    CHECK(system->particles(handle).size() == 4);
}

TEST_CASE("The same seed gives the same particles") {
    auto system = make();
    const EmitterDesc desc{.rate = 30.0f,
                           .shape = EmitterShape::Sphere,
                           .extents = {1.0f, 0.0f, 0.0f},
                           .speed = {0.5f, 2.0f},
                           .seed = 7};
    const EmitterHandle first = emitter(*system, desc);
    const EmitterHandle second = emitter(*system, desc);
    for (int step = 0; step < 20; ++step) {
        system->update(first, k_identity, 1.0f / 60.0f, true);
        system->update(second, k_identity, 1.0f / 60.0f, true);
    }
    REQUIRE(system->particles(first).size() == system->particles(second).size());
    REQUIRE_FALSE(system->particles(first).empty());
    CHECK(system->particles(first).back().position.x ==
          system->particles(second).back().position.x);
}

TEST_CASE("Emitters with impossible settings aren't made") {
    auto system = make();
    CHECK_FALSE(system->create_emitter(EmitterDesc{.rate = -1.0f}).has_value());
    CHECK_FALSE(system->create_emitter(EmitterDesc{.max_particles = 0}).has_value());
    CHECK_FALSE(system->create_emitter(EmitterDesc{.lifetime = {0.0f, 1.0f}}).has_value());
    CHECK_FALSE(system->create_emitter(EmitterDesc{.speed = {2.0f, 1.0f}}).has_value());
    CHECK_FALSE(system->create_emitter(EmitterDesc{.cone_angle = 4.0f}).has_value());
}
