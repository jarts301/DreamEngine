#include "decoder.h"

#include "dream/audio/audio.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"

#include <mixer/mixer.h>
#include <portaudio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace audio;
using portaudio_backend::Decoder;

constexpr std::size_t k_max_voices = 64;
constexpr std::size_t k_bus_count = static_cast<std::size_t>(Bus::Count);

// The rate used when mixing without sound hardware. With a device, its own rate is used.
constexpr unsigned k_offline_rate = 48000;

// Frames decoded at a time when a whole sound is decoded up front.
constexpr std::size_t k_decode_chunk = 4096;

core::Error audio_error(const std::string& what, PaError error) {
    return core::Error{core::ErrorCode::Unknown, what + ": " + Pa_GetErrorText(error)};
}

mixer::Vec3 to_mixer(core::Vec3 v) {
    return mixer::Vec3{v.x, v.y, v.z};
}

// Decodes a streamed sound's file while a voice plays it. It shares the file's bytes with the
// sound.
class DecoderStream final : public mixer::Stream {
public:
    DecoderStream(std::shared_ptr<const std::vector<std::byte>> file,
                  std::unique_ptr<Decoder> decoder)
        : m_file(std::move(file)), m_decoder(std::move(decoder)) {}

    unsigned channels() const override {
        return m_decoder->channels();
    }

    unsigned sample_rate() const override {
        return m_decoder->sample_rate();
    }

    std::size_t read(std::span<float> interleaved) override {
        return m_decoder->read(interleaved.data(), interleaved.size() / m_decoder->channels());
    }

    bool rewind() override {
        return m_decoder->rewind();
    }

private:
    std::shared_ptr<const std::vector<std::byte>> m_file;
    std::unique_ptr<Decoder> m_decoder;
};

struct SoundData {
    // Set for sounds decoded up front.
    std::shared_ptr<const mixer::Sound> decoded;

    // Set for streamed sounds: the file, decoded by each voice as it plays.
    std::shared_ptr<const std::vector<std::byte>> encoded;

    float length_seconds = 0.0f;
};

struct VoiceData {
    mixer::VoiceId id;
    core::Handle<SoundData> sound;
};

// Plays sounds through PortAudio, mixed by the engine's own mixer and decoded by dr_libs.
class PortAudioDevice final : public AudioDevice {
public:
    PortAudioDevice() = default;
    PortAudioDevice(const PortAudioDevice&) = delete;
    PortAudioDevice& operator=(const PortAudioDevice&) = delete;

    ~PortAudioDevice() override {
        // The callback reads the mixer, so the stream goes first.
        if (m_stream != nullptr) {
            Pa_AbortStream(m_stream);
            Pa_CloseStream(m_stream);
        }
        if (m_initialized) {
            Pa_Terminate();
        }
    }

    core::Result<void> initialize(bool with_device) {
        if (!with_device) {
            m_mixer = std::make_unique<mixer::Mixer>(k_offline_rate, k_bus_count, k_max_voices);
            return {};
        }

        if (const PaError error = Pa_Initialize(); error != paNoError) {
            return audio_error("Could not start PortAudio", error);
        }
        m_initialized = true;

        const PaDeviceIndex device = output_device();
        const PaDeviceInfo* const info = device == paNoDevice ? nullptr : Pa_GetDeviceInfo(device);
        if (info == nullptr || info->maxOutputChannels < 2) {
            return core::Error{core::ErrorCode::Unknown, "There is no stereo audio output device"};
        }

        // Mixing at the device's own rate spares the system a conversion.
        const auto rate = static_cast<unsigned>(info->defaultSampleRate);
        m_mixer = std::make_unique<mixer::Mixer>(rate, k_bus_count, k_max_voices);

        PaStreamParameters output{};
        output.device = device;
        output.channelCount = 2;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = info->defaultLowOutputLatency;
        if (const PaError error =
                Pa_OpenStream(&m_stream, nullptr, &output, info->defaultSampleRate,
                              paFramesPerBufferUnspecified, paClipOff, &on_audio, m_mixer.get());
            error != paNoError) {
            m_stream = nullptr;
            return audio_error("Could not open the audio output", error);
        }
        if (const PaError error = Pa_StartStream(m_stream); error != paNoError) {
            return audio_error("Could not start the audio output", error);
        }
        DREAM_LOG_INFO("PortAudio: playing on '{}' through {} at {} Hz", info->name,
                       Pa_GetHostApiInfo(info->hostApi)->name, rate);
        return {};
    }

