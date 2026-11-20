#pragma once

#include "dream/core/result.h"
#include "dream/rhi/rhi.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/model.h"
#include "dream/runtime/texture.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dream::runtime {

// Environment lighting (ADR 0040): an HDR image of the scene's surroundings lights every surface
// from each side and is reflected in shiny ones, in place of the sun's flat ambient light.

// An HDR image, red, green and blue floats per pixel, row by row from the top.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> pixels;
};

// Reads a Radiance .hdr file (RGBE), flat or run-length encoded, as image editors and HDRI sites
// save them.
[[nodiscard]] core::Result<HdrImage> decode_hdr(std::span<const std::byte> file);

// The levels the renderer reads, from an equirectangular image: longitude across, latitude down,
// +Y up. Level 0 is the image at 256 x 128; levels 1 to 4 are it as surfaces of roughness 0.25, 0.5,
// 0.75 and 1 reflect it, by GGX; level 5, 8 x 4, is what a matte surface receives from each side,
// cosine-weighted and divided by pi. Rgba16Float, every level, largest first.
[[nodiscard]] TextureData prefilter_environment(const HdrImage& image);

// An environment made ready for the renderer. Load it with
// `app.assets().load<EnvironmentAsset>("assets:/environments/sky.hdr")` after
// Renderer::initialize, which registers its loader.
struct EnvironmentAsset {
    GpuTexture radiance;
    std::uint32_t levels = 0;
};

// Lights the scene from its surroundings. The world's first one is used. Scene files write it as
//
//     [entities.sky.environment]
//     map = "assets:/environments/sky.hdr"
//     intensity = 1.0       # All optional but the map.
//     rotation = 0.0        # Degrees around +Y.
//     background = true     # Drawn behind the scene, in place of the clear color.
struct EnvironmentLight {
    AssetRef<EnvironmentAsset> map;
    float intensity = 1.0f;
    float rotation_degrees = 0.0f;
    bool background = true;
};

class SceneLoader;

// For Renderer: the "environment" component and EnvironmentAsset's loader.
void register_environment(SceneLoader& scenes, AssetManager& assets, rhi::Device& device);

} // namespace dream::runtime
