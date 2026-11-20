// No particles: emitters are accepted and give off nothing, so a project can leave effects out,
// such as on a server, without changing its scenes.

#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/particles/particles.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace particles;

class NullParticleSystem final : public ParticleSystem {
public:
    core::Result<EmitterHandle> create_emitter(const EmitterDesc& desc) override {
        const core::Handle<bool> made = m_emitters.create(!desc.loop);
        return EmitterHandle{.index = made.index, .generation = made.generation};
    }

    void destroy_emitter(EmitterHandle emitter) override {
        m_emitters.destroy(core::Handle<bool>{.index = emitter.index,
                                              .generation = emitter.generation});
    }

    void update(EmitterHandle, const core::Mat4&, float, bool) override {}
    void burst(EmitterHandle, const core::Mat4&, std::uint32_t) override {}
    void restart(EmitterHandle) override {}

    std::span<const Particle> particles(EmitterHandle) const override {
        return {};
    }

    // An emitter that doesn't loop is over at once, so games waiting for one to end go on.
    bool finished(EmitterHandle emitter) const override {
        const bool* const ends = m_emitters.get(
            core::Handle<bool>{.index = emitter.index, .generation = emitter.generation});
        return ends == nullptr || *ends;
    }

private:
    core::HandlePool<bool> m_emitters;
};

core::Result<std::unique_ptr<ParticleSystem>> create(const core::Config& /*config*/) {
    return std::unique_ptr<ParticleSystem>{std::make_unique<NullParticleSystem>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_particles_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<ParticleSystem>{.name = "null", .create = &create});
}

} // namespace dream::modules
