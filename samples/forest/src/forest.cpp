// A forest (docs/adr/0031, 0032, 0042): the game plants 3,000 trees and 600 rocks from three
// models, each an entity with a MeshRenderer. The renderer does the rest:
//
// - Instancing: copies of a model are drawn together, so the forest takes a handful of draws, not
//   thousands; a tint per copy still varies their color.
// - Culling: what is outside the view isn't drawn, nor shadowed if it can't shadow what is seen.
// - Levels of detail: dream.toml lists a mesh_simplifier, so the build makes simpler versions of
//   each model, and far trees use them.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, stops the camera's
// drift. Escape quits.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <numbers>
#include <string>

using namespace dream;
using core::Vec3;

namespace {

constexpr int k_trees = 3000;
constexpr int k_rocks = 600;
// Half the forest's width, in meters.
constexpr float k_half_size = 80.0f;
constexpr float k_clearing = 14.0f;

// The same forest every run, so checks compare the same picture: a small linear congruential
// generator, rather than std's, whose distributions differ between standard libraries.
class Random {
public:
    float next() {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>(m_state >> 40) / static_cast<float>(1ULL << 24);
    }
    float between(float low, float high) {
        return low + (high - low) * next();
    }

private:
    std::uint64_t m_state = 12345;
};

class Forest final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/forest.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        if (auto planted = plant(app); !planted.has_value()) {
            return planted;
        }
        m_camera.attach(*m_world);
        m_camera.max_distance = 120.0f;

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        // Checks compare a picture, so the camera holds still there.
        m_camera.auto_turn = config.get_bool("demo.hold").value_or(false) ? 0.0f : 0.05f;
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        if (app.input().was_pressed("pause")) {
            m_camera.auto_turn = m_camera.auto_turn == 0.0f ? 0.05f : 0.0f;
        }
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.62f, 0.75f, 0.9f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report =
            std::format("frames={} objects={} drawn={} draw_calls={} simplified={} culled={}",
                        m_frames, m_objects, m_renderer.last_draw_count(),
                        m_renderer.last_draw_call_count(), m_renderer.last_simplified_count(),
                        m_renderer.last_culled_count());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    core::Result<void> plant(runtime::App& app) {
        auto load = [&app](const char* path) { return app.assets().load<runtime::ModelAsset>(path); };
        auto ground = load("assets:/models/ground.glb");
        auto trunk = load("assets:/models/trunk.glb");
        auto canopy = load("assets:/models/canopy.glb");
        auto rock = load("assets:/models/rock.glb");
        for (const auto* model : {&ground, &trunk, &canopy, &rock}) {
            if (!model->has_value()) {
                return model->error();
            }
        }

        // The ground, in squares of 20 m, so its checks stay a sensible size.
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 8; ++column) {
                const runtime::Entity tile = add(ground.value(), Vec3{0.32f, 0.42f, 0.25f}, false);
                runtime::Transform& transform = m_world->registry().get<runtime::Transform>(tile);
                transform.position = Vec3{-k_half_size + 10.0f + 20.0f * static_cast<float>(column),
                                          0.0f,
                                          -k_half_size + 10.0f + 20.0f * static_cast<float>(row)};
                transform.scale = Vec3{10.0f, 1.0f, 10.0f};
            }
        }

        Random random;
        const Vec3 up{0.0f, 1.0f, 0.0f};
        for (int index = 0; index < k_trees; ++index) {
            Vec3 place{random.between(-k_half_size, k_half_size), 0.0f,
                       random.between(-k_half_size, k_half_size)};
            // A clearing in the middle, where the camera looks.
            while (core::length(place) < k_clearing) {
                place = Vec3{random.between(-k_half_size, k_half_size), 0.0f,
                             random.between(-k_half_size, k_half_size)};
            }
            const float size = random.between(0.8f, 1.5f);
            const runtime::Entity tree = m_world->create_entity();
            runtime::Transform& transform = m_world->registry().get<runtime::Transform>(tree);
            transform.position = place;
            transform.rotation = core::quat_from_axis_angle(up, random.between(0.0f, 6.28f));
            transform.scale = Vec3{size, size, size};

            const runtime::Entity stem = add(trunk.value(), Vec3{1.0f, 1.0f, 1.0f}, true);
            const runtime::Entity leaves =
                add(canopy.value(),
                    Vec3{random.between(0.7f, 1.1f), random.between(0.85f, 1.15f),
                         random.between(0.6f, 1.0f)},
                    true);
            runtime::Transform& crown = m_world->registry().get<runtime::Transform>(leaves);
            crown.position = Vec3{0.0f, 2.6f, 0.0f};
            crown.scale = Vec3{1.1f, 1.4f, 1.1f};
            (void)m_world->set_parent(stem, tree);
            (void)m_world->set_parent(leaves, tree);
            m_objects += 2;
        }
        for (int index = 0; index < k_rocks; ++index) {
            const runtime::Entity stone = add(rock.value(), Vec3{1.0f, 1.0f, 1.0f}, true);
            runtime::Transform& transform = m_world->registry().get<runtime::Transform>(stone);
            const float size = random.between(0.3f, 1.0f);
            transform.position = Vec3{random.between(-k_half_size, k_half_size), size * 0.3f,
                                      random.between(-k_half_size, k_half_size)};
            transform.rotation = core::quat_from_axis_angle(up, random.between(0.0f, 6.28f));
            transform.scale = Vec3{size * 1.3f, size * 0.8f, size};
            ++m_objects;
        }
        m_world->update_transforms();
        DREAM_LOG_INFO("Planted {} trees and {} rocks", k_trees, k_rocks);
        return {};
    }

    runtime::Entity add(const runtime::AssetRef<runtime::ModelAsset>& model, Vec3 tint,
                        bool casts_shadows) {
        const runtime::Entity entity = m_world->create_entity();
        m_world->registry().emplace<runtime::MeshRenderer>(
            entity, runtime::MeshRenderer{.model = model,
                                          .tint = core::Vec4{tint.x, tint.y, tint.z, 1.0f},
                                          .casts_shadows = casts_shadows});
        return entity;
    }

    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    int m_objects = 0;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Forest>();
}
