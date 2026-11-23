#include "dream/runtime/particles.h"

#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "fake_device.h"
#include "fake_platform.h"
#include "fake_renderer.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec3;
using doctest::Approx;

namespace {

constexpr platform::SurfaceSize k_size{800, 600};

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

// Gives each emitter one particle where the emitter is, as big as the seconds it was moved on by
// in all, and remembers what it was asked.
struct FakeParticleState {
    std::vector<particles::EmitterDesc> created;
    int alive_emitters = 0;
    std::vector<std::uint32_t> bursts;
    int restarts = 0;
};

class FakeParticles final : public particles::ParticleSystem {
public:
    explicit FakeParticles(FakeParticleState& state) : m_state(state) {}

    core::Result<particles::EmitterHandle> create_emitter(
        const particles::EmitterDesc& desc) override {
        if (desc.rate < 0.0f) {
            return core::Error{core::ErrorCode::InvalidArgument, "bad rate"};
        }
        m_state.created.push_back(desc);
        ++m_state.alive_emitters;
        m_emitters.push_back(Emitter{});
        return particles::EmitterHandle{.index = static_cast<std::uint32_t>(m_emitters.size() - 1),
                                        .generation = 1};
    }
    void destroy_emitter(particles::EmitterHandle) override {
        --m_state.alive_emitters;
    }
    void update(particles::EmitterHandle handle, const core::Mat4& transform, float seconds,
                bool emitting) override {
        Emitter& emitter = m_emitters[handle.index];
        emitter.age += seconds;
        emitter.drawn.clear();
        if (emitting) {
            emitter.drawn.push_back(particles::Particle{
                .position = core::transform_point(transform, Vec3{}),
                .size = emitter.age,
                .color = {1.0f, 0.5f, 0.25f, 1.0f},
                .rotation = 0.0f});
        }
    }
    void burst(particles::EmitterHandle, const core::Mat4&, std::uint32_t count) override {
        m_state.bursts.push_back(count);
    }
    void restart(particles::EmitterHandle handle) override {
        ++m_state.restarts;
        m_emitters[handle.index] = Emitter{};
    }
    std::span<const particles::Particle> particles(particles::EmitterHandle handle) const override {
        return m_emitters[handle.index].drawn;
    }
    bool finished(particles::EmitterHandle handle) const override {
        return m_emitters[handle.index].age > 10.0f;
    }

private:
    struct Emitter {
        float age = 0.0f;
        std::vector<particles::Particle> drawn;
    };
    FakeParticleState& m_state;
    std::vector<Emitter> m_emitters;
};

// Runs a Renderer and Particles inside a real App.
struct ParticleFixture {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;
    FakeParticleState particle_state;

    ParticleFixture() {
        state.assets["shaders/fake_renderer.txt"] = std::vector<std::byte>(4);
    }

    template <typename Body> void in_frame(Body body) {
        struct TestGame final : Game {
            Renderer renderer;
            Particles particles;
            FakeRendererState* renderer_state = nullptr;
            FakeParticleState* particle_state = nullptr;
            Body* run = nullptr;
            core::Result<void> on_start(App& app) override {
                if (auto ready = renderer.initialize(
                        app, std::make_unique<FakeRenderer>(*renderer_state));
                    !ready.has_value()) {
                    return ready;
                }
                return particles.initialize(app, std::make_unique<FakeParticles>(*particle_state));
            }
            void on_update(App&, const core::FrameTime&) override {}
            void on_render(App& app, rhi::CommandList& commands) override {
                (*run)(app, renderer, particles, commands);
            }
        };

        state.script = {k_end_of_frame};
        App app(std::make_unique<FakePlatform>(state), k_config,
                std::make_unique<FakeDevice>(device_state));
        TestGame game;
        game.renderer_state = &renderer_state;
        game.particle_state = &particle_state;
        game.run = &body;
        REQUIRE(app.run(game).has_value());
    }
};

} // namespace

