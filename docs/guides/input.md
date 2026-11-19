# Input: actions, keyboards, touch and gamepads

This guide explains how a game reads its player, with the keyboard, mouse, touch screen and
gamepads, and how to use gamepads well. The design is in
[ADR 0035](../adr/0035-gamepads.md).

## In short

- Game code asks about **actions** by name, `jump` or `move_left`, never about a key or button.
  `dream.toml` binds each action to any number of keys, buttons and sticks.
- List `input_device = ["sdl3_gamepad", "null"]` to get gamepads. Without it, `gamepad:` bindings
  are accepted and never pressed.
- Gamepad buttons are named by **position**: `gamepad:south` is the bottom face button, A on Xbox,
  Cross on PlayStation, B on Switch.
- Sticks and triggers are **analog**: `value(action)` says how far, from 0 to 1, and
  `axis(negative, positive)` gives -1 to 1 for movement.

## Setting up

```toml
[modules.windows]
platform = ["sdl3"]
input_device = ["sdl3_gamepad", "null"]
# ...the rest of the project's modules.

[modules.android]
platform = ["sdl3"]
input_device = ["sdl3_gamepad", "null"]

[input]
stick_deadzone = 0.2     # Optional. Of the stick's reach, around its centre.
trigger_deadzone = 0.1   # Optional.

[input.actions]
jump = ["key:space", "touch", "gamepad:south"]
pause = ["key:escape", "key:back", "gamepad:start"]
move_left = ["key:a", "key:left", "gamepad:left_stick_left", "gamepad:dpad_left"]
move_right = ["key:d", "key:right", "gamepad:left_stick_right", "gamepad:dpad_right"]
move_up = ["key:w", "key:up", "gamepad:left_stick_up", "gamepad:dpad_up"]
move_down = ["key:s", "key:down", "gamepad:left_stick_down", "gamepad:dpad_down"]
accelerate = ["key:left_shift", "gamepad:right_trigger"]
```

| `input_device` | What it does |
|---|---|
| `sdl3_gamepad` | Gamepads through SDL3: Xbox, PlayStation, Switch Pro and most others on Windows, and Bluetooth or USB gamepads on Android (not yet tried on a phone). Plugging in and out works while the game runs. |
| `null` | No gamepads. |
| *(not listed)* | The same as `null`. |

## Reading actions

```cpp
void on_update(runtime::App& app, const core::FrameTime& time) override {
    const runtime::InputActions& input = app.input();

    if (input.was_pressed("jump")) {     // This frame only.
        jump();
    }
    if (input.is_down("accelerate")) {   // While held.
        speed += acceleration * time.delta_seconds;
    }

    // -1 to 1: full for keys and the d-pad, partway for a stick pushed partway.
    const float x = input.axis("move_left", "move_right");
    const float y = input.axis("move_down", "move_up");
    position += core::Vec3{x, 0.0f, -y} * walk_speed * time.delta_seconds;
}
```

| Call | Means |
|---|---|
| `is_down(action)` | Any of its bindings is held. An analog binding counts from halfway. |
| `was_pressed(action)` | It went down during this frame. |
| `was_released(action)` | It went up during this frame. |
| `value(action)` | How strongly it is held, from 0 to 1: the largest of its bindings. |
| `axis(negative, positive)` | `value(positive) - value(negative)`. |
| `pointer_position()` | Where the mouse or first finger last was, in pixels. |
| `last_source()` | `KeyboardMouse`, `Touch` or `Gamepad`: what the player last pressed something on. |

Asking about an action that isn't in the configuration is a programming error, caught by an
assertion in debug builds. Every configuration a game runs with, including its test
configurations, needs every action the game asks about.

## Bindings

