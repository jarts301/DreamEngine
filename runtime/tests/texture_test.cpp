#include "dream/runtime/texture.h"

#include "fake_device.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using dream::runtime::tests::FakeDevice;
using dream::runtime::tests::FakeDeviceState;

namespace {

// The identifier every KTX2 file starts with, followed by a few bytes of a header.
std::vector<std::byte> ktx2_header() {
    constexpr std::array<unsigned char, 16> bytes{0xAB, 'K',  'T', 'X', ' ', '2', '0', 0xBB,
                                                  '\r', '\n', 0x1A, '\n', 0, 0, 0, 0};
    std::vector<std::byte> file;
    for (const unsigned char byte : bytes) {
        file.push_back(std::byte{byte});
    }
    return file;
}

} // namespace

TEST_CASE("is_ktx2 recognizes a KTX2 file by its first bytes") {
    CHECK(is_ktx2(ktx2_header()));
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

TEST_CASE("load_ktx2 explains that this build reads PNG textures only (ADR 0039)") {
    FakeDeviceState state;
    const FakeDevice device(state);
    const core::Result<TextureData> texture = load_ktx2(ktx2_header(), device);
    REQUIRE_FALSE(texture.has_value());
    CHECK(texture.error().message.find("PNG") != std::string::npos);
    CHECK_FALSE(load_ktx2({}, device).has_value());
}

TEST_CASE("cooked_color_format is plain RGBA8") {
    FakeDeviceState state;
    CHECK(cooked_color_format(FakeDevice(state)) == rhi::TextureFormat::Rgba8Srgb);
}
