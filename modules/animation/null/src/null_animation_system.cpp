// No animation: every skeleton stays in its rest pose, so a project without an animation library
// still draws its characters, standing still.

#include "dream/animation/animation.h"
#include "dream/core/handle.h"
#include "dream/core/module_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace animation;

// The same handle, for the module's pool of the type it names, or the other way.
template <typename To, typename From> core::Handle<To> retag(core::Handle<From> handle) {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}


struct Skeleton {
    std::vector<std::int32_t> parents;
    // Each joint's rest transform in the skeleton's space, worked out once.
    std::vector<core::Mat4> model_space;
};

struct Clip {
    float duration = 0.0f;
};

class NullAnimationSystem final : public AnimationSystem {
public:
    core::Result<SkeletonHandle> create_skeleton(const SkeletonDesc& desc) override {
        const std::size_t count = desc.parents.size();
        if (count == 0 || desc.rest_pose.size() != count) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "A skeleton needs joints, each with a parent and a rest transform"};
        }
        Skeleton skeleton{.parents = {desc.parents.begin(), desc.parents.end()},
                          .model_space = std::vector<core::Mat4>(count)};
        std::vector<bool> done(count, false);
        // Parents may come after their children, so each joint follows its chain up first.
        for (std::size_t joint = 0; joint < count; ++joint) {
            std::vector<std::size_t> chain;
            std::optional<std::size_t> at = joint;
            while (at.has_value() && !done[*at] && chain.size() <= count) {
                chain.push_back(*at);
                const std::int32_t parent = desc.parents[*at];
                if (parent < -1 || parent >= static_cast<std::int32_t>(count)) {
                    return core::Error{core::ErrorCode::InvalidArgument,
                                       "A joint's parent isn't another joint of its skeleton"};
                }
                at = parent < 0 ? std::nullopt : std::optional(static_cast<std::size_t>(parent));
            }
            if (chain.size() > count) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "A skeleton's joints form a loop"};
            }
            for (auto link = chain.rbegin(); link != chain.rend(); ++link) {
                const JointTransform& rest = desc.rest_pose[*link];
                const core::Mat4 local =
                    core::transform(rest.translation, core::normalized(rest.rotation), rest.scale);
                const std::int32_t parent = desc.parents[*link];
                skeleton.model_space[*link] =
                    parent < 0 ? local
                               : skeleton.model_space[static_cast<std::size_t>(parent)] * local;
                done[*link] = true;
            }
        }
        return retag<SkeletonTag>(m_skeletons.create(std::move(skeleton)));
    }

    core::Result<ClipHandle> create_clip(SkeletonHandle skeleton, const ClipDesc& desc) override {
        if (!m_skeletons.contains(retag<Skeleton>(skeleton))) {
            return core::Error{core::ErrorCode::InvalidArgument, "The clip's skeleton doesn't exist"};
        }
        return retag<ClipTag>(m_clips.create(Clip{desc.duration}));
    }

    core::Result<PlayerHandle> create_player(SkeletonHandle skeleton) override {
        if (!m_skeletons.contains(retag<Skeleton>(skeleton))) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The player's skeleton doesn't exist"};
        }
        return retag<PlayerTag>(m_players.create(skeleton));
    }

    void destroy_skeleton(SkeletonHandle skeleton) override {
        m_skeletons.destroy(retag<Skeleton>(skeleton));
    }
    void destroy_clip(ClipHandle clip) override {
        m_clips.destroy(retag<Clip>(clip));
    }
    void destroy_player(PlayerHandle player) override {
        m_players.destroy(retag<SkeletonHandle>(player));
    }

    float duration(ClipHandle clip) const override {
        const Clip* const found = m_clips.get(retag<Clip>(clip));
        return found != nullptr ? found->duration : 0.0f;
    }

    void pose(PlayerHandle player, std::span<const Layer> /*layers*/,
              std::span<core::Mat4> model_space) override {
        const SkeletonHandle* const skeleton_handle = m_players.get(retag<SkeletonHandle>(player));
        const Skeleton* const skeleton =
            skeleton_handle != nullptr ? m_skeletons.get(retag<Skeleton>(*skeleton_handle)) : nullptr;
        if (skeleton == nullptr) {
            return;
        }
        const std::size_t count = std::min(model_space.size(), skeleton->model_space.size());
        std::copy_n(skeleton->model_space.begin(), count, model_space.begin());
    }

private:
    core::HandlePool<Skeleton> m_skeletons;
    core::HandlePool<Clip> m_clips;
    core::HandlePool<SkeletonHandle> m_players;
};

core::Result<std::unique_ptr<AnimationSystem>> create(const core::Config& /*config*/) {
    return std::unique_ptr<AnimationSystem>{std::make_unique<NullAnimationSystem>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_animation_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<AnimationSystem>{.name = "null", .create = &create});
}

} // namespace dream::modules
