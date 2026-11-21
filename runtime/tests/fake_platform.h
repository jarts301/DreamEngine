#pragma once

#include "dream/platform/platform.h"

#include <deque>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::runtime::tests {

// One step of a script: an event, or std::nullopt for "no more events this frame".
using Step = std::optional<platform::Event>;
inline constexpr std::nullopt_t k_end_of_frame = std::nullopt;

// Plays back a fixed list of events, then asks to quit. Shared state lives in a struct the test
// keeps, because the App takes ownership of the platform.
struct FakeState {
    std::deque<Step> script;
    int wait_calls = 0;
    std::optional<platform::WindowSettings> window;
    platform::SurfaceSize size{640, 480};
    platform::NativeWindow native_window;
    float display_scale = 1.0f;
    std::optional<platform::SurfaceRect> safe_area;
    bool text_input = false;
    std::map<std::string, std::vector<std::byte>, std::less<>> assets;
    std::map<std::string, std::vector<std::byte>, std::less<>> user_files;
    // What file_version reports for assets, once a test turns it on: the version given here, 1 for
    // other assets that exist, and 0 for missing ones. Off, it reports nothing, as an APK does.
    bool reports_versions = false;
    std::map<std::string, std::int64_t, std::less<>> asset_versions;
    // What on_battery reports, and the refresh rates asked of the display, in order.
    std::optional<bool> on_battery;
    std::vector<float> frame_rate_hints;
    // What post_http answers, and the URLs it was sent, in order.
    int http_status = 200;
    bool http_offline = false;
    std::vector<std::string> http_urls;
};

class FakePlatform final : public platform::Platform {
public:
    explicit FakePlatform(FakeState& state) : m_state(state) {}

    core::Result<void> create_window(const platform::WindowSettings& settings) override {
        m_state.window = settings;
        return {};
    }

    platform::SurfaceSize surface_size() const override {
        return m_state.size;
    }

    platform::NativeWindow native_window() const override {
        return m_state.native_window;
    }

    float display_scale() const override {
        return m_state.display_scale;
    }

    void set_text_input(bool enabled) override {
        m_state.text_input = enabled;
    }

    platform::SurfaceRect safe_area() const override {
        return m_state.safe_area.value_or(
            platform::SurfaceRect{0, 0, m_state.size.width, m_state.size.height});
    }

    std::optional<platform::Event> poll_event() override {
        if (m_state.script.empty()) {
            return platform::QuitRequested{};
        }
        Step step = std::move(m_state.script.front());
        m_state.script.pop_front();
        return step;
    }

    platform::Event wait_event() override {
        ++m_state.wait_calls;
        while (!m_state.script.empty()) {
            Step step = std::move(m_state.script.front());
            m_state.script.pop_front();
            if (step.has_value()) {
                return *step;
            }
        }
        return platform::QuitRequested{};
    }

    core::Result<std::vector<std::byte>> read_file(platform::FileRoot root,
                                                   std::string_view path) override {
        const auto& files = root == platform::FileRoot::Assets ? m_state.assets : m_state.user_files;
        const auto file = files.find(path);
        if (file == files.end()) {
            return core::Error{core::ErrorCode::NotFound, std::format("Not found: {}", path)};
        }
        return file->second;
    }

    std::optional<bool> on_battery() override {
        return m_state.on_battery;
    }

    core::Result<platform::HttpResponse> post_http(const platform::HttpRequest& request) override {
        m_state.http_urls.push_back(request.url);
        if (m_state.http_offline) {
            return core::Error{core::ErrorCode::IoError, "offline"};
        }
        return platform::HttpResponse{.status = m_state.http_status, .body = {}};
    }

    void set_frame_rate_hint(float frames_per_second) override {
        m_state.frame_rate_hints.push_back(frames_per_second);
    }

    std::optional<std::int64_t> file_version(platform::FileRoot root,
                                             std::string_view path) override {
        if (!m_state.reports_versions || root != platform::FileRoot::Assets) {
            return std::nullopt;
        }
        if (const auto version = m_state.asset_versions.find(path);
            version != m_state.asset_versions.end()) {
            return version->second;
        }
        return m_state.assets.contains(path) ? 1 : 0;
    }

    core::Result<void> write_user_file(std::string_view path,
                                       std::span<const std::byte> data) override {
        m_state.user_files.insert_or_assign(std::string(path),
                                            std::vector<std::byte>(data.begin(), data.end()));
        return {};
    }

private:
    FakeState& m_state;
};

} // namespace dream::runtime::tests
