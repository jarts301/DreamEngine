#include "dream/runtime/app.h"

#include "dream/core/clock.h"
#include "dream/core/crash_handler.h"
#include "dream/core/log.h"
#include "dream/core/profile.h"

#include "dream/runtime/crash_reports.h"

#include "memory_guard.h"
#include "resource_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
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

double seconds_since(core::TimePoint start) {
    return std::chrono::duration<double>(core::now() - start).count();
}

std::string megabytes(const std::optional<std::uint64_t>& bytes) {
    return bytes.has_value()
               ? std::format("{:.1f} MB", static_cast<double>(*bytes) / (1024.0 * 1024.0))
               : "unknown";
}

} // namespace

App::App(std::unique_ptr<platform::Platform> platform, const core::Config& config,
         std::unique_ptr<rhi::Device> device, const core::ModuleProject* modules)
    : m_platform(std::move(platform)), m_device(std::move(device)), m_config(config),
      m_modules(modules), m_files(*m_platform), m_assets(m_files), m_scenes(m_files, m_assets) {}

core::Result<void> App::attach_surface() {
    if (m_device == nullptr || m_surface_attached) {
        return {};
    }
    core::Result<void> attached =
        m_device->attach_surface(m_platform->native_window(), m_surface_size);
    m_surface_attached = attached.has_value();
    return attached;
}

core::Result<void> App::render_frame(Game& game, double& wait_seconds) {
    if (m_device == nullptr) {
        return {};
    }

    // Android can report a surface before it's usable, so a failed attach is retried each frame.
    if (const core::Result<void> attached = attach_surface(); !attached.has_value()) {
        DREAM_LOG_WARNING("{}", attached.error().message);
        return {};
    }

    // Where the CPU waits for the GPU, or for the display, as the device starts a frame.
    const core::TimePoint begin_start = core::now();
    const core::Result<rhi::CommandList*> frame = [this] {
        DREAM_PROFILE_ZONE("Begin GPU frame");
        return m_device->begin_frame();
    }();
    wait_seconds += seconds_since(begin_start);
    if (!frame.has_value()) {
        return frame.error();
    }
    if (frame.value() == nullptr) {
        return {};
    }

    {
        DREAM_PROFILE_ZONE("Render");
        game.on_render(*this, *frame.value());
    }
    // Presenting can also wait for the display, so it counts as waiting too.
    DREAM_PROFILE_ZONE("Submit and present");
    const core::TimePoint end_start = core::now();
    core::Result<void> ended = m_device->end_frame();
    wait_seconds += seconds_since(end_start);
    return ended;
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
    const core::Result<std::uint32_t> fixed_rate = read_setting(m_config, "app.fixed_rate", 60, 1);
    if (!fixed_rate.has_value()) {
        return fixed_rate.error();
    }
    if (fixed_rate.value() > 1000) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "Setting 'app.fixed_rate' can be at most 1000"};
    }
    m_fixed_steps = core::FixedTimestep(1.0 / fixed_rate.value());
    const core::Result<std::uint32_t> sample_rate =
        read_setting(m_config, "profiler.sample_rate", 10, 1);
    if (!sample_rate.has_value()) {
        return sample_rate.error();
    }
    if (sample_rate.value() > 1000) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "Setting 'profiler.sample_rate' can be at most 1000"};
    }

    // Only the simulated crash is read here. run_program installs the crash handler itself.
    const core::Result<CrashReportConfig> crash_config = CrashReportConfig::from_config(m_config);
    if (!crash_config.has_value()) {
        return crash_config.error();
    }

    const core::Result<MemorySettings> memory_settings = MemorySettings::from_config(m_config);
    if (!memory_settings.has_value()) {
        return memory_settings.error();
    }
    m_refuse_loads_when_critical = memory_settings.value().refuse_loads;
    // Before the game starts, because loading in on_start is when memory is most likely to run
    // out. The game can read the first check's result there, through memory_pressure().
    MemoryGuard memory(memory_settings.value());
    if (const std::optional<MemoryPressureChanged> change = memory.check(core::now())) {
        handle_memory_pressure(game, *change, false);
    }

    const platform::WindowSettings window{
        .title = std::string(m_config.get_string("app.title").value_or("Dream Engine")),
        .width = width.value(),
        .height = height.value(),
    };
    {
        // Creating the window and the game's on_start, where most loading happens.
        DREAM_PROFILE_ZONE("Start");
        if (core::Result<void> created = m_platform->create_window(window);
            !created.has_value()) {
            return created;
        }
        m_surface_size = m_platform->surface_size();

        if (core::Result<void> attached = attach_surface(); !attached.has_value()) {
            return attached;
        }

        if (core::Result<void> started = game.on_start(*this); !started.has_value()) {
            return started;
        }
    }

    using Duration = std::chrono::steady_clock::duration;
    const Duration frame_period =
        max_fps.value() == 0 ? Duration::zero()
                             : std::chrono::duration_cast<Duration>(
                                   std::chrono::duration<double>(1.0 / max_fps.value()));

    core::Clock clock;
    core::TimePoint next_frame = core::now();
    core::Result<void> outcome;
    ResourceMonitor resources(sample_rate.value());
    std::optional<core::TimePoint> previous_frame_end;
    // Counted from 0, as the profiler's frame marks are.
    std::uint64_t frame_number = 0;

    while (m_running) {
        m_input.begin_frame();
        {
            // Outside the frame's zone, because while paused this waits for the next event.
            DREAM_PROFILE_ZONE("Platform events");
            pump_events(game);
        }

        if (!m_running || !can_update()) {
            // Time spent paused belongs to no frame.
            previous_frame_end.reset();
            continue;
        }
        if (!previous_frame_end.has_value()) {
            previous_frame_end = core::now();
        }

        FrameTiming timing;
        core::set_crash_frame(frame_number);
        {
            DREAM_PROFILE_ZONE("Frame");
            // First, so the game hears of a change before it updates.
            if (const std::optional<MemoryPressureChanged> change = memory.check(core::now())) {
                handle_memory_pressure(game, *change, true);
            }
            {
                DREAM_PROFILE_ZONE("Queued events");
                m_events.flush();
            }
            const core::FrameTime time = clock.tick();
            const int fixed_steps = m_fixed_steps.advance(time.delta_seconds);
            timing.fixed_steps = fixed_steps;
            for (int step = 0; step < fixed_steps; ++step) {
                DREAM_PROFILE_ZONE("Fixed update");
                game.on_fixed_update(*this, m_fixed_steps.step_seconds());
            }
            {
                DREAM_PROFILE_ZONE("Update");
                game.on_update(*this, time);
            }
            if (crash_config.value().simulate.has_value() &&
                frame_number == crash_config.value().simulate_after_frames) {
                const core::CrashKind kind = *crash_config.value().simulate;
                DREAM_LOG_WARNING("Crashing on purpose, with '{}', as [crash_report] simulate "
                                  "asks",
                                  core::crash_kind_name(kind));
                core::simulate_crash(kind);
            }

            outcome = render_frame(game, timing.wait_seconds);
            if (!outcome.has_value()) {
                break;
            }

            // Stops the loop using a whole CPU core when nothing else sets the pace.
            if (frame_period > Duration::zero()) {
                DREAM_PROFILE_ZONE("Frame limit");
                const core::TimePoint sleep_start = core::now();
                next_frame = std::max(next_frame + frame_period, core::now());
                std::this_thread::sleep_until(next_frame);
                timing.wait_seconds += seconds_since(sleep_start);
            }
        }
        const core::TimePoint frame_end = core::now();
        timing.frame_seconds =
            std::chrono::duration<double>(frame_end - *previous_frame_end).count();
        previous_frame_end = frame_end;
        resources.end_frame(timing, m_device.get());

        // After the frame's zone and values, so a profiler that records a range of frames gets
        // each one whole.
        DREAM_PROFILE_FRAME();
        ++frame_number;
    }

    {
        DREAM_PROFILE_ZONE("Stop");
        game.on_stop(*this);
    }
    return outcome;
}

