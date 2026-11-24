#include "dream/runtime/environment.h"

#include "dream/core/math/vector.h"
#include "dream/core/profile.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <charconv>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace dream::runtime {
namespace {

using core::Vec3;

constexpr float k_pi = 3.14159265358979f;
constexpr std::uint32_t k_base_width = 256;
constexpr std::uint32_t k_base_height = 128;
// Levels 0 to 4 for roughness 0 to 1, and level 5 for matte surfaces.
constexpr std::uint32_t k_specular_levels = 5;
constexpr std::uint32_t k_levels = k_specular_levels + 1;
constexpr std::uint32_t k_samples = 64;

core::Error hdr_error(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Not a Radiance HDR image: {}", message)};
}

// One RGBE pixel as floats.
void rgbe_to_float(const std::uint8_t* rgbe, float* rgb) {
    if (rgbe[3] == 0) {
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        return;
    }
    const float scale = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8));
    rgb[0] = (static_cast<float>(rgbe[0]) + 0.5f) * scale;
    rgb[1] = (static_cast<float>(rgbe[1]) + 0.5f) * scale;
    rgb[2] = (static_cast<float>(rgbe[2]) + 0.5f) * scale;
}

// A float as an IEEE half, rounded to nearest, saturating at the largest finite value.
std::uint16_t to_half(float value) {
    value = std::clamp(value, -65504.0f, 65504.0f);
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<std::uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    // Round to nearest, which may carry into the exponent, as it should.
    if ((mantissa & 0x1fffu) > 0x1000u || ((mantissa & 0x1fffu) == 0x1000u && (half & 1u))) {
        ++half;
    }
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(half, sign | 0x7bffu));
}

// The direction at the middle of a texel of an equirectangular image, as environment.slang maps
// directions to images.
Vec3 texel_direction(float u, float v) {
    const float phi = (u - 0.5f) * 2.0f * k_pi;
    const float theta = v * k_pi;
    return Vec3{std::sin(theta) * std::sin(phi), std::cos(theta),
                -std::sin(theta) * std::cos(phi)};
}

std::array<float, 2> direction_uv(Vec3 d) {
    const float u = std::atan2(d.x, -d.z) / (2.0f * k_pi) + 0.5f;
    const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / k_pi;
    return {u, v};
}

// An RGB float image, for working on.
struct Plane {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Vec3> texels;

    [[nodiscard]] Vec3 at(std::int64_t x, std::int64_t y) const {
        const auto w = static_cast<std::int64_t>(width);
        const auto h = static_cast<std::int64_t>(height);
        x = ((x % w) + w) % w; // Wraps around across.
        y = std::clamp<std::int64_t>(y, 0, h - 1);
        return texels[static_cast<std::size_t>(y * w + x)];
    }

    // Bilinear, wrapping across and clamped at the poles.
    [[nodiscard]] Vec3 sample(float u, float v) const {
        const float x = u * static_cast<float>(width) - 0.5f;
        const float y = v * static_cast<float>(height) - 0.5f;
        const auto x0 = static_cast<std::int64_t>(std::floor(x));
        const auto y0 = static_cast<std::int64_t>(std::floor(y));
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const Vec3 top = core::lerp(at(x0, y0), at(x0 + 1, y0), fx);
        const Vec3 bottom = core::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx);
        return core::lerp(top, bottom, fy);
    }
};

// The image at the base size: each base texel the average of the source pixels over it, or the
// source sampled there when it is smaller.
Plane resample(const HdrImage& image) {
    Plane source{image.width, image.height, {}};
    source.texels.resize(std::size_t{image.width} * image.height);
    for (std::size_t index = 0; index < source.texels.size(); ++index) {
        source.texels[index] = Vec3{image.pixels[index * 3], image.pixels[index * 3 + 1],
                                    image.pixels[index * 3 + 2]};
    }
    Plane base{k_base_width, k_base_height, std::vector<Vec3>(std::size_t{k_base_width} *
                                                              k_base_height)};
    const std::uint32_t step_x = std::max(1u, image.width / k_base_width);
    const std::uint32_t step_y = std::max(1u, image.height / k_base_height);
    for (std::uint32_t y = 0; y < k_base_height; ++y) {
        for (std::uint32_t x = 0; x < k_base_width; ++x) {
            Vec3 sum{};
            for (std::uint32_t j = 0; j < step_y; ++j) {
                for (std::uint32_t i = 0; i < step_x; ++i) {
                    const float u = (static_cast<float>(x) +
                                     (static_cast<float>(i) + 0.5f) / static_cast<float>(step_x)) /
                                    static_cast<float>(k_base_width);
                    const float v = (static_cast<float>(y) +
                                     (static_cast<float>(j) + 0.5f) / static_cast<float>(step_y)) /
                                    static_cast<float>(k_base_height);
                    sum = sum + source.sample(u, v);
                }
            }
            base.texels[std::size_t{y} * k_base_width + x] =
                sum / static_cast<float>(step_x * step_y);
        }
    }
    return base;
}

