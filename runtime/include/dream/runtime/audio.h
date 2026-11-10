#pragma once

#include "dream/audio/audio.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/event_bus.h"
#include "dream/runtime/world.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dream::runtime {

class App;

// A sound file loaded through the AssetManager. Files over 256 KB, such as music, are streamed.
class SoundAsset {
public:
    SoundAsset(audio::AudioDevice& device, audio::SoundHandle handle)
        : m_device(&device), m_handle(handle) {}

    SoundAsset(SoundAsset&& other) noexcept
        : m_device(other.m_device), m_handle(std::exchange(other.m_handle, audio::SoundHandle{})) {}

    SoundAsset& operator=(SoundAsset&&) = delete;

    ~SoundAsset() {
        if (m_handle.is_valid()) {
            m_device->destroy_sound(m_handle);
        }
    }

    [[nodiscard]] audio::SoundHandle get() const noexcept {
        return m_handle;
    }

private:
    audio::AudioDevice* m_device;
    audio::SoundHandle m_handle;
};

// A sound played by an entity, once, when Audio first sees it with `autoplay`. With `spatial`, it
// is heard from the entity's position, which it follows. Removing the component stops it.
struct AudioSource {
    AssetRef<SoundAsset> sound;
    audio::PlayDesc desc;
    bool autoplay = true;
    bool spatial = false;
};

// Where the player hears from, usually on the camera's entity. Only the first one is used.
struct AudioListener {};

// Plays sounds through the audio module listed in the project configuration. Typical use:
//
//     on_start:   audio.initialize(app);
//     on_update:  audio.update(world.get());   // After World::update_transforms.
//
// It suspends output while the program is in the background. Scene files get the components
// "audio_source" and "audio_listener":
//
//     [entities.radio.audio_source]
//     sound = "assets:/sounds/song.wav"
//     bus = "music"                 # "effects" (the default), "music" or "interface".
//     volume = 1.0                  # The rest are optional too.
//     pitch = 1.0
//     loop = false
//     autoplay = true
//     spatial = false               # Heard from the entity's position.
//     min_distance = 1.0
//     max_distance = 50.0
//
//     [entities.camera.audio_listener]
//
// Starting bus volumes come from
//
//     [audio]
//     effects_volume = 1.0
//     music_volume = 1.0
//     interface_volume = 1.0
class Audio {
public:
    Audio() = default;
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    [[nodiscard]] core::Result<void> initialize(App& app);
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<audio::AudioDevice> backend);

    // Moves the listener, starts sources and follows them, stops the voices of sources that are
    // gone, and releases finished voices. `world` may be null. Passing another world stops the
    // previous world's sources.
    void update(const World* world);

    // The voice an entity's AudioSource is playing, if it started.
    [[nodiscard]] audio::VoiceHandle source_voice(Entity entity) const;

    // A sound not tied to an entity. Returns an invalid handle if it can't play.
    audio::VoiceHandle play(const AssetRef<SoundAsset>& sound, const audio::PlayDesc& desc = {});

    // The same, heard from a place in the world.
    audio::VoiceHandle play_at(const AssetRef<SoundAsset>& sound, core::Vec3 position,
                               audio::PlayDesc desc = {});

    void stop(audio::VoiceHandle voice);
    void set_bus_volume(audio::Bus bus, float volume);
    [[nodiscard]] float bus_volume(audio::Bus bus) const;

    // Null until initialized.
    [[nodiscard]] audio::AudioDevice* device() noexcept {
        return m_device.get();
    }

private:
    std::unique_ptr<audio::AudioDevice> m_device;
    std::vector<Subscription> m_subscriptions;
    std::unordered_map<Entity, audio::VoiceHandle> m_source_voices;
    const World* m_last_world = nullptr;
};

} // namespace dream::runtime
