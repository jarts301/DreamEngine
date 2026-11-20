#pragma once

#include "dream/core/handle.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::animation {

// Plays skeletal animation (ADR 0036): poses a skeleton from clips of keyframes, blends poses, and
// gives each joint's transform in the model's space. The runtime builds skeletons and clips from a
// model's glTF data as it loads, and asks for a pose of every animated model each frame; it turns
// the result into skinning matrices for the renderer.

// One joint's transform relative to its parent.
struct JointTransform {
    core::Vec3 translation;
    core::Quat rotation;
    core::Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct SkeletonDesc {
    // Each joint's parent, as an index into these lists, or -1 for a root. Any order.
    std::span<const std::int32_t> parents;
    // Each joint's transform when no clip moves it.
    std::span<const JointTransform> rest_pose;
    // For messages. May be empty.
    std::span<const std::string> names;
};

template <typename Value> struct Key {
    float time = 0.0f;
    Value value;
};

// One joint's keyframes, each list in time order and interpolated linearly: rotations along the
// shortest arc. A joint whose list is empty keeps its rest transform for that part.
struct JointTrack {
    std::vector<Key<core::Vec3>> translations;
    std::vector<Key<core::Quat>> rotations;
    std::vector<Key<core::Vec3>> scales;
};

struct ClipDesc {
    std::string_view name;
    // In seconds; keys past it are ignored.
    float duration = 0.0f;
    // One per joint of the skeleton, in its order.
    std::span<const JointTrack> tracks;
};

struct SkeletonTag;
struct ClipTag;
struct PlayerTag;
using SkeletonHandle = core::Handle<SkeletonTag>;
using ClipHandle = core::Handle<ClipTag>;
// What one animated object needs to be posed, such as the backend's caches. One per character.
using PlayerHandle = core::Handle<PlayerTag>;

// A clip at a time, with a weight among the layers posed together.
struct Layer {
    ClipHandle clip;
    // In seconds, from 0 to the clip's duration.
    float time = 0.0f;
    float weight = 1.0f;
};

class AnimationSystem {
public:
    static constexpr std::string_view k_module_interface = "animation";

    virtual ~AnimationSystem() = default;

    [[nodiscard]] virtual core::Result<SkeletonHandle> create_skeleton(const SkeletonDesc& desc) = 0;
    // A clip for one skeleton.
    [[nodiscard]] virtual core::Result<ClipHandle> create_clip(SkeletonHandle skeleton,
                                                               const ClipDesc& desc) = 0;
    [[nodiscard]] virtual core::Result<PlayerHandle> create_player(SkeletonHandle skeleton) = 0;

    // A skeleton outlives its clips and players.
    virtual void destroy_skeleton(SkeletonHandle skeleton) = 0;
    virtual void destroy_clip(ClipHandle clip) = 0;
    virtual void destroy_player(PlayerHandle player) = 0;

    [[nodiscard]] virtual float duration(ClipHandle clip) const = 0;

    // Samples each layer's clip at its time, blends them by weight, and writes every joint's
    // transform from its space to the skeleton's, `model_space` having one per joint. With no
    // layers, or weights that add up to 0, the rest pose.
    virtual void pose(PlayerHandle player, std::span<const Layer> layers,
                      std::span<core::Mat4> model_space) = 0;
};

} // namespace dream::animation
