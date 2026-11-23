#include "dream/runtime/animation.h"

#include "dream/core/log.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace dream::runtime {
namespace {

core::Result<void> read_animator(ComponentContext& context) {
    if (core::Result<void> known =
            context.data.check_keys({"clip", "speed", "loop", "playing", "time"});
        !known.has_value()) {
        return known;
    }
    Animator animator;
    core::Result<std::string> clip = context.data.text("clip", "");
    const core::Result<float> speed = context.data.number("speed", animator.speed);
    const core::Result<bool> loop = context.data.flag("loop", animator.loop);
    const core::Result<bool> playing = context.data.flag("playing", animator.playing);
    const core::Result<float> time = context.data.number("time", animator.time);
    for (const core::Error* error : {clip.has_value() ? nullptr : &clip.error(),
                                     speed.has_value() ? nullptr : &speed.error(),
                                     loop.has_value() ? nullptr : &loop.error(),
                                     playing.has_value() ? nullptr : &playing.error(),
                                     time.has_value() ? nullptr : &time.error()}) {
        if (error != nullptr) {
            return *error;
        }
    }
    animator.clip = std::move(clip).value();
    animator.speed = speed.value();
    animator.loop = loop.value();
    animator.playing = playing.value();
    animator.time = time.value();
    context.world.registry().emplace_or_replace<Animator>(context.entity, std::move(animator));
    return {};
}

std::optional<std::size_t> clip_index(const ModelAsset& model, std::string_view name) {
    const auto found = std::ranges::find(model.clips, name, &ModelClip::name);
    if (found == model.clips.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - model.clips.begin());
}

// A clip's time after `seconds` more, wrapped or held at the ends.
float advance(float time, float seconds, float duration, bool loop, bool& playing) {
    time += seconds;
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (loop) {
        time = std::fmod(time, duration);
        return time < 0.0f ? time + duration : time;
    }
    if (time >= duration || time <= 0.0f) {
        playing = false;
    }
    return std::clamp(time, 0.0f, duration);
}

} // namespace

Animation::Animation() = default;

Animation::~Animation() {
    for (auto& [key, live] : m_live) {
        release(live);
    }
    if (m_system != nullptr) {
        for (auto& [model, built] : m_built) {
            for (const animation::ClipHandle clip : built.clips) {
                m_system->destroy_clip(clip);
            }
            m_system->destroy_skeleton(built.skeleton);
        }
    }
}

core::Result<void> Animation::initialize(App& app) {
    core::Result<std::unique_ptr<animation::AnimationSystem>> system =
        app.create_module<animation::AnimationSystem>();
    if (!system.has_value()) {
        return system.error();
    }
    return initialize(app, std::move(system).value());
}

core::Result<void> Animation::initialize(App& app,
                                         std::unique_ptr<animation::AnimationSystem> system) {
    if (system == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "Animation needs a module"};
    }
    m_system = std::move(system);
    app.scenes().register_component("animator", &read_animator);
    return {};
}

const Animation::Built* Animation::build(const AssetRef<ModelAsset>& model) {
    if (const auto found = m_built.find(model.get()); found != m_built.end()) {
        return &found->second;
    }
    const ModelSkeleton& skeleton = model->skeleton;
    const core::Result<animation::SkeletonHandle> made =
        m_system->create_skeleton(animation::SkeletonDesc{.parents = skeleton.parents,
                                                          .rest_pose = skeleton.rest_pose,
                                                          .names = skeleton.names});
    if (!made.has_value()) {
        DREAM_LOG_ERROR("A model's skeleton can't be animated: {}", made.error().message);
        return nullptr;
    }
    Built built{.model = model, .skeleton = made.value(), .clips = {}};
    for (const ModelClip& clip : model->clips) {
        core::Result<animation::ClipHandle> handle = m_system->create_clip(
            built.skeleton,
            animation::ClipDesc{.name = clip.name, .duration = clip.duration, .tracks = clip.tracks});
        if (!handle.has_value()) {
            DREAM_LOG_ERROR("Clip '{}' can't be played: {}", clip.name, handle.error().message);
        }
        // An unusable clip keeps its place, invalid, so indices still match the model's.
        built.clips.push_back(handle.value_or(animation::ClipHandle{}));
    }
    return &m_built.emplace(model.get(), std::move(built)).first->second;
}

void Animation::release(Live& live) {
    if (m_system != nullptr && live.player.is_valid()) {
        m_system->destroy_player(live.player);
    }
    live.player = {};
}

