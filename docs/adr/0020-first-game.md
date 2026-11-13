# 0020. Toppler, the first game

- Status: Accepted
- Date: 2026-11-13

## Context

Phase 9 of the plan is to ship a small original game on PC and Android, built on the arcade sample,
because a real game shows which interfaces are wrong in a way samples don't. It was skipped while
Phase 10 added second backends, so those are now tried by a game too.

## Decision

**The game.** Toppler is a physics puzzle in `samples/toppler`. Each of its 10 levels stands gold
crates on platforms, towers, posts and behind walls, and the player knocks every one of them down
within the level's throws. A tap or a click throws a ball that lands where the player pointed, on a
curve that takes the same time for any distance at the ball's speed, so aiming means choosing what
to hit rather than allowing for gravity. A gold crate counts once it has fallen 0.9 m below where it
started. Clearing a level within its par earns three stars, one throw over two, and more one. It
has a title screen, a level select, a pause menu with music and effects volumes, and a result
screen.

**Levels are scenes.** Each level is an ordinary scene file in `assets/levels/`, with two
components the game registers: `level`, for its name, throws and par, and `target`, which marks a
gold crate. Gold crates are a new plain model, `block.glb`, tinted gold, so they stand out from the
wooden crates. A level can be changed without touching code.

**Progress is saved** as TOML in `user:/progress.toml`, through the engine's file system: the best
stars for each level and the two volumes. Each level opens once the one before it is cleared. A file
that can't be read starts afresh rather than stopping the game.

**It checks itself.** With `scripted = true`, the game plays through: it opens the level select,
starts the first level, turns the music down from the pause menu, then on each level taps where the
first gold crate still standing is on screen, as a player would, and tries a lost level once more.
At the end it reads its save back and logs `cleared=10 stars=... saved_stars=... saved_music=0.30`.
CTest runs it with no graphics device or sound hardware, with the first backends and, on Windows,
the second ones. On PC its storage is named `TopplerCheck`, apart from a player's. On Android, user
storage belongs to the app's package whatever the name, so a scripted APK shares a player's save.

### What the game changed in the engine

- **`runtime::camera_ray` and `camera_view`.** Turning a tapped pixel into a ray was copied into
  each game, and the arcade's copy ignored a camera turned without a target. The runtime now builds
  the camera's view once, for drawing and for rays, and the arcade sample uses it too.
- **`ui::GameUi::set_value`.** A game could read a slider but not set one, so saved volumes couldn't
  be shown. It sets a control's value without a Change event, in RmlUi, markup and the null
  backend. markup's library gained `Context::set_value`.
- **Assets are copied on every build.** A game's assets were copied only after its program was
  linked, so a changed level or model wasn't seen until some code changed too. They are now copied
  by a target of their own, after the shaders are compiled.

## Consequences

- The engine has a game that uses graphics, physics, audio, the game interface, scenes, saving and
  input together, on both platforms and with either backend of each.
- The scripted run only proves each level can be cleared by hitting gold crates directly. Whether
  the levels are fun, and their pars fair, needs people to play them.
- Toppler has no debug tools or profiler listed, as a shipped game wouldn't. They can be listed in
  `dream.toml` like any other module.
- Music restarts with each level, because each level is a new scene with its own music source.
