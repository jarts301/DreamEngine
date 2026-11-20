#include "dream/runtime/environment.h"

#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "fake_device.h"
#include "fake_platform.h"
#include "fake_renderer.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using doctest::Approx;

namespace {

// A Radiance file of `width` x `height` pixels, all one RGBE value, flat or run-length encoded.
std::vector<std::byte> hdr_file(int width, int height, std::array<std::uint8_t, 4> rgbe,
                                bool encoded) {
    std::string text = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) +
                       " +X " + std::to_string(width) + "\n";
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    for (int y = 0; y < height; ++y) {
        if (encoded) {
            for (const std::uint8_t b : {std::uint8_t{2}, std::uint8_t{2},
                                         static_cast<std::uint8_t>(width >> 8),
                                         static_cast<std::uint8_t>(width & 0xff)}) {
                bytes.push_back(static_cast<std::byte>(b));
            }
            // Each component as one run, in pieces of at most 127.
            for (const std::uint8_t value : rgbe) {
                for (int left = width; left > 0;) {
                    const int run = std::min(left, 127);
                    bytes.push_back(static_cast<std::byte>(128 + run));
                    bytes.push_back(static_cast<std::byte>(value));
                    left -= run;
                }
            }
        } else {
            for (int x = 0; x < width; ++x) {
                for (const std::uint8_t value : rgbe) {
                    bytes.push_back(static_cast<std::byte>(value));
                }
            }
        }
    }
    return bytes;
}

float from_half(std::uint16_t half) {
    const std::uint32_t sign = (half & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1fu;
    const std::uint32_t mantissa = half & 0x3ffu;
    if (exponent == 0) {
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    }
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

} // namespace

TEST_CASE("Radiance HDR files are read flat and run-length encoded") {
    // RGBE (128, 64, 32, 129): 0.5, 0.25, 0.125, scaled by 2.
    for (const bool encoded : {false, true}) {
        const auto file = hdr_file(16, 3, {128, 64, 32, 129}, encoded);
        const core::Result<HdrImage> image = decode_hdr(file);
        REQUIRE(image.has_value());
        CHECK(image.value().width == 16);
        CHECK(image.value().height == 3);
        CHECK(image.value().pixels[0] == Approx(1.0f).epsilon(0.01));
        CHECK(image.value().pixels[1] == Approx(0.5f).epsilon(0.01));
        CHECK(image.value().pixels.back() == Approx(0.25f).epsilon(0.02));
    }
    const std::string png = "\x89PNG";
    std::vector<std::byte> not_hdr(png.size());
    std::memcpy(not_hdr.data(), png.data(), png.size());
    CHECK_FALSE(decode_hdr(not_hdr).has_value());
    auto short_file = hdr_file(16, 3, {128, 64, 32, 129}, false);
    short_file.resize(short_file.size() - 10);
    CHECK_FALSE(decode_hdr(short_file).has_value());
}

TEST_CASE("An even environment gives the same light at every level, matte included") {
    HdrImage image{64, 32, std::vector<float>(64 * 32 * 3)};
    for (std::size_t index = 0; index < image.pixels.size(); index += 3) {
        image.pixels[index] = 0.8f;
        image.pixels[index + 1] = 0.4f;
        image.pixels[index + 2] = 0.2f;
    }
    const TextureData data = prefilter_environment(image);
    CHECK(data.desc.format == rhi::TextureFormat::Rgba16Float);
    CHECK(data.desc.width == 256);
    CHECK(data.desc.height == 128);
    REQUIRE(data.desc.mip_levels == 6);
    std::size_t texels = 0;
    for (std::uint32_t level = 0; level < 6; ++level) {
        texels += std::size_t{256u >> level} * (128u >> level);
    }
    REQUIRE(data.pixels.size() == texels * 8);
    // A texel from each level, and the last, matte, level, which integrates the whole sphere.
    std::size_t offset = 0;
    for (std::uint32_t level = 0; level < 6; ++level) {
        const std::size_t width = 256u >> level;
        const std::size_t height = 128u >> level;
        const std::size_t middle = offset + ((height / 2) * width + width / 3) * 8;
        std::array<std::uint16_t, 4> half{};
        std::memcpy(half.data(), data.pixels.data() + middle, 8);
        CAPTURE(level);
        CHECK(from_half(half[0]) == Approx(0.8f).epsilon(0.03));
        CHECK(from_half(half[1]) == Approx(0.4f).epsilon(0.03));
        CHECK(from_half(half[3]) == Approx(1.0f));
        offset += width * height * 8;
    }
}

TEST_CASE("Rough levels of an environment blur a bright spot") {
    // A small bright patch at the top middle of an otherwise dark image.
    HdrImage image{128, 64, std::vector<float>(128 * 64 * 3, 0.0f)};
    for (std::uint32_t y = 20; y < 24; ++y) {
        for (std::uint32_t x = 62; x < 66; ++x) {
            for (std::uint32_t c = 0; c < 3; ++c) {
                image.pixels[(y * 128 + x) * 3 + c] = 50.0f;
            }
        }
    }
    const TextureData data = prefilter_environment(image);
    const auto texel = [&](std::uint32_t level, std::uint32_t x, std::uint32_t y) {
        std::size_t offset = 0;
        for (std::uint32_t l = 0; l < level; ++l) {
            offset += std::size_t{256u >> l} * (128u >> l) * 8;
        }
        std::array<std::uint16_t, 4> half{};
        std::memcpy(half.data(), data.pixels.data() + offset + (y * (256u >> level) + x) * 8, 8);
        return from_half(half[0]);
    };
    // Sharp at level 0: bright on the patch, dark beside it. Spread at level 3: lit beside it too.
    CHECK(texel(0, 128, 43) > 10.0f);
    CHECK(texel(0, 150, 43) == Approx(0.0f));
    CHECK(texel(3, 32 * 150 / 256, 16 * 43 / 128) > 0.01f);
}