| Binding | Input |
|---|---|
| `key:a` to `key:z`, `key:0` to `key:9`, `key:f1` to `key:f12` | Letters, digits and function keys, by position, so WASD stays in place on any layout |
| `key:space`, `key:enter`, `key:escape`, `key:tab`, `key:backspace`, `key:delete`, `key:home`, `key:end` | |
| `key:left`, `key:right`, `key:up`, `key:down` | Arrow keys |
| `key:left_shift`, `key:right_shift`, `key:left_control`, `key:right_control`, `key:left_alt`, `key:right_alt` | |
| `key:back` | Android's back button or gesture |
| `mouse:left`, `mouse:right`, `mouse:middle` | |
| `touch` | Held while any finger is on the screen |
| `gamepad:south`, `gamepad:east`, `gamepad:west`, `gamepad:north` | Face buttons by position |
| `gamepad:a`, `gamepad:b`, `gamepad:x`, `gamepad:y` | The same, by their Xbox names |
| `gamepad:start`, `gamepad:back`, `gamepad:guide` | Menu / Options / Plus; View / Share / Minus; the logo button |
| `gamepad:left_shoulder`, `gamepad:right_shoulder` | LB and RB, L1 and R1 |
| `gamepad:left_stick_click`, `gamepad:right_stick_click` | Pressing a stick in |
| `gamepad:dpad_up`, `gamepad:dpad_down`, `gamepad:dpad_left`, `gamepad:dpad_right` | |
| `gamepad:left_trigger`, `gamepad:right_trigger` | Analog, 0 to 1 |
| `gamepad:left_stick_up`, `_down`, `_left`, `_right`, and the same for `right_stick_` | A stick pushed one way; analog |

Face buttons by position, south for confirm and east for cancel, is the Xbox and PlayStation
habit. Nintendo's habit is the opposite (east to confirm); a game that wants it can offer a
setting that swaps two bindings.

### Analog inputs, deadzones and thresholds

A stick never rests exactly at its centre, so a small circle around it, the **deadzone**, counts as
not pushed. Past it, the rest of the reach is stretched back to 0 to 1, so the value grows
smoothly from the edge of the deadzone. The deadzone is round, so pushing diagonally counts as soon
as pushing straight does, and a stick pushed fully diagonally gives about 0.71 in each direction,
never more than 1 in total.

An analog binding makes its action **down** from 0.5, and up again only below 0.4, so a stick
resting near halfway doesn't flicker between pressed and released. Use `value()` or `axis()` for
movement, and `was_pressed()` for discrete things such as stepping through a menu with a stick.

Raise `stick_deadzone` if worn gamepads drift: a camera turning on its own, or a character
creeping, is a stick resting outside the deadzone. 0.25 covers most old gamepads.

## Gamepads in detail

Every connected gamepad drives the actions, so a single-player game works with whichever the
player picks up. Games that tell players apart read each gamepad:

```cpp
for (const input_device::GamepadState& pad : app.gamepads()) {
    Player& player = player_for(pad.id);   // The id stays the same while it is connected.
    player.move(pad.axis(input_device::GamepadAxis::LeftX),
                pad.axis(input_device::GamepadAxis::LeftY));
    if (pad.button(input_device::GamepadButton::South)) {
        player.jump();
    }
}
```

These values are raw: no deadzone is applied, so apply one before moving anything with them.

**Rumble:** `app.rumble(pad.id, low, high, milliseconds)` runs the heavy (low-frequency) and light
(high-frequency) motors from 0 to 1, replacing any rumble before. It returns false for a gamepad
without motors. Keep rumbles short and occasional: a hit, a landing, an explosion.

**Button prompts:** `pad.kind` says `Xbox`, `PlayStation`, `NintendoSwitch` or `Other`, and
`input.last_source()` whether the player last used a gamepad, so a game can show "Press A",
"Press ✕" or "Press Space" to match.

**Focus:** while another program has the focus on a PC, the game ignores gamepads, so playing
something else doesn't move it.

## Designing for gamepads

- **Bind every action to a gamepad as well as keys,** including pause (`gamepad:start`) and back
  or cancel (`gamepad:east`). On Steam Deck and TV there may be no keyboard at all.
- **Move with `axis()`,** not `is_down()`, so a stick pushed gently walks slowly.
- **Aim needs a reticle.** A game that aims with the pointer can't be played with a stick unless
  it draws a cursor or crosshair that the stick moves. The engine has no shared one yet.
- **Menus need focus.** Menus built with the `ui` interface follow the pointer and touch; moving
  focus between buttons with a d-pad isn't there yet. Until it is, give gamepad players actions
  for what menus do, as the samples bind `menu` to `gamepad:start`.
- **Test with the gamepad unplugged and plugged in mid-game.** The gamepad list changes between
  frames; ids of gamepads that remain don't.

## Testing

Test configurations list `input_device = ["null"]`, so a gamepad on the desk can't change an
unattended check. Unit tests feed `InputActions::apply` gamepad states directly, and the
`sdl3_gamepad` module's tests use SDL's virtual gamepads, which go through the same code in SDL as
real ones.

## Limits

- No focus navigation in `ui` menus with a gamepad.
- No rebinding while the game runs; bindings come from the configuration.
- No gyroscopes, touchpads, lights or adaptive triggers.
- No shared on-screen cursor for aiming with a stick.
