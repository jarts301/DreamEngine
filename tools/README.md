# Tools

Offline tools such as shader compilation and asset cooking. Tools run on the development machine and
are not part of a shipped game.

| Tool | Purpose |
|---|---|
| `dream/` | The `dream` command: `dream new <name>` makes a game project from `templates/game` (ADR 0045). Installed into the SDK's `bin` folder. |
| `editor/` | `dream_editor`, which edits a project's scene files, with Play through hot reload (ADR 0046). Installed into the SDK's `editor` folder. |
| `pack/` | `dream_pack`, which packs a PC game's assets into `assets.pak`, and each of its bundles into `<bundle>.pak` (ADR 0024, 0047). |
| `model_cook/` | `dream_model_cook`, which makes models' levels of detail at build time (ADR 0042). |
| `check_layers.py` | Fails if code breaks the layer rules. Needs Python 3 and runs in CI. |
| `test_check_layers.py` | Tests for `check_layers.py`. |
| `make_sample_models.py` | Generates the models in `samples/assets`, which the samples share. |
| `make_sample_sounds.py` | Generates the sounds in `samples/assets`. |
