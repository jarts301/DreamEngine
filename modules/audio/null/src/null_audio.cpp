#include "dream/audio/audio.h"
#include "dream/core/handle.h"
#include "dream/core/module_registry.h"

#include <array>
#include <memory>
#include <string>

namespace dream::modules {
namespace {

using namespace audio;

struct Sound {};

struct Voice {
    core::Handle<Sound> sound;
    bool looping = false;
    bool positioned = false;
};

// Plays nothing, for automated runs and machines without audio. Voices that don't loop finish at
// the next update; looping voices play until stopped. It applies the interface's rules, so mistakes
// in engine code are caught without sound hardware.
class NullAudio final : public AudioDevice {
public:
    core::Result<SoundHandle> create_sound(std::span<const std::byte> file, bool /*stream*/) override {
        if (file.empty()) {
            return core::Error{core::ErrorCode::InvalidArgument, "A sound file can't be empty"};
        }
        return handle_cast<SoundTag>(m_sounds.create());
    }

    void destroy_sound(SoundHandle sound) override {
        const auto handle = handle_cast<Sound>(sound);
        if (!m_sounds.destroy(handle)) {
            return;
        }
        m_voices.for_each([&](core::Handle<Voice> voice, const Voice& value) {
            if (value.sound == handle) {
                m_voices.destroy(voice);
            }
        });
    }

    std::optional<float> sound_length(SoundHandle /*sound*/) const override {
        return std::nullopt;
    }

    core::Result<VoiceHandle> play(SoundHandle sound, const PlayDesc& desc) override {
        if (!m_sounds.contains(handle_cast<Sound>(sound))) {
            return core::Error{core::ErrorCode::InvalidArgument, "Playing a sound that doesn't exist"};
        }
        if (desc.bus >= Bus::Count) {
            return core::Error{core::ErrorCode::InvalidArgument, "Playing on a bus that doesn't exist"};
        }
        return handle_cast<VoiceTag>(m_voices.create(
            Voice{handle_cast<Sound>(sound), desc.looping, desc.position.has_value()}));
    }

    void stop(VoiceHandle voice) override {
        m_voices.destroy(handle_cast<Voice>(voice));
    }

    bool is_playing(VoiceHandle voice) const override {
        return m_voices.contains(handle_cast<Voice>(voice));
    }

    void set_volume(VoiceHandle /*voice*/, float /*volume*/) override {}
    void set_pitch(VoiceHandle /*voice*/, float /*pitch*/) override {}
    void set_position(VoiceHandle /*voice*/, core::Vec3 /*position*/) override {}
    void set_listener(core::Vec3 /*position*/, core::Vec3 /*forward*/, core::Vec3 /*up*/) override {}

    void set_bus_volume(Bus bus, float volume) override {
        if (bus < Bus::Count) {
            m_bus_volumes[static_cast<std::size_t>(bus)] = volume < 0.0f ? 0.0f : volume;
        }
    }

    float bus_volume(Bus bus) const override {
        return bus < Bus::Count ? m_bus_volumes[static_cast<std::size_t>(bus)] : 0.0f;
    }

    void suspend() override {}
    void resume() override {}

    void update() override {
        m_voices.for_each([&](core::Handle<Voice> voice, const Voice& value) {
            if (!value.looping) {
                m_voices.destroy(voice);
            }
        });
    }

    std::size_t voice_count() const override {
        return m_voices.size();
    }

private:
    core::HandlePool<Sound> m_sounds;
    core::HandlePool<Voice> m_voices;
    std::array<float, static_cast<std::size_t>(Bus::Count)> m_bus_volumes{1.0f, 1.0f, 1.0f};
};

core::Result<std::unique_ptr<AudioDevice>> create(const core::Config& /*config*/) {
    return std::unique_ptr<AudioDevice>{std::make_unique<NullAudio>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_audio_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<AudioDevice>{.name = "null", .create = &create});
}

} // namespace dream::modules
