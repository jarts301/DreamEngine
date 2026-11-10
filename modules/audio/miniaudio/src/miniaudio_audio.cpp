#include "dream/audio/audio.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dream::modules {
namespace {

using namespace audio;

constexpr std::size_t k_max_voices = 64;
constexpr std::size_t k_bus_count = static_cast<std::size_t>(Bus::Count);

core::Error audio_error(const char* what, ma_result result) {
    return core::Error{core::ErrorCode::Unknown,
                       std::string(what) + ": " + ma_result_description(result)};
}

struct SoundData {
    // Decoded sounds: interleaved samples at the engine's rate.
    std::vector<float> samples;
    ma_uint64 frame_count = 0;
    ma_uint32 channels = 0;

    // Streamed sounds: the file, decoded by each voice as it plays.
    std::vector<std::byte> encoded;
    bool stream = false;
    float length_seconds = 0.0f;
};

// miniaudio keeps pointers into these, so each lives on the heap and never moves.
struct VoiceData {
    core::Handle<SoundData> sound;
    ma_audio_buffer_ref buffer{};
    ma_decoder decoder{};
    ma_sound sound_object{};
    bool has_buffer = false;
    bool has_decoder = false;
    bool has_sound = false;

    ~VoiceData() {
        if (has_sound) {
            ma_sound_uninit(&sound_object);
        }
        if (has_decoder) {
            ma_decoder_uninit(&decoder);
        }
        if (has_buffer) {
            ma_audio_buffer_ref_uninit(&buffer);
        }
    }
};

class MiniaudioDevice final : public AudioDevice {
public:
    ~MiniaudioDevice() override {
        m_voices = {};
        for (std::size_t i = 0; i < m_groups_ready; ++i) {
            ma_sound_group_uninit(&m_groups[i]);
        }
        if (m_engine_ready) {
            ma_engine_uninit(&m_engine);
        }
    }

    core::Result<void> initialize(bool with_device) {
        ma_engine_config config = ma_engine_config_init();
        if (!with_device) {
            // Mixed only when read, for tests and machines without sound hardware.
            config.noDevice = MA_TRUE;
            config.channels = 2;
            config.sampleRate = 48000;
        }
        if (const ma_result result = ma_engine_init(&config, &m_engine); result != MA_SUCCESS) {
            return audio_error("Could not start audio output", result);
        }
        m_engine_ready = true;
        for (ma_sound_group& group : m_groups) {
            if (const ma_result result = ma_sound_group_init(&m_engine, 0, nullptr, &group);
                result != MA_SUCCESS) {
                return audio_error("Could not create an audio bus", result);
            }
            ++m_groups_ready;
        }
        return {};
    }

    core::Result<SoundHandle> create_sound(std::span<const std::byte> file, bool stream) override {
        if (file.empty()) {
            return core::Error{core::ErrorCode::InvalidArgument, "A sound file can't be empty"};
        }
        const ma_uint32 rate = ma_engine_get_sample_rate(&m_engine);
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, rate);
        ma_decoder decoder;
        if (const ma_result result =
                ma_decoder_init_memory(file.data(), file.size(), &config, &decoder);
            result != MA_SUCCESS) {
            return audio_error("Could not read the sound", result);
        }

        SoundData data;
        data.stream = stream;
        data.channels = decoder.outputChannels;
        ma_uint64 frames = 0;
        ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
        data.length_seconds = static_cast<float>(frames) / static_cast<float>(rate);

        if (stream) {
            data.encoded.assign(file.begin(), file.end());
        } else {
            // Read in pieces, because some formats only know their length once decoded.
            constexpr ma_uint64 k_piece = 4096;
            std::vector<float> piece(k_piece * data.channels);
            while (true) {
                ma_uint64 read = 0;
                const ma_result result =
                    ma_decoder_read_pcm_frames(&decoder, piece.data(), k_piece, &read);
                data.samples.insert(data.samples.end(), piece.begin(),
                                    piece.begin() + static_cast<std::ptrdiff_t>(read * data.channels));
                if (result != MA_SUCCESS || read < k_piece) {
                    break;
                }
            }
            data.frame_count = data.samples.size() / data.channels;
            data.length_seconds = static_cast<float>(data.frame_count) / static_cast<float>(rate);
        }
        ma_decoder_uninit(&decoder);

