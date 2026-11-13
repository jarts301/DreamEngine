#include "dream/runtime/memory_budget.h"

#include <format>
#include <string>

namespace dream::runtime {
namespace {

constexpr double k_megabyte = 1024.0 * 1024.0;

// Leaving a level needs a reading this much clear of its threshold.
constexpr double k_margin = 0.1;

core::Error setting_error(std::string_view key, std::string_view problem) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Setting 'memory.{}' {}", key, problem)};
}

core::Result<void> read_bool(const core::Config& config, std::string_view key, bool& value) {
    const std::string full_key = std::format("memory.{}", key);
    if (!config.contains(full_key)) {
        return {};
    }
    const std::optional<bool> read = config.get_bool(full_key);
    if (!read.has_value()) {
        return setting_error(key, "must be true or false");
    }
    value = *read;
    return {};
}

core::Result<void> read_megabytes(const core::Config& config, std::string_view key,
                                  double maximum, std::uint64_t& bytes) {
    const std::string full_key = std::format("memory.{}", key);
    if (!config.contains(full_key)) {
        return {};
    }
    const std::optional<double> megabytes = config.get_float(full_key);
    if (!megabytes.has_value()) {
        return setting_error(key, "must be a number of megabytes");
    }
    if (!(*megabytes >= 0.0) || *megabytes > maximum) {
        return setting_error(key, std::format("must be from 0 to {}", maximum));
    }
    bytes = static_cast<std::uint64_t>(*megabytes * k_megabyte);
    return {};
}

} // namespace

std::string_view to_string(MemoryPressure pressure) noexcept {
    switch (pressure) {
    case MemoryPressure::Normal:
        return "normal";
    case MemoryPressure::Warning:
        return "warning";
    case MemoryPressure::Critical:
        return "critical";
    }
    return "unknown";
}

core::Result<MemorySettings> MemorySettings::from_config(const core::Config& config) {
    MemorySettings settings;
#if defined(__ANDROID__)
    // Phones keep little RAM free on purpose, so the PC thresholds would warn all the time. Android
    // reports low memory itself, as platform::LowMemory.
    settings.enabled = false;
#endif

    // A petabyte, so no value can overflow when turned into bytes.
    constexpr double k_largest = 1'000'000'000.0;
    for (const core::Result<void>& read : {
             read_bool(config, "enabled", settings.enabled),
             read_bool(config, "refuse_loads", settings.refuse_loads),
             read_megabytes(config, "budget_mb", k_largest, settings.budget_bytes),
             read_megabytes(config, "low_system_mb", k_largest, settings.low_system_bytes),
             read_megabytes(config, "critical_system_mb", k_largest,
                            settings.critical_system_bytes),
             // A reserve is memory the game can't use, so it is kept modest.
             read_megabytes(config, "reserve_mb", 1024.0, settings.reserve_bytes),
         }) {
        if (!read.has_value()) {
            return read.error();
        }
    }

    if (config.contains("memory.warning_at")) {
        const std::optional<double> warning_at = config.get_float("memory.warning_at");
        if (!warning_at.has_value() || !(*warning_at > 0.0) || *warning_at > 1.0) {
            return setting_error("warning_at", "must be a number above 0 and at most 1");
        }
        settings.warning_at = *warning_at;
    }
    if (config.contains("memory.check_rate")) {
        const std::optional<std::int64_t> rate = config.get_int("memory.check_rate");
        if (!rate.has_value() || *rate < 1 || *rate > 1000) {
            return setting_error("check_rate", "must be a whole number from 1 to 1000");
        }
        settings.check_rate = static_cast<std::uint32_t>(*rate);
    }
    return settings;
}

MemoryPressure MemoryBudget::evaluate(const MemoryReading& reading) {
    const bool was_warning = m_pressure != MemoryPressure::Normal;
    const bool was_critical = m_pressure == MemoryPressure::Critical;

    // Over a limit. A level already reached is kept until the value is 10% under it.
    const auto over = [](std::uint64_t value, double limit, bool already) {
        const auto amount = static_cast<double>(value);
        return already ? amount > limit * (1.0 - k_margin) : amount >= limit;
    };
    // Under a floor. A level already reached is kept until the value is 10% above it.
    const auto under = [](std::uint64_t value, double floor, bool already) {
        const auto amount = static_cast<double>(value);
        return already ? amount < floor * (1.0 + k_margin) : amount < floor;
    };

    bool critical = reading.allocation_failed;
    bool warning = false;
    if (m_settings.budget_bytes > 0 && reading.process_bytes.has_value()) {
        const auto budget = static_cast<double>(m_settings.budget_bytes);
        critical = critical || over(*reading.process_bytes, budget, was_critical);
        warning = warning ||
                  over(*reading.process_bytes, budget * m_settings.warning_at, was_warning);
    }
    if (m_settings.critical_system_bytes > 0 && reading.available_commit_bytes.has_value()) {
        critical = critical ||
                   under(*reading.available_commit_bytes,
                         static_cast<double>(m_settings.critical_system_bytes), was_critical);
    }
    if (m_settings.low_system_bytes > 0 && reading.available_physical_bytes.has_value()) {
        warning = warning ||
                  under(*reading.available_physical_bytes,
                        static_cast<double>(m_settings.low_system_bytes), was_warning);
    }

    m_pressure = critical  ? MemoryPressure::Critical
                 : warning ? MemoryPressure::Warning
                           : MemoryPressure::Normal;
    return m_pressure;
}

} // namespace dream::runtime
