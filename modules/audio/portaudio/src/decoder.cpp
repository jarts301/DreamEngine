// dr_libs' decoders and their implementations, compiled into this file only. Their functions are
// made static, so they can't clash with another copy in the same program, such as miniaudio's.
// This file is built without the engine's warnings, because dr_libs' code isn't held to them.

#include "decoder.h"

#define DRWAV_API static
#define DRWAV_PRIVATE static
#define DRMP3_API static
#define DRMP3_PRIVATE static
#define DRFLAC_API static
#define DRFLAC_PRIVATE static
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>

#include <cstring>
#include <utility>

namespace dream::modules::portaudio_backend {
namespace {

class WavDecoder final : public Decoder {
public:
    ~WavDecoder() override {
        if (m_open) {
            drwav_uninit(&m_wav);
        }
    }
    bool open(std::span<const std::byte> file) {
        m_open = drwav_init_memory(&m_wav, file.data(), file.size(), nullptr) == DRWAV_TRUE;
        return m_open;
    }
    unsigned channels() const override {
        return m_wav.channels;
    }
    unsigned sample_rate() const override {
        return m_wav.sampleRate;
    }
    std::optional<std::uint64_t> frame_count() override {
        return m_wav.totalPCMFrameCount;
    }
    std::size_t read(float* interleaved, std::size_t frames) override {
        return static_cast<std::size_t>(drwav_read_pcm_frames_f32(&m_wav, frames, interleaved));
    }
    bool rewind() override {
        return drwav_seek_to_pcm_frame(&m_wav, 0) == DRWAV_TRUE;
    }

private:
    drwav m_wav{};
    bool m_open = false;
};

class FlacDecoder final : public Decoder {
public:
    ~FlacDecoder() override {
        drflac_close(m_flac);
    }
    bool open(std::span<const std::byte> file) {
        m_flac = drflac_open_memory(file.data(), file.size(), nullptr);
        return m_flac != nullptr;
    }
    unsigned channels() const override {
        return m_flac->channels;
    }
    unsigned sample_rate() const override {
        return m_flac->sampleRate;
    }
    std::optional<std::uint64_t> frame_count() override {
        if (m_flac->totalPCMFrameCount == 0) {
            return std::nullopt;
        }
        return m_flac->totalPCMFrameCount;
    }
    std::size_t read(float* interleaved, std::size_t frames) override {
        return static_cast<std::size_t>(drflac_read_pcm_frames_f32(m_flac, frames, interleaved));
    }
    bool rewind() override {
        return drflac_seek_to_pcm_frame(m_flac, 0) == DRFLAC_TRUE;
    }

private:
    drflac* m_flac = nullptr;
};

class Mp3Decoder final : public Decoder {
public:
    ~Mp3Decoder() override {
        if (m_open) {
            drmp3_uninit(&m_mp3);
        }
    }
    bool open(std::span<const std::byte> file) {
        m_open = drmp3_init_memory(&m_mp3, file.data(), file.size(), nullptr) == DRMP3_TRUE;
        return m_open;
    }
    unsigned channels() const override {
        return m_mp3.channels;
    }
    unsigned sample_rate() const override {
        return m_mp3.sampleRate;
    }
    // MP3 files don't record their length, so finding it means decoding them.
    std::optional<std::uint64_t> frame_count() override {
        const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&m_mp3);
        drmp3_seek_to_pcm_frame(&m_mp3, 0);
        return frames;
    }
    std::size_t read(float* interleaved, std::size_t frames) override {
        return static_cast<std::size_t>(drmp3_read_pcm_frames_f32(&m_mp3, frames, interleaved));
    }
    bool rewind() override {
        return drmp3_seek_to_pcm_frame(&m_mp3, 0) == DRMP3_TRUE;
    }

private:
    drmp3 m_mp3{};
    bool m_open = false;
};

template <typename Format> std::unique_ptr<Decoder> try_open(std::span<const std::byte> file) {
    auto decoder = std::make_unique<Format>();
    if (!decoder->open(file) || decoder->channels() == 0 || decoder->sample_rate() == 0) {
        return nullptr;
    }
    return decoder;
}

} // namespace

std::unique_ptr<Decoder> Decoder::open(std::span<const std::byte> file) {
    if (file.empty()) {
        return nullptr;
    }
    // MP3 goes last: its decoder searches the bytes for anything that looks like audio.
    if (auto wav = try_open<WavDecoder>(file)) {
        return wav;
    }
    if (auto flac = try_open<FlacDecoder>(file)) {
        return flac;
    }
    return try_open<Mp3Decoder>(file);
}

} // namespace dream::modules::portaudio_backend
