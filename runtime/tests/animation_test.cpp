#include "dream/runtime/animation.h"

#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "fake_device.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using doctest::Approx;

namespace {

// Poses every joint at a translation along X equal to the layers' weighted time, so a test can
// read back which clips played, when, and how much. Remembers what it was asked.
struct FakeAnimationState {
    std::vector<float> durations;
    std::vector<animation::Layer> last_layers;
    int players = 0;
};

class FakeAnimation final : public animation::AnimationSystem {
public:
    explicit FakeAnimation(FakeAnimationState& state) : m_state(state) {}

    core::Result<animation::SkeletonHandle> create_skeleton(const animation::SkeletonDesc&) override {
        return animation::SkeletonHandle{.index = 0, .generation = 1};
    }
    core::Result<animation::ClipHandle> create_clip(animation::SkeletonHandle,
                                                    const animation::ClipDesc& desc) override {
        m_state.durations.push_back(desc.duration);
        return animation::ClipHandle{.index = static_cast<std::uint32_t>(m_state.durations.size()),
                                     .generation = 1};
    }
    core::Result<animation::PlayerHandle> create_player(animation::SkeletonHandle) override {
        ++m_state.players;
        return animation::PlayerHandle{.index = 0, .generation = 1};
    }
    void destroy_skeleton(animation::SkeletonHandle) override {}
    void destroy_clip(animation::ClipHandle) override {}
    void destroy_player(animation::PlayerHandle) override {
        --m_state.players;
    }
    float duration(animation::ClipHandle clip) const override {
        return m_state.durations[clip.index - 1];
    }
    void pose(animation::PlayerHandle, std::span<const animation::Layer> layers,
              std::span<core::Mat4> model_space) override {
        m_state.last_layers.assign(layers.begin(), layers.end());
        float x = 0.0f;
        for (const animation::Layer& layer : layers) {
            x += layer.time * layer.weight;
        }
        for (core::Mat4& joint : model_space) {
            joint = core::translation(core::Vec3{x, 0.0f, 0.0f});
        }
    }

private:
    FakeAnimationState& m_state;
};

// A model with one joint, one skin bound at rest, and clips "walk" (2 s) and "jump" (1 s).
std::shared_ptr<const ModelAsset> animated_model(rhi::Device& device) {
    ModelData data;
    data.vertices.resize(3);
    data.indices = {0, 1, 2};
    data.materials.emplace_back();
    data.parts.push_back(
        ModelPart{.first_index = 0, .index_count = 3, .material = 0, .bounds = {}, .skin = 0});
    data.skeleton.parents = {-1};
    data.skeleton.rest_pose.emplace_back();
    data.skeleton.names = {"root"};
    data.skins.push_back(ModelSkin{.joints = {0}, .inverse_bind = {core::Mat4{}}});
    data.clips.push_back(ModelClip{.name = "walk", .duration = 2.0f, .tracks = {{}}});
    data.clips.push_back(ModelClip{.name = "jump", .duration = 1.0f, .tracks = {{}}});
    return std::make_shared<const ModelAsset>(create_model_asset(device, data).value());
}

struct Fixture {
    FakeState platform;
    FakeDeviceState device_state;
    const core::Config config = core::Config::parse("").value();
    App app{std::make_unique<FakePlatform>(platform), config,
            std::make_unique<FakeDevice>(device_state)};
    FakeAnimationState state;
    Animation animation;
    World world;
    Entity entity = world.create_entity();

    Fixture() {
        REQUIRE(animation.initialize(app, std::make_unique<FakeAnimation>(state)).has_value());
        world.registry().emplace<MeshRenderer>(entity, animated_model(*app.device()));
    }

    // The joint's X, from the skinning matrix the renderer would draw with.
    float posed_x() {
        const Animator& animator = world.registry().get<Animator>(entity);
        REQUIRE(animator.skinning.size() == 1);
        return animator.skinning[0][0].columns[3].x;
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "Animation moves a clip on, loops it, and poses the model") {
    world.registry().emplace<Animator>(entity, Animator{.clip = "walk"});
    animation.update(world, 0.5);
    CHECK(posed_x() == Approx(0.5f));
    animation.update(world, 2.0); // Past the 2 s clip's end, it starts again.
    CHECK(world.registry().get<Animator>(entity).time == Approx(0.5f));
    CHECK(state.players == 1);
}

TEST_CASE_FIXTURE(Fixture, "Animation stops a clip that doesn't loop on its last pose") {
    world.registry().emplace<Animator>(entity, Animator{.clip = "jump", .loop = false});
    animation.update(world, 3.0);
    const Animator& animator = world.registry().get<Animator>(entity);
    CHECK(animator.time == Approx(1.0f));
    CHECK_FALSE(animator.playing);
}

TEST_CASE_FIXTURE(Fixture, "Animation fades from the last clip to the next") {
    world.registry().emplace<Animator>(entity, Animator{.clip = "walk"});
    animation.update(world, 1.0);
    REQUIRE(animation.play(world, entity, "jump", 0.5f).has_value());
    animation.update(world, 0.25);

    // Halfway through the fade: walk, a quarter second on, and jump, from its start, half each.
    REQUIRE(state.last_layers.size() == 2);
    CHECK(state.last_layers[0].weight == Approx(0.5f));
    CHECK(state.last_layers[0].time == Approx(1.25f));
    CHECK(state.last_layers[1].weight == Approx(0.5f));
    CHECK(state.last_layers[1].time == Approx(0.25f));

    animation.update(world, 0.5);
    CHECK(state.last_layers.size() == 1);

    CHECK_FALSE(animation.play(world, entity, "fly").has_value());
}

TEST_CASE_FIXTURE(Fixture, "Animation lets go of entities that stop animating") {
    world.registry().emplace<Animator>(entity, Animator{.clip = "walk"});
    animation.update(world, 0.1);
    CHECK(state.players == 1);
    world.registry().remove<Animator>(entity);
    animation.update(world, 0.1);
    CHECK(state.players == 0);
}

TEST_CASE_FIXTURE(Fixture, "Animation names a model's clips and their lengths") {
    const ModelAsset& model = *world.registry().get<MeshRenderer>(entity).model;
    CHECK(Animation::clip_names(model) == std::vector<std::string_view>{"walk", "jump"});
    CHECK(Animation::duration(model, "jump") == Approx(1.0f));
    CHECK(Animation::duration(model, "fly") == 0.0f);
}
