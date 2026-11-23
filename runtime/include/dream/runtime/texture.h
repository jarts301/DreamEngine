#pragma once

#include "dream/core/result.h"
#include "dream/rhi/rhi.h"
#include "dream/texture_decoder/texture_decoder.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// A texture as create_texture takes it: every level, largest first, in `desc.format`.
using TextureData = texture_decoder::Decoded;

// Whether a file is a KTX2 texture, by its first bytes.
[[nodiscard]] bool is_ktx2(std::span<const std::byte> file) noexcept;

// A cooked texture, read with the project's texture decoder. Fails, naming `path`, when the
// project lists none for the platform.
[[nodiscard]] core::Result<TextureData>
decode_cooked_texture(texture_decoder::TextureDecoder* decoder, std::span<const std::byte> file,
                      const rhi::Device& device, std::string_view path);

// A format's name, for messages.
[[nodiscard]] std::string_view texture_format_name(rhi::TextureFormat format) noexcept;

// The cooked name of a texture file: "textures/rock.png" becomes "textures/rock.ktx2".
[[nodiscard]] std::string cooked_texture_path(std::string_view path);

} // namespace dream::runtime
