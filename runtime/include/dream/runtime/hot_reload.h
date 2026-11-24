#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dream::runtime {

// Reloading assets while the game runs, from the project configuration, all optional:
//
//     [hot_reload]
//     enabled = true        # Debug builds on PC; off in release builds and on Android.
//     interval_ms = 500     # How often the asset files are checked.
//
// While on, the App checks the files of every asset in use between frames, and reloads those
// that changed: rebuild the game's assets target, such as `cmake --build <folder> --target
// <game>_assets`, while the game runs. The renderer's shaders reload the same way. Scene files the
// SceneLoader loaded aren't reloaded for the game, which alone knows what to keep: the App publishes
// ScenesChanged, and the game loads them again (ADR 0046). Files in an
// APK or an assets.pak can't change, so nothing reloads there. See docs/adr/0025-hot-reload.md.
struct HotReloadSettings {
    bool enabled = false;
    std::uint32_t interval_ms = 500;

    [[nodiscard]] static core::Result<HotReloadSettings> from_config(const core::Config& config);
};

// Published on the App's event bus after assets were reloaded, for game code that built something
// of its own from them, which AssetRef::version also tells.
struct AssetsReloaded {
    std::vector<std::string> paths;
};

} // namespace dream::runtime
