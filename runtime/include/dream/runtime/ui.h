#pragma once

#include "dream/core/result.h"
#include "dream/debug_ui/debug_ui.h"
#include "dream/runtime/event_bus.h"
#include "dream/ui/game_ui.h"

#include <memory>
#include <string>
#include <vector>

namespace dream::runtime {

class App;
class Renderer;

// Developer tools over the game, from the debug_ui module listed in the project configuration:
//
//     on_start:   overlay.initialize(app, renderer);
//     on_update:  debug_ui::DebugUi& tools = overlay.begin_frame(app, delta);
//                 if (tools.begin_window("Stats")) { tools.text("..."); }
//                 tools.end_window();
//                 overlay.end_frame();
//
// The renderer draws the result over everything else, inside the screen's safe area. Input reaches
// the tools through the App's event bus; check wants_pointer before treating a click as the game's.
// Destroy this before the renderer.
class DebugOverlay {
public:
    DebugOverlay() = default;
    ~DebugOverlay();

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    [[nodiscard]] core::Result<void> initialize(App& app, Renderer& renderer);
    [[nodiscard]] core::Result<void> initialize(App& app, Renderer& renderer,
                                                std::unique_ptr<debug_ui::DebugUi> backend);

    // Widgets may be used until end_frame.
    debug_ui::DebugUi& begin_frame(App& app, float delta_seconds);
    void end_frame();

    // Hidden tools draw nothing and ignore input, but their widgets can still be called.
    void set_visible(bool visible) noexcept {
        m_visible = visible;
    }

    [[nodiscard]] bool visible() const noexcept {
        return m_visible;
    }

    [[nodiscard]] bool wants_pointer() const;
    [[nodiscard]] bool wants_keyboard() const;

private:
    std::unique_ptr<debug_ui::DebugUi> m_backend;
    Renderer* m_renderer = nullptr;
    std::vector<Subscription> m_subscriptions;
    core::Vec2 m_origin;
    bool m_visible = true;
};

// Published on the App's event bus by UserInterface::update for each ui::UiEvent.
struct UiEventPublished {
    ui::UiEvent event;
};

// The player-facing interface, from the ui module listed in the project configuration:
//
//     on_start:   interface.initialize(app, renderer);
//                 interface.ui().load_font("assets:/fonts/game.ttf");
//                 auto menu = interface.ui().load_document("assets:/ui/menu.rml");
//                 interface.ui().show(menu.value());
//     on_update:  interface.update(app, delta);
//                 for (const ui::UiEvent& event : interface.ui().events()) { ... }
//
// Documents are laid out inside the screen's safe area, away from status bars and cutouts. It also
// turns text input on while a text field has focus. Destroy this before the renderer.
class UserInterface {
public:
    UserInterface() = default;
    ~UserInterface();

    UserInterface(const UserInterface&) = delete;
    UserInterface& operator=(const UserInterface&) = delete;

    [[nodiscard]] core::Result<void> initialize(App& app, Renderer& renderer);
    [[nodiscard]] core::Result<void> initialize(App& app, Renderer& renderer,
                                                std::unique_ptr<ui::GameUi> backend);

    // Valid after initialize.
    [[nodiscard]] ui::GameUi& ui() noexcept {
        return *m_backend;
    }

    // Lays out the documents, publishes what the player did, and hands the result to the renderer.
    void update(App& app, float delta_seconds);

    [[nodiscard]] bool wants_pointer() const;

private:
    std::unique_ptr<ui::GameUi> m_backend;
    Renderer* m_renderer = nullptr;
    std::vector<Subscription> m_subscriptions;
    core::Vec2 m_origin;
    bool m_text_input = false;
};

} // namespace dream::runtime
