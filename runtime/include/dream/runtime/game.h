#pragma once

#include "dream/core/clock.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/rhi/rhi.h"

#include <memory>

namespace dream::runtime {

class App;

// A game, written once for every platform. Only on_update is required.
class Game {
public:
    virtual ~Game() = default;

    // Called once, after the window exists. Returning an error ends the program, without on_stop.
    [[nodiscard]] virtual core::Result<void> on_start(App& /*app*/) {
        return {};
    }

    // Called every frame, but never while paused or without a surface.
    virtual void on_update(App& app, const core::FrameTime& time) = 0;

    // Called zero or more times per frame before on_update, so that over time it runs at the fixed
    // rate set by `fixed_rate` under [app]. Simulation such as physics belongs here.
    virtual void on_fixed_update(App& /*app*/, double /*step_seconds*/) {}

    // Called after on_update whenever the frame can be drawn. Projects without a graphics module
    // never get this.
    virtual void on_render(App& /*app*/, rhi::CommandList& /*commands*/) {}

    // Called once when the program ends normally. Android may kill a paused program without
    // calling this, so save progress in on_pause.
    virtual void on_stop(App& /*app*/) {}

    virtual void on_pause(App& /*app*/) {}
    virtual void on_resume(App& /*app*/) {}

    // Anything tied to the drawing surface must be released in on_surface_lost and recreated in
    // on_surface_restored. On Android these happen on every trip to the background.
    virtual void on_surface_lost(App& /*app*/) {}
    virtual void on_surface_restored(App& /*app*/) {}

    virtual void on_surface_resized(App& /*app*/, platform::SurfaceSize /*size*/) {}

    virtual void on_low_memory(App& /*app*/) {}
};

// Every game defines this function. The engine calls it once at startup.
[[nodiscard]] std::unique_ptr<Game> create_game();

} // namespace dream::runtime
