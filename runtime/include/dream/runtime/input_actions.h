#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// Named game actions, so game code never mentions a specific key or button. Bindings come from the
// project configuration, and an action is active when any of its bindings is:
//
//     [input.actions]
//     jump = ["key:space", "mouse:left", "touch"]
//
// Keys are written as "key:a", "key:0", "key:f1", "key:space", "key:enter", "key:escape",
// "key:tab", "key:backspace", "key:delete", "key:home", "key:end", "key:left", "key:right", "key:up", "key:down", "key:left_shift",
// "key:right_shift", "key:left_control", "key:right_control", "key:left_alt", "key:right_alt" and
// "key:back". Pointers are "mouse:left", "mouse:right", "mouse:middle" and "touch".
class InputActions {
public:
    struct PointerPosition {
        float x = 0.0f;
        float y = 0.0f;
    };

    InputActions() = default;

    // Fails if a binding isn't recognized.
    [[nodiscard]] static core::Result<InputActions> from_config(const core::Config& config);

    // The action is held down.
    [[nodiscard]] bool is_down(std::string_view action) const;

    // The action started or ended during this frame. A press and release within one frame
    // reports both.
    [[nodiscard]] bool was_pressed(std::string_view action) const;
    [[nodiscard]] bool was_released(std::string_view action) const;

    // Where the mouse or primary finger last was, in pixels.
    [[nodiscard]] PointerPosition pointer_position() const noexcept {
        return m_pointer;
    }

    // Used by the main loop.
    void begin_frame() noexcept;
    void apply(const platform::KeyChanged& event) noexcept;
    void apply(const platform::PointerButtonChanged& event) noexcept;
    void apply(const platform::PointerMoved& event) noexcept;

    // Actions don't use these.
    void apply(const platform::PointerWheel& /*event*/) noexcept {}
    void apply(const platform::TextEntered& /*event*/) noexcept {}
    void release_all() noexcept;

private:
    static constexpr std::size_t k_key_count = static_cast<std::size_t>(platform::Key::Count);
    static constexpr std::size_t k_input_count =
        k_key_count + static_cast<std::size_t>(platform::PointerButton::Count);

    struct InputState {
        bool down = false;
        bool pressed = false;
        bool released = false;
    };

    using Bindings = std::vector<std::size_t>;

    template <bool InputState::*Field>
    [[nodiscard]] bool any_binding(std::string_view action) const;

    void set_down(std::size_t input, bool down) noexcept;

    std::map<std::string, Bindings, std::less<>> m_actions;
    std::array<InputState, k_input_count> m_inputs{};
    PointerPosition m_pointer;
};

} // namespace dream::runtime
