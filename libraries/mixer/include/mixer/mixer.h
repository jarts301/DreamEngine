#pragma once

// A small software audio mixer: voices reading from decoded sounds or streams, resampled by linear
// interpolation, with volume, pitch, balance, buses and distance-based attenuation and panning.
// It produces interleaved stereo floats and knows nothing about audio devices or file formats.
//
// Thread safety: every member function of Mixer may be called from any thread. mix() is meant for
// an audio device's callback while the rest is called from the game, so they share one lock.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace mixer {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Decoded audio that any number of voices can play at once.
struct Sound {
    // Interleaved: `channels` samples per frame.
    std::vector<float> samples;
    unsigned channels = 1;
    unsigned sample_rate = 48000;

    [[nodiscard]] std::size_t frames() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

// Audio produced while it plays, such as a file decoded bit by bit. One voice owns each stream.
class Stream {
public:
    virtual ~Stream() = default;

    [[nodiscard]] virtual unsigned channels() const = 0;
    [[nodiscard]] virtual unsigned sample_rate() const = 0;

    // Fills `interleaved` with whole frames and returns how many it wrote. Fewer than fit means the
    // stream has ended.
    virtual std::size_t read(std::span<float> interleaved) = 0;

    // Goes back to the start, for looping. Returns false if it can't.
    virtual bool rewind() = 0;
};

// A stream over a shared Sound, which is how voices play decoded audio.
[[nodiscard]] std::unique_ptr<Stream> make_sound_stream(std::shared_ptr<const Sound> sound);

struct VoiceId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const VoiceId&, const VoiceId&) = default;
};

struct VoiceSettings {
    std::size_t bus = 0;

    // A multiplier: 1 is as recorded.
    float volume = 1.0f;

    // A playback speed multiplier, which also changes the pitch.
    float pitch = 1.0f;

    // From -1 (left) to 1 (right). Ignored for positioned voices.
    float pan = 0.0f;

    bool looping = false;

    // Set for a voice heard from a place, relative to the listener. Full volume within
    // min_distance, fading as min_distance / distance up to max_distance.
    std::optional<Vec3> position;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
};

class Mixer {
public:
    Mixer(unsigned sample_rate, std::size_t bus_count, std::size_t max_voices);
    ~Mixer();

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    [[nodiscard]] unsigned sample_rate() const noexcept {
        return m_sample_rate;
    }

    // Fails if the stream is empty, the bus doesn't exist, or all voices are in use.
    [[nodiscard]] std::optional<VoiceId> play(std::unique_ptr<Stream> stream,
                                              const VoiceSettings& settings);

    // Stops and releases a voice at once. Unknown voices are ignored.
    void stop(VoiceId voice);

    // False once a voice has finished or been stopped.
    [[nodiscard]] bool is_playing(VoiceId voice) const;

    void set_volume(VoiceId voice, float volume);
    void set_pitch(VoiceId voice, float pitch);

    // Only for voices started with a position.
    void set_position(VoiceId voice, Vec3 position);

    // `forward` and `up` needn't be normalized.
    void set_listener(Vec3 position, Vec3 forward, Vec3 up);

    // Volumes below zero become zero. Unknown buses are ignored, and read as zero.
    void set_bus_volume(std::size_t bus, float volume);
    [[nodiscard]] float bus_volume(std::size_t bus) const;

    // Releases voices that have finished playing, and returns how many there were.
    std::size_t release_finished();

    // Voices that haven't been released, whether or not they are still playing.
    [[nodiscard]] std::size_t voice_count() const;

    // Overwrites `stereo` (left, right, left, ...) with the next frames of every voice, limited to
    // the range -1 to 1.
    void mix(std::span<float> stereo);

private:
    struct Voice;

    [[nodiscard]] Voice* find(VoiceId voice);
    [[nodiscard]] const Voice* find(VoiceId voice) const;

    // Gains for the left and right channel of a voice, from its settings and the listener.
    void gains(const Voice& voice, float& left, float& right) const;

    unsigned m_sample_rate;
    std::vector<float> m_bus_volumes;
    std::vector<std::unique_ptr<Voice>> m_slots;
    std::vector<std::uint32_t> m_generations;
    std::size_t m_live = 0;
    Vec3 m_listener_position;
    Vec3 m_listener_right{1.0f, 0.0f, 0.0f};
    mutable std::mutex m_mutex;
};

} // namespace mixer
