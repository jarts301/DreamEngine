#include "dream/runtime/app.h"

#include "dream/core/clock.h"
#include "dream/core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace dream::runtime {
namespace {

// Reads a whole number that must be at least `minimum`.
core::Result<std::uint32_t> read_setting(const core::Config& config, std::string_view key,
                                         std::uint32_t fallback, std::uint32_t minimum) {
    const std::int64_t value = config.get_int(key).value_or(fallback);
    if (value < minimum || value > std::numeric_limits<std::uint32_t>::max()) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting '{}' has invalid value {}", key, value)};
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

App::App(std::unique_ptr<platform::Platform> platform, const core::Config& config,
         std::unique_ptr<rhi::Device> device)
    : m_platform(std::move(platform)), m_device(std::move(device)), m_config(config),
      m_files(*m_platform) {}

core::Result<void> App::attach_surface() {
    if (m_device == nullptr || m_surface_attached) {
        return {};
    }
    core::Result<void> attached =
        m_device->attach_surface(m_platform->native_window(), m_surface_size);
    m_surface_attached = attached.has_value();
    return attached;
}

core::Result<void> App::render_frame(Game& game) {
    if (m_device == nullptr) {
        return {};
    }

    // Android can report a surface before it's usable, so a failed attach is retried each frame.
    if (const core::Result<void> attached = attach_surface(); !attached.has_value()) {
        DREAM_LOG_WARNING("{}", attached.error().message);
        return {};
    }

    const core::Result<rhi::CommandList*> frame = m_device->begin_frame();
    if (!frame.has_value()) {
        return frame.error();
    }
    if (frame.value() == nullptr) {
        return {};
    }

    game.on_render(*this, *frame.value());
    return m_device->end_frame();
}

core::Result<void> App::run(Game& game) {
    core::Result<InputActions> input = InputActions::from_config(m_config);
    if (!input.has_value()) {
        return input.error();
    }
    m_input = std::move(input).value();

    const core::Result<std::uint32_t> width = read_setting(m_config, "app.width", 1280, 1);
    if (!width.has_value()) {
        return width.error();
    }
    const core::Result<std::uint32_t> height = read_setting(m_config, "app.height", 720, 1);
    if (!height.has_value()) {
        return height.error();
    }
    const core::Result<std::uint32_t> max_fps =
        read_setting(m_config, "app.max_fps", m_device != nullptr ? 0 : 60, 0);
    if (!max_fps.has_value()) {
        return max_fps.error();
    }

    const platform::WindowSettings window{
        .title = std::string(m_config.get_string("app.title").value_or("Dream Engine")),
        .width = width.value(),
        .height = height.value(),
    };
    if (core::Result<void> created = m_platform->create_window(window); !created.has_value()) {
        return created;
    }
    m_surface_size = m_platform->surface_size();

    if (core::Result<void> attached = attach_surface(); !attached.has_value()) {
        return attached;
    }

    if (core::Result<void> started = game.on_start(*this); !started.has_value()) {
        return started;
    }

    using Duration = std::chrono::steady_clock::duration;
    const Duration frame_period =
        max_fps.value() == 0 ? Duration::zero()
                             : std::chrono::duration_cast<Duration>(
                                   std::chrono::duration<double>(1.0 / max_fps.value()));

    core::Clock clock;
    core::TimePoint next_frame = core::now();
    core::Result<void> outcome;

    while (m_running) {
        m_input.begin_frame();
        pump_events(game);

        if (!m_running || !can_update()) {
            continue;
        }

        game.on_update(*this, clock.tick());

        outcome = render_frame(game);
        if (!outcome.has_value()) {
            break;
        }

        // Stops the loop using a whole CPU core when nothing else sets the pace.
        if (frame_period > Duration::zero()) {
            next_frame = std::max(next_frame + frame_period, core::now());
            std::this_thread::sleep_until(next_frame);
        }
    }

    game.on_stop(*this);
    return outcome;
}

void App::pump_events(Game& game) {
    if (!can_update()) {
        handle_event(game, m_platform->wait_event());
    }
    while (m_running) {
        const std::optional<platform::Event> event = m_platform->poll_event();
        if (!event.has_value()) {
            break;
        }
        handle_event(game, *event);
    }
}

void App::handle_event(Game& game, const platform::Event& event) {
    std::visit(
        [&](const auto& value) {
            using Type = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<Type, platform::QuitRequested>) {
                m_running = false;
            } else if constexpr (std::is_same_v<Type, platform::AppPaused>) {
                if (!m_paused) {
                    m_paused = true;
                    m_input.release_all();
                    game.on_pause(*this);
                }
            } else if constexpr (std::is_same_v<Type, platform::AppResumed>) {
                if (m_paused) {
                    m_paused = false;
                    game.on_resume(*this);
                }
            } else if constexpr (std::is_same_v<Type, platform::SurfaceLost>) {
                if (m_has_surface) {
                    m_has_surface = false;
                    game.on_surface_lost(*this);
                    if (m_surface_attached) {
                        m_device->detach_surface();
                        m_surface_attached = false;
                    }
                }
            } else if constexpr (std::is_same_v<Type, platform::SurfaceRestored>) {
                if (!m_has_surface) {
                    m_has_surface = true;
                    m_surface_size = m_platform->surface_size();
                    game.on_surface_restored(*this);
                }
            } else if constexpr (std::is_same_v<Type, platform::SurfaceResized>) {
                if (value.size != m_surface_size) {
                    m_surface_size = value.size;
                    if (m_surface_attached) {
                        m_device->resize_surface(value.size);
                    }
                    game.on_surface_resized(*this, value.size);
                }
            } else if constexpr (std::is_same_v<Type, platform::LowMemory>) {
                game.on_low_memory(*this);
            } else {
                m_input.apply(value);
            }
        },
        event);
}

} // namespace dream::runtime
