#pragma once

#include "dream/core/result.h"
#include "dream/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dream::runtime {

// One way of naming files on every platform:
//
//     assets:/textures/rock.png    Read-only files shipped with the game.
//     user:/saves/slot1.dat        Writable storage that survives between runs.
//
// Paths use forward slashes and can't contain "." or ".." parts, empty parts, backslashes or
// colons, so a path can't reach outside its root.
class VirtualFileSystem {
public:
    // `platform` must outlive this object.
    explicit VirtualFileSystem(platform::Platform& platform) : m_platform(platform) {}

    [[nodiscard]] core::Result<std::vector<std::byte>> read(std::string_view path) const;

    // Only "user:/" paths can be written.
    [[nodiscard]] core::Result<void> write(std::string_view path,
                                           std::span<const std::byte> data) const;

    // A number that changes when the file does, 0 when it doesn't exist, or nothing when the
    // platform can't tell. See platform::Platform::file_version.
    [[nodiscard]] std::optional<std::int64_t> version(std::string_view path) const;

    // Called with the path of every read, whether it succeeds or not, so the asset manager can
    // tell which files an asset came from. Empty for none.
    void set_read_observer(std::function<void(std::string_view path)> observer) {
        m_read_observer = std::move(observer);
    }

private:
    platform::Platform& m_platform;
    std::function<void(std::string_view)> m_read_observer;
};

} // namespace dream::runtime