void App::handle_memory_pressure(Game& game, const MemoryPressureChanged& change, bool started) {
    m_memory_pressure = change.pressure;
    m_assets.set_refusing_loads(m_refuse_loads_when_critical &&
                                change.pressure == MemoryPressure::Critical);

    const MemoryReading& reading = change.reading;
    const std::string details = std::format(
        "private memory {}, system RAM available {}, room to allocate {}{}",
        megabytes(reading.process_bytes), megabytes(reading.available_physical_bytes),
        megabytes(reading.available_commit_bytes),
        reading.allocation_failed ? ", and an allocation failed" : "");
    switch (change.pressure) {
    case MemoryPressure::Critical:
        DREAM_LOG_ERROR("Memory pressure is critical: {}{}", details,
                        m_refuse_loads_when_critical ? ". New assets are refused until it eases"
                                                     : "");
        break;
    case MemoryPressure::Warning:
        DREAM_LOG_WARNING("Memory pressure is raised: {}", details);
        break;
    case MemoryPressure::Normal:
        DREAM_LOG_INFO("Memory pressure is back to normal: {}", details);
        break;
    }

    m_events.publish(change);
    if (started && change.pressure > change.previous) {
        game.on_low_memory(*this);
    }
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

    std::visit([&](const auto& value) { m_events.publish(value); }, event);
}

} // namespace dream::runtime