Plane halve(const Plane& plane) {
    Plane half{std::max(1u, plane.width / 2), std::max(1u, plane.height / 2), {}};
    half.texels.resize(std::size_t{half.width} * half.height);
    for (std::uint32_t y = 0; y < half.height; ++y) {
        for (std::uint32_t x = 0; x < half.width; ++x) {
            const auto sx = static_cast<std::int64_t>(x) * 2;
            const auto sy = static_cast<std::int64_t>(y) * 2;
            half.texels[std::size_t{y} * half.width + x] =
                (plane.at(sx, sy) + plane.at(sx + 1, sy) + plane.at(sx, sy + 1) +
                 plane.at(sx + 1, sy + 1)) *
                0.25f;
        }
    }
    return half;
}

// The pyramid sampled between its levels, for filtered importance sampling.
Vec3 sample_pyramid(const std::vector<Plane>& pyramid, float u, float v, float level) {
    level = std::clamp(level, 0.0f, static_cast<float>(pyramid.size() - 1));
    const auto low = static_cast<std::size_t>(level);
    const std::size_t high = std::min(low + 1, pyramid.size() - 1);
    const float t = level - static_cast<float>(low);
    return core::lerp(pyramid[low].sample(u, v), pyramid[high].sample(u, v), t);
}

// Low-discrepancy points on the unit square.
std::array<float, 2> hammersley(std::uint32_t index, std::uint32_t count) {
    std::uint32_t bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return {static_cast<float>(index) / static_cast<float>(count),
            static_cast<float>(bits) * 2.3283064365386963e-10f};
}

// The image as a surface of `roughness` reflects it, looking straight at it, by GGX importance
// sampling, each sample reading the pyramid at the level that covers its share of the sphere.
Plane prefilter_level(const std::vector<Plane>& pyramid, float roughness, std::uint32_t width,
                      std::uint32_t height) {
    Plane out{width, height, std::vector<Vec3>(std::size_t{width} * height)};
    const float alpha = roughness * roughness;
    const float texel_solid_angle =
        4.0f * k_pi / static_cast<float>(pyramid[0].width * pyramid[0].height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Vec3 normal = texel_direction((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                                                (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
            const Vec3 up = std::abs(normal.y) < 0.999f ? Vec3{0.0f, 1.0f, 0.0f}
                                                        : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 tangent = core::normalized(core::cross(up, normal));
            const Vec3 bitangent = core::cross(normal, tangent);
            Vec3 sum{};
            float total = 0.0f;
            for (std::uint32_t index = 0; index < k_samples; ++index) {
                const auto [e1, e2] = hammersley(index, k_samples);
                const float phi = 2.0f * k_pi * e1;
                const float cos_theta =
                    std::sqrt((1.0f - e2) / (1.0f + (alpha * alpha - 1.0f) * e2));
                const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
                const Vec3 half = core::normalized(
                    tangent * (sin_theta * std::cos(phi)) + bitangent * (sin_theta * std::sin(phi)) +
                    normal * cos_theta);
                const float n_dot_h = core::dot(normal, half);
                const Vec3 light = half * (2.0f * n_dot_h) - normal;
                const float n_dot_l = core::dot(normal, light);
                if (n_dot_l <= 0.0f) {
                    continue;
                }
                // The sample's share of the sphere, from GGX's density with view = normal.
                const float a2 = alpha * alpha;
                const float denominator = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
                const float distribution = a2 / (k_pi * denominator * denominator);
                const float pdf = distribution * 0.25f;
                const float sample_solid_angle =
                    1.0f / (static_cast<float>(k_samples) * std::max(pdf, 1e-6f));
                const float level =
                    0.5f * std::log2(sample_solid_angle / texel_solid_angle) + 1.0f;
                const auto [u, v] = direction_uv(light);
                sum = sum + sample_pyramid(pyramid, u, v, level) * n_dot_l;
                total += n_dot_l;
            }
            out.texels[std::size_t{y} * width + x] = total > 0.0f ? sum / total : Vec3{};
        }
    }
    return out;
}

// What a matte surface facing each texel's direction receives, cosine-weighted, over pi: the
// average of a small version of the image, each texel weighted by its share of the sphere.
Plane irradiance_level(const Plane& small, std::uint32_t width, std::uint32_t height) {
    Plane out{width, height, std::vector<Vec3>(std::size_t{width} * height)};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Vec3 normal = texel_direction((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                                                (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
            Vec3 sum{};
            for (std::uint32_t sy = 0; sy < small.height; ++sy) {
                const float v = (static_cast<float>(sy) + 0.5f) / static_cast<float>(small.height);
                const float solid_angle = (2.0f * k_pi / static_cast<float>(small.width)) *
                                          (k_pi / static_cast<float>(small.height)) *
                                          std::sin(v * k_pi);
                for (std::uint32_t sx = 0; sx < small.width; ++sx) {
                    const Vec3 direction = texel_direction(
                        (static_cast<float>(sx) + 0.5f) / static_cast<float>(small.width), v);
                    const float cosine = core::dot(normal, direction);
                    if (cosine > 0.0f) {
                        sum = sum + small.texels[std::size_t{sy} * small.width + sx] *
                                        (cosine * solid_angle);
                    }
                }
            }
            out.texels[std::size_t{y} * width + x] = sum / k_pi;
        }
    }
    return out;
}

void append_level(const Plane& plane, std::vector<std::byte>& bytes) {
    const std::size_t start = bytes.size();
    bytes.resize(start + plane.texels.size() * 8);
    std::byte* out = bytes.data() + start;
    for (const Vec3& texel : plane.texels) {
        const std::array<std::uint16_t, 4> half{to_half(texel.x), to_half(texel.y),
                                                to_half(texel.z), to_half(1.0f)};
        std::memcpy(out, half.data(), 8);
        out += 8;
    }
}

core::Result<void> read_environment(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (auto known = data.check_keys({"map", "intensity", "rotation", "background"});
        !known.has_value()) {
        return known;
    }
    EnvironmentLight light;
    const core::Result<std::string> map = data.text("map");
    const core::Result<float> intensity = data.number("intensity", light.intensity);
    const core::Result<float> rotation = data.number("rotation", light.rotation_degrees);
    const core::Result<bool> background = data.flag("background", light.background);
    for (const core::Error* error : {map.has_value() ? nullptr : &map.error(),
                                     intensity.has_value() ? nullptr : &intensity.error(),
                                     rotation.has_value() ? nullptr : &rotation.error(),
                                     background.has_value() ? nullptr : &background.error()}) {
        if (error != nullptr) {
            return *error;
        }
    }
    if (!(intensity.value() >= 0.0f)) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "An environment's intensity must be 0 or more"};
    }
    core::Result<AssetRef<EnvironmentAsset>> loaded =
        context.assets.load<EnvironmentAsset>(map.value());
    if (!loaded.has_value()) {
        return core::Error{loaded.error().code,
                           std::format("Environment '{}': {}", map.value(), loaded.error().message)};
    }
    light.map = std::move(loaded).value();
    light.intensity = intensity.value();
    light.rotation_degrees = rotation.value();
    light.background = background.value();
    context.world.registry().emplace_or_replace<EnvironmentLight>(context.entity,
                                                                  std::move(light));
    return {};
}

} // namespace

