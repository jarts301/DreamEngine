#include "dream/runtime/app.h"

#include "dream/core/clock.h"
#include "dream/core/crash_handler.h"
#include "dream/core/log.h"
#include "dream/core/profile.h"

#include "dream/runtime/crash_reports.h"
#include "dream/runtime/screenshot.h"

#include "memory_guard.h"
#include "resource_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
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

// How many frames a screenshot may wait for one that is drawn, such as while a window appears.
constexpr std::uint64_t k_screenshot_patience_frames = 120;

// Saves the captured frame, then compares it with the reference if there is one. An error means
// the screenshot test failed.
core::Result<void> check_screenshot(const ScreenshotSettings& settings, rhi::CapturedFrame frame,
                                    const VirtualFileSystem& files) {
    const ImageData actual{.width = frame.width, .height = frame.height,
                           .pixels = std::move(frame.pixels)};
    const std::vector<std::byte> png = encode_png(actual);
    std::ofstream out(settings.save, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        return core::Error{core::ErrorCode::IoError,
                           std::format("Could not save the screenshot to '{}'", settings.save)};
    }
    out.close();
    if (settings.reference.empty()) {
        DREAM_LOG_INFO("Screenshot saved to '{}' ({}x{})", settings.save, actual.width,
                       actual.height);
        return {};
    }

    core::Result<std::vector<std::byte>> bytes = files.read(settings.reference);
    if (!bytes.has_value()) {
        return core::Error{bytes.error().code,
                           std::format("Screenshot reference: {}. The capture is in '{}'",
                                       bytes.error().message, settings.save)};
    }
    const core::Result<ImageData> expected = decode_image(bytes.value());
    if (!expected.has_value()) {
        return expected.error();
    }
#if defined(__ANDROID__)
    // The screen sets the size on a phone, so a reference made on PC rarely fits it.
    if (actual.width != expected.value().width || actual.height != expected.value().height) {
        DREAM_LOG_WARNING("Screenshot saved to '{}' but not compared: the screen is {}x{}, and "
                          "'{}' is {}x{}",
                          settings.save, actual.width, actual.height, settings.reference,
                          expected.value().width, expected.value().height);
        return {};
    }
#endif
    const core::Result<ImageDifference> difference =
        compare_images(actual, expected.value(), settings.tolerance);
    if (!difference.has_value()) {
        return core::Error{difference.error().code,
                           std::format("Screenshot differs from '{}': {}. The capture is in '{}'",
                                       settings.reference, difference.error().message,
                                       settings.save)};
    }
    const std::string summary =
        std::format("{:.3f}% of pixels are off by more than {} (largest {})",
                    difference.value().differing_share() * 100.0, settings.tolerance,
                    difference.value().largest);
    if (difference.value().differing_share() > settings.max_differing) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Screenshot differs from '{}': {}, over the {:.3f}% "
                                       "allowed. The capture is in '{}'",
                                       settings.reference, summary,
                                       settings.max_differing * 100.0, settings.save)};
    }
    DREAM_LOG_INFO("Screenshot matches '{}': {}", settings.reference, summary);
    return {};
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
    // A new surface forgets the rate asked of the last one.
    if (m_surface_attached && m_frame_rate_hint > 0.0f) {
        m_platform->set_frame_rate_hint(m_frame_rate_hint);
    }
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
    const core::Result<std::uint32_t> battery_fps =
        read_setting(m_config, "app.max_fps_on_battery", 0, 0);
    if (!battery_fps.has_value()) {
        return battery_fps.error();
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

    const core::Result<std::optional<ScreenshotSettings>> screenshot =
        ScreenshotSettings::from_config(m_config);
    if (!screenshot.has_value()) {
        return screenshot.error();
    }

    const core::Result<HotReloadSettings> hot_reload = HotReloadSettings::from_config(m_config);
    if (!hot_reload.has_value()) {
        return hot_reload.error();
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

    platform::WindowMode window_mode = platform::WindowMode::Normal;
    if (m_config.contains("app.window")) {
        const std::optional<std::string_view> mode = m_config.get_string("app.window");
        if (mode == "background") {
            window_mode = platform::WindowMode::Background;
        } else if (mode == "hidden") {
            window_mode = platform::WindowMode::Hidden;
        } else if (mode != "normal") {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Setting 'app.window' must be \"normal\", \"background\" or "
                               "\"hidden\""};
        }
    }
    const platform::WindowSettings window{
        .title = std::string(m_config.get_string("app.title").value_or("Dream Engine")),
        .width = width.value(),
        .height = height.value(),
        .mode = window_mode,
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

        // Gamepads are optional: a game without them still runs.
        if (has_module<input_device::InputDevice>()) {
            core::Result<std::unique_ptr<input_device::InputDevice>> gamepads =
                create_module<input_device::InputDevice>();
            if (gamepads.has_value()) {
                m_gamepads = std::move(gamepads).value();
            } else {
                DREAM_LOG_WARNING("No gamepads: {}", gamepads.error().message);
            }
        }

        if (core::Result<void> started = game.on_start(*this); !started.has_value()) {
            return started;
        }
    }

    using Duration = std::chrono::steady_clock::duration;
    // The limit in force, which a battery limit lowers while the device is unplugged.
    std::uint32_t frame_limit = 0;
    bool limit_applied = false;
    Duration frame_period = Duration::zero();
    const auto apply_limit = [&](std::uint32_t limit) {
        if (limit_applied && limit == frame_limit) {
            return;
        }
        limit_applied = true;
        frame_limit = limit;
        frame_period = limit == 0 ? Duration::zero()
                                  : std::chrono::duration_cast<Duration>(
                                        std::chrono::duration<double>(1.0 / limit));
        // With a graphics device, the display is asked to match, where it can.
        if (m_device != nullptr) {
            m_frame_rate_hint = static_cast<float>(limit);
            m_platform->set_frame_rate_hint(m_frame_rate_hint);
        }
        if (limit != 0) {
            DREAM_LOG_INFO("Frames are limited to {} per second", limit);
        }
    };
    apply_limit(max_fps.value());
    const bool battery_limit =
        battery_fps.value() != 0 &&
        (max_fps.value() == 0 || battery_fps.value() < max_fps.value());
    core::TimePoint next_battery_check = core::now();

    core::Clock clock;
    core::TimePoint next_frame = core::now();
    core::Result<void> outcome;
    ResourceMonitor resources(sample_rate.value());
    std::optional<core::TimePoint> previous_frame_end;
    // Counted from 0, as the profiler's frame marks are.
    std::uint64_t frame_number = 0;
    bool screenshot_asked = false;
    bool screenshot_done = false;
    // Screenshot tests compare a fixed moment, which a reload mustn't change.
    const bool reloading = hot_reload.value().enabled && !screenshot.value().has_value();
    const auto reload_period = std::chrono::milliseconds(hot_reload.value().interval_ms);
    core::TimePoint next_reload_check = core::now() + reload_period;
    if (reloading) {
        DREAM_LOG_INFO("Hot reloading is on: assets reload when their files change");
    }

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
        if (m_gamepads != nullptr) {
            DREAM_PROFILE_ZONE("Gamepads");
            m_gamepads->update();
            m_input.apply(m_gamepads->gamepads());
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
            // Between frames, so nothing the GPU still draws with is replaced mid-frame, and
            // before the events, so the game hears of the reload this frame.
            if (reloading && core::now() >= next_reload_check) {
                next_reload_check = core::now() + reload_period;
                DREAM_PROFILE_ZONE("Hot reload");
                std::vector<std::string> reloaded = m_assets.reload_changed();
                if (!reloaded.empty()) {
                    m_events.publish(AssetsReloaded{std::move(reloaded)});
                }
                // Scenes are the game's to load again, as only it knows what to keep.
                std::vector<std::string> scenes = m_scenes.changed_scenes();
                if (!scenes.empty()) {
                    m_events.publish(ScenesChanged{std::move(scenes)});
                }
            }
            {
                DREAM_PROFILE_ZONE("Queued events");
                m_events.flush();
            }
            core::FrameTime time = clock.tick();
            if (screenshot.value().has_value()) {
                // The same moment of the game on every machine, for screenshot tests.
                time.delta_seconds = m_fixed_steps.step_seconds();
            }
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

            const std::optional<ScreenshotSettings>& shot = screenshot.value();
            if (shot.has_value() && !screenshot_done && !screenshot_asked &&
                frame_number >= shot->frame) {
                if (m_device == nullptr || !m_device->capture_next_frame()) {
                    outcome = core::Error{core::ErrorCode::InvalidArgument,
                                          "[screenshot] is set, but the graphics device can't "
                                          "capture frames"};
                    break;
                }
                screenshot_asked = true;
            }

            outcome = render_frame(game, timing.wait_seconds);
            if (!outcome.has_value()) {
                break;
            }

            if (screenshot_asked && !screenshot_done) {
                if (std::optional<rhi::CapturedFrame> captured = m_device->take_captured_frame()) {
                    screenshot_done = true;
                    outcome = check_screenshot(*shot, std::move(*captured), m_files);
                    if (!outcome.has_value()) {
                        break;
                    }
                    if (shot->quit) {
                        m_running = false;
                    }
                } else if (frame_number > shot->frame + k_screenshot_patience_frames) {
                    outcome = core::Error{core::ErrorCode::InvalidArgument,
                                          "[screenshot] asked for a frame, but none was drawn"};
                    break;
                }
            }

            // On battery, the lower limit, checked every few seconds as it costs a moment.
            if (battery_limit && core::now() >= next_battery_check) {
                next_battery_check = core::now() + std::chrono::seconds(3);
                const bool unplugged = m_platform->on_battery().value_or(false);
                apply_limit(unplugged ? battery_fps.value() : max_fps.value());
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