        if (data.channels == 0 || (!stream && data.frame_count == 0)) {
            return core::Error{core::ErrorCode::InvalidArgument, "The sound has no audio in it"};
        }
        return handle_cast<SoundTag>(m_sounds.create(std::move(data)));
    }

    void destroy_sound(SoundHandle sound) override {
        const auto handle = handle_cast<SoundData>(sound);
        if (!m_sounds.contains(handle)) {
            return;
        }
        m_voices.for_each([&](core::Handle<std::unique_ptr<VoiceData>> voice,
                              std::unique_ptr<VoiceData>& data) {
            if (data->sound == handle) {
                m_voices.destroy(voice);
            }
        });
        m_sounds.destroy(handle);
    }

    std::optional<float> sound_length(SoundHandle sound) const override {
        const SoundData* const data = m_sounds.get(handle_cast<SoundData>(sound));
        return data == nullptr ? std::nullopt : std::optional<float>(data->length_seconds);
    }

    core::Result<VoiceHandle> play(SoundHandle sound, const PlayDesc& desc) override {
        SoundData* const data = m_sounds.get(handle_cast<SoundData>(sound));
        if (data == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "Playing a sound that doesn't exist"};
        }
        if (desc.bus >= Bus::Count) {
            return core::Error{core::ErrorCode::InvalidArgument, "Playing on a bus that doesn't exist"};
        }
        if (m_voices.size() >= k_max_voices) {
            return core::Error{core::ErrorCode::OutOfMemory, "Too many sounds are playing"};
        }

        auto voice = std::make_unique<VoiceData>();
        voice->sound = handle_cast<SoundData>(sound);
        ma_data_source* source = nullptr;
        if (data->stream) {
            ma_decoder_config config =
                ma_decoder_config_init(ma_format_f32, 0, ma_engine_get_sample_rate(&m_engine));
            if (const ma_result result = ma_decoder_init_memory(
                    data->encoded.data(), data->encoded.size(), &config, &voice->decoder);
                result != MA_SUCCESS) {
                return audio_error("Could not stream the sound", result);
            }
            voice->has_decoder = true;
            source = &voice->decoder;
        } else {
            if (const ma_result result =
                    ma_audio_buffer_ref_init(ma_format_f32, data->channels, data->samples.data(),
                                             data->frame_count, &voice->buffer);
                result != MA_SUCCESS) {
                return audio_error("Could not play the sound", result);
            }
            voice->buffer.sampleRate = ma_engine_get_sample_rate(&m_engine);
            voice->has_buffer = true;
            source = &voice->buffer;
        }

        const ma_uint32 flags = desc.position.has_value() ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (const ma_result result = ma_sound_init_from_data_source(
                &m_engine, source, flags, &m_groups[static_cast<std::size_t>(desc.bus)],
                &voice->sound_object);
            result != MA_SUCCESS) {
            return audio_error("Could not play the sound", result);
        }
        voice->has_sound = true;

