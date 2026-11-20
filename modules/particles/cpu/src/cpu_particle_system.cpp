// Particles simulated on the CPU (ADR 0038): each emitter keeps its particles in one array, moves
// them on with simple physics every update, and writes them out in world space for the renderer.
// A few thousand particles cost well under a millisecond on a phone.

#include "dream/core/handle.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/module_registry.h"
#include "dream/particles/particles.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;
using namespace particles;

// More than this many in one emitter is almost always a mistake, and would fill the renderer's
// per-frame memory on its own.
constexpr std::uint32_t k_max_particles_per_emitter = 65536;

// Longer steps are cut to this, so a stall doesn't throw particles far through the world.
constexpr float k_longest_step = 0.25f;

template <typename To, typename From> core::Handle<To> retag(core::Handle<From> handle) {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

// A small, fast generator (PCG-style), so the same seed always gives the same particles.
class Random {
public:
    explicit Random(std::uint32_t seed) : m_state(seed * 2654435761u + 1u) {}

    // In [0, 1).
    float next() {
        m_state = m_state * 747796405u + 2891336453u;
        std::uint32_t word = ((m_state >> ((m_state >> 28u) + 4u)) ^ m_state) * 277803737u;
        word = (word >> 22u) ^ word;
        return static_cast<float>(word >> 8u) * (1.0f / 16777216.0f);
    }

    float between(Range range) {
        return range.min + (range.max - range.min) * next();
    }

    Vec3 unit_vector() {
        const float z = 2.0f * next() - 1.0f;
        const float angle = core::k_two_pi * next();
        const float across = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return Vec3{across * std::cos(angle), across * std::sin(angle), z};
    }

private:
    std::uint32_t m_state;
};

struct State {
    Vec3 position;
    Vec3 velocity;
    float age = 0.0f;
    float lifetime = 1.0f;
    float size = 0.0f;
    float rotation = 0.0f;
    float spin = 0.0f;
};

struct Emitter {
    EmitterDesc desc;
    Random random;
    std::vector<State> states;
    std::vector<Particle> drawn;
    float cycle_time = 0.0f;
    // Particles owed by the rate but not born yet: a fraction of one.
    float owed = 0.0f;
    bool started = false;
    bool ended = false;
};

core::Error invalid(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

bool finite(float value) {
    return std::isfinite(value);
}

core::Result<void> check(const EmitterDesc& desc) {
    const auto range_ok = [](Range range) {
        return finite(range.min) && finite(range.max) && range.min <= range.max;
    };
    if (!finite(desc.rate) || desc.rate < 0.0f) {
        return invalid("A particle emitter's rate must be 0 or more");
    }
    if (!finite(desc.duration) || desc.duration < 0.0f) {
        return invalid("A particle emitter's duration must be 0 or more");
    }
    if (desc.max_particles == 0 || desc.max_particles > k_max_particles_per_emitter) {
        return invalid(std::format("A particle emitter's max_particles must be from 1 to {}",
                                   k_max_particles_per_emitter));
    }
    if (!range_ok(desc.lifetime) || desc.lifetime.min <= 0.0f) {
        return invalid("A particle emitter's lifetime must be above 0, with its smallest first");
    }
    for (const Range range : {desc.speed, desc.size, desc.rotation, desc.spin}) {
        if (!range_ok(range)) {
            return invalid("A particle emitter's ranges must have their smallest value first");
        }
    }
    if (desc.size.min < 0.0f || !finite(desc.end_size) || desc.end_size < 0.0f) {
        return invalid("A particle emitter's sizes must be 0 or more");
    }
    if (!finite(desc.cone_angle) || desc.cone_angle < 0.0f || desc.cone_angle > core::k_pi) {
        return invalid("A particle emitter's cone angle must be from 0 to pi radians");
    }
    if (!finite(desc.drag) || desc.drag < 0.0f) {
        return invalid("A particle emitter's drag must be 0 or more");
    }
    for (const float extent : {desc.extents.x, desc.extents.y, desc.extents.z}) {
        if (!finite(extent) || extent < 0.0f) {
            return invalid("A particle emitter's extents must be 0 or more");
        }
    }
    return {};
}

// Gives off `count` particles, the k-th of them `ages[k]` seconds old already, so particles born
// during a long frame spread out along their way instead of starting in a clump.
void emit(Emitter& emitter, const Mat4& transform, std::uint32_t count, float step) {
    const EmitterDesc& desc = emitter.desc;
    const std::size_t room = desc.max_particles - std::min<std::size_t>(desc.max_particles,
                                                                         emitter.states.size());
    const std::uint32_t born = static_cast<std::uint32_t>(std::min<std::size_t>(count, room));
    Random& random = emitter.random;
    for (std::uint32_t index = 0; index < born; ++index) {
        Vec3 position{};
        Vec3 direction{0.0f, 1.0f, 0.0f};
        switch (desc.shape) {
        case EmitterShape::Point:
            direction = random.unit_vector();
            break;
        case EmitterShape::Sphere: {
            direction = random.unit_vector();
            position = direction * (desc.extents.x * std::cbrt(random.next()));
            break;
        }
        case EmitterShape::Box:
            position = Vec3{(2.0f * random.next() - 1.0f) * desc.extents.x,
                            (2.0f * random.next() - 1.0f) * desc.extents.y,
                            (2.0f * random.next() - 1.0f) * desc.extents.z};
            direction = random.unit_vector();
            break;
        case EmitterShape::Cone: {
            const float radius = desc.extents.x * std::sqrt(random.next());
            const float around = core::k_two_pi * random.next();
            position = Vec3{radius * std::cos(around), 0.0f, radius * std::sin(around)};
            // Even over the cone's cap of directions.
            const float cos_angle = 1.0f - random.next() * (1.0f - std::cos(desc.cone_angle));
            const float sin_angle = std::sqrt(std::max(0.0f, 1.0f - cos_angle * cos_angle));
            const float turn = core::k_two_pi * random.next();
            direction = Vec3{sin_angle * std::cos(turn), cos_angle, sin_angle * std::sin(turn)};
            break;
        }
        }
        Vec3 velocity = direction * random.between(desc.speed);
        if (!desc.local_space) {
            position = core::transform_point(transform, position);
            velocity = core::transform_direction(transform, velocity);
        }
        State state{.position = position,
                    .velocity = velocity,
                    .age = 0.0f,
                    .lifetime = random.between(desc.lifetime),
                    .size = random.between(desc.size),
                    .rotation = random.between(desc.rotation),
                    .spin = random.between(desc.spin)};
        if (step > 0.0f && born > 1) {
            const float age = step * (static_cast<float>(born - 1 - index) + 0.5f) /
                              static_cast<float>(born);
            state.age = age;
            state.position = state.position + state.velocity * age;
            state.rotation += state.spin * age;
        }
        emitter.states.push_back(state);
    }
}

void write_drawn(Emitter& emitter, const Mat4& transform) {
    const EmitterDesc& desc = emitter.desc;
    emitter.drawn.clear();
    emitter.drawn.reserve(emitter.states.size());
    for (const State& state : emitter.states) {
        const float t = std::clamp(state.age / state.lifetime, 0.0f, 1.0f);
        emitter.drawn.push_back(Particle{
            .position = desc.local_space ? core::transform_point(transform, state.position)
                                         : state.position,
            .size = state.size * (1.0f + (desc.end_size - 1.0f) * t),
            .color = core::lerp(desc.start_color, desc.end_color, t),
            .rotation = state.rotation,
        });
    }
}

class CpuParticleSystem final : public ParticleSystem {
public:
    core::Result<EmitterHandle> create_emitter(const EmitterDesc& desc) override {
        if (core::Result<void> valid = check(desc); !valid.has_value()) {
            return valid.error();
        }
        return retag<EmitterTag>(
            m_emitters.create(Emitter{.desc = desc, .random = Random(desc.seed)}));
    }

    void destroy_emitter(EmitterHandle emitter) override {
        m_emitters.destroy(retag<Emitter>(emitter));
    }

    void update(EmitterHandle handle, const Mat4& transform, float seconds,
                bool emitting) override {
        Emitter* const emitter = m_emitters.get(retag<Emitter>(handle));
        if (emitter == nullptr) {
            return;
        }
        const float step = finite(seconds) ? std::clamp(seconds, 0.0f, k_longest_step) : 0.0f;
        const EmitterDesc& desc = emitter->desc;

        // The living move on, and the dead go, keeping the order of the rest.
        const float keep = std::exp(-desc.drag * step);
        std::erase_if(emitter->states, [&](State& state) {
            state.age += step;
            if (state.age >= state.lifetime) {
                return true;
            }
            state.velocity = (state.velocity + desc.gravity * step) * keep;
            state.position = state.position + state.velocity * step;
            state.rotation += state.spin * step;
            return false;
        });

        if (emitting && !emitter->ended) {
            if (!emitter->started) {
                emitter->started = true;
                emit(*emitter, transform, desc.burst, 0.0f);
                if (!desc.loop && desc.duration <= 0.0f) {
                    emitter->ended = true;
                }
            }
            if (!emitter->ended) {
                // The rate's particles for this step, then the cycle's end, if it comes.
                float within = step;
                if (desc.duration > 0.0f && !desc.loop) {
                    within = std::min(step, desc.duration - emitter->cycle_time);
                }
                emitter->owed += desc.rate * std::max(0.0f, within);
                const auto due = static_cast<std::uint32_t>(emitter->owed);
                emitter->owed -= static_cast<float>(due);
                emit(*emitter, transform, due, within);
                emitter->cycle_time += step;
                if (desc.duration > 0.0f && emitter->cycle_time >= desc.duration) {
                    if (desc.loop) {
                        emitter->cycle_time = std::fmod(emitter->cycle_time, desc.duration);
                        emit(*emitter, transform, desc.burst, 0.0f);
                    } else {
                        emitter->ended = true;
                    }
                }
            }
        }
        write_drawn(*emitter, transform);
    }

    void burst(EmitterHandle handle, const Mat4& transform, std::uint32_t count) override {
        if (Emitter* const emitter = m_emitters.get(retag<Emitter>(handle))) {
            emit(*emitter, transform, count, 0.0f);
            write_drawn(*emitter, transform);
        }
    }

    void restart(EmitterHandle handle) override {
        if (Emitter* const emitter = m_emitters.get(retag<Emitter>(handle))) {
            emitter->states.clear();
            emitter->drawn.clear();
            emitter->random = Random(emitter->desc.seed);
            emitter->cycle_time = 0.0f;
            emitter->owed = 0.0f;
            emitter->started = false;
            emitter->ended = false;
        }
    }

    std::span<const Particle> particles(EmitterHandle handle) const override {
        const Emitter* const emitter = m_emitters.get(retag<Emitter>(handle));
        return emitter != nullptr ? std::span<const Particle>(emitter->drawn)
                                  : std::span<const Particle>{};
    }

    bool finished(EmitterHandle handle) const override {
        const Emitter* const emitter = m_emitters.get(retag<Emitter>(handle));
        return emitter == nullptr || (emitter->ended && emitter->states.empty());
    }

private:
    core::HandlePool<Emitter> m_emitters;
};

core::Result<std::unique_ptr<ParticleSystem>> create(const core::Config& /*config*/) {
    return std::unique_ptr<ParticleSystem>{std::make_unique<CpuParticleSystem>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_particles_cpu(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<ParticleSystem>{.name = "cpu", .create = &create});
}

} // namespace dream::modules
