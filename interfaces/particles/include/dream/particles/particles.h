#pragma once

#include "dream/core/handle.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace dream::particles {

// Simulates particle effects (ADR 0038): emitters that give off many small, short-lived particles,
// such as fire, smoke, sparks, dust and magic. A module moves each emitter's particles on every
// frame and gives them back in world space; the runtime hands them to the renderer, which draws
// each as a quad facing the camera.

// A value chosen at random between min and max, for each particle as it is born.
struct Range {
    float min = 0.0f;
    float max = 0.0f;

    constexpr bool operator==(const Range&) const = default;
};

// Where in the emitter's space particles are born, and which way they leave.
enum class EmitterShape : std::uint8_t {
    // At the emitter's origin, leaving every way.
    Point,
    // Inside a sphere of radius `extents.x`, leaving away from its middle.
    Sphere,
    // Inside a box of half-size `extents`, leaving every way.
    Box,
    // On a disc of radius `extents.x` across the emitter's XZ plane, leaving up its +Y axis within
    // `cone_angle` of it: fountains, fire and smoke.
    Cone,
};

// How one emitter behaves. Every distance is in meters and every time in seconds.
struct EmitterDesc {
    // How many particles are born per second while the emitter is emitting.
    float rate = 10.0f;
    // How many are born at once when the emitter starts, and again each time it loops.
    std::uint32_t burst = 0;
    // How long one cycle lasts: 0 emits for as long as the emitter lives.
    float duration = 0.0f;
    // At the end of a cycle, start another; otherwise stop emitting, and finish once the last
    // particle has died. With a duration of 0, an emitter that doesn't loop gives off only its
    // burst: a one-off such as an explosion.
    bool loop = true;
    // The most particles alive at once. None are born while there are this many.
    std::uint32_t max_particles = 256;

    EmitterShape shape = EmitterShape::Point;
    core::Vec3 extents{0.0f, 0.0f, 0.0f};
    // For Cone: how far from the +Y axis particles may leave, in radians.
    float cone_angle = 0.4f;

    Range lifetime{1.0f, 1.0f};
    // Meters per second as a particle is born, the way its shape sends it.
    Range speed{1.0f, 1.0f};
    // Across, in meters, as a particle is born.
    Range size{0.1f, 0.1f};
    // How much a particle's size is multiplied by when it dies, changing evenly over its life.
    float end_size = 1.0f;
    // In radians around the view direction, as a particle is born, and how fast it turns.
    Range rotation{0.0f, 0.0f};
    Range spin{0.0f, 0.0f};
    // Linear color and opacity as a particle is born and as it dies, mixed evenly in between.
    // Colors may go above 1 for glowing effects in HDR.
    core::Vec4 start_color{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec4 end_color{1.0f, 1.0f, 1.0f, 0.0f};

    // Acceleration in world space, such as {0, -9.8, 0} for falling sparks, or upward for smoke.
    core::Vec3 gravity{0.0f, 0.0f, 0.0f};
    // How quickly particles slow down: each second their speed is multiplied by exp(-drag).
    float drag = 0.0f;
    // Particles move with the emitter, as a jet's exhaust; otherwise they stay where they were
    // born in the world, as a trail of smoke.
    bool local_space = false;
    // The same seed gives the same particles for the same updates, for screenshot checks.
    std::uint32_t seed = 1;

    bool operator==(const EmitterDesc&) const = default;
};

// One live particle, as the renderer draws it.
struct Particle {
    // In world space.
    core::Vec3 position;
    // Across, in meters.
    float size = 0.0f;
    // Linear, with its opacity in w.
    core::Vec4 color;
    // In radians around the view direction.
    float rotation = 0.0f;
};

struct EmitterTag;
using EmitterHandle = core::Handle<EmitterTag>;

class ParticleSystem {
public:
    static constexpr std::string_view k_module_interface = "particles";

    virtual ~ParticleSystem() = default;

    // An emitter with no particles, at the start of its first cycle.
    [[nodiscard]] virtual core::Result<EmitterHandle> create_emitter(const EmitterDesc& desc) = 0;
    virtual void destroy_emitter(EmitterHandle emitter) = 0;

    // Moves the emitter's particles on by `seconds`, and while `emitting`, gives off new ones from
    // where `transform` places the emitter, from its space to the world's.
    virtual void update(EmitterHandle emitter, const core::Mat4& transform, float seconds,
                        bool emitting) = 0;

    // Gives off `count` particles at once, as a burst, up to the emitter's limit.
    virtual void burst(EmitterHandle emitter, const core::Mat4& transform, std::uint32_t count) = 0;

    // Removes every particle and starts the first cycle again.
    virtual void restart(EmitterHandle emitter) = 0;

    // The live particles after the last update, valid until the next call on the emitter.
    [[nodiscard]] virtual std::span<const Particle> particles(EmitterHandle emitter) const = 0;

    // Whether an emitter that doesn't loop has ended its cycle and its last particle has died.
    [[nodiscard]] virtual bool finished(EmitterHandle emitter) const = 0;
};

} // namespace dream::particles
