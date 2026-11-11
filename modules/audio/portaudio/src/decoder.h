#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace dream::modules::portaudio_backend {

// Reads WAV, MP3 or FLAC audio as interleaved floats, using dr_libs. The file's bytes must outlive
// the decoder.
class Decoder {
public:
    // Null if the file isn't one of the formats, or holds no audio.
    [[nodiscard]] static std::unique_ptr<Decoder> open(std::span<const std::byte> file);

    virtual ~Decoder() = default;

    [[nodiscard]] virtual unsigned channels() const = 0;
    [[nodiscard]] virtual unsigned sample_rate() const = 0;

    // How many frames the whole file holds, if the format says without decoding it all.
    [[nodiscard]] virtual std::optional<std::uint64_t> frame_count() = 0;

    // Fills up to `frames` frames and returns how many it read. Fewer means the end.
    virtual std::size_t read(float* interleaved, std::size_t frames) = 0;

    virtual bool rewind() = 0;
};

} // namespace dream::modules::portaudio_backend
