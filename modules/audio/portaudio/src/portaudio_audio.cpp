#include "decoder.h"

#include "dream/audio/audio.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"

#include <mixer/mixer.h>
#include <portaudio.h>
// Only installed when PortAudio is built with WASAPI, as vcpkg's is.
#if defined(_WIN32) && __has_include(<pa_win_wasapi.h>)
#include <pa_win_wasapi.h>
#define DREAM_PORTAUDIO_WASAPI 1
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// Debug builds of PortAudio, such as vcpkg's Debug one, print a trace of their work to stderr:
// "before paHostApiInitializers[0].", "PA - PlatformId = 0x2" and so on. This function sends that
// trace elsewhere. It isn't in PortAudio's installed headers (it's in pa_debugprint.h), but every
// Windows build exports it: the DLL's .def file lists it, and static libraries contain it.
extern "C" void PaUtil_SetDebugPrintFunction(void (*callback)(const char* text));
#endif

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

#if defined(_WIN32)
// Receives PortAudio's debug trace, in pieces that aren't always whole lines, and logs each line at
// the debug level, so it only shows when the engine's log asks for it. PortAudio's threads print
// too, so each thread gathers its own line.
void log_portaudio_trace(const char* text) {
    thread_local std::string line;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '\n') {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                DREAM_LOG_DEBUG("PortAudio: {}", line);
            }
            line.clear();
        } else {
            line.push_back(*c);
        }
    }
}
#endif

std::string device_name(PaDeviceIndex device) {
    const PaDeviceInfo* const info = Pa_GetDeviceInfo(device);
    const PaHostApiInfo* const api = info == nullptr ? nullptr : Pa_GetHostApiInfo(info->hostApi);
    if (info == nullptr || api == nullptr) {
        return "an unknown device";
    }
    return std::string("'") + info->name + "' through " + api->name;
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

#if defined(_WIN32)
        PaUtil_SetDebugPrintFunction(&log_portaudio_trace);
#endif
        if (const PaError error = Pa_Initialize(); error != paNoError) {
            return audio_error("Could not start PortAudio", error);
        }
        m_initialized = true;

        // The first output that opens is used. Each failure is logged, and all of them are given
        // back if none opens.
        std::string failures;
        for (const PaDeviceIndex device : output_devices()) {
            const PaError error = start_output(device);
            if (error == paNoError) {
                DREAM_LOG_INFO("PortAudio: playing on {} at {} Hz", device_name(device),
                               m_mixer->sample_rate());
                return {};
            }
            const std::string failure = device_name(device) + ": " + Pa_GetErrorText(error);
            DREAM_LOG_WARNING("PortAudio: could not open {}", failure);
            failures += (failures.empty() ? "" : "; ") + failure;
        }
        if (failures.empty()) {
            return core::Error{core::ErrorCode::Unknown, "There is no audio output device"};
        }
        return core::Error{core::ErrorCode::Unknown, "Could not open an audio output: " + failures};
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

    // The outputs to try, best first. PortAudio's default on Windows is MME, whose delay is long
    // enough to hear, so WASAPI's default device comes first, then DirectSound's, then MME's.
    static std::vector<PaDeviceIndex> output_devices() {
        std::vector<PaDeviceIndex> devices;
        const auto add = [&](PaDeviceIndex device) {
            if (device != paNoDevice &&
                std::find(devices.begin(), devices.end(), device) == devices.end()) {
                devices.push_back(device);
            }
        };
#if defined(_WIN32)
        for (const PaHostApiTypeId type : {paWASAPI, paDirectSound, paMME}) {
            const PaHostApiIndex api = Pa_HostApiTypeIdToHostApiIndex(type);
            if (const PaHostApiInfo* const info = api < 0 ? nullptr : Pa_GetHostApiInfo(api)) {
                add(info->defaultOutputDevice);
            }
        }
#endif
        add(Pa_GetDefaultOutputDevice());
        return devices;
    }

    // Opens and starts a stereo output at the device's own rate, which spares the system a
    // conversion, with a mixer for that rate. On failure, nothing is left open.
    PaError start_output(PaDeviceIndex device) {
        const PaDeviceInfo* const info = Pa_GetDeviceInfo(device);
        if (info == nullptr) {
            return paInvalidDevice;
        }
        if (info->maxOutputChannels < 2) {
            return paInvalidChannelCount;
        }

        PaStreamParameters output{};
        output.device = device;
        output.channelCount = 2;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = info->defaultLowOutputLatency;

#if defined(DREAM_PORTAUDIO_WASAPI)
        // WASAPI's shared mode only takes the exact format Windows mixes in, which is often not
        // stereo (5.1 or 7.1 speakers, virtual surround headsets, HDMI receivers) and not
        // always floats, and PortAudio then fails with "Invalid sample rate" or "Invalid number
        // of channels". Auto-conversion has Windows convert this stream to that format instead.
        PaWasapiStreamInfo wasapi{};
        const PaHostApiInfo* const api = Pa_GetHostApiInfo(info->hostApi);
        if (api != nullptr && api->type == paWASAPI) {
            wasapi.size = sizeof(wasapi);
            wasapi.hostApiType = paWASAPI;
            wasapi.version = 1;
            wasapi.flags = paWinWasapiAutoConvert;
            output.hostApiSpecificStreamInfo = &wasapi;
        }
#endif

        // The callback is given the mixer, so it must exist before the stream does.
        auto mixer = std::make_unique<mixer::Mixer>(static_cast<unsigned>(info->defaultSampleRate),
                                                    k_bus_count, k_max_voices);
        PaStream* stream = nullptr;
        const PaError error =
            Pa_OpenStream(&stream, nullptr, &output, info->defaultSampleRate,
                          paFramesPerBufferUnspecified, paClipOff, &on_audio, mixer.get());
        if (error != paNoError) {
            return error;
        }
        if (const PaError started = Pa_StartStream(stream); started != paNoError) {
            Pa_CloseStream(stream);
            return started;
        }
        m_mixer = std::move(mixer);
        m_stream = stream;
        return paNoError;
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
