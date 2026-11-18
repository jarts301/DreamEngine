#include "dream/runtime/texture.h"

#include <ktx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <string>

namespace dream::runtime {
namespace {

core::Error texture_error(std::string message) {
    return core::Error{core::ErrorCode::ParseError, std::move(message)};
}

struct KtxDeleter {
    void operator()(ktxTexture2* texture) const {
        ktxTexture_Destroy(ktxTexture(texture));
    }
};

using KtxPointer = std::unique_ptr<ktxTexture2, KtxDeleter>;

// The 12 bytes every KTX2 file starts with.
constexpr std::array<unsigned char, 12> k_ktx2_identifier{0xAB, 'K', 'T', 'X', ' ', '2',
                                                          '0',  0xBB, '\r', '\n', 0x1A, '\n'};

// Vulkan's numbers for the two formats a texture may already be stored in.
constexpr ktx_uint32_t k_vk_format_r8g8b8a8_unorm = 37;
constexpr ktx_uint32_t k_vk_format_r8g8b8a8_srgb = 43;

struct Target {
    rhi::TextureFormat format;
    ktx_transcode_fmt_e transcode;
};

// The best format the device takes for a texture of this size and color space.
Target choose_target(const rhi::Device& device, std::uint32_t width, std::uint32_t height,
                     bool srgb) {
    const bool whole_blocks = width % 4 == 0 && height % 4 == 0;
    const std::array<Target, 2> compressed{
        Target{srgb ? rhi::TextureFormat::Bc7Srgb : rhi::TextureFormat::Bc7Unorm,
               KTX_TTF_BC7_RGBA},
        Target{srgb ? rhi::TextureFormat::Astc4x4Srgb : rhi::TextureFormat::Astc4x4Unorm,
               KTX_TTF_ASTC_4x4_RGBA},
    };
    if (whole_blocks) {
        for (const Target& target : compressed) {
            if (device.supports_texture_format(target.format)) {
                return target;
            }
        }
    }
    return Target{srgb ? rhi::TextureFormat::Rgba8Srgb : rhi::TextureFormat::Rgba8Unorm,
                  KTX_TTF_RGBA32};
}

} // namespace

bool is_ktx2(std::span<const std::byte> file) noexcept {
    return file.size() >= k_ktx2_identifier.size() &&
           std::memcmp(file.data(), k_ktx2_identifier.data(), k_ktx2_identifier.size()) == 0;
}

core::Result<TextureData> load_ktx2(std::span<const std::byte> file, const rhi::Device& device) {
    if (!is_ktx2(file)) {
        return texture_error("The file is not a KTX2 texture");
    }

    ktxTexture2* raw = nullptr;
    const KTX_error_code created = ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(file.data()), file.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw);
    if (created != KTX_SUCCESS) {
        return texture_error(std::format("Could not read a KTX2 texture: {}",
                                         ktxErrorString(created)));
    }
    const KtxPointer texture(raw);

    if (texture->numDimensions != 2 || texture->numFaces != 1 || texture->numLayers != 1 ||
        texture->baseDepth != 1) {
        return texture_error("Only flat 2D KTX2 textures are supported, without layers or faces");
    }
    const std::uint32_t width = texture->baseWidth;
    const std::uint32_t height = texture->baseHeight;
    const bool srgb = ktxTexture2_GetOETF_e(texture.get()) == KHR_DF_TRANSFER_SRGB;

    rhi::TextureFormat format{};
    if (ktxTexture2_NeedsTranscoding(texture.get())) {
        const Target target = choose_target(device, width, height, srgb);
        const KTX_error_code transcoded =
            ktxTexture2_TranscodeBasis(texture.get(), target.transcode, 0);
        if (transcoded != KTX_SUCCESS) {
            return texture_error(std::format("Could not transcode a KTX2 texture: {}",
                                             ktxErrorString(transcoded)));
        }
        format = target.format;
    } else if (texture->vkFormat == k_vk_format_r8g8b8a8_srgb) {
        format = rhi::TextureFormat::Rgba8Srgb;
    } else if (texture->vkFormat == k_vk_format_r8g8b8a8_unorm) {
        format = rhi::TextureFormat::Rgba8Unorm;
    } else {
        return texture_error("A KTX2 texture must be Basis Universal or RGBA8; cook it with "
                             "'ktx create --encode uastc'");
    }

    // KTX2 stores the smallest level first, so the levels are copied out in the other order.
    TextureData data;
    data.desc = rhi::TextureDesc{.width = width,
                                 .height = height,
                                 .format = format,
                                 .mip_levels = texture->numLevels,
                                 .generate_mipmaps = texture->numLevels == 1};
    data.pixels.resize(rhi::texture_bytes(data.desc));
    const ktx_uint8_t* const source = ktxTexture_GetData(ktxTexture(texture.get()));
    std::size_t offset = 0;
    for (std::uint32_t level = 0; level < texture->numLevels; ++level) {
        ktx_size_t level_offset = 0;
        if (ktxTexture_GetImageOffset(ktxTexture(texture.get()), level, 0, 0, &level_offset) !=
            KTX_SUCCESS) {
            return texture_error("A KTX2 texture's levels are inconsistent");
        }
        const std::size_t expected = rhi::texture_level_bytes(
            format, rhi::mip_size(width, level), rhi::mip_size(height, level));
        if (ktxTexture_GetImageSize(ktxTexture(texture.get()), level) != expected) {
            return texture_error("A KTX2 texture's level has an unexpected size");
        }
        std::memcpy(data.pixels.data() + offset, source + level_offset, expected);
        offset += expected;
    }
    if (const auto problem = rhi::texture_problem(data.desc, data.pixels.size())) {
        return texture_error(std::string(*problem));
    }
    return data;
}

rhi::TextureFormat cooked_color_format(const rhi::Device& device) {
    return choose_target(device, 4, 4, true).format;
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
