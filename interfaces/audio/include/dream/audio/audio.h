#pragma once

#include "dream/core/handle.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace dream::audio {

// Sound playback. A sound is decoded audio, or encoded audio decoded while it plays; a voice is one
// playing instance of a sound. Positions use the engine's coordinates: meters, +Y up.

struct SoundTag;
struct VoiceTag;

using SoundHandle = core::Handle<SoundTag>;
using VoiceHandle = core::Handle<VoiceTag>;

// For backends, which keep their own types in a core::HandlePool.
template <typename To, typename From>
[[nodiscard]] constexpr core::Handle<To> handle_cast(core::Handle<From> handle) noexcept {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

// Each voice plays through a bus, whose volume the game or player can set as a whole.
enum class Bus : std::uint8_t { Effects, Music, Interface, Count };

struct PlayDesc {
    Bus bus = Bus::Effects;

    // A multiplier: 1 plays the sound as recorded, 0 is silent.
    float volume = 1.0f;

    // A playback speed multiplier, which also changes the pitch.
    float pitch = 1.0f;

    // From -1 (left) to 1 (right). Ignored for positioned voices.
    float pan = 0.0f;

    bool looping = false;

    // Set for a voice heard from a place in the world, relative to the listener. It is at full
    // volume within min_distance and fades with distance up to max_distance.
    std::optional<core::Vec3> position;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
};

// The audio output. Not thread-safe: call everything from one thread. Handles that no longer exist
// are ignored by every function that takes one.
class AudioDevice {
public:
    static constexpr std::string_view k_module_interface = "audio";

    virtual ~AudioDevice() = default;

    // `file` is a whole WAV, MP3 or FLAC file. Without `stream`, it is decoded now and can be
    // released afterwards. With `stream`, the encoded bytes are kept and decoded while playing,
    // which saves memory for long music.
    [[nodiscard]] virtual core::Result<SoundHandle> create_sound(std::span<const std::byte> file,
                                                                 bool stream) = 0;

    // Also stops the sound's voices.
    virtual void destroy_sound(SoundHandle sound) = 0;

    // Seconds of audio, if the backend knows it.
    [[nodiscard]] virtual std::optional<float> sound_length(SoundHandle sound) const = 0;

    // Fails if the sound doesn't exist or too many voices are playing.
    [[nodiscard]] virtual core::Result<VoiceHandle> play(SoundHandle sound,
                                                         const PlayDesc& desc) = 0;

    virtual void stop(VoiceHandle voice) = 0;

    // False once a voice has finished or been stopped.
    [[nodiscard]] virtual bool is_playing(VoiceHandle voice) const = 0;

    virtual void set_volume(VoiceHandle voice, float volume) = 0;
    virtual void set_pitch(VoiceHandle voice, float pitch) = 0;

    // Only for voices started with a position.
    virtual void set_position(VoiceHandle voice, core::Vec3 position) = 0;

    // Where the player hears from. `forward` and `up` needn't be normalized.
    virtual void set_listener(core::Vec3 position, core::Vec3 forward, core::Vec3 up) = 0;

    virtual void set_bus_volume(Bus bus, float volume) = 0;
    [[nodiscard]] virtual float bus_volume(Bus bus) const = 0;

    // Stops all output while the program is in the background, and starts it again.
    virtual void suspend() = 0;
    virtual void resume() = 0;

    // Releases voices that have finished. Call once per frame.
    virtual void update() = 0;

    [[nodiscard]] virtual std::size_t voice_count() const = 0;
};

} // namespace dream::audio
