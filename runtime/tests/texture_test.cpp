#include "dream/runtime/texture.h"

#include "fake_device.h"

#include <doctest/doctest.h>
#include <ktx.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using dream::runtime::tests::FakeDevice;
using dream::runtime::tests::FakeDeviceState;

namespace {

constexpr ktx_uint32_t k_vk_format_r8g8b8a8_srgb = 43;

// An image whose left half is orange and right half sky blue, all opaque.
std::vector<ktx_uint8_t> two_colors(std::uint32_t width, std::uint32_t height) {
    std::vector<ktx_uint8_t> pixels(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            ktx_uint8_t* const pixel = &pixels[(std::size_t{y} * width + x) * 4];
            const bool left = x < width / 2;
            pixel[0] = left ? 240 : 60;
            pixel[1] = left ? 140 : 170;
            pixel[2] = left ? 20 : 230;
            pixel[3] = 255;
        }
    }
    return pixels;
}

// Makes a KTX2 file the way the build's cooking does, with libktx's own encoder: sRGB, one level,
// or every level when `mipmaps`, each a box filter of the one before, and UASTC when `uastc`.
std::vector<std::byte> make_ktx2(std::uint32_t width, std::uint32_t height, bool uastc,
                                 bool mipmaps) {
    ktxTextureCreateInfo info{};
    info.vkFormat = k_vk_format_r8g8b8a8_srgb;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = 1;
    info.numLayers = 1;
    info.numFaces = 1;
    info.generateMipmaps = KTX_FALSE;
    if (mipmaps) {
        for (std::uint32_t size = std::max(width, height); size > 1; size /= 2) {
            ++info.numLevels;
        }
    }

    ktxTexture2* texture = nullptr;
    REQUIRE(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) == KTX_SUCCESS);
    std::vector<ktx_uint8_t> level = two_colors(width, height);
    for (std::uint32_t index = 0; index < info.numLevels; ++index) {
        const std::uint32_t level_width = rhi::mip_size(width, index);
        const std::uint32_t level_height = rhi::mip_size(height, index);
        if (index > 0) {
            level = two_colors(level_width, level_height);
        }
        REQUIRE(ktxTexture_SetImageFromMemory(ktxTexture(texture), index, 0, 0, level.data(),
                                              level.size()) == KTX_SUCCESS);
    }
    if (uastc) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = 1;
        REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
    }

    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    REQUIRE(ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size) == KTX_SUCCESS);
    const auto* const start = reinterpret_cast<const std::byte*>(bytes);
    std::vector<std::byte> file(start, start + size);
    std::free(bytes);
    ktxTexture_Destroy(ktxTexture(texture));
    return file;
}

} // namespace

TEST_CASE("is_ktx2 recognizes a KTX2 file by its first bytes") {
    CHECK(is_ktx2(make_ktx2(4, 4, false, false)));
    const std::vector<std::byte> png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                                     std::byte{'G'}};
    CHECK_FALSE(is_ktx2(png));
    CHECK_FALSE(is_ktx2({}));
}

TEST_CASE("cooked_texture_path swaps the extension for .ktx2") {
    CHECK(cooked_texture_path("assets:/models/textures/rock.png") ==
          "assets:/models/textures/rock.ktx2");
    CHECK(cooked_texture_path("assets:/a.b/rock") == "assets:/a.b/rock.ktx2");
    CHECK(cooked_texture_path("rock.jpg") == "rock.ktx2");
}

TEST_CASE("load_ktx2 reads plain RGBA8, and lets the device make mipmaps for one level") {
    FakeDeviceState state;
    const FakeDevice device(state);
    const core::Result<TextureData> texture = load_ktx2(make_ktx2(6, 2, false, false), device);
    REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
    CHECK(texture.value().desc.format == rhi::TextureFormat::Rgba8Srgb);
    CHECK(texture.value().desc.width == 6);
    CHECK(texture.value().desc.mip_levels == 1);
    CHECK(texture.value().desc.generate_mipmaps);
    REQUIRE(texture.value().pixels.size() == 6 * 2 * 4);
    CHECK(std::to_integer<int>(texture.value().pixels[0]) == 240);
    CHECK(std::to_integer<int>(texture.value().pixels[5 * 4 + 2]) == 230);
}

TEST_CASE("load_ktx2 transcodes UASTC to the best format the device takes") {
    const std::vector<std::byte> file = make_ktx2(8, 8, true, true);
    FakeDeviceState state;

    SUBCASE("BC7 first") {
        state.compressed_formats = {rhi::TextureFormat::Astc4x4Srgb, rhi::TextureFormat::Bc7Srgb};
        const core::Result<TextureData> texture = load_ktx2(file, FakeDevice(state));
        REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
        CHECK(texture.value().desc.format == rhi::TextureFormat::Bc7Srgb);
        CHECK(texture.value().desc.mip_levels == 4);
        CHECK_FALSE(texture.value().desc.generate_mipmaps);
        // 4 blocks, then 1, 1 and 1.
        CHECK(texture.value().pixels.size() == 7 * 16);
    }
    SUBCASE("then ASTC") {
        state.compressed_formats = {rhi::TextureFormat::Astc4x4Srgb};
        const core::Result<TextureData> texture = load_ktx2(file, FakeDevice(state));
        REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
        CHECK(texture.value().desc.format == rhi::TextureFormat::Astc4x4Srgb);
        CHECK(texture.value().pixels.size() == 7 * 16);
    }
    SUBCASE("then RGBA8, close to the original colors") {
        const core::Result<TextureData> texture = load_ktx2(file, FakeDevice(state));
        REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
        CHECK(texture.value().desc.format == rhi::TextureFormat::Rgba8Srgb);
        CHECK(texture.value().desc.mip_levels == 4);
        // 8x8, 4x4, 2x2 and 1x1.
        REQUIRE(texture.value().pixels.size() == (64 + 16 + 4 + 1) * 4);
        const auto near = [&](std::size_t byte, int expected) {
            return std::abs(std::to_integer<int>(texture.value().pixels[byte]) - expected) <= 8;
        };
        CHECK(near(0, 240));
        CHECK(near(1, 140));
        CHECK(near(2, 20));
        CHECK(near(7 * 4 + 0, 60));
        CHECK(near(7 * 4 + 2, 230));
    }
}

TEST_CASE("load_ktx2 keeps sizes that aren't whole blocks uncompressed") {
    FakeDeviceState state;
    state.compressed_formats = {rhi::TextureFormat::Bc7Srgb};
    const core::Result<TextureData> texture =
        load_ktx2(make_ktx2(6, 6, true, false), FakeDevice(state));
    REQUIRE_MESSAGE(texture.has_value(), texture.error().message);
    CHECK(texture.value().desc.format == rhi::TextureFormat::Rgba8Srgb);
    CHECK(texture.value().pixels.size() == 6 * 6 * 4);
}

TEST_CASE("load_ktx2 rejects files that aren't KTX2 textures") {
    FakeDeviceState state;
    const FakeDevice device(state);
    CHECK_FALSE(load_ktx2({}, device).has_value());
    std::vector<std::byte> broken = make_ktx2(4, 4, true, false);
    broken.resize(40);
    CHECK_FALSE(load_ktx2(broken, device).has_value());
}
