#pragma once

#include "dream/core/result.h"
#include "dream/rhi/rhi.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// A texture as create_texture takes it: every level, largest first, in `desc.format`.
struct TextureData {
    rhi::TextureDesc desc;
    std::vector<std::byte> pixels;
};

// Whether a file is a KTX2 texture, by its first bytes.
[[nodiscard]] bool is_ktx2(std::span<const std::byte> file) noexcept;

// Reads a cooked texture: a KTX2 file of UASTC or ETC1S data, as the build's texture cooking makes,
// or of plain RGBA8. A Basis Universal texture is transcoded to the best format the device takes:
// BC7, then ASTC 4x4, then RGBA8. Compressed formats are used only for sizes that are whole 4x4
// blocks. Whether colors are sRGB comes from the file.
[[nodiscard]] core::Result<TextureData> load_ktx2(std::span<const std::byte> file,
                                                 const rhi::Device& device);

// What load_ktx2 transcodes colors of a size in whole 4x4 blocks to on this device.
[[nodiscard]] rhi::TextureFormat cooked_color_format(const rhi::Device& device);

// A format's name, for messages.
[[nodiscard]] std::string_view texture_format_name(rhi::TextureFormat format) noexcept;

// The cooked name of a texture file: "textures/rock.png" becomes "textures/rock.ktx2".
[[nodiscard]] std::string cooked_texture_path(std::string_view path);

} // namespace dream::runtime