TEST_CASE_FIXTURE(ParticleFixture, "Scene files set up particle emitters") {
    const std::string_view scene = R"(
[entities.fire.particle_emitter]
blend = "additive"
rate = 25.0
burst = 4
shape = "cone"
radius = 0.3
cone_angle = 90.0
lifetime = [0.5, 1.5]
speed = 2.0
rotation = [0.0, 180.0]
start_color = [3.0, 1.0, 0.2]
end_alpha = 0.25
gravity = [0.0, 1.0, 0.0]
local_space = true
seed = 9
time_scale = 0.5
)";
    ParticleEmitter emitter;
    in_frame([&](App& app, Renderer&, Particles&, rhi::CommandList&) {
        auto world = app.scenes().parse(scene);
        REQUIRE(world.has_value());
        emitter = world.value()->registry().get<ParticleEmitter>(world.value()->find("fire"));
    });
    const particles::EmitterDesc& desc = emitter.desc;
    CHECK(emitter.blend == renderer::ParticleBlend::Additive);
    CHECK(emitter.time_scale == Approx(0.5f));
    CHECK_FALSE(emitter.texture);
    CHECK(desc.rate == Approx(25.0f));
    CHECK(desc.burst == 4);
    CHECK(desc.shape == particles::EmitterShape::Cone);
    CHECK(desc.extents.x == Approx(0.3f));
    CHECK(desc.cone_angle == Approx(1.5707963f));
    CHECK(desc.lifetime.min == Approx(0.5f));
    CHECK(desc.lifetime.max == Approx(1.5f));
    CHECK(desc.speed.min == Approx(2.0f));
    CHECK(desc.speed.max == Approx(2.0f));
    CHECK(desc.rotation.max == Approx(3.14159265f));
    CHECK(desc.start_color.x == Approx(3.0f));
    CHECK(desc.start_color.w == Approx(1.0f));
    CHECK(desc.end_color.w == Approx(0.25f));
    CHECK(desc.gravity.y == Approx(1.0f));
    CHECK(desc.local_space);
    CHECK(desc.seed == 9);
}

TEST_CASE_FIXTURE(ParticleFixture, "Scene files with a wrong particle setting don't load") {
    std::vector<std::string> errors;
    in_frame([&](App& app, Renderer&, Particles&, rhi::CommandList&) {
        for (const std::string_view scene :
             {"[entities.a.particle_emitter]\nshape = \"donut\"\n",
              "[entities.a.particle_emitter]\nblend = \"multiply\"\n",
              "[entities.a.particle_emitter]\nburst = 2.5\n",
              "[entities.a.particle_emitter]\nlifetime = \"long\"\n",
              "[entities.a.particle_emitter]\ncolour = [1.0, 0.0, 0.0]\n",
              "[entities.a.particle_emitter]\ntexture = \"assets:/textures/missing.png\"\n"}) {
            auto world = app.scenes().parse(scene);
            CHECK_FALSE(world.has_value());
            if (!world.has_value()) {
                errors.push_back(world.error().message);
            }
        }
    });
    CHECK(errors.size() == 6);
}

TEST_CASE_FIXTURE(ParticleFixture, "Particles are moved on where their entity is, and drawn") {
    World world;
    in_frame([&](App&, Renderer& renderer, Particles& particles, rhi::CommandList& commands) {
        const Entity camera = world.create_entity();
        world.registry().emplace<Camera>(camera);
        const Entity smoke = world.create_entity();
        world.registry().get<Transform>(smoke).position = Vec3{1.0f, 2.0f, 3.0f};
        world.registry().emplace<ParticleEmitter>(
            smoke, ParticleEmitter{.blend = renderer::ParticleBlend::Additive, .time_scale = 2.0f});
        world.update_transforms();
        particles.update(world, 0.25);
        renderer.render(world, commands, k_size);
        CHECK(renderer.last_particle_count() == 0); // The fake renderer draws nothing.
        world.registry().clear();
    });
    REQUIRE_FALSE(renderer_state.frames.empty());
    const FakeFrame& frame = renderer_state.frames.back();
    REQUIRE(frame.particles.size() == 1);
    CHECK(frame.particles[0].blend == renderer::ParticleBlend::Additive);
    CHECK_FALSE(frame.particles[0].texture.is_valid());
    REQUIRE(frame.particles[0].particles.size() == 1);
    const renderer::ParticleQuad& quad = frame.particles[0].particles[0];
    CHECK(quad.position.z == Approx(3.0f));
    // A quarter of a second, at twice the speed.
    CHECK(quad.size == Approx(0.5f));
    CHECK(quad.color.y == Approx(0.5f));
}

