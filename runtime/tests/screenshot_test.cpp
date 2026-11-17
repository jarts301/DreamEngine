#include "dream/runtime/screenshot.h"

#include "dream/runtime/model.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <ostream> // Needed for doctest to print std::string.
#include <string>

using namespace dream;
using namespace dream::runtime;

namespace {

ImageData filled(std::uint32_t width, std::uint32_t height, std::byte red) {
    ImageData image{.width = width, .height = height};
    image.pixels.resize(std::size_t{width} * height * 4, std::byte{40});
    for (std::size_t pixel = 0; pixel < std::size_t{width} * height; ++pixel) {
        image.pixels[pixel * 4] = red;
        image.pixels[pixel * 4 + 3] = std::byte{255};
    }
    return image;
}

std::optional<ScreenshotSettings> parse(const std::string& text) {
    const core::Config config = core::Config::parse(text).value();
    core::Result<std::optional<ScreenshotSettings>> settings =
        ScreenshotSettings::from_config(config);
    REQUIRE(settings.has_value());
    return settings.value();
}

} // namespace

TEST_CASE("Screenshot settings are read from [screenshot], with defaults") {
    CHECK_FALSE(parse("[app]\nwidth = 10\n").has_value());

    const std::optional<ScreenshotSettings> defaults = parse("[screenshot]\n");
    REQUIRE(defaults.has_value());
    CHECK(defaults->frame == 60);
    CHECK(defaults->save == "screenshot.png");
    CHECK(defaults->reference.empty());
    CHECK(defaults->tolerance == 16);
    CHECK(defaults->quit);

    const std::optional<ScreenshotSettings> set =
        parse("[screenshot]\nframe = 5\nsave = \"a.png\"\nreference = \"assets:/b.png\"\n"
              "tolerance = 3\nmax_differing = 0.25\nquit = false\n");
    REQUIRE(set.has_value());
    CHECK(set->frame == 5);
    CHECK(set->save == "a.png");
    CHECK(set->reference == "assets:/b.png");
    CHECK(set->tolerance == 3);
    CHECK(set->max_differing == doctest::Approx(0.25));
    CHECK_FALSE(set->quit);

    for (const char* wrong : {"[screenshot]\nframe = -1\n", "[screenshot]\ntolerance = 300\n",
                              "[screenshot]\nmax_differing = 2.0\n", "[screenshot]\nsave = \"\"\n",
                              "[screenshot]\nreference = 3\n", "[screenshot]\nquit = 1\n"}) {
        const core::Config config = core::Config::parse(wrong).value();
        CHECK_FALSE(ScreenshotSettings::from_config(config).has_value());
    }
}

TEST_CASE("compare_images counts pixels off by more than the tolerance, ignoring alpha") {
    const ImageData base = filled(4, 2, std::byte{100});
    ImageData other = base;
    other.pixels[0] = std::byte{110};  // Within a tolerance of 16.
    other.pixels[4] = std::byte{140};  // Beyond it.
    other.pixels[11] = std::byte{0};   // Alpha only.

    const core::Result<ImageDifference> difference = compare_images(other, base, 16);
    REQUIRE(difference.has_value());
    CHECK(difference.value().total_pixels == 8);
    CHECK(difference.value().differing_pixels == 1);
    CHECK(difference.value().largest == 40);
    CHECK(difference.value().differing_share() == doctest::Approx(0.125));

    CHECK_FALSE(compare_images(filled(4, 2, std::byte{0}), filled(2, 4, std::byte{0}), 0)
                    .has_value());
}

TEST_CASE("encode_png writes an image that decode_image reads back the same") {
    ImageData image = filled(3, 2, std::byte{200});
    image.pixels[5] = std::byte{7};
    const core::Result<ImageData> decoded = decode_image(encode_png(image));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().width == 3);
    CHECK(decoded.value().height == 2);
    CHECK(decoded.value().pixels == image.pixels);
}
