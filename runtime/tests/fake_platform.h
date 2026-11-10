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
