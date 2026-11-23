#include "dream/runtime/texture.h"

#include <format>
#include <string>

namespace dream::runtime {

core::Result<TextureData> decode_cooked_texture(texture_decoder::TextureDecoder* decoder,
                                                std::span<const std::byte> file,
                                                const rhi::Device& device, std::string_view path) {
    if (decoder == nullptr) {
        return core::Error{core::ErrorCode::NotFound,
                           std::format("'{}' is a cooked texture, and the project lists no "
                                       "texture_decoder for this platform to read it; list "
                                       "\"ktx\"",
                                       path)};
    }
    return decoder->decode(file, device);
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