        ma_sound* const object = &voice->sound_object;
        ma_sound_set_volume(object, std::max(desc.volume, 0.0f));
        ma_sound_set_pitch(object, std::max(desc.pitch, 0.01f));
        ma_sound_set_looping(object, desc.looping ? MA_TRUE : MA_FALSE);
        if (desc.position.has_value()) {
            ma_sound_set_position(object, desc.position->x, desc.position->y, desc.position->z);
            ma_sound_set_attenuation_model(object, ma_attenuation_model_inverse);
            ma_sound_set_min_distance(object, std::max(desc.min_distance, 0.01f));
            ma_sound_set_max_distance(object, std::max(desc.max_distance, desc.min_distance));
        } else {
            ma_sound_set_pan(object, std::clamp(desc.pan, -1.0f, 1.0f));
        }
        if (const ma_result result = ma_sound_start(object); result != MA_SUCCESS) {
            return audio_error("Could not start the sound", result);
        }
        return handle_cast<VoiceTag>(m_voices.create(std::move(voice)));
    }

    void stop(VoiceHandle voice) override {
        m_voices.destroy(handle_cast<std::unique_ptr<VoiceData>>(voice));
    }

    bool is_playing(VoiceHandle voice) const override {
        const auto* const data = m_voices.get(handle_cast<std::unique_ptr<VoiceData>>(voice));
        return data != nullptr && !ma_sound_at_end(&(*data)->sound_object);
    }

    void set_volume(VoiceHandle voice, float volume) override {
        if (ma_sound* const object = find(voice)) {
            ma_sound_set_volume(object, std::max(volume, 0.0f));
        }
    }

    void set_pitch(VoiceHandle voice, float pitch) override {
        if (ma_sound* const object = find(voice)) {
            ma_sound_set_pitch(object, std::max(pitch, 0.01f));
        }
    }

    void set_position(VoiceHandle voice, core::Vec3 position) override {
        if (ma_sound* const object = find(voice)) {
            ma_sound_set_position(object, position.x, position.y, position.z);
        }
    }

    void set_listener(core::Vec3 position, core::Vec3 forward, core::Vec3 up) override {
        ma_engine_listener_set_position(&m_engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&m_engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&m_engine, 0, up.x, up.y, up.z);
    }

    void set_bus_volume(Bus bus, float volume) override {
        if (bus < Bus::Count) {
            ma_sound_group_set_volume(&m_groups[static_cast<std::size_t>(bus)],
                                      std::max(volume, 0.0f));
        }
    }

    float bus_volume(Bus bus) const override {
        return bus < Bus::Count
                   ? ma_sound_group_get_volume(&m_groups[static_cast<std::size_t>(bus)])
                   : 0.0f;
    }

    void suspend() override {
        ma_engine_stop(&m_engine);
    }

    void resume() override {
        ma_engine_start(&m_engine);
    }

    void update() override {
        m_voices.for_each([&](core::Handle<std::unique_ptr<VoiceData>> voice,
                              std::unique_ptr<VoiceData>& data) {
            if (ma_sound_at_end(&data->sound_object)) {
                m_voices.destroy(voice);
            }
        });
    }

    std::size_t voice_count() const override {
        return m_voices.size();
    }

    // For tests of a device without output: mixes and returns the next frames.
    ma_uint64 read(std::span<float> interleaved_stereo) {
        ma_uint64 read = 0;
        ma_engine_read_pcm_frames(&m_engine, interleaved_stereo.data(),
                                  interleaved_stereo.size() / 2, &read);
        return read;
    }

private:
    ma_sound* find(VoiceHandle voice) {
        auto* const data = m_voices.get(handle_cast<std::unique_ptr<VoiceData>>(voice));
        return data == nullptr ? nullptr : &(*data)->sound_object;
    }

    ma_engine m_engine{};
    std::array<ma_sound_group, k_bus_count> m_groups{};
    std::size_t m_groups_ready = 0;
    bool m_engine_ready = false;
    core::HandlePool<SoundData> m_sounds;
    core::HandlePool<std::unique_ptr<VoiceData>> m_voices;
};

// Settings: [audio.miniaudio] device = false mixes without sound hardware, for tests.
core::Result<std::unique_ptr<AudioDevice>> create(const core::Config& config) {
    auto device = std::make_unique<MiniaudioDevice>();
    const bool with_device = config.get_bool("audio.miniaudio.device").value_or(true);
    if (core::Result<void> started = device->initialize(with_device); !started.has_value()) {
        return started.error();
    }
    return std::unique_ptr<AudioDevice>{std::move(device)};
}

} // namespace

[[nodiscard]] core::Result<void> register_audio_miniaudio(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<AudioDevice>{.name = "miniaudio", .create = &create});
}

// Test access: mixes the next frames of a device this module created without output.
std::uint64_t miniaudio_read_frames(AudioDevice& device, std::span<float> interleaved_stereo) {
    return static_cast<MiniaudioDevice&>(device).read(interleaved_stereo);
}

} // namespace dream::modules
