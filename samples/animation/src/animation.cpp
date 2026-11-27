// Skeletal animation (docs/guides/animation.md): each tentacle has an `animator` in the scene file,
// which Animation::update plays and the renderer skins on the GPU. The game changes the middle
// one's clip every few seconds, or when asked, with Animation::play, which fades the last clip out
// as the next one fades in.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, changes the middle
// tentacle's clip. Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/animation.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>

using namespace dream;

namespace {

constexpr double k_step = 1.0 / 60.0;
constexpr double k_seconds_per_clip = 2.0;
constexpr float k_fade_seconds = 0.6f;

class AnimationSample final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        // Before the scene loads, so its "animator" components are known.
        if (auto ready = m_animation.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/animation.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        m_dancer = m_world->find("dancer");
        if (m_dancer == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument, "The scene needs a dancer"};
        }
        m_world->update_transforms();

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        // Checks compare a picture: they play every clip, and the change of clip, in fixed steps up
        // to a moment in the middle of a fade, and hold it there.
        if (const std::optional<double> held = config.get_float("demo.time")) {
            for (double played = 0.0; played < *held; played += k_step) {
                advance(k_step);
            }
            m_held = true;
        }
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (m_held) {
            m_animation.update(*m_world, 0.0);
        } else {
            if (app.input().was_pressed("switch_clip")) {
                switch_clip();
            }
            advance(time.delta_seconds);
        }
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size());
    }

    void on_stop(runtime::App& /*app*/) override {
        const auto animated = m_world->registry().view<const runtime::Animator>().size();
        const std::string report = std::format("frames={} draws={} animated={}", m_frames,
                                               m_renderer.last_draw_count(), animated);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    void advance(double seconds) {
        m_until_switch -= seconds;
        if (m_until_switch <= 0.0) {
            switch_clip();
        }
        m_animation.update(*m_world, seconds);
    }

    // The dancer goes from sway to curl and back, fading over a little more than half a second.
    void switch_clip() {
        const std::string& playing = m_world->registry().get<runtime::Animator>(m_dancer).clip;
        const char* next = playing == "sway" ? "curl" : "sway";
        if (auto played = m_animation.play(*m_world, m_dancer, next, k_fade_seconds);
            !played.has_value()) {
            DREAM_LOG_WARNING("{}", played.error().message);
        }
        m_until_switch = k_seconds_per_clip;
    }

    runtime::Renderer m_renderer;
    runtime::Animation m_animation;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    runtime::Entity m_dancer = runtime::k_no_entity;
    // The first change comes after one second.
    double m_until_switch = 1.0;
    bool m_held = false;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<AnimationSample>();
}
