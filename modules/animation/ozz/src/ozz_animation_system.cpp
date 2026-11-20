// Skeletal animation by ozz-animation (MIT): skeletons and clips are built with its offline
// builders as a model loads, and posed with its sampling, blending and local-to-model jobs, which
// work on four joints at a time (ADR 0036).

#include "dream/animation/animation.h"
#include "dream/core/handle.h"
#include "dream/core/module_registry.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/memory/unique_ptr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace animation;

// The same handle, for the module's pool of the type it names, or the other way.
template <typename To, typename From> core::Handle<To> retag(core::Handle<From> handle) {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

namespace offline = ozz::animation::offline;

core::Error animation_error(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

ozz::math::Float3 to_ozz(core::Vec3 v) {
    return ozz::math::Float3(v.x, v.y, v.z);
}

ozz::math::Quaternion to_ozz(core::Quat q) {
    return ozz::math::Quaternion(q.x, q.y, q.z, q.w);
}

struct Skeleton {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    // Ozz orders joints depth first. Each of ours, by our index, is this one of ozz's.
    std::vector<int> ozz_index;
    std::vector<JointTransform> rest_pose;
};

struct Clip {
    SkeletonHandle skeleton;
    ozz::unique_ptr<ozz::animation::Animation> animation;
};

struct Player {
    SkeletonHandle skeleton;
    // One sampling context and pose per layer, kept so each layer's clip samples incrementally.
    std::vector<std::unique_ptr<ozz::animation::SamplingJob::Context>> contexts;
    std::vector<std::vector<ozz::math::SoaTransform>> layer_poses;
    std::vector<ozz::math::SoaTransform> blended;
    std::vector<ozz::math::Float4x4> model;
    std::vector<ozz::animation::BlendingJob::Layer> blend_layers;
};

class OzzAnimationSystem final : public AnimationSystem {
public:
    core::Result<SkeletonHandle> create_skeleton(const SkeletonDesc& desc) override {
        const std::size_t count = desc.parents.size();
        if (count == 0 || desc.rest_pose.size() != count ||
            count > static_cast<std::size_t>(ozz::animation::Skeleton::kMaxJoints)) {
            return animation_error(std::format(
                "A skeleton needs from 1 to {} joints, each with a parent and a rest transform",
                static_cast<int>(ozz::animation::Skeleton::kMaxJoints)));
        }
        for (std::size_t joint = 0; joint < count; ++joint) {
            const std::int32_t parent = desc.parents[joint];
            if (parent < -1 || parent >= static_cast<std::int32_t>(count) ||
                parent == static_cast<std::int32_t>(joint)) {
                return animation_error("A joint's parent isn't another joint of its skeleton");
            }
        }

        // The tree, built from the parents. Each joint is named by its index, which finds it again
        // in ozz's order afterwards.
        offline::RawSkeleton raw;
        std::vector<std::vector<std::size_t>> children(count);
        std::vector<std::size_t> roots;
        for (std::size_t joint = 0; joint < count; ++joint) {
            const std::int32_t parent = desc.parents[joint];
            (parent < 0 ? roots : children[static_cast<std::size_t>(parent)]).push_back(joint);
        }
        std::size_t added = 0;
        const auto build = [&](auto& self, std::size_t joint, offline::RawSkeleton::Joint& out,
                               std::size_t depth) -> bool {
            if (depth > count) {
                return false;
            }
            ++added;
            const JointTransform& rest = desc.rest_pose[joint];
            out.name = std::to_string(joint).c_str();
            out.transform.translation = to_ozz(rest.translation);
            out.transform.rotation = to_ozz(core::normalized(rest.rotation));
            out.transform.scale = to_ozz(rest.scale);
            out.children.resize(children[joint].size());
            for (std::size_t child = 0; child < children[joint].size(); ++child) {
                if (!self(self, children[joint][child], out.children[child], depth + 1)) {
                    return false;
                }
            }
            return true;
        };
        raw.roots.resize(roots.size());
        for (std::size_t root = 0; root < roots.size(); ++root) {
            if (!build(build, roots[root], raw.roots[root], 0)) {
                return animation_error("A skeleton's joints form a loop");
            }
        }
        if (added != count) {
            return animation_error("A skeleton's joints form a loop");
        }

        Skeleton skeleton;
        skeleton.skeleton = offline::SkeletonBuilder()(raw);
        if (skeleton.skeleton == nullptr) {
            return animation_error("ozz could not build the skeleton");
        }
        skeleton.ozz_index.assign(count, 0);
        const auto names = skeleton.skeleton->joint_names();
        for (std::size_t index = 0; index < names.size(); ++index) {
            skeleton.ozz_index[static_cast<std::size_t>(std::atoi(names[index]))] =
                static_cast<int>(index);
        }
        skeleton.rest_pose.assign(desc.rest_pose.begin(), desc.rest_pose.end());
        return retag<SkeletonTag>(m_skeletons.create(std::move(skeleton)));
    }

    core::Result<ClipHandle> create_clip(SkeletonHandle skeleton_handle,
                                         const ClipDesc& desc) override {
        const Skeleton* const skeleton = m_skeletons.get(retag<Skeleton>(skeleton_handle));
        if (skeleton == nullptr) {
            return animation_error("The clip's skeleton doesn't exist");
        }
        if (desc.tracks.size() != skeleton->ozz_index.size() || !(desc.duration > 0.0f)) {
            return animation_error(std::format(
                "Clip '{}' needs one track per joint of its skeleton, and a duration above 0",
                desc.name));
        }

        offline::RawAnimation raw;
        raw.duration = desc.duration;
        raw.name = std::string(desc.name).c_str();
        raw.tracks.resize(desc.tracks.size());
        const auto clamp_time = [&](float time) { return std::clamp(time, 0.0f, desc.duration); };
        for (std::size_t joint = 0; joint < desc.tracks.size(); ++joint) {
            const JointTrack& track = desc.tracks[joint];
            const JointTransform& rest = skeleton->rest_pose[joint];
            offline::RawAnimation::JointTrack& out =
                raw.tracks[static_cast<std::size_t>(skeleton->ozz_index[joint])];
            // A channel without keys keeps the rest value, which ozz would otherwise make identity.
            for (const Key<core::Vec3>& key : track.translations) {
                out.translations.push_back({clamp_time(key.time), to_ozz(key.value)});
            }
            if (out.translations.empty()) {
                out.translations.push_back({0.0f, to_ozz(rest.translation)});
            }
            for (const Key<core::Quat>& key : track.rotations) {
                out.rotations.push_back({clamp_time(key.time), to_ozz(core::normalized(key.value))});
            }
            if (out.rotations.empty()) {
                out.rotations.push_back({0.0f, to_ozz(core::normalized(rest.rotation))});
            }
            for (const Key<core::Vec3>& key : track.scales) {
                out.scales.push_back({clamp_time(key.time), to_ozz(key.value)});
            }
            if (out.scales.empty()) {
                out.scales.push_back({0.0f, to_ozz(rest.scale)});
            }
        }
        if (!raw.Validate()) {
            return animation_error(std::format(
                "Clip '{}' has keys out of time order", desc.name));
        }

        Clip clip{.skeleton = skeleton_handle, .animation = offline::AnimationBuilder()(raw)};
        if (clip.animation == nullptr) {
            return animation_error(std::format("ozz could not build clip '{}'", desc.name));
        }
        return retag<ClipTag>(m_clips.create(std::move(clip)));
    }

    core::Result<PlayerHandle> create_player(SkeletonHandle skeleton_handle) override {
        const Skeleton* const skeleton = m_skeletons.get(retag<Skeleton>(skeleton_handle));
        if (skeleton == nullptr) {
            return animation_error("The player's skeleton doesn't exist");
        }
        Player player{.skeleton = skeleton_handle};
        player.blended.resize(static_cast<std::size_t>(skeleton->skeleton->num_soa_joints()));
        player.model.resize(static_cast<std::size_t>(skeleton->skeleton->num_joints()));
        return retag<PlayerTag>(m_players.create(std::move(player)));
    }

    void destroy_skeleton(SkeletonHandle skeleton) override {
        m_skeletons.destroy(retag<Skeleton>(skeleton));
    }
    void destroy_clip(ClipHandle clip) override {
        m_clips.destroy(retag<Clip>(clip));
    }
    void destroy_player(PlayerHandle player) override {
        m_players.destroy(retag<Player>(player));
    }

    float duration(ClipHandle handle) const override {
        const Clip* const clip = m_clips.get(retag<Clip>(handle));
        return clip != nullptr ? clip->animation->duration() : 0.0f;
    }

    void pose(PlayerHandle player_handle, std::span<const Layer> layers,
              std::span<core::Mat4> model_space) override {
        Player* const player = m_players.get(retag<Player>(player_handle));
        const Skeleton* const skeleton =
            player != nullptr ? m_skeletons.get(retag<Skeleton>(player->skeleton)) : nullptr;
        if (skeleton == nullptr) {
            return;
        }
        const ozz::animation::Skeleton& ozz_skeleton = *skeleton->skeleton;
        const auto soa_joints = static_cast<std::size_t>(ozz_skeleton.num_soa_joints());

        // Each layer sampled into its own pose, kept per layer so its cache follows its clip.
        player->blend_layers.clear();
        for (std::size_t index = 0; index < layers.size(); ++index) {
            const Layer& layer = layers[index];
            const Clip* const clip = m_clips.get(retag<Clip>(layer.clip));
            if (clip == nullptr || clip->skeleton != player->skeleton || !(layer.weight > 0.0f)) {
                continue;
            }
            while (player->contexts.size() <= index) {
                player->contexts.push_back(std::make_unique<ozz::animation::SamplingJob::Context>(
                    ozz_skeleton.num_joints()));
                player->layer_poses.emplace_back(soa_joints);
            }
            ozz::animation::SamplingJob sampling;
            sampling.animation = clip->animation.get();
            sampling.context = player->contexts[index].get();
            const float duration = clip->animation->duration();
            sampling.ratio = duration > 0.0f ? std::clamp(layer.time / duration, 0.0f, 1.0f) : 0.0f;
            sampling.output = ozz::make_span(player->layer_poses[index]);
            if (!sampling.Run()) {
                continue;
            }
            ozz::animation::BlendingJob::Layer blend;
            blend.weight = layer.weight;
            blend.transform = ozz::make_span(player->layer_poses[index]);
            player->blend_layers.push_back(blend);
        }

        // Weights below the threshold in total fall back to the rest pose.
        ozz::animation::BlendingJob blending;
        blending.threshold = 0.001f;
        blending.layers = ozz::make_span(player->blend_layers);
        blending.rest_pose = ozz_skeleton.joint_rest_poses();
        blending.output = ozz::make_span(player->blended);
        if (!blending.Run()) {
            return;
        }

        ozz::animation::LocalToModelJob to_model;
        to_model.skeleton = &ozz_skeleton;
        to_model.input = ozz::make_span(player->blended);
        to_model.output = ozz::make_span(player->model);
        if (!to_model.Run()) {
            return;
        }

        const std::size_t count = std::min(model_space.size(), skeleton->ozz_index.size());
        for (std::size_t joint = 0; joint < count; ++joint) {
            const ozz::math::Float4x4& matrix =
                player->model[static_cast<std::size_t>(skeleton->ozz_index[joint])];
            core::Mat4& out = model_space[joint];
            for (std::size_t column = 0; column < 4; ++column) {
                ozz::math::StorePtrU(matrix.cols[column], &out.columns[column].x);
            }
        }
    }

private:
    core::HandlePool<Skeleton> m_skeletons;
    core::HandlePool<Clip> m_clips;
    core::HandlePool<Player> m_players;
};

core::Result<std::unique_ptr<AnimationSystem>> create(const core::Config& /*config*/) {
    return std::unique_ptr<AnimationSystem>{std::make_unique<OzzAnimationSystem>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_animation_ozz(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<AnimationSystem>{.name = "ozz", .create = &create});
}

} // namespace dream::modules
