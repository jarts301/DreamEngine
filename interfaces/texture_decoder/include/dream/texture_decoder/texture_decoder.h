#pragma once

#include "dream/core/result.h"
#include "dream/rhi/rhi.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace dream::texture_decoder {

// Reads the KTX2 textures the build cooked (ADR 0024, 0043) into a format the device draws. The
// texture cooker a project picks writes the files, and the decoder it lists for the platform reads
// them; a project that cooks no textures lists "null", or nothing, and links no KTX library.

// A texture as the device takes it: every level, largest first, in `desc.format`.
struct Decoded {
    rhi::TextureDesc desc;
    std::vector<std::byte> pixels;
};

class TextureDecoder {
public:
    static constexpr std::string_view k_module_interface = "texture_decoder";

    virtual ~TextureDecoder() = default;

    // A cooked texture, transcoded for `device` where it needs to be. Fails for a file that isn't
    // one this decoder reads.
    [[nodiscard]] virtual core::Result<Decoded> decode(std::span<const std::byte> file,
                                                       const rhi::Device& device) = 0;

    // What colors whose size is whole 4x4 blocks become on `device`, for the log.
    [[nodiscard]] virtual rhi::TextureFormat color_format(const rhi::Device& device) const = 0;
};

} // namespace dream::texture_decoder
