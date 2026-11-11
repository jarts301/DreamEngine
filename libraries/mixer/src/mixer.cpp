#include "mixer/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace mixer {
namespace {

// Frames of a stream decoded ahead of the read position.
constexpr std::size_t k_window_frames = 4096;

// Pitches are kept within this range, which also bounds how far one output frame can move.
constexpr float k_lowest_pitch = 0.01f;
constexpr float k_highest_pitch = 16.0f;

// Positioned voices are panned by at most this much, so neither ear ever goes silent.
constexpr float k_spatial_pan = 0.8f;

float length(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

class SoundStream final : public Stream {
public:
    explicit SoundStream(std::shared_ptr<const Sound> sound) : m_sound(std::move(sound)) {}

    unsigned channels() const override {
        return m_sound->channels;
    }

    unsigned sample_rate() const override {
        return m_sound->sample_rate;
    }

    std::size_t read(std::span<float> interleaved) override {
        const std::size_t channels = m_sound->channels;
        const std::size_t frames =
            std::min(interleaved.size() / channels, m_sound->frames() - m_cursor);
        if (frames == 0) {
            return 0;
        }
        std::memcpy(interleaved.data(), m_sound->samples.data() + m_cursor * channels,
                    frames * channels * sizeof(float));
        m_cursor += frames;
        return frames;
    }

    bool rewind() override {
        m_cursor = 0;
        return true;
    }

private:
    std::shared_ptr<const Sound> m_sound;
    std::size_t m_cursor = 0;
};

} // namespace

std::unique_ptr<Stream> make_sound_stream(std::shared_ptr<const Sound> sound) {
    if (sound == nullptr || sound->channels == 0) {
        return nullptr;
    }
    return std::make_unique<SoundStream>(std::move(sound));
}

// A playing stream and the frames decoded from it so far. Positions count source frames from the
// start, continuing across loops.
struct Mixer::Voice {
    std::unique_ptr<Stream> stream;
    VoiceSettings settings;
    unsigned channels = 1;
    double rate_ratio = 1.0;

    // Frames [window_start, window_start + window_frames) of the stream.
    std::vector<float> window;
    std::uint64_t window_start = 0;
    std::size_t window_frames = 0;

    double position = 0.0;
    bool exhausted = false;
    bool finished = false;

    [[nodiscard]] std::uint64_t window_end() const noexcept {
        return window_start + window_frames;
    }

    // Reads up to `count` frames into `out`, starting over at the end of a looping stream.
    // Returns how many it read; fewer means the stream has ended for good.
    std::size_t read_frames(float* out, std::size_t count) {
        std::size_t total = 0;
        // Set between a rewind and the next frame read, so an empty loop can't go on forever.
        bool rewound = false;
        while (total < count && !exhausted) {
            const std::size_t read =
                stream->read(std::span<float>(out + total * channels, (count - total) * channels));
            total += read;
            if (read > 0) {
                rewound = false;
            }
            if (total < count) {
                if (settings.looping && !rewound && stream->rewind()) {
                    rewound = true;
                } else {
                    exhausted = true;
                }
            }
        }
        return total;
    }

    // Reads until the window is full or the stream ends.
    void fill() {
        window_frames +=
            read_frames(window.data() + window_frames * channels, k_window_frames - window_frames);
    }