void Animation::update(World& world, double seconds) {
    if (m_system == nullptr) {
        return;
    }
    DREAM_PROFILE_ZONE("Animation");
    ++m_update;
    const auto step = static_cast<float>(seconds);
    Registry& registry = world.registry();

    for (const auto [entity, animator, mesh] : registry.view<Animator, const MeshRenderer>().each()) {
        if (mesh.model == nullptr || mesh.model->skeleton.parents.empty()) {
            continue;
        }
        const ModelAsset& model = *mesh.model;
        const Built* const built = build(mesh.model);
        if (built == nullptr) {
            continue;
        }

        // The entity's player, made again if its model changed.
        Live& live = m_live[{world.id(), static_cast<std::uint32_t>(entt::to_integral(entity))}];
        if (live.model != &model) {
            release(live);
            live.model = &model;
        }
        if (!live.player.is_valid()) {
            live.player = m_system->create_player(built->skeleton).value_or(animation::PlayerHandle{});
        }
        live.seen = m_update;

        // Time moves on, for the clip and for the one fading out.
        m_layers.clear();
        const std::optional<std::size_t> current = clip_index(model, animator.clip);
        if (current.has_value() && animator.playing) {
            animator.time = advance(animator.time, step * animator.speed,
                                    model.clips[*current].duration, animator.loop,
                                    animator.playing);
        }
        float weight = 1.0f;
        if (animator.fade_left > 0.0f && animator.fade_length > 0.0f) {
            animator.fade_left = std::max(animator.fade_left - step, 0.0f);
            const float fading_weight = animator.fade_left / animator.fade_length;
            // A finished fade drops the last clip the same frame.
            if (animator.fade_left <= 0.0f) {
                animator.fading_clip.clear();
            } else if (const std::optional<std::size_t> fading =
                           clip_index(model, animator.fading_clip)) {
                bool fading_plays = true;
                animator.fading_time = advance(animator.fading_time, step * animator.speed,
                                               model.clips[*fading].duration, animator.loop,
                                               fading_plays);
                m_layers.push_back(animation::Layer{.clip = built->clips[*fading],
                                                    .time = animator.fading_time,
                                                    .weight = fading_weight});
                weight = 1.0f - fading_weight;
            }
        }
        if (current.has_value()) {
            m_layers.push_back(animation::Layer{
                .clip = built->clips[*current], .time = animator.time, .weight = weight});
        }

        m_model_space.resize(model.skeleton.parents.size());
        m_system->pose(live.player, m_layers, m_model_space);
        animator.skinning.resize(model.skins.size());
        for (std::size_t skin = 0; skin < model.skins.size(); ++skin) {
            skinning_matrices(model.skins[skin], m_model_space, animator.skinning[skin]);
        }
    }

    // Players of entities no longer animated in this world go, and those of destroyed worlds.
    m_worlds.note(world);
    for (auto it = m_live.begin(); it != m_live.end();) {
        const bool gone = it->first.first == world.id() ? it->second.seen != m_update
                                                        : m_worlds.destroyed(it->first.first);
        if (gone) {
            release(it->second);
            it = m_live.erase(it);
        } else {
            ++it;
        }
    }
    m_worlds.prune();
}

core::Result<void> Animation::play(World& world, Entity entity, std::string_view clip,
                                   float fade_seconds, bool loop) {
    Registry& registry = world.registry();
    const MeshRenderer* const mesh = registry.try_get<MeshRenderer>(entity);
    if (mesh == nullptr || mesh->model == nullptr || !clip_index(*mesh->model, clip).has_value()) {
        return core::Error{core::ErrorCode::NotFound,
                           std::format("The entity's model has no clip '{}'", clip)};
    }
    Animator& animator = registry.get_or_emplace<Animator>(entity);
    if (fade_seconds > 0.0f && !animator.clip.empty() && animator.clip != clip) {
        animator.fading_clip = animator.clip;
        animator.fading_time = animator.time;
        animator.fade_left = fade_seconds;
        animator.fade_length = fade_seconds;
    } else {
        animator.fade_left = 0.0f;
    }
    animator.clip = std::string(clip);
    animator.time = 0.0f;
    animator.loop = loop;
    animator.playing = true;
    return {};
}

std::vector<std::string_view> Animation::clip_names(const ModelAsset& model) {
    std::vector<std::string_view> names;
    for (const ModelClip& clip : model.clips) {
        names.push_back(clip.name);
    }
    return names;
}

float Animation::duration(const ModelAsset& model, std::string_view clip) {
    const std::optional<std::size_t> index = clip_index(model, clip);
    return index.has_value() ? model.clips[*index].duration : 0.0f;
}

} // namespace dream::runtime
