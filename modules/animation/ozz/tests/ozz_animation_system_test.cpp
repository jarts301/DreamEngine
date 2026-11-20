#include "dream/animation/animation.h"
#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/module_registry.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_animation_ozz(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::animation;
using doctest::Approx;

namespace {

std::unique_ptr<AnimationSystem> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_animation_ozz(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"ozz"};
    auto created = registry.create<AnimationSystem>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

// An arm: joint 1 is the root at the origin, and joint 0 its child, 1 above it. Listed child
// first, which the backend must sort out.
constexpr std::array<std::int32_t, 2> k_parents{1, -1};
const std::array<JointTransform, 2> k_rest{
    JointTransform{.translation = {0.0f, 1.0f, 0.0f}},
    JointTransform{},
};

// The root turning a quarter about +Z over a second, from rest.
std::vector<JointTrack> turning_root() {
    std::vector<JointTrack> tracks(2);
    tracks[1].rotations = {
        {0.0f, core::Quat{}},
        {1.0f, core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, std::numbers::pi_v<float> / 2.0f)},
    };
    return tracks;
}

core::Vec3 position_of(const core::Mat4& matrix) {
    return core::transform_point(matrix, core::Vec3{});
}

struct Arm {
    std::unique_ptr<AnimationSystem> system = make();
    SkeletonHandle skeleton;
    PlayerHandle player;
    ClipHandle turn;
    std::vector<JointTrack> turn_tracks = turning_root();

    Arm() {
        auto made = system->create_skeleton(SkeletonDesc{.parents = k_parents, .rest_pose = k_rest});
        REQUIRE_MESSAGE(made.has_value(), made.error().message);
        skeleton = made.value();
        auto clip = system->create_clip(
            skeleton, ClipDesc{.name = "turn", .duration = 1.0f, .tracks = turn_tracks});
        REQUIRE_MESSAGE(clip.has_value(), clip.error().message);
        turn = clip.value();
        player = system->create_player(skeleton).value();
    }

    // Where the child joint is, posed with these layers.
    core::Vec3 hand(std::span<const Layer> layers) {
        std::array<core::Mat4, 2> model{};
        system->pose(player, layers, model);
        CHECK(position_of(model[1]).x == Approx(0.0f));
        return position_of(model[0]);
    }
};

} // namespace

TEST_CASE("ozz poses a skeleton at rest without layers") {
    Arm arm;
    const core::Vec3 hand = arm.hand({});
    CHECK(hand.x == Approx(0.0f));
    CHECK(hand.y == Approx(1.0f));
}

TEST_CASE("ozz samples a clip between its keys, and keeps rest values it doesn't animate") {
    Arm arm;
    CHECK(arm.system->duration(arm.turn) == Approx(1.0f));

    // A quarter turn about +Z takes the hand from +Y to -X; half of it to between them.
    const std::array end{Layer{.clip = arm.turn, .time = 1.0f}};
    const core::Vec3 turned = arm.hand(end);
    CHECK(turned.x == Approx(-1.0f).epsilon(0.001));
    CHECK(turned.y == Approx(0.0f).epsilon(0.001));

    const std::array middle{Layer{.clip = arm.turn, .time = 0.5f}};
    const core::Vec3 half = arm.hand(middle);
    CHECK(half.x == Approx(-std::sqrt(0.5f)).epsilon(0.001));
    CHECK(half.y == Approx(std::sqrt(0.5f)).epsilon(0.001));
}

TEST_CASE("ozz blends layers by weight") {
    Arm arm;
    // The start and the end of the turn, half each: an eighth of a turn.
    const std::array layers{Layer{.clip = arm.turn, .time = 0.0f, .weight = 1.0f},
                            Layer{.clip = arm.turn, .time = 1.0f, .weight = 1.0f}};
    const core::Vec3 hand = arm.hand(layers);
    CHECK(hand.x == Approx(-std::sqrt(0.5f)).epsilon(0.01));
    CHECK(hand.y == Approx(std::sqrt(0.5f)).epsilon(0.01));

    // Layers that weigh nothing leave the rest pose.
    const std::array none{Layer{.clip = arm.turn, .time = 1.0f, .weight = 0.0f}};
    CHECK(arm.hand(none).y == Approx(1.0f));
}

TEST_CASE("ozz refuses broken skeletons and clips") {
    const std::unique_ptr<AnimationSystem> system = make();
    const std::array<std::int32_t, 2> loop{1, 0};
    CHECK_FALSE(system->create_skeleton(SkeletonDesc{.parents = loop, .rest_pose = k_rest})
                    .has_value());
    const std::array<std::int32_t, 2> outside{5, -1};
    CHECK_FALSE(system->create_skeleton(SkeletonDesc{.parents = outside, .rest_pose = k_rest})
                    .has_value());

    const SkeletonHandle skeleton =
        system->create_skeleton(SkeletonDesc{.parents = k_parents, .rest_pose = k_rest}).value();
    const std::vector<JointTrack> one_track(1);
    CHECK_FALSE(system->create_clip(skeleton, ClipDesc{.name = "short", .duration = 1.0f,
                                                       .tracks = one_track})
                    .has_value());
    std::vector<JointTrack> backwards(2);
    backwards[0].translations = {{0.8f, {}}, {0.2f, {}}};
    CHECK_FALSE(system->create_clip(skeleton, ClipDesc{.name = "backwards", .duration = 1.0f,
                                                       .tracks = backwards})
                    .has_value());
}
