#pragma once

#include "dream/platform/event.h"

#include <cstddef>
#include <span>

namespace dream::platform {

// Where a graphics device that draws on the CPU hands its finished frames, for a platform to show
// them, such as the web platform sending them to a browser (ADR 0039). A platform::WebWindow points
// at one.
class WebCanvas {
public:
    // `rgba` holds size.width * size.height pixels of four bytes, red, green, blue and alpha, as
    // the screen shows them, row by row from the top. Copied before it returns. Called from the
    // thread that draws.
    virtual void present(SurfaceSize size, std::span<const std::byte> rgba) = 0;

protected:
    ~WebCanvas() = default;
};

} // namespace dream::platform
