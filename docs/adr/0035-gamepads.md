# 0035. Gamepads

- Status: Accepted
- Date: 2026-11-19

## Context

Games took input from the keyboard, mouse and touch screen only, through named actions bound in
`[input.actions]`. Gamepads are required on Steam Deck, expected on PC and TV, and common on
phones. Every action was on or off; sticks and triggers need values between.

## Decision

### An interface of its own

Gamepads are a module, as ADR 0006 asks of every feature. The new `input_device` interface reports
the connected gamepads, each with an id that isn't reused while the program runs, a name, its
family (Xbox, PlayStation, Nintendo Switch or other, for button prompts), 15 buttons and 6 axes,
and can rumble one:

- **`sdl3_gamepad`** uses SDL3's gamepad support: XInput, DirectInput and Windows.Gaming.Input on
  Windows, and Android's gamepads through SDL's Java side, which the platform module already
  ships. SDL's database of mappings names buttons by position.
- **`null`** reports none.

Buttons are named by position, not label: `South` is A on Xbox, Cross on PlayStation and B on
Switch, so a binding means the same place on every gamepad. Stick Y is positive up, away from the
player, and triggers go from 0 to 1. The module reports raw values; deadzones are the runtime's.

The SDL module reads buttons and axes directly each frame rather than through events, so it works
whichever platform module pumps SDL's events. It learns of gamepads coming and going through an
event watch, which sees events without taking them from the platform's queue, and lists the
gamepads again only then. It keeps SDL's default of ignoring gamepads while another program has
the focus.

### The App feeds the actions

When a project lists an `input_device`, `App::run` creates it after the window, updates it every
frame after the platform's events, and gives the gamepads to `InputActions`. Games bind gamepads
as they bind keys:

```toml
[input.actions]
jump = ["key:space", "gamepad:south"]
move_left = ["key:a", "gamepad:left_stick_left", "gamepad:dpad_left"]
```

Every connected gamepad drives the actions. `App::gamepads()` gives each gamepad's state, for
games that tell players apart, and `App::rumble` shakes one.

### Analog actions

Each input now has a value from 0 to 1: 1 for a key or button, and how far a trigger or a stick's
direction is pushed. `InputActions::value(action)` is the largest of an action's bindings, and
`axis(negative, positive)` their difference, so moving with keys and sticks is one line. An
analog input is down from 0.5 and up again below 0.4, so a value near the threshold doesn't
flicker. Sticks have a round deadzone, 0.2 of their reach by default, and triggers one of 0.1,
both set in `[input]`; past it, the rest of the range is stretched back to 0 to 1.

`InputActions::last_source()` says whether the player last pressed something on the keyboard and
mouse, the touch screen or a gamepad, for showing the right prompts.

### Tests stay unattended

Every sample lists `input_device = ["sdl3_gamepad", "null"]`, and every test configuration
`["null"]`, so a gamepad on the desk can't change a check's result. The SDL module's tests use
SDL's virtual gamepads.

## Consequences

- Games get gamepads by listing the module and adding bindings. The render demo orbits and zooms
  its camera with the left stick, the d-pad or the triggers, as with keys.
- Games that aim with the pointer, such as Toppler, the arcade and the physics demo, only bind
  their buttons: aiming with a stick needs an on-screen reticle, which each game draws itself.
- Menus made with the `ui` interface don't move focus with a gamepad yet. That needs focus
  navigation in the `ui` modules.
- Bindings are fixed by the configuration; rebinding in a settings menu is later work.
- Gyroscopes, touchpads, adaptive triggers and lights on gamepads aren't read.
- A real Xbox 360 controller was found on the development PC; buttons and axes were checked
  through SDL's virtual gamepads, which use the same path inside SDL. A gamepad on Android hasn't
  been tried.
