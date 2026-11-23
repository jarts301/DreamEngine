// A decoder that reads nothing, for projects that cook no textures: their games load PNG files
// and link no KTX library.

#include "dream/core/module_registry.h"
#include "dream/texture_decoder/texture_decoder.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace texture_decoder;

class NullDecoder final : public TextureDecoder {
public:
    core::Result<Decoded> decode(std::span<const std::byte> /*file*/,
                                 const rhi::Device& /*device*/) override {
        return core::Error{core::ErrorCode::NotFound,
                           "Cooked textures can't be read: the project lists the \"null\" "
                           "texture_decoder; list \"ktx\" for the platform, or cook no textures"};
    }
    rhi::TextureFormat color_format(const rhi::Device& /*device*/) const override {
        return rhi::TextureFormat::Rgba8Srgb;
    }
};

core::Result<std::unique_ptr<TextureDecoder>> create(const core::Config& /*config*/) {
    return std::unique_ptr<TextureDecoder>{std::make_unique<NullDecoder>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_texture_decoder_null(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<TextureDecoder>{.name = "null", .create = &create});
}

} // namespace dream::modules
