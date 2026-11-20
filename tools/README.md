# Tools

Tools run on the development machine and are not part of a shipped game.

| Tool | Purpose |
|---|---|
| `hub/` | `dream_hub`: the web page from which the engine is built, its games started and played, its tests run and its screenshots compared (ADR 0039). Built with the engine. |
| `pack/` | `dream_pack`: packs a game's assets into `assets.pak`, for `<game>_package` targets. |
| `check_layers.py` | Fails if code breaks the layer rules. Optional; needs Python 3. |
| `test_check_layers.py` | Tests for `check_layers.py`. |
| `make_sample_models.py` | Generated the models in `samples/assets`. Optional; needs Python 3. |
| `make_sample_sounds.py` | Generated the sounds in `samples/assets`. |
| `make_particle_textures.py` | Generated the particle textures of `render_demo`. |

The generated files are kept in the repository, so nothing here has to run for a build.