core::Result<HdrImage> decode_hdr(std::span<const std::byte> file) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(file.data());
    const std::size_t size = file.size();
    std::size_t at = 0;
    const auto read_line = [&]() -> std::string_view {
        const std::size_t start = at;
        while (at < size && bytes[at] != '\n') {
            ++at;
        }
        const std::string_view line(reinterpret_cast<const char*>(bytes + start), at - start);
        if (at < size) {
            ++at;
        }
        return line;
    };
    const std::string_view magic = read_line();
    if (!magic.starts_with("#?")) {
        return hdr_error("it doesn't start with #?");
    }
    bool rgbe = false;
    for (std::string_view line = read_line(); !line.empty() && at < size; line = read_line()) {
        if (line == "FORMAT=32-bit_rle_rgbe") {
            rgbe = true;
        } else if (line.starts_with("FORMAT=")) {
            return hdr_error(std::format("it is {}, not 32-bit_rle_rgbe", line.substr(7)));
        }
    }
    (void)rgbe;
    // "-Y <height> +X <width>": stored top to bottom, left to right, the usual layout.
    std::string_view resolution = read_line();
    int height = 0;
    int width = 0;
    const auto number = [&](std::string_view prefix, int& out) {
        if (!resolution.starts_with(prefix)) {
            return false;
        }
        resolution.remove_prefix(prefix.size());
        const auto [end, error] =
            std::from_chars(resolution.data(), resolution.data() + resolution.size(), out);
        resolution.remove_prefix(static_cast<std::size_t>(end - resolution.data()));
        return error == std::errc{};
    };
    if (!number("-Y ", height) || !number(" +X ", width) || width <= 0 || height <= 0 ||
        width > 32768 || height > 32768) {
        return hdr_error("only images stored top to bottom, as -Y h +X w, are read");
    }
    HdrImage image{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 3);
    std::vector<std::uint8_t> scanline(std::size_t{image.width} * 4);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const bool encoded = image.width >= 8 && image.width < 32768 && at + 4 <= size &&
                             bytes[at] == 2 && bytes[at + 1] == 2 && (bytes[at + 2] & 0x80) == 0;
        if (encoded) {
            if (((static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3]) != image.width) {
                return hdr_error("a scanline's length is wrong");
            }
            at += 4;
            // Each of the four components in turn, as runs and literal spans.
            for (std::uint32_t component = 0; component < 4; ++component) {
                std::uint32_t x = 0;
                while (x < image.width) {
                    if (at >= size) {
                        return hdr_error("it ends early");
                    }
                    std::uint32_t count = bytes[at++];
                    if (count > 128) {
                        count -= 128;
                        if (at >= size || x + count > image.width) {
                            return hdr_error("a run goes past its scanline");
                        }
                        const std::uint8_t value = bytes[at++];
                        for (std::uint32_t i = 0; i < count; ++i) {
                            scanline[(x++) * 4 + component] = value;
                        }
                    } else {
                        if (count == 0 || at + count > size || x + count > image.width) {
                            return hdr_error("a span goes past its scanline");
                        }
                        for (std::uint32_t i = 0; i < count; ++i) {
                            scanline[(x++) * 4 + component] = bytes[at++];
                        }
                    }
                }
            }
        } else {
            if (at + scanline.size() > size) {
                return hdr_error("it ends early");
            }
            std::memcpy(scanline.data(), bytes + at, scanline.size());
            at += scanline.size();
        }
        for (std::uint32_t x = 0; x < image.width; ++x) {
            rgbe_to_float(&scanline[std::size_t{x} * 4],
                          &image.pixels[(std::size_t{y} * image.width + x) * 3]);
        }
    }
    return image;
}

