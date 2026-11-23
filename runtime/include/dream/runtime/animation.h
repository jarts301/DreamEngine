#pragma once

#include "dream/animation/animation.h"
#include "dream/core/math/matrix.h"
#include "dream/core/result.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/model.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dream::runtime {

class App;

// Plays one of its model's clips on an entity that has a MeshRenderer (ADR 0036). Animation::update
// moves it on and poses the model; the renderer draws the pose. Set `clip` and `time` directly, or
// use Animation::play for a smooth change from the last clip.
struct Animator {
    // The clip's name in the model, as its glTF file names it. Empty, or a name the model doesn't
    // have, leaves the model at rest.
    std::string clip;
    // In seconds since the clip started.
    float time = 0.0f;
    // How fast time passes for the clip: 1 as authored, 0 to hold a pose, negative to play back.
    float speed = 1.0f;
    // At the end, start again; otherwise stop on the last pose.
    bool loop = true;
    bool playing = true;

    // While Animation::play fades from the last clip, set by it.
    std::string fading_clip;
    float fading_time = 0.0f;
    float fade_left = 0.0f;
    float fade_length = 0.0f;

    // Each of the model's skins' joint matrices, as posed by the last update: what the renderer
    // draws with. Animation fills them.
    std::vector<std::vector<core::Mat4>> skinning;
};

// Plays animators with the project's animation module, chosen by its `animation` list in
// dream.toml, such as `animation = ["ozz", "null"]` (ADR 0036). Games call update once per frame,
// before rendering:
//
//     on_start:   animation.initialize(app);
//     on_update:  animation.update(*world, time.delta_seconds);   // Before Renderer::render.
//
// A skinned model without an Animator, or in a project without this, is drawn at rest. initialize
// lets scene files use the component "animator":
//
//     [entities.hero.animator]
//     clip = "walk"          # Optional; nothing plays without it.
//     speed = 1.0            # All optional.
//     loop = true
//     playing = true
//     time = 0.0
//
// docs/guides/animation.md explains how to make and use animated models.
class Animation {
public:
    Animation();
    ~Animation();

    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;

    // Creates the configured animation module. Game::on_start is the place for it, before any scene
    // with animators loads.
    [[nodiscard]] core::Result<void> initialize(App& app);

    // The same with a given module, for tests or a game that makes its own.
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<animation::AnimationSystem> system);

    // Moves every animator of the world on by `seconds`, and poses its model.
    void update(World& world, double seconds);

    // Starts `clip` on the entity's Animator, adding one if it has none, from its start. The last
    // clip fades out over `fade_seconds` while this one fades in; 0 changes at once. Fails if the
    // entity has no model with that clip.
    [[nodiscard]] core::Result<void> play(World& world, Entity entity, std::string_view clip,
                                          float fade_seconds = 0.2f, bool loop = true);

    // The names of a model's clips, in its file's order.
    [[nodiscard]] static std::vector<std::string_view> clip_names(const ModelAsset& model);

    // How long a model's clip lasts, in seconds, or 0 if it has no such clip.
    [[nodiscard]] static float duration(const ModelAsset& model, std::string_view clip);

    // Null until initialized.
    [[nodiscard]] animation::AnimationSystem* system() noexcept {
        return m_system.get();
    }

private:
    // A model's skeleton and clips, as the module built them.
    struct Built {
        // Keeps the model alive while its handles are.
        AssetRef<ModelAsset> model;
        animation::SkeletonHandle skeleton;
        std::vector<animation::ClipHandle> clips;
    };
    // One animated entity's player, kept while it is seen each update.
    struct Live {
        animation::PlayerHandle player;
        const ModelAsset* model = nullptr;
        std::uint64_t seen = 0;
    };

    [[nodiscard]] const Built* build(const AssetRef<ModelAsset>& model);
    void release(Live& live);

    std::unique_ptr<animation::AnimationSystem> m_system;
    std::unordered_map<const ModelAsset*, Built> m_built;
    // By World::id and entity.
    std::map<std::pair<std::uint64_t, std::uint32_t>, Live> m_live;
    WorldLifetimes m_worlds;
    std::uint64_t m_update = 0;
    std::vector<animation::Layer> m_layers;
    std::vector<core::Mat4> m_model_space;
};

} // namespace dream::runtime