    core::Result<SoundHandle> create_sound(std::span<const std::byte> file, bool stream) override {
        if (file.empty()) {
            return core::Error{core::ErrorCode::InvalidArgument, "A sound file can't be empty"};
        }
        std::unique_ptr<Decoder> decoder = Decoder::open(file);
        if (decoder == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Could not read the sound, which must be WAV, MP3 or FLAC"};
        }
        const auto rate = static_cast<float>(decoder->sample_rate());

        SoundData data;
        if (stream) {
            data.encoded = std::make_shared<const std::vector<std::byte>>(file.begin(), file.end());
            data.length_seconds = static_cast<float>(decoder->frame_count().value_or(0)) / rate;
        } else {
            auto sound = std::make_shared<mixer::Sound>();
            sound->channels = decoder->channels();
            sound->sample_rate = decoder->sample_rate();
            // Read in pieces, because some formats only know their length once decoded.
            std::size_t frames = 0;
            while (true) {
                sound->samples.resize((frames + k_decode_chunk) * sound->channels);
                const std::size_t read =
                    decoder->read(sound->samples.data() + frames * sound->channels, k_decode_chunk);
                frames += read;
                if (read < k_decode_chunk) {
                    break;
                }
            }
            sound->samples.resize(frames * sound->channels);
            if (frames == 0) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "The sound has no audio in it"};
            }
            data.length_seconds = static_cast<float>(frames) / rate;
            data.decoded = std::move(sound);
        }
        return handle_cast<SoundTag>(m_sounds.create(std::move(data)));
    }

    void destroy_sound(SoundHandle sound) override {
        const auto key = handle_cast<SoundData>(sound);
        if (!m_sounds.contains(key)) {
            return;
        }
        m_voices.for_each([&](core::Handle<VoiceData> voice, const VoiceData& data) {
            if (data.sound == key) {
                m_mixer->stop(data.id);
                m_voices.destroy(voice);
            }
        });
        m_sounds.destroy(key);
    }

    std::optional<float> sound_length(SoundHandle sound) const override {
        const SoundData* const data = m_sounds.get(handle_cast<SoundData>(sound));
        return data == nullptr ? std::nullopt : std::optional<float>(data->length_seconds);
    }

    core::Result<VoiceHandle> play(SoundHandle sound, const PlayDesc& desc) override {
        const auto key = handle_cast<SoundData>(sound);
        const SoundData* const data = m_sounds.get(key);
        if (data == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Playing a sound that doesn't exist"};
        }
        if (desc.bus >= Bus::Count) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Playing on a bus that doesn't exist"};
        }

        std::unique_ptr<mixer::Stream> source;
        if (data->decoded != nullptr) {
            source = mixer::make_sound_stream(data->decoded);
        } else {
            std::unique_ptr<Decoder> decoder = Decoder::open(*data->encoded);
            if (decoder == nullptr) {
                return core::Error{core::ErrorCode::InvalidArgument, "Could not stream the sound"};
            }
            source = std::make_unique<DecoderStream>(data->encoded, std::move(decoder));
        }

        mixer::VoiceSettings settings{
            .bus = static_cast<std::size_t>(desc.bus),
            .volume = desc.volume,
            .pitch = desc.pitch,
            .pan = desc.pan,
            .looping = desc.looping,
            .min_distance = desc.min_distance,
            .max_distance = desc.max_distance,
        };
        if (desc.position.has_value()) {
            settings.position = to_mixer(*desc.position);
        }
        const std::optional<mixer::VoiceId> id = m_mixer->play(std::move(source), settings);
        if (!id.has_value()) {
            return core::Error{core::ErrorCode::OutOfMemory, "Too many sounds are playing"};
        }
        return handle_cast<VoiceTag>(m_voices.create(VoiceData{*id, key}));
    }

    void stop(VoiceHandle voice) override {
        const auto key = handle_cast<VoiceData>(voice);
        if (const VoiceData* const data = m_voices.get(key)) {
            m_mixer->stop(data->id);
            m_voices.destroy(key);
        }
    }

    bool is_playing(VoiceHandle voice) const override {
        const VoiceData* const data = m_voices.get(handle_cast<VoiceData>(voice));
        return data != nullptr && m_mixer->is_playing(data->id);
    }

    void set_volume(VoiceHandle voice, float volume) override {
        if (const VoiceData* const data = m_voices.get(handle_cast<VoiceData>(voice))) {
            m_mixer->set_volume(data->id, volume);
        }
    }

    void set_pitch(VoiceHandle voice, float pitch) override {
        if (const VoiceData* const data = m_voices.get(handle_cast<VoiceData>(voice))) {
            m_mixer->set_pitch(data->id, pitch);
        }
    }

    void set_position(VoiceHandle voice, core::Vec3 position) override {
        if (const VoiceData* const data = m_voices.get(handle_cast<VoiceData>(voice))) {
            m_mixer->set_position(data->id, to_mixer(position));
        }
    }

    void set_listener(core::Vec3 position, core::Vec3 forward, core::Vec3 up) override {
        m_mixer->set_listener(to_mixer(position), to_mixer(forward), to_mixer(up));
    }

    void set_bus_volume(Bus bus, float volume) override {
        if (bus < Bus::Count) {
            m_mixer->set_bus_volume(static_cast<std::size_t>(bus), volume);
        }
    }

    float bus_volume(Bus bus) const override {
        return bus < Bus::Count ? m_mixer->bus_volume(static_cast<std::size_t>(bus)) : 0.0f;
    }

    void suspend() override {
        if (m_stream != nullptr && Pa_IsStreamActive(m_stream) == 1) {
            Pa_StopStream(m_stream);
        }
    }

    void resume() override {
        if (m_stream != nullptr && Pa_IsStreamStopped(m_stream) == 1) {
            if (const PaError error = Pa_StartStream(m_stream); error != paNoError) {
                DREAM_LOG_WARNING("PortAudio: could not resume the audio output: {}",
                                  Pa_GetErrorText(error));
            }
        }
    }

    void update() override {
        m_voices.for_each([&](core::Handle<VoiceData> voice, const VoiceData& data) {
            if (!m_mixer->is_playing(data.id)) {
                m_mixer->stop(data.id);
                m_voices.destroy(voice);
            }
        });
    }

    std::size_t voice_count() const override {
        return m_voices.size();
    }

    // For tests of a device without output: mixes and returns the next frames.
    std::uint64_t read(std::span<float> interleaved_stereo) {
        if (m_stream != nullptr) {
            return 0;
        }
        m_mixer->mix(interleaved_stereo);
        return interleaved_stereo.size() / 2;
    }

