#include "dream/runtime/texture.h"

#include <array>
#include <cstring>
#include <string>

namespace dream::runtime {
namespace {

// The 12 bytes every KTX2 file starts with.
constexpr std::array<unsigned char, 12> k_ktx2_identifier{0xAB, 'K', 'T', 'X', ' ', '2',
                                                          '0',  0xBB, '\r', '\n', 0x1A, '\n'};

} // namespace

bool is_ktx2(std::span<const std::byte> file) noexcept {
    return file.size() >= k_ktx2_identifier.size() &&
           std::memcmp(file.data(), k_ktx2_identifier.data(), k_ktx2_identifier.size()) == 0;
}

// This build has no texture cooking and no KTX library (ADR 0039): games ship their textures as
// PNG files, which the software renderer reads directly, so a KTX2 file is always an error here.
core::Result<TextureData> load_ktx2(std::span<const std::byte> file,
                                    const rhi::Device& /*device*/) {
    if (!is_ktx2(file)) {
        return core::Error{core::ErrorCode::ParseError, "The file is not a KTX2 texture"};
    }
    return core::Error{core::ErrorCode::InvalidArgument,
                       "This build reads no KTX2 textures; ship the texture as a PNG (ADR 0039)"};
}

// Uncompressed, the only kind of texture this build makes.
rhi::TextureFormat cooked_color_format(const rhi::Device& /*device*/) {
    return rhi::TextureFormat::Rgba8Srgb;
}

std::string_view texture_format_name(rhi::TextureFormat format) noexcept {
    switch (format) {
    case rhi::TextureFormat::Rgba8Srgb:
    case rhi::TextureFormat::Rgba8Unorm:
        return "RGBA8";
    case rhi::TextureFormat::Bc7Srgb:
    case rhi::TextureFormat::Bc7Unorm:
        return "BC7";
    case rhi::TextureFormat::Astc4x4Srgb:
    case rhi::TextureFormat::Astc4x4Unorm:
        return "ASTC 4x4";
    case rhi::TextureFormat::Rgba16Float:
        return "RGBA16 float";
    case rhi::TextureFormat::Rg11B10Float:
        return "R11G11B10 float";
    case rhi::TextureFormat::Rgba32Float:
        return "RGBA32 float";
    case rhi::TextureFormat::Depth32Float:
        return "Depth32 float";
    }
    return "unknown";
}

std::string cooked_texture_path(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(path) + ".ktx2";
    }
    return std::string(path.substr(0, dot)) + ".ktx2";
}

} // namespace dream::runtime
