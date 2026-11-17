#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"
#include "dream/runtime/model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dream::runtime {

// Screenshot tests: the App captures one frame, saves it, and compares it with a reference image,
// failing the run if they differ by more than the tolerance allows. See docs/adr/0021.
//
//     [screenshot]
//     frame = 60                    # The frame to capture, counting from 0.
//     save = "screenshot.png"       # Where the capture is written, relative to the working
//                                   # directory. This is the default.
//     reference = "assets:/screenshots/title.png"   # Optional. Without it, the capture is
//                                                   # only saved.
//     tolerance = 16                # How far a pixel's color can be off, 0 to 255, per channel.
//     max_differing = 0.005         # The share of pixels that can be off by more.
//     quit = true                   # End the game once the frame is checked. The default.
//
// While [screenshot] is set, each frame advances the game by exactly 1 / fixed_rate seconds,
// whatever the clock says, so a frame shows the same moment on every machine.
//
// On Android, where the screen sets the size, a capture of another size than the reference is
// saved but not compared.
struct ScreenshotSettings {
    std::uint64_t frame = 60;
    std::string save = "screenshot.png";
    std::string reference;
    std::uint32_t tolerance = 16;
    double max_differing = 0.005;
    bool quit = true;

    // Nothing when the configuration has no [screenshot] table.
    [[nodiscard]] static core::Result<std::optional<ScreenshotSettings>>
    from_config(const core::Config& config);
};

struct ImageDifference {
    // Pixels with any channel further off than the tolerance.
    std::size_t differing_pixels = 0;
    std::size_t total_pixels = 0;
    // The furthest any channel was off, 0 to 255.
    std::uint32_t largest = 0;

    [[nodiscard]] double differing_share() const noexcept {
        return total_pixels == 0 ? 0.0
                                 : static_cast<double>(differing_pixels) /
                                       static_cast<double>(total_pixels);
    }
};

// Compares two images of four bytes per pixel, ignoring alpha. Fails if their sizes differ.
[[nodiscard]] core::Result<ImageDifference> compare_images(const ImageData& actual,
                                                           const ImageData& expected,
                                                           std::uint32_t tolerance);

// Encodes four bytes per pixel, row by row from the top, as a PNG file.
[[nodiscard]] std::vector<std::byte> encode_png(const ImageData& image);

} // namespace dream::runtime
