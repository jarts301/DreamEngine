#include "dream/runtime/hot_reload.h"

#include <format>

namespace dream::runtime {

namespace {

#if !defined(NDEBUG) && !defined(__ANDROID__)
constexpr bool k_enabled_by_default = true;
#else
constexpr bool k_enabled_by_default = false;
#endif

} // namespace

core::Result<HotReloadSettings> HotReloadSettings::from_config(const core::Config& config) {
    HotReloadSettings settings;
    settings.enabled = config.get_bool("hot_reload.enabled").value_or(k_enabled_by_default);
    const std::int64_t interval = config.get_int("hot_reload.interval_ms").value_or(500);
    if (interval < 50 || interval > 60'000) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting 'hot_reload.interval_ms' must be from 50 to "
                                       "60000, not {}",
                                       interval)};
    }
    settings.interval_ms = static_cast<std::uint32_t>(interval);
    return settings;
}

} // namespace dream::runtime
