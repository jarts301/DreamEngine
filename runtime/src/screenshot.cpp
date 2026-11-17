#include "dream/runtime/screenshot.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <limits>
#include <string_view>
#include <utility>

namespace dream::runtime {
namespace {

core::Error setting_error(std::string_view key, std::string_view problem) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Setting 'screenshot.{}' {}", key, problem)};
}

} // namespace

core::Result<std::optional<ScreenshotSettings>>
ScreenshotSettings::from_config(const core::Config& config) {
    const std::vector<std::string_view> tables = config.tables_in("");
    if (std::ranges::find(tables, std::string_view("screenshot")) == tables.end()) {
        return std::optional<ScreenshotSettings>{};
    }
    ScreenshotSettings settings;
    if (config.contains("screenshot.frame")) {
        const std::optional<std::int64_t> frame = config.get_int("screenshot.frame");
        if (!frame.has_value() || *frame < 0) {
            return setting_error("frame", "must be a whole number from 0");
        }
        settings.frame = static_cast<std::uint64_t>(*frame);
    }
    for (auto [key, target] : {std::pair{"save", &settings.save},
                               std::pair{"reference", &settings.reference}}) {
        const std::string full = std::format("screenshot.{}", key);
        if (config.contains(full)) {
            const std::optional<std::string_view> text = config.get_string(full);
            if (!text.has_value()) {
                return setting_error(key, "must be a string");
            }
            *target = std::string(*text);
        }
    }
    if (settings.save.empty()) {
        return setting_error("save", "must not be empty");
    }
    if (config.contains("screenshot.tolerance")) {
        const std::optional<std::int64_t> tolerance = config.get_int("screenshot.tolerance");
        if (!tolerance.has_value() || *tolerance < 0 || *tolerance > 255) {
            return setting_error("tolerance", "must be a whole number from 0 to 255");
        }
        settings.tolerance = static_cast<std::uint32_t>(*tolerance);
    }
    if (config.contains("screenshot.max_differing")) {
        const std::optional<double> share = config.get_float("screenshot.max_differing");
        if (!share.has_value() || *share < 0.0 || *share > 1.0) {
            return setting_error("max_differing", "must be a number from 0 to 1");
        }
        settings.max_differing = *share;
    }
    if (config.contains("screenshot.quit")) {
        const std::optional<bool> quit = config.get_bool("screenshot.quit");
        if (!quit.has_value()) {
            return setting_error("quit", "must be true or false");
        }
        settings.quit = *quit;
    }
    return std::optional<ScreenshotSettings>{std::move(settings)};
}

core::Result<ImageDifference> compare_images(const ImageData& actual, const ImageData& expected,
                                             std::uint32_t tolerance) {
    if (actual.width != expected.width || actual.height != expected.height) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("The images' sizes differ: {}x{} against {}x{}",
                                       actual.width, actual.height, expected.width,
                                       expected.height)};
    }
    const std::size_t count = std::size_t{actual.width} * actual.height;
    if (actual.pixels.size() < count * 4 || expected.pixels.size() < count * 4) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "An image has fewer pixels than its size says"};
    }
    ImageDifference difference{.total_pixels = count};
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        std::uint32_t furthest = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int a = std::to_integer<int>(actual.pixels[pixel * 4 + channel]);
            const int b = std::to_integer<int>(expected.pixels[pixel * 4 + channel]);
            furthest = std::max(furthest, static_cast<std::uint32_t>(std::abs(a - b)));
        }
        difference.largest = std::max(difference.largest, furthest);
        if (furthest > tolerance) {
            ++difference.differing_pixels;
        }
    }
    return difference;
}

std::vector<std::byte> encode_png(const ImageData& image) {
    std::vector<std::byte> file;
    const auto append = [](void* context, void* data, int size) {
        auto* const out = static_cast<std::vector<std::byte>*>(context);
        const auto* const bytes = static_cast<const std::byte*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    const int width = static_cast<int>(image.width);
    stbi_write_png_to_func(append, &file, width, static_cast<int>(image.height), 4,
                           image.pixels.data(), width * 4);
    return file;
}

} // namespace dream::runtime
