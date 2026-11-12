// The smallest complete game: it opens a window, reads an asset, saves a file between runs, reacts
// to input actions and logs what the operating system does to it. It draws nothing yet, because
// the renderer arrives in a later phase. It also marks a zone and a value for the profiler.

#include "dream/core/log.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dream;

namespace {

std::string to_text(const std::vector<std::byte>& bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

class HelloGame final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        m_quit_after_frames = app.config().get_int("hello.quit_after_frames").value_or(0);

        const auto asset = app.files().read("assets:/hello.txt");
        m_asset = asset.has_value() ? to_text(asset.value()) : "<missing>";

        // Counts how many times the game has run, to show that user files persist. A zone shows how
        // long this takes when the project lists a profiler.
        DREAM_PROFILE_ZONE("Count runs");
        const auto previous = app.files().read("user:/run_count.txt");
        if (previous.has_value()) {
            const std::string text = to_text(previous.value());
            std::from_chars(text.data(), text.data() + text.size(), m_runs);
        }
        ++m_runs;

        const std::string count = std::to_string(m_runs);
        return app.files().write("user:/run_count.txt", std::as_bytes(std::span(count)));
    }

    void on_update(runtime::App& app, const core::FrameTime& /*time*/) override {
        ++m_frames;
        DREAM_PROFILE_VALUE("Hello frames", m_frames);

        if (app.input().was_pressed("confirm")) {
            const auto pointer = app.input().pointer_position();
            DREAM_LOG_INFO("Confirm pressed, pointer at {}, {}", pointer.x, pointer.y);
        }
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report = std::format("frames={} asset={} runs={}", m_frames, m_asset, m_runs);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

    void on_pause(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Paused");
    }

    void on_resume(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Resumed");
    }

    void on_surface_lost(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Surface lost");
    }

    void on_surface_restored(runtime::App& app) override {
        DREAM_LOG_INFO("Surface restored at {}x{}", app.surface_size().width,
                       app.surface_size().height);
    }

    void on_surface_resized(runtime::App& /*app*/, platform::SurfaceSize size) override {
        DREAM_LOG_INFO("Surface resized to {}x{}", size.width, size.height);
    }

private:
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
    std::int64_t m_runs = 0;
    std::string m_asset;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<HelloGame>();
}