private:
    static int on_audio(const void* /*input*/, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo* /*time*/, PaStreamCallbackFlags /*flags*/,
                        void* mixer) {
        static_cast<mixer::Mixer*>(mixer)->mix(
            std::span<float>(static_cast<float*>(output), static_cast<std::size_t>(frames) * 2));
        return paContinue;
    }

    // PortAudio's default on Windows is MME, whose delay is long enough to hear, so WASAPI's
    // default device is preferred when it is there.
    static PaDeviceIndex output_device() {
#if defined(_WIN32)
        const PaHostApiIndex wasapi = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
        if (wasapi >= 0) {
            const PaHostApiInfo* const api = Pa_GetHostApiInfo(wasapi);
            if (api != nullptr && api->defaultOutputDevice != paNoDevice) {
                return api->defaultOutputDevice;
            }
        }
#endif
        return Pa_GetDefaultOutputDevice();
    }

    std::unique_ptr<mixer::Mixer> m_mixer;
    PaStream* m_stream = nullptr;
    bool m_initialized = false;
    core::HandlePool<SoundData> m_sounds;
    core::HandlePool<VoiceData> m_voices;
};

// Settings: [audio.portaudio] device = false mixes without sound hardware, for tests.
core::Result<std::unique_ptr<AudioDevice>> create(const core::Config& config) {
    auto device = std::make_unique<PortAudioDevice>();
    const bool with_device = config.get_bool("audio.portaudio.device").value_or(true);
    if (core::Result<void> started = device->initialize(with_device); !started.has_value()) {
        return started.error();
    }
    return std::unique_ptr<AudioDevice>{std::move(device)};
}

} // namespace

[[nodiscard]] core::Result<void> register_audio_portaudio(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<AudioDevice>{.name = "portaudio", .create = &create});
}

// Test access: mixes the next frames of a device this module created without output.
std::uint64_t portaudio_read_frames(AudioDevice& device, std::span<float> interleaved_stereo) {
    return static_cast<PortAudioDevice&>(device).read(interleaved_stereo);
}

} // namespace dream::modules
