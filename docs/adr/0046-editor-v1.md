# 0046. Editor v1: scene editing over the engine, with panels as modules

- Status: Accepted
- Date: 2026-11-24

## Context

Scenes were written by hand in TOML. Phase 13 of the roadmap asks for a first editor, built on the
engine with its interface in Dear ImGui (decided in section 5 of the roadmap), with:

- a scene view;
- an entity tree, and an inspector whose controls come from components describing their fields;
- saving scenes to TOML;
- play-in-editor through the existing hot reload;
- panels as `editor_panel` modules over `debug_ui`, so a project can add its own.

Three facts shaped the design:

- A game's code, and so its own components, is compiled into the game, not into an editor. An
  editor that loaded game code would need games built as libraries, and a way to reload them.
- The scene file is the source of truth: games load it, people read it in reviews, and git merges
  it. An editor that saved its world instead would lose what the world doesn't hold, such as a
  component the editor doesn't know.
- The `debug_ui` interface held only a debug overlay's widgets: no trees, text fields or drags.

## Decision

### The editor edits the scene file, and builds the world from it

The editor keeps the scene as a `runtime::SceneDocument`: its tables and settings, in the file's
order, with the comment lines at the top of the file. Every change edits the document, and the
world in the scene view is built again from the document's text by the program's `SceneLoader`.
Saving writes the document back: tables, settings and their order stay, and values keep their
types, so `2` stays an integer and `2.0` a float. Comments other than the leading ones are lost,
since TOML readers drop them.

A change the loader refuses, such as a spot light's inner angle above its outer one, is kept, the
inspector shows why, and the view keeps the last world that built, so the next change can fix it.

The editor can't build a game's own components, so its loader skips components nobody registered
(`SceneLoader::set_skip_unknown_components`). They stay in the document, and the inspector edits
their settings by their values' types.

Changes within half a second of each other, such as a drag, undo as one step. Undo keeps 200 steps.

### The reflection registry: components describe their fields

`SceneLoader::register_component` takes, beside the reader, a list of `core::FieldInfo` (in
`core/fields.h`): each field's name, kind (number, integer, flag, text, choice, asset, two or three
numbers, color), value when left out, range, choices and help. The inspector shows a control for
each described field, with "(default)" beside one the scene leaves out, and changing it writes the
setting. Settings no field describes get a control by their type, and a button to remove them.

Described today: `transform`, `camera`, `directional_light`, `point_light`, `spot_light`, `mesh`,
`environment`, `camera_2d` and `sprite`. Others, and games' components, work by type until they
describe theirs.

### Panels are modules, and every listed one is created

`editor_panel::EditorPanel` draws one window's contents with `debug_ui` widgets, and sees and
changes the scene through `editor_panel::SceneModel`, which names entities by their names in the
file. Two modules come with the engine:

| Module | Shows |
|---|---|
| `hierarchy` | Entities under their parents. Add makes an entity inside the selected one; Remove moves its children up |
| `inspector` | The selected entity's name, parent and components, with a control per setting, and adding and removing components |

Unlike other interfaces, all panels a program lists are created, in order, through the new
`ModuleProject::create_all` and `App::create_modules`. The editor places the panels on the side
each asks for. `debug_ui` gained the widgets panels need: placed windows, trees, selectable rows,
headers, drags, colors, text fields that hand over their text when left, combo boxes and tooltips.
They have defaults, so a backend without them still works. The debug overlay now turns the
platform's text input on while a field is typed in, as the game interface already did, so typed
text reaches the panels; until now the debug tools had only buttons and sliders.

### The program

`dream_editor`, in `tools/editor`, is built like a game, with its own `editor.toml`, on the build
machine, and installed into the SDK's `editor` folder:

```
dream_editor <project folder>     Opens the project's scene.
dream_editor <scene file>         Opens that scene of the project it is in.
```

Programs now get their command line through `runtime::program_arguments()`. The editor reads the
project's `assets` folder before its own, through `VirtualFileSystem::add_asset_folder`, so a
scene's `assets:/` paths reach the project's source files: textures load from their PNG files, and
models' levels of detail are made as they load.

The scene view draws the world from a camera the editor adds to it, with a priority above any
scene's: cameras now have a `priority`, and the highest draws (`runtime::active_camera`). Right drag
turns around a point, middle drag moves it, the wheel goes nearer, F frames the selection, and a
click selects the nearest model, or the marker of an entity without one, such as a light. The
selected entity has a box around it. Particles and animation move in the view when the editor's
configuration lists their modules.

Ctrl+S saves, Ctrl+Z and Ctrl+Y undo and redo, and Delete removes the selected entity. A scene
changed on disk by another program is read again, unless there are unsaved changes here. Unsaved
changes are written beside the scene, as `<scene>.unsaved`, when the editor closes.

### Play

A project's `dream.toml` says what the editor opens and how it plays the game:

```toml
[editor]
scene = "assets:/scenes/main.toml"   # The default.
build = "build/windows-debug"        # From the project's folder; the default.
game = "my_game"                     # The game's target; the folder's name by default.
assets = ["assets", "../shared"]     # Its asset folders, as its build's ASSETS lists them.
```

The editor reads `assets:/` paths from those folders, later ones first, as the build merges them:
the engine's samples list their own folder and the shared `samples/assets`, and each sample's
`[editor]` table names its scene, so `dream_editor samples/render_demo` opens the yard.

Play saves, builds the game with `cmake --build <build> --target <game>`, and starts it. While it
runs, each save builds its assets target again, and the game's hot reload picks the change up:
models and textures reload by themselves, and scene files are now watched too. When a scene file
the `SceneLoader` loaded changes, the App publishes `runtime::ScenesChanged`, and the game decides
what to load again; the project template loads its scene again, keeping the last one if the new
one fails. The game's output goes to `<build>/editor_game.log`. Stop asks the game to close, as its
window's close button would, so it saves and ends as usual, and ends it only if it doesn't within a
few seconds.

### Checks

`dream_editor_tests` checks the scene model: building, refused changes, undo, entities, settings
inside tables, saving and changes on disk. The panels' tests drive them with a scripted interface,
and the imgui module's tests type into a real text field. `dream_editor_null` opens the project
template on the `null` backends in CTest. Two checks need a GPU: `dream_editor_vulkan_check`
saves a picture of the template with its crate selected, and `dream_editor_play_check`, given a
project made with `dream new`, plays it, moves the crate and saves while it runs, then quits; the
game's log then shows it loaded the scene again.

## Consequences

- Scenes keep being plain TOML files, readable and mergeable, which the editor and people both
  edit. Comments inside a scene, other than at its top, don't survive the editor's save.
- The editor works with any game's scenes without linking its code, at the cost of not running its
  components in the view: a game's own behavior shows when it plays.
- Every edit builds the world again. That is immediate for the scenes the samples have; very large
  scenes would need edits applied to the world in place.
- Play needs CMake on the PATH, and a game built in debug, where hot reload is on.
- Components describe their fields beside their readers, which can drift apart: a field the reader
  stops reading still shows.
- Still to do: an asset browser, moving entities with handles in the view, editing 2D scenes from
  a 2D camera, multi-selection and copying, field descriptions for the remaining built-in
  components, and applying edits to the world without building it again.
