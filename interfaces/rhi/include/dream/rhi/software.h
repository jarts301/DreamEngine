#pragma once

#include "dream/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dream::rhi {

// An optional capability of a device that draws on the CPU, for machines with no GPU (ADR 0039).
// Such a device can't run shaders, so a renderer made for it, such as the software renderer, reads
// the contents of buffers and textures itself and writes the frame's pixels directly. Get it from
// Device::software_access.

// A texture's contents: every level, the full size first, as Device::create_texture takes them.
// Mipmaps the device made itself are included, and `desc.mip_levels` counts them. Render targets
// hold zeros. Valid until the texture is destroyed or, for a dynamic one, updated.
struct SoftwareTexture {
    TextureDesc desc;
    std::span<const std::byte> pixels;
};

// The window's pixels for the frame being recorded: width * height pixels of four bytes, red,
// green, blue and alpha, sRGB-encoded as the screen shows them, row by row from the top. What is
// in them when the frame ends is presented.
struct SoftwareTarget {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<std::byte> pixels;
};

class SoftwareAccess {
public:
    // Empty for a buffer that doesn't exist.
    [[nodiscard]] virtual std::span<const std::byte> buffer_contents(BufferHandle buffer) const = 0;

    // Nothing for a texture that doesn't exist.
    [[nodiscard]] virtual std::optional<SoftwareTexture>
    texture_contents(TextureHandle texture) const = 0;

    // Only between Device::begin_frame and end_frame.
    [[nodiscard]] virtual SoftwareTarget window_target() = 0;

protected:
    ~SoftwareAccess() = default;
};

} // namespace dream::rhi