    // Makes frame `first` and the one after it available, if the stream has them. Positions only
    // move forward, so `first` is never before the window.
    void load(std::uint64_t first) {
        if (first + 1 < window_end()) {
            return;
        }
        if (first < window_end()) {
            // Keeps the frames from `first` on, moved to the front.
            const auto kept = static_cast<std::size_t>(window_end() - first);
            std::memmove(window.data(), window.data() + (first - window_start) * channels,
                         kept * channels * sizeof(float));
            window_frames = kept;
        } else {
            // Discards frames the voice moved past without reading them.
            std::uint64_t skipped = first - window_end();
            while (skipped > 0 && !exhausted) {
                const auto chunk =
                    static_cast<std::size_t>(std::min<std::uint64_t>(skipped, k_window_frames));
                skipped -= read_frames(window.data(), chunk);
            }
            window_frames = 0;
        }
        window_start = first;
        fill();
    }
};

Mixer::Mixer(unsigned sample_rate, std::size_t bus_count, std::size_t max_voices)
    : m_sample_rate(std::max(sample_rate, 1u)), m_bus_volumes(bus_count, 1.0f), m_slots(max_voices),
      m_generations(max_voices, 1) {}

Mixer::~Mixer() = default;

std::optional<VoiceId> Mixer::play(std::unique_ptr<Stream> stream, const VoiceSettings& settings) {
    if (stream == nullptr || stream->channels() == 0 || stream->sample_rate() == 0) {
        return std::nullopt;
    }
    const std::lock_guard lock(m_mutex);
    if (settings.bus >= m_bus_volumes.size()) {
        return std::nullopt;
    }
    const auto free = std::find(m_slots.begin(), m_slots.end(), nullptr);
    if (free == m_slots.end()) {
        return std::nullopt;
    }

    auto voice = std::make_unique<Voice>();
    voice->channels = stream->channels();
    voice->rate_ratio =
        static_cast<double>(stream->sample_rate()) / static_cast<double>(m_sample_rate);
    voice->stream = std::move(stream);
    voice->settings = settings;
    voice->window.resize(k_window_frames * voice->channels);
    voice->fill();
    if (voice->window_frames == 0) {
        return std::nullopt;
    }

    *free = std::move(voice);
    ++m_live;
    const auto index = static_cast<std::uint32_t>(free - m_slots.begin());
    return VoiceId{index, m_generations[index]};
}

Mixer::Voice* Mixer::find(VoiceId voice) {
    if (voice.index >= m_slots.size() || m_generations[voice.index] != voice.generation) {
        return nullptr;
    }
    return m_slots[voice.index].get();
}

const Mixer::Voice* Mixer::find(VoiceId voice) const {
    return const_cast<Mixer*>(this)->find(voice);
}

void Mixer::stop(VoiceId voice) {
    const std::lock_guard lock(m_mutex);
    if (find(voice) != nullptr) {
        m_slots[voice.index].reset();
        ++m_generations[voice.index];
        --m_live;
    }
}

bool Mixer::is_playing(VoiceId voice) const {
    const std::lock_guard lock(m_mutex);
    const Voice* const found = find(voice);
    return found != nullptr && !found->finished;
}

void Mixer::set_volume(VoiceId voice, float volume) {
    const std::lock_guard lock(m_mutex);
    if (Voice* const found = find(voice)) {
        found->settings.volume = volume;
    }
}

void Mixer::set_pitch(VoiceId voice, float pitch) {
    const std::lock_guard lock(m_mutex);
    if (Voice* const found = find(voice)) {
        found->settings.pitch = pitch;
    }
}

void Mixer::set_position(VoiceId voice, Vec3 position) {
    const std::lock_guard lock(m_mutex);
    Voice* const found = find(voice);
    if (found != nullptr && found->settings.position.has_value()) {
        found->settings.position = position;
    }
}

void Mixer::set_listener(Vec3 position, Vec3 forward, Vec3 up) {
    const std::lock_guard lock(m_mutex);
    m_listener_position = position;
    const Vec3 right = cross(forward, up);
    const float size = length(right);
    if (size > 1e-6f) {
        m_listener_right = Vec3{right.x / size, right.y / size, right.z / size};
    }
}

void Mixer::set_bus_volume(std::size_t bus, float volume) {
    const std::lock_guard lock(m_mutex);
    if (bus < m_bus_volumes.size()) {
        m_bus_volumes[bus] = std::isfinite(volume) ? std::max(volume, 0.0f) : 0.0f;
    }
}

float Mixer::bus_volume(std::size_t bus) const {
    const std::lock_guard lock(m_mutex);
    return bus < m_bus_volumes.size() ? m_bus_volumes[bus] : 0.0f;
}

std::size_t Mixer::release_finished() {
    const std::lock_guard lock(m_mutex);
    std::size_t released = 0;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i] != nullptr && m_slots[i]->finished) {
            m_slots[i].reset();
            ++m_generations[i];
            ++released;
        }
    }
    m_live -= released;
    return released;
}

std::size_t Mixer::voice_count() const {
    const std::lock_guard lock(m_mutex);
    return m_live;
}

void Mixer::gains(const Voice& voice, float& left, float& right) const {
    const VoiceSettings& settings = voice.settings;
    float gain = std::max(settings.volume, 0.0f) * m_bus_volumes[settings.bus];
    float pan = std::clamp(settings.pan, -1.0f, 1.0f);

    if (settings.position.has_value()) {
        const Vec3 offset{settings.position->x - m_listener_position.x,
                          settings.position->y - m_listener_position.y,
                          settings.position->z - m_listener_position.z};
        const float distance = length(offset);
        const float nearest = std::max(settings.min_distance, 0.01f);
        const float farthest = std::max(settings.max_distance, nearest);
        gain *= nearest / std::clamp(distance, nearest, farthest);
        pan = distance > 1e-6f ? k_spatial_pan * dot(offset, m_listener_right) / distance : 0.0f;
    }
    if (!std::isfinite(gain)) {
        gain = 0.0f;
    }
    left = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
    right = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
}

void Mixer::mix(std::span<float> stereo) {
    std::fill(stereo.begin(), stereo.end(), 0.0f);
    const std::size_t frames = stereo.size() / 2;

    const std::lock_guard lock(m_mutex);
    for (const std::unique_ptr<Voice>& slot : m_slots) {
        if (slot == nullptr || slot->finished) {
            continue;
        }
        Voice& voice = *slot;
        float left_gain = 0.0f;
        float right_gain = 0.0f;
        gains(voice, left_gain, right_gain);
        const float pitch = std::isfinite(voice.settings.pitch)
                                ? std::clamp(voice.settings.pitch, k_lowest_pitch, k_highest_pitch)
                                : 1.0f;
        const double step = voice.rate_ratio * pitch;
        const unsigned channels = voice.channels;

        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto index = static_cast<std::uint64_t>(voice.position);
            voice.load(index);
            if (index < voice.window_start || index >= voice.window_end()) {
                voice.finished = true;
                break;
            }
            const float* const current =
                voice.window.data() + (index - voice.window_start) * channels;
            const float* const next = index + 1 < voice.window_end() ? current + channels : current;
            const auto t = static_cast<float>(voice.position - static_cast<double>(index));

            const float left = current[0] + (next[0] - current[0]) * t;
            const float right = channels > 1 ? current[1] + (next[1] - current[1]) * t : left;
            stereo[frame * 2] += left * left_gain;
            stereo[frame * 2 + 1] += right * right_gain;
            voice.position += step;
        }
        if (voice.exhausted && voice.position >= static_cast<double>(voice.window_end())) {
            voice.finished = true;
        }
    }

    for (float& sample : stereo) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
}

} // namespace mixer
