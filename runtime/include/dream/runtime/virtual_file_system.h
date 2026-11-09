#pragma once

#include "dream/core/result.h"
#include "dream/platform/platform.h"

#include <cstddef>
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

private:
    platform::Platform& m_platform;
};

} // namespace dream::runtime