TextureData prefilter_environment(const HdrImage& image) {
    DREAM_PROFILE_ZONE("Prefilter environment");
    std::vector<Plane> pyramid{resample(image)};
    while (pyramid.back().width > 1 && pyramid.back().height > 1) {
        pyramid.push_back(halve(pyramid.back()));
    }
    TextureData data;
    data.desc = rhi::TextureDesc{.width = k_base_width,
                                 .height = k_base_height,
                                 .format = rhi::TextureFormat::Rgba16Float,
                                 .mip_levels = k_levels,
                                 .generate_mipmaps = false};
    append_level(pyramid[0], data.pixels);
    for (std::uint32_t level = 1; level < k_specular_levels; ++level) {
        const float roughness =
            static_cast<float>(level) / static_cast<float>(k_specular_levels - 1);
        append_level(prefilter_level(pyramid, roughness, k_base_width >> level,
                                     k_base_height >> level),
                     data.pixels);
    }
    // The matte level from the 32 x 16 version of the image.
    const auto small = std::ranges::find_if(pyramid, [](const Plane& plane) {
        return plane.width <= 32;
    });
    append_level(irradiance_level(small != pyramid.end() ? *small : pyramid.back(),
                                  k_base_width >> k_specular_levels,
                                  k_base_height >> k_specular_levels),
                 data.pixels);
    return data;
}

void register_environment(SceneLoader& scenes, AssetManager& assets, rhi::Device& device) {
    rhi::Device* const target = &device;
    assets.register_loader<EnvironmentAsset>(
        [target](AssetLoadContext& context) -> core::Result<EnvironmentAsset> {
            const auto file = context.files.read(context.path);
            if (!file.has_value()) {
                return file.error();
            }
            core::Result<HdrImage> image = decode_hdr(file.value());
            if (!image.has_value()) {
                return image.error();
            }
            const TextureData data = prefilter_environment(image.value());
            const core::Result<rhi::TextureHandle> texture =
                target->create_texture(data.desc, data.pixels);
            if (!texture.has_value()) {
                return texture.error();
            }
            return EnvironmentAsset{.radiance = GpuTexture(*target, texture.value()),
                                    .levels = data.desc.mip_levels};
        });
    scenes.register_component(
        "environment", &read_environment,
        {core::FieldInfo{.name = "map",
                         .kind = core::FieldKind::Asset,
                         .fallback = std::string(),
                         .help = "An .hdr image of the sky all around"},
         core::FieldInfo{.name = "intensity",
                         .kind = core::FieldKind::Number,
                         .fallback = 1.0,
                         .minimum = 0.0f,
                         .maximum = 5.0f},
         core::FieldInfo{.name = "rotation",
                         .kind = core::FieldKind::Number,
                         .fallback = 0.0,
                         .minimum = -180.0f,
                         .maximum = 180.0f,
                         .help = "Degrees around the up axis"},
         core::FieldInfo{.name = "background", .kind = core::FieldKind::Flag, .fallback = true}});
}

} // namespace dream::runtime