TEST_CASE_FIXTURE(ParticleFixture, "Emitters start again when their settings change, and go with "
                                   "their entity") {
    World world;
    in_frame([&](App&, Renderer&, Particles& particles, rhi::CommandList&) {
        const Entity entity = world.create_entity();
        world.registry().emplace<ParticleEmitter>(entity);
        world.update_transforms();
        particles.update(world, 0.1);
        particles.update(world, 0.1);
        CHECK(particle_state.created.size() == 1);

        world.registry().get<ParticleEmitter>(entity).desc.rate = 99.0f;
        particles.update(world, 0.1);
        REQUIRE(particle_state.created.size() == 2);
        CHECK(particle_state.created[1].rate == Approx(99.0f));
        CHECK(particle_state.alive_emitters == 1);

        particles.burst(world, entity, 12);
        particles.restart(world, entity);
        CHECK(particle_state.bursts == std::vector<std::uint32_t>{12});
        CHECK(particle_state.restarts == 1);
        CHECK(world.registry().get<ParticleEmitter>(entity).particles.empty());
        CHECK_FALSE(particles.finished(world, entity));

        // Settings the module refuses leave the entity without particles, and don't stop others.
        world.registry().get<ParticleEmitter>(entity).desc.rate = -1.0f;
        particles.update(world, 0.1);
        CHECK(world.registry().get<ParticleEmitter>(entity).particles.empty());
        CHECK(particle_state.alive_emitters == 0);

        world.registry().get<ParticleEmitter>(entity).desc.rate = 5.0f;
        particles.update(world, 0.1);
        CHECK(particle_state.alive_emitters == 1);
        world.destroy_entity(entity);
        particles.update(world, 0.1);
        CHECK(particle_state.alive_emitters == 0);
    });
}

TEST_CASE_FIXTURE(ParticleFixture, "Emitters of a destroyed world go, and a new world at its "
                                   "address starts with none") {
    in_frame([&](App&, Renderer&, Particles& particles, rhi::CommandList&) {
        World paused;
        const Entity frozen = paused.create_entity();
        paused.registry().emplace<ParticleEmitter>(frozen);
        paused.update_transforms();
        particles.update(paused, 0.5);
        {
            auto first = std::make_unique<World>();
            const Entity entity = first->create_entity();
            first->registry().emplace<ParticleEmitter>(entity);
            first->update_transforms();
            particles.update(*first, 2.0);
            CHECK(particle_state.alive_emitters == 2);
            first.reset();
            // Very often at the same address, with the same entity.
            auto second = std::make_unique<World>();
            const Entity again = second->create_entity();
            second->registry().emplace<ParticleEmitter>(again);
            second->update_transforms();
            particles.update(*second, 0.25);
            // A new emitter, not the first world's, which is released.
            CHECK(particle_state.created.size() == 3);
            CHECK(particle_state.alive_emitters == 2);
            const auto& drawn = second->registry().get<ParticleEmitter>(again).particles;
            REQUIRE(drawn.size() == 1);
            CHECK(drawn[0].size == Approx(0.25f));
        }
        // A world that isn't updated for a while keeps its emitter.
        World other;
        for (int i = 0; i < 1000; ++i) {
            particles.update(other, 0.01);
        }
        particles.update(paused, 0.5);
        const auto& drawn = paused.registry().get<ParticleEmitter>(frozen).particles;
        REQUIRE(drawn.size() == 1);
        CHECK(drawn[0].size == Approx(1.0f));
    });
}

TEST_CASE_FIXTURE(ParticleFixture, "Warming up moves emitters on in steps of 1/60 s") {
    World world;
    float size = 0.0f;
    in_frame([&](App&, Renderer&, Particles& particles, rhi::CommandList&) {
        const Entity entity = world.create_entity();
        world.registry().emplace<ParticleEmitter>(entity);
        world.update_transforms();
        particles.warm_up(world, 0.5);
        size = world.registry().get<ParticleEmitter>(entity).particles.at(0).size;
        world.registry().clear();
    });
    CHECK(size == Approx(0.5f).epsilon(0.001));
}
