// Draws the scene on the CPU, for machines with no GPU (ADR 0039). It needs a device with the
// software capability (rhi/software.h), from which it reads models' buffers and textures, and writes
// the finished frame into the window's pixels.
//
// It draws what the forward renderer draws, with the same lighting model, so a scene looks much the
// same either way: metallic-roughness materials with normal maps, occlusion and emission, the sun
// with a shadow map, point and spot lights, masked and blended surfaces, skinned parts, particles,
// debug lines and the interface's layers, then exposure and Khronos PBR Neutral tone mapping.
//
// Opaque surfaces go through a visibility buffer: each pixel first keeps only the nearest triangle
// and where on it the pixel falls, then each pixel is lit once. The screen is cut into horizontal
// bands, one per thread, so threads never write the same pixel.

#include "dream/core/log.h"
#include "dream/core/math/bounds.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/module_registry.h"
#include "dream/core/profile.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/software.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dream::modules {
namespace {

using namespace renderer;
using core::Mat4;
using core::Vec2;
using core::Vec3;
using core::Vec4;

constexpr std::uint32_t k_no_triangle = std::numeric_limits<std::uint32_t>::max();
constexpr float k_min_roughness = 0.045f;

float saturate(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

Vec3 mul(Vec3 a, Vec3 b) {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// Lookup tables between bytes and linear light.
struct ColorTables {
    std::array<float, 256> srgb_to_linear{};
    std::array<float, 256> unorm{};
    // Linear light from 0 to 1 in k_encode_steps steps, to sRGB bytes.
    static constexpr std::size_t k_encode_steps = 4096;
    std::array<std::uint8_t, k_encode_steps + 1> linear_to_srgb{};

    ColorTables() {
        for (std::size_t value = 0; value < 256; ++value) {
            const float unit = static_cast<float>(value) / 255.0f;
            srgb_to_linear[value] = modules::srgb_to_linear(unit);
            unorm[value] = unit;
        }
        for (std::size_t step = 0; step <= k_encode_steps; ++step) {
            const float linear = static_cast<float>(step) / static_cast<float>(k_encode_steps);
            const float encoded = linear <= 0.0031308f
                                      ? linear * 12.92f
                                      : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            linear_to_srgb[step] = static_cast<std::uint8_t>(saturate(encoded) * 255.0f + 0.5f);
        }
    }

    [[nodiscard]] std::byte encode(float linear) const {
        const auto step = static_cast<std::size_t>(saturate(linear) * k_encode_steps + 0.5f);
        return static_cast<std::byte>(linear_to_srgb[step]);
    }
};

const ColorTables& color_tables() {
    static const ColorTables tables;
    return tables;
}

// Runs a function on every band of the screen, one per thread, and waits for them all.
class BandPool {
public:
    explicit BandPool(std::uint32_t threads) {
        for (std::uint32_t index = 1; index < threads; ++index) {
            m_workers.emplace_back([this, index] { work(index); });
        }
    }

    ~BandPool() {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_all();
        for (std::thread& worker : m_workers) {
            worker.join();
        }
    }

    BandPool(const BandPool&) = delete;
    BandPool& operator=(const BandPool&) = delete;

    [[nodiscard]] std::uint32_t bands() const {
        return static_cast<std::uint32_t>(m_workers.size()) + 1;
    }

    // Calls job(band) for every band from 0 to bands() - 1, this thread taking band 0.
    void run(const std::function<void(std::uint32_t)>& job) {
        if (m_workers.empty()) {
            job(0);
            return;
        }
        {
            const std::lock_guard lock(m_mutex);
            m_job = &job;
            m_pending = static_cast<std::uint32_t>(m_workers.size());
            ++m_generation;
        }
        m_wake.notify_all();
        job(0);
        std::unique_lock lock(m_mutex);
        m_done.wait(lock, [this] { return m_pending == 0; });
        m_job = nullptr;
    }

private:
    void work(std::uint32_t band) {
        std::uint64_t seen = 0;
        while (true) {
            const std::function<void(std::uint32_t)>* job = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] { return m_stopping || m_generation != seen; });
                if (m_stopping) {
                    return;
                }
                seen = m_generation;
                job = m_job;
            }
            (*job)(band);
            {
                const std::lock_guard lock(m_mutex);
                --m_pending;
            }
            m_done.notify_one();
        }
    }

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(std::uint32_t)>* m_job = nullptr;
    std::uint32_t m_pending = 0;
    std::uint64_t m_generation = 0;
    bool m_stopping = false;
};

// A texture as the renderer samples it: every level, from the device's memory.
struct SampledTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    bool srgb = false;
    // Where each level starts in `pixels`.
    std::array<std::size_t, 16> offsets{};
    const std::byte* pixels = nullptr;

    [[nodiscard]] float lod_bias() const {
        return 0.5f * std::log2(static_cast<float>(width) * static_cast<float>(height));
    }

    // Bilinear, at the level nearest `lod`, repeating or clamping at the edges. Linear light.
    [[nodiscard]] Vec4 sample(Vec2 uv, float lod, bool repeat) const {
        const ColorTables& tables = color_tables();
        const std::uint32_t level =
            std::min(levels - 1, static_cast<std::uint32_t>(std::max(0.0f, lod + 0.5f)));
        const auto level_width = static_cast<std::int32_t>(rhi::mip_size(width, level));
        const auto level_height = static_cast<std::int32_t>(rhi::mip_size(height, level));
        const std::byte* const data = pixels + offsets[level];

        const float x = uv.x * static_cast<float>(level_width) - 0.5f;
        const float y = uv.y * static_cast<float>(level_height) - 0.5f;
        const float fx = std::floor(x);
        const float fy = std::floor(y);
        const float tx = x - fx;
        const float ty = y - fy;
        const auto wrap = [repeat](std::int32_t value, std::int32_t size) {
            if (repeat) {
                value %= size;
                return value < 0 ? value + size : value;
            }
            return std::clamp(value, 0, size - 1);
        };
        // Far outside 0..1, as tiled ground can be, the integer conversion still has to be safe.
        const auto to_int = [](float value) {
            return static_cast<std::int32_t>(std::clamp(value, -1.0e8f, 1.0e8f));
        };
        const std::int32_t x0 = wrap(to_int(fx), level_width);
        const std::int32_t x1 = wrap(to_int(fx) + 1, level_width);
        const std::int32_t y0 = wrap(to_int(fy), level_height);
        const std::int32_t y1 = wrap(to_int(fy) + 1, level_height);
        const auto& table = srgb ? tables.srgb_to_linear : tables.unorm;
        const auto texel = [&](std::int32_t tx_, std::int32_t ty_) {
            const std::byte* const p = data + (static_cast<std::size_t>(ty_) * level_width + tx_) * 4;
            return Vec4{table[static_cast<std::size_t>(p[0])], table[static_cast<std::size_t>(p[1])],
                        table[static_cast<std::size_t>(p[2])], tables.unorm[static_cast<std::size_t>(p[3])]};
        };
        const Vec4 top = texel(x0, y0) * (1.0f - tx) + texel(x1, y0) * tx;
        const Vec4 bottom = texel(x0, y1) * (1.0f - tx) + texel(x1, y1) * tx;
        return top * (1.0f - ty) + bottom * ty;
    }
};

// A material with its textures found.
struct Material {
    const MeshMaterial* source = nullptr;
    const SampledTexture* base_color = nullptr;
    const SampledTexture* normal = nullptr;
    const SampledTexture* metallic_roughness = nullptr;
    const SampledTexture* occlusion = nullptr;
    const SampledTexture* emissive = nullptr;
};

// A vertex after the model and view-projection matrices.
struct ClipVertex {
    Vec4 clip;
    Vec3 world;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent;
};

ClipVertex mix(const ClipVertex& a, const ClipVertex& b, float t) {
    return ClipVertex{.clip = a.clip + (b.clip - a.clip) * t,
                      .world = a.world + (b.world - a.world) * t,
                      .normal = a.normal + (b.normal - a.normal) * t,
                      .uv = a.uv + (b.uv - a.uv) * t,
                      .tangent = a.tangent + (b.tangent - a.tangent) * t};
}

// A vertex on the screen, in pixels from the top-left corner, with depth from 0 to 1.
struct ScreenVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float inverse_w = 1.0f;
};

// A triangle ready to fill, and its vertices' attributes for lighting.
struct Triangle {
    std::array<ScreenVertex, 3> screen;
    std::array<ClipVertex, 3> vertices;
    std::uint32_t material = 0;
    // log2 of the texels of a 1x1 texture each pixel covers, halved; add a texture's lod_bias.
    float lod = 0.0f;
};

// Clips a triangle against the near plane, clip z = 0, leaving up to two triangles.
std::uint32_t clip_near(const std::array<ClipVertex, 3>& input,
                        std::array<std::array<ClipVertex, 3>, 2>& output) {
    std::array<ClipVertex, 4> polygon{};
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const ClipVertex& current = input[index];
        const ClipVertex& next = input[(index + 1) % 3];
        const bool current_in = current.clip.z >= 0.0f;
        const bool next_in = next.clip.z >= 0.0f;
        if (current_in) {
            polygon[count++] = current;
        }
        if (current_in != next_in) {
            const float t = current.clip.z / (current.clip.z - next.clip.z);
            polygon[count++] = mix(current, next, t);
        }
    }
    if (count < 3) {
        return 0;
    }
    output[0] = {polygon[0], polygon[1], polygon[2]};
    if (count == 4) {
        output[1] = {polygon[0], polygon[2], polygon[3]};
        return 2;
    }
    return 1;
}

ScreenVertex to_screen(const Vec4& clip, float width, float height) {
    const float inverse_w = 1.0f / clip.w;
    return ScreenVertex{.x = (clip.x * inverse_w * 0.5f + 0.5f) * width,
                        .y = (0.5f - clip.y * inverse_w * 0.5f) * height,
                        .z = clip.z * inverse_w,
                        .inverse_w = inverse_w};
}

// Twice the signed area in screen space, where y grows downward. A front face, counter-clockwise
// in the engine's +Y-up clip space and so as seen, has a negative area here.
float signed_area(const std::array<ScreenVertex, 3>& v) {
    return (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
}

// Where a pixel falls on a triangle: perspective-correct weights of its vertices, and its depth.
struct Coverage {
    std::int32_t x = 0;
    std::int32_t y = 0;
    float depth = 0.0f;
    std::array<float, 3> weights{};
};

// Calls visit(coverage) for every pixel of rows [row_begin, row_end) and columns [0, width) whose
// center the triangle covers, either way round. Shared edges belong to one triangle only.
template <typename Visit>
void fill_triangle(const std::array<ScreenVertex, 3>& v, std::int32_t width, std::int32_t row_begin,
                   std::int32_t row_end, std::int32_t clip_left, std::int32_t clip_right,
                   Visit&& visit) {
    const float area = signed_area(v);
    if (area == 0.0f || !std::isfinite(area)) {
        return;
    }
    const float min_x = std::min({v[0].x, v[1].x, v[2].x});
    const float max_x = std::max({v[0].x, v[1].x, v[2].x});
    const float min_y = std::min({v[0].y, v[1].y, v[2].y});
    const float max_y = std::max({v[0].y, v[1].y, v[2].y});
    const auto x0 = std::max({0, clip_left, static_cast<std::int32_t>(std::floor(std::max(min_x, -1.0e7f)))});
    const auto x1 = std::min({width - 1, clip_right - 1,
                              static_cast<std::int32_t>(std::ceil(std::min(max_x, 1.0e7f)))});
    const auto y0 =
        std::max(row_begin, static_cast<std::int32_t>(std::floor(std::max(min_y, -1.0e7f))));
    const auto y1 =
        std::min(row_end - 1, static_cast<std::int32_t>(std::ceil(std::min(max_y, 1.0e7f))));
    if (x0 > x1 || y0 > y1) {
        return;
    }

    // Edge i is opposite vertex i. Its function is positive inside for either winding once
    // multiplied by `sign`.
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    const float inverse_area = 1.0f / std::abs(area);
    struct Edge {
        float a, b, c;
        bool owns_zero;
    };
    std::array<Edge, 3> edges{};
    for (std::size_t index = 0; index < 3; ++index) {
        const ScreenVertex& p = v[(index + 1) % 3];
        const ScreenVertex& q = v[(index + 2) % 3];
        Edge edge{.a = (p.y - q.y) * sign, .b = (q.x - p.x) * sign, .c = 0.0f, .owns_zero = false};
        edge.c = -(edge.a * p.x + edge.b * p.y);
        // The top-left rule: a pixel exactly on an edge belongs to the triangle if the edge is a
        // top edge or a left edge.
        edge.owns_zero = edge.a > 0.0f || (edge.a == 0.0f && edge.b < 0.0f);
        edges[index] = edge;
    }

    for (std::int32_t y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (std::int32_t x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            std::array<float, 3> e{};
            bool inside = true;
            for (std::size_t index = 0; index < 3; ++index) {
                e[index] = edges[index].a * px + edges[index].b * py + edges[index].c;
                if (e[index] < 0.0f || (e[index] == 0.0f && !edges[index].owns_zero)) {
                    inside = false;
                    break;
                }
            }
            if (!inside) {
                continue;
            }
            const float b0 = e[0] * inverse_area;
            const float b1 = e[1] * inverse_area;
            const float b2 = e[2] * inverse_area;
            const float depth = b0 * v[0].z + b1 * v[1].z + b2 * v[2].z;
            const float w0 = b0 * v[0].inverse_w;
            const float w1 = b1 * v[1].inverse_w;
            const float w2 = b2 * v[2].inverse_w;
            const float sum = w0 + w1 + w2;
            const float inverse_sum = sum != 0.0f ? 1.0f / sum : 0.0f;
            visit(Coverage{.x = x, .y = y, .depth = depth,
                           .weights = {w0 * inverse_sum, w1 * inverse_sum, w2 * inverse_sum}});
        }
    }
}

template <typename T> T blend3(const std::array<float, 3>& w, const T& a, const T& b, const T& c) {
    return a * w[0] + b * w[1] + c * w[2];
}

// Khronos PBR Neutral, as tonemap.slang has it.
Vec3 pbr_neutral(Vec3 color) {
    constexpr float start_compression = 0.8f - 0.04f;
    constexpr float desaturation = 0.15f;
    const float x = std::min({color.x, color.y, color.z});
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color = color - Vec3{offset, offset, offset};
    const float peak = std::max({color.x, color.y, color.z});
    if (peak < start_compression) {
        return color;
    }
    constexpr float d = 1.0f - start_compression;
    const float new_peak = 1.0f - d * d / (peak + d - start_compression);
    color = color * (new_peak / peak);
    const float g = 1.0f - 1.0f / (desaturation * (peak - new_peak) + 1.0f);
    return lerp(color, Vec3{new_peak, new_peak, new_peak}, g);
}

// GGX and its helpers, as forward.slang has them.
float distribution(float n_dot_h, float alpha) {
    const float alpha2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (core::k_pi * d * d);
}

float visibility(float n_dot_v, float n_dot_l, float alpha) {
    const float mixed = 2.0f * n_dot_l * n_dot_v +
                        (n_dot_l + n_dot_v - 2.0f * n_dot_l * n_dot_v) * alpha;
    return 0.5f / std::max(mixed, 1e-5f);
}

Vec3 fresnel(Vec3 f0, float cosine) {
    const float x = 1.0f - cosine;
    const float x2 = x * x;
    const float factor = x2 * x2 * x;
    return f0 + (Vec3{1.0f, 1.0f, 1.0f} - f0) * factor;
}

Vec3 ambient_specular(Vec3 f0, float roughness, float n_dot_v) {
    const Vec4 c0{-1.0f, -0.0275f, -0.572f, 0.022f};
    const Vec4 c1{1.0f, 0.0425f, 1.04f, -0.04f};
    const Vec4 r = c0 * roughness + c1;
    const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * n_dot_v)) * r.x + r.y;
    const float scale = -1.04f * a004 + r.z;
    const float bias = 1.04f * a004 + r.w;
    return f0 * scale + Vec3{bias, bias, bias};
}

struct Settings {
    float exposure = 1.0f;
    bool neutral = true;
    bool culling = true;
    std::uint32_t max_lights = 16;
    std::uint32_t threads = 1;
    // 0 turns the sun's shadows off.
    std::uint32_t shadow_resolution = 1024;
    float shadow_distance = 40.0f;
};

core::Error setting_error(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

core::Result<Settings> read_settings(const core::Config& config) {
    Settings settings;
    const double exposure = config.get_float("renderer.exposure").value_or(1.0);
    if (!(exposure > 0.0 && exposure <= 1000.0)) {
        return setting_error("Setting 'renderer.exposure' must be above 0 and at most 1000");
    }
    settings.exposure = static_cast<float>(exposure);
    const std::string_view tone_mapping =
        config.get_string("renderer.tone_mapping").value_or("neutral");
    if (tone_mapping == "neutral") {
        settings.neutral = true;
    } else if (tone_mapping == "none") {
        settings.neutral = false;
    } else {
        return setting_error("Setting 'renderer.tone_mapping' must be \"neutral\" or \"none\"");
    }
    if (const std::optional<bool> culling = config.get_bool("renderer.culling")) {
        settings.culling = *culling;
    }
    if (const std::optional<std::int64_t> max_lights = config.get_int("renderer.max_lights")) {
        if (*max_lights < 0 || *max_lights > 256) {
            return setting_error("Setting 'renderer.max_lights' must be from 0 to 256");
        }
        settings.max_lights = static_cast<std::uint32_t>(*max_lights);
    }
    const std::int64_t hardware = std::max(1U, std::thread::hardware_concurrency());
    const std::int64_t threads =
        config.get_int("renderer.software.threads").value_or(std::min<std::int64_t>(hardware, 16));
    if (threads < 1 || threads > 64) {
        return setting_error("Setting 'renderer.software.threads' must be from 1 to 64");
    }
    settings.threads = static_cast<std::uint32_t>(threads);
    const std::string_view shadows = config.get_string("renderer.shadows.mode").value_or("auto");
    const std::int64_t resolution = config.get_int("renderer.shadows.resolution").value_or(1024);
    if (resolution < 128 || resolution > 4096) {
        return setting_error("Setting 'renderer.shadows.resolution' must be from 128 to 4096");
    }
    if (shadows == "off") {
        settings.shadow_resolution = 0;
    } else if (shadows == "auto" || shadows == "on") {
        settings.shadow_resolution = static_cast<std::uint32_t>(resolution);
    } else {
        return setting_error("Setting 'renderer.shadows.mode' must be \"auto\", \"on\" or \"off\"");
    }
    const double distance = config.get_float("renderer.shadows.distance").value_or(40.0);
    if (!(distance >= 1.0 && distance <= 10000.0)) {
        return setting_error("Setting 'renderer.shadows.distance' must be from 1 to 10000");
    }
    settings.shadow_distance = static_cast<float>(distance);
    return settings;
}

// The sun's shadow map: depths seen from the light, over the first stretch of the camera's view.
struct ShadowMap {
    std::uint32_t size = 0;
    Mat4 view_projection;
    std::vector<float> depth;
    // How far a lookup moves along the surface's normal, in world units, against acne.
    float normal_offset = 0.0f;

    // 1 where the sun reaches `world`, 0 in full shadow, softened over 2x2 texels.
    [[nodiscard]] float lit(Vec3 world, Vec3 normal) const {
        if (size == 0) {
            return 1.0f;
        }
        const Vec4 clip = view_projection * Vec4{world.x + normal.x * normal_offset,
                                                 world.y + normal.y * normal_offset,
                                                 world.z + normal.z * normal_offset, 1.0f};
        const float u = (clip.x * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f;
        const float v = (0.5f - clip.y * 0.5f) * static_cast<float>(size) - 0.5f;
        const float reference = clip.z - 0.0015f;
        if (reference >= 1.0f || reference <= 0.0f) {
            return 1.0f;
        }
        const float fu = std::floor(u);
        const float fv = std::floor(v);
        const float tu = u - fu;
        const float tv = v - fv;
        const auto at = [&](float x, float y) {
            const auto ix = static_cast<std::int32_t>(x);
            const auto iy = static_cast<std::int32_t>(y);
            if (ix < 0 || iy < 0 || ix >= static_cast<std::int32_t>(size) ||
                iy >= static_cast<std::int32_t>(size)) {
                return 1.0f;
            }
            return reference <= depth[static_cast<std::size_t>(iy) * size + ix] ? 1.0f : 0.0f;
        };
        const float top = at(fu, fv) * (1.0f - tu) + at(fu + 1.0f, fv) * tu;
        const float bottom = at(fu, fv + 1.0f) * (1.0f - tu) + at(fu + 1.0f, fv + 1.0f) * tu;
        return top * (1.0f - tv) + bottom * tv;
    }
};

struct LightData {
    Light light;
    float cos_inner = 1.0f;
    float cos_outer = 0.0f;
};

class SoftwareRenderer final : public SceneRenderer {
public:
    explicit SoftwareRenderer(const Settings& settings)
        : m_settings(settings), m_pool(settings.threads),
          m_describe("Software renderer, not started: it needs the software rhi module") {}

    core::Result<void> start(rhi::Device& device) override {
        m_access = device.software_access();
        if (m_access == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The software renderer needs a device that draws on the CPU, such "
                               "as the 'software' rhi module"};
        }
        m_describe = std::format("Software renderer: {} threads, {}", m_pool.bands(),
                                 m_settings.shadow_resolution > 0
                                     ? std::format("{}x{} sun shadow map",
                                                   m_settings.shadow_resolution,
                                                   m_settings.shadow_resolution)
                                     : std::string("no shadows"));
        return {};
    }

    // Nothing to load: everything it draws with is C++.
    core::Result<void> load_shaders(const FileReader& /*read_file*/) override {
        return {};
    }

    std::string_view describe() const override {
        return m_describe;
    }

    FrameStats render(rhi::CommandList& commands, const Frame& frame) override {
        DREAM_PROFILE_ZONE("Software renderer");
        // The pass clears the window to the clear color; the frame is then drawn over it.
        commands.begin_render_pass(frame.clear_color);
        commands.end_render_pass();
        const rhi::SoftwareTarget target = m_access->window_target();
        m_stats = {};
        if (target.width == 0 || target.height == 0) {
            return m_stats;
        }
        m_width = static_cast<std::int32_t>(target.width);
        m_height = static_cast<std::int32_t>(target.height);
        const std::size_t pixels = std::size_t{target.width} * target.height;
        m_color.assign(pixels, Vec3{frame.clear_color.r, frame.clear_color.g, frame.clear_color.b});
        m_textures.clear();

        if (frame.view.has_value()) {
            m_depth.assign(pixels, 1.0f);
            m_visible.assign(pixels, k_no_triangle);
            m_weights.resize(pixels);
            draw_scene(frame, *frame.view);
        }
        finish(frame, target);
        return m_stats;
    }

private:
    // Rows [begin, end) of band `band`.
    [[nodiscard]] std::pair<std::int32_t, std::int32_t> band_rows(std::uint32_t band,
                                                                  std::int32_t height) const {
        const auto count = static_cast<std::int32_t>(m_pool.bands());
        const std::int32_t rows = (height + count - 1) / count;
        const std::int32_t begin = std::min(height, static_cast<std::int32_t>(band) * rows);
        return {begin, std::min(height, begin + rows)};
    }

    const SampledTexture* texture(rhi::TextureHandle handle) {
        if (!handle.is_valid()) {
            return nullptr;
        }
        const std::uint64_t key = (std::uint64_t{handle.generation} << 32) | handle.index;
        const auto found = m_textures.find(key);
        if (found != m_textures.end()) {
            return found->second.has_value() ? &*found->second : nullptr;
        }
        std::optional<SampledTexture> sampled;
        const std::optional<rhi::SoftwareTexture> contents = m_access->texture_contents(handle);
        if (contents.has_value() && !rhi::is_compressed(contents->desc.format) &&
            (contents->desc.format == rhi::TextureFormat::Rgba8Srgb ||
             contents->desc.format == rhi::TextureFormat::Rgba8Unorm)) {
            SampledTexture texture{.width = contents->desc.width,
                                   .height = contents->desc.height,
                                   .levels = std::min<std::uint32_t>(contents->desc.mip_levels, 16),
                                   .srgb = rhi::is_srgb(contents->desc.format),
                                   .pixels = contents->pixels.data()};
            std::size_t offset = 0;
            for (std::uint32_t level = 0; level < texture.levels; ++level) {
                texture.offsets[level] = offset;
                offset += rhi::texture_level_bytes(contents->desc.format,
                                                   rhi::mip_size(texture.width, level),
                                                   rhi::mip_size(texture.height, level));
            }
            if (offset <= contents->pixels.size()) {
                sampled = texture;
            }
        }
        const auto inserted = m_textures.emplace(key, sampled);
        return inserted.first->second.has_value() ? &*inserted.first->second : nullptr;
    }

    // Transforms one part's vertices and clips its triangles, appending them to `out`. Returns
    // false when its buffers can't be read.
    bool prepare_part(const MeshDraw& mesh, const Mat4& view_projection, std::uint32_t material,
                      bool cull_back, std::vector<Triangle>& out) {
        const std::span<const std::byte> vertex_bytes = m_access->buffer_contents(mesh.vertices);
        const std::span<const std::byte> index_bytes = m_access->buffer_contents(mesh.indices);
        const std::size_t vertex_count = vertex_bytes.size() / sizeof(MeshVertex);
        const std::size_t index_count = index_bytes.size() / sizeof(std::uint32_t);
        if (vertex_count == 0 || std::size_t{mesh.first_index} + mesh.index_count > index_count) {
            return false;
        }
        const auto* const vertices = reinterpret_cast<const MeshVertex*>(vertex_bytes.data());
        const auto* const indices = reinterpret_cast<const std::uint32_t*>(index_bytes.data());

        // Each vertex the part uses is transformed once.
        m_transformed.resize(vertex_count);
        m_transformed_stamp.resize(vertex_count, 0);
        ++m_stamp;
        const Mat4 to_clip = view_projection * mesh.model;
        const auto transform = [&](std::uint32_t index) -> const ClipVertex& {
            if (m_transformed_stamp[index] == m_stamp) {
                return m_transformed[index];
            }
            const MeshVertex& vertex = vertices[index];
            Vec3 position = vertex.position;
            Vec3 normal = vertex.normal;
            Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
            if (!mesh.joint_matrices.empty()) {
                Vec3 skinned_position{};
                Vec3 skinned_normal{};
                Vec3 skinned_tangent{};
                for (std::size_t influence = 0; influence < 4; ++influence) {
                    const float weight = static_cast<float>(vertex.weights[influence]) / 255.0f;
                    const std::size_t joint = vertex.joints[influence];
                    if (weight <= 0.0f || joint >= mesh.joint_matrices.size()) {
                        continue;
                    }
                    const Mat4& matrix = mesh.joint_matrices[joint];
                    skinned_position += core::transform_point(matrix, position) * weight;
                    skinned_normal += core::transform_direction(matrix, normal) * weight;
                    skinned_tangent += core::transform_direction(matrix, tangent) * weight;
                }
                position = skinned_position;
                normal = skinned_normal;
                tangent = skinned_tangent;
            }
            const Vec3 world_tangent = core::normalized(core::transform_direction(mesh.model, tangent));
            m_transformed[index] = ClipVertex{
                .clip = to_clip * Vec4{position.x, position.y, position.z, 1.0f},
                .world = core::transform_point(mesh.model, position),
                .normal = core::normalized(core::transform_direction(mesh.model, normal)),
                .uv = vertex.uv,
                .tangent = Vec4{world_tangent.x, world_tangent.y, world_tangent.z, vertex.tangent.w}};
            m_transformed_stamp[index] = m_stamp;
            return m_transformed[index];
        };

        const auto width = static_cast<float>(m_target_width);
        const auto height = static_cast<float>(m_target_height);
        std::array<std::array<ClipVertex, 3>, 2> clipped{};
        for (std::uint32_t corner = 0; corner + 2 < mesh.index_count; corner += 3) {
            const std::uint32_t* const triangle = indices + mesh.first_index + corner;
            if (triangle[0] >= vertex_count || triangle[1] >= vertex_count ||
                triangle[2] >= vertex_count) {
                continue;
            }
            const std::array<ClipVertex, 3> input{transform(triangle[0]), transform(triangle[1]),
                                                  transform(triangle[2])};
            // Wholly outside one side of the view.
            const auto outside = [&](auto&& test) {
                return test(input[0].clip) && test(input[1].clip) && test(input[2].clip);
            };
            if (outside([](const Vec4& c) { return c.x > c.w; }) ||
                outside([](const Vec4& c) { return c.x < -c.w; }) ||
                outside([](const Vec4& c) { return c.y > c.w; }) ||
                outside([](const Vec4& c) { return c.y < -c.w; }) ||
                outside([](const Vec4& c) { return c.z > c.w; }) ||
                outside([](const Vec4& c) { return c.z < 0.0f; })) {
                continue;
            }
            const std::uint32_t pieces = clip_near(input, clipped);
            for (std::uint32_t piece = 0; piece < pieces; ++piece) {
                Triangle out_triangle;
                out_triangle.vertices = clipped[piece];
                for (std::size_t index = 0; index < 3; ++index) {
                    out_triangle.screen[index] = to_screen(clipped[piece][index].clip, width, height);
                }
                const float area = signed_area(out_triangle.screen);
                if (area == 0.0f || (cull_back && area > 0.0f)) {
                    continue;
                }
                const Vec2 uv_a = out_triangle.vertices[1].uv - out_triangle.vertices[0].uv;
                const Vec2 uv_b = out_triangle.vertices[2].uv - out_triangle.vertices[0].uv;
                const float uv_area = std::abs(uv_a.x * uv_b.y - uv_a.y * uv_b.x);
                out_triangle.lod = 0.5f * std::log2(std::max(uv_area, 1e-12f) /
                                                    std::max(std::abs(area), 1e-6f));
                out_triangle.material = material;
                out.push_back(out_triangle);
            }
        }
        return true;
    }

    // Fills the visibility buffer with `triangles`, nearest first winning, for the rows of `band`.
    void fill_visibility(std::uint32_t band) {
        const auto [begin, end] = band_rows(band, m_height);
        for (std::size_t index = 0; index < m_opaque.size(); ++index) {
            const Triangle& triangle = m_opaque[index];
            const Material& material = m_materials[triangle.material];
            const bool masked = material.source->alpha_mode == AlphaMode::Mask;
            fill_triangle(triangle.screen, m_width, begin, end, 0, m_width, [&](const Coverage& c) {
                const std::size_t pixel = static_cast<std::size_t>(c.y) * m_width + c.x;
                if (c.depth >= m_depth[pixel] || c.depth < 0.0f) {
                    return;
                }
                if (masked) {
                    const Vec2 uv = blend3(c.weights, triangle.vertices[0].uv,
                                           triangle.vertices[1].uv, triangle.vertices[2].uv);
                    float alpha = material.source->base_color.w;
                    if (material.base_color != nullptr) {
                        alpha *= material.base_color
                                     ->sample(uv, triangle.lod + material.base_color->lod_bias(), true)
                                     .w;
                    }
                    if (alpha < material.source->alpha_cutoff) {
                        return;
                    }
                }
                m_depth[pixel] = c.depth;
                m_visible[pixel] = static_cast<std::uint32_t>(index);
                m_weights[pixel] = {c.weights[0], c.weights[1]};
            });
        }
    }

    struct Shaded {
        Vec3 color;
        float coverage = 1.0f;
    };

    // The light leaving a surface toward the camera, as forward.slang computes it.
    Shaded shade(const Triangle& triangle, const std::array<float, 3>& w, Vec3 eye,
                 const Frame& frame) const {
        const Material& material = m_materials[triangle.material];
        const MeshMaterial& source = *material.source;
        const auto& v = triangle.vertices;
        const Vec2 uv = blend3(w, v[0].uv, v[1].uv, v[2].uv);
        const Vec3 world = blend3(w, v[0].world, v[1].world, v[2].world);
        Vec3 geometric = core::normalized(blend3(w, v[0].normal, v[1].normal, v[2].normal));
        const float lod = triangle.lod;

        Vec4 base = source.base_color;
        if (material.base_color != nullptr) {
            const Vec4 texel = material.base_color->sample(uv, lod + material.base_color->lod_bias(), true);
            base = Vec4{base.x * texel.x, base.y * texel.y, base.z * texel.z, base.w * texel.w};
        }
        float metallic = source.metallic;
        float roughness = source.roughness;
        if (material.metallic_roughness != nullptr) {
            const Vec4 texel = material.metallic_roughness->sample(
                uv, lod + material.metallic_roughness->lod_bias(), true);
            metallic *= texel.z;
            roughness *= texel.y;
        }
        metallic = saturate(metallic);
        roughness = std::clamp(roughness, k_min_roughness, 1.0f);
        const float alpha = roughness * roughness;
        float occlusion = 1.0f;
        if (material.occlusion != nullptr && source.occlusion_strength != 0.0f) {
            occlusion += source.occlusion_strength *
                         (material.occlusion->sample(uv, lod + material.occlusion->lod_bias(), true).x - 1.0f);
        }
        Vec3 emissive = source.emissive;
        if (material.emissive != nullptr) {
            const Vec4 texel = material.emissive->sample(uv, lod + material.emissive->lod_bias(), true);
            emissive = mul(emissive, Vec3{texel.x, texel.y, texel.z});
        }

        const Vec3 to_camera = core::normalized(eye - world);
        Vec3 normal = geometric;
        if (material.normal != nullptr && source.normal_scale != 0.0f) {
            const Vec4 tangent4 = blend3(w, v[0].tangent, v[1].tangent, v[2].tangent);
            Vec3 tangent{tangent4.x, tangent4.y, tangent4.z};
            if (core::length_squared(tangent) > 1e-8f) {
                tangent = core::normalized(tangent - geometric * core::dot(geometric, tangent));
                const Vec3 bitangent =
                    core::cross(geometric, tangent) * (tangent4.w < 0.0f ? -1.0f : 1.0f);
                const Vec4 texel =
                    material.normal->sample(uv, lod + material.normal->lod_bias(), true);
                Vec3 mapped{texel.x * 2.0f - 1.0f, texel.y * 2.0f - 1.0f, texel.z * 2.0f - 1.0f};
                mapped.x *= source.normal_scale;
                mapped.y *= source.normal_scale;
                normal = core::normalized(tangent * mapped.x + bitangent * mapped.y +
                                          geometric * mapped.z);
            }
        }
        if (source.double_sided && core::dot(geometric, to_camera) < 0.0f) {
            geometric = -geometric;
            normal = -normal;
        }
        const float n_dot_v = std::max(core::dot(normal, to_camera), 1e-4f);
        const Vec3 base_rgb{base.x, base.y, base.z};
        const Vec3 f0 = lerp(Vec3{0.04f, 0.04f, 0.04f}, base_rgb, metallic);
        const Vec3 diffuse_color = base_rgb * (1.0f - metallic);

        Vec3 diffuse_light{};
        Vec3 specular_light{};
        const auto add_light = [&](Vec3 to_light, Vec3 radiance) {
            const float n_dot_l = saturate(core::dot(normal, to_light));
            if (n_dot_l <= 0.0f) {
                return;
            }
            const Vec3 halfway = core::normalized(to_light + to_camera);
            const float n_dot_h = saturate(core::dot(normal, halfway));
            const Vec3 f = fresnel(f0, saturate(core::dot(to_light, halfway)));
            const Vec3 specular =
                f * (distribution(n_dot_h, alpha) * visibility(n_dot_v, n_dot_l, alpha) * core::k_pi);
            const Vec3 lit = radiance * n_dot_l;
            diffuse_light += mul(mul(diffuse_color, Vec3{1.0f, 1.0f, 1.0f} - f), lit);
            specular_light += mul(specular, lit);
        };

        float ambient = 0.0f;
        if (frame.sun.has_value()) {
            const Sunlight& sun = *frame.sun;
            ambient = sun.ambient;
            const Vec3 to_light = -core::normalized(sun.direction);
            if (core::dot(geometric, to_light) > 0.0f) {
                const float shadow = sun.casts_shadows ? m_shadow.lit(world, geometric) : 1.0f;
                if (shadow > 0.0f) {
                    add_light(to_light, sun.color * (sun.intensity * shadow));
                }
            }
        }
        for (const LightData& data : m_lights) {
            const Light& light = data.light;
            const Vec3 offset = light.position - world;
            const float distance2 = core::length_squared(offset);
            if (distance2 >= light.range * light.range) {
                continue;
            }
            const Vec3 to_light = offset * (1.0f / std::sqrt(std::max(distance2, 1e-8f)));
            const float ratio = distance2 / (light.range * light.range);
            const float window = saturate(1.0f - ratio * ratio);
            float falloff = window * window / std::max(distance2, 1e-4f);
            if (light.type == LightType::Spot) {
                falloff *= smoothstep(data.cos_outer, data.cos_inner,
                                      core::dot(-to_light, light.direction));
            }
            if (falloff > 0.0f) {
                add_light(to_light, light.color * (light.intensity * falloff));
            }
        }
        diffuse_light += diffuse_color * (ambient * occlusion);
        specular_light += mul(ambient_specular(f0, roughness, n_dot_v), Vec3{1.0f, 1.0f, 1.0f}) *
                          (ambient * occlusion);

        const float coverage = source.alpha_mode == AlphaMode::Blend ? base.w : 1.0f;
        return Shaded{.color = diffuse_light * coverage + specular_light + emissive,
                      .coverage = coverage};
    }

    void draw_scene(const Frame& frame, const View& view) {
        m_target_width = m_width;
        m_target_height = m_height;
        const core::Frustum frustum = core::frustum_from(view.view_projection);

        // Materials and parts in view.
        m_materials.clear();
        m_opaque.clear();
        m_blended_parts.clear();
        for (const MeshDraw& mesh : frame.meshes) {
            if (m_settings.culling && !mesh.bounds.empty() &&
                !core::intersects(frustum, core::transformed(mesh.model, mesh.bounds))) {
                ++m_stats.culled;
                continue;
            }
            const auto material = static_cast<std::uint32_t>(m_materials.size());
            m_materials.push_back(Material{
                .source = &mesh.material,
                .base_color = texture(mesh.material.base_color_texture),
                .normal = texture(mesh.material.normal_texture),
                .metallic_roughness = texture(mesh.material.metallic_roughness_texture),
                .occlusion = texture(mesh.material.occlusion_texture),
                .emissive = texture(mesh.material.emissive_texture)});
            ++m_stats.draws;
            ++m_stats.draw_calls;
            if (!mesh.joint_matrices.empty()) {
                ++m_stats.skinned;
            }
            if (mesh.material.alpha_mode == AlphaMode::Blend) {
                ++m_stats.blended;
                m_blended_parts.push_back(BlendedPart{&mesh, material, 0.0f});
                continue;
            }
            prepare_part(mesh, view.view_projection, material, !mesh.material.double_sided,
                         m_opaque);
        }

        prepare_lights(frame, view);
        draw_shadows(frame, view);

        {
            DREAM_PROFILE_ZONE("Visibility");
            m_pool.run([this](std::uint32_t band) { fill_visibility(band); });
        }
        {
            DREAM_PROFILE_ZONE("Lighting");
            m_pool.run([&](std::uint32_t band) {
                const auto [begin, end] = band_rows(band, m_height);
                for (std::int32_t y = begin; y < end; ++y) {
                    for (std::int32_t x = 0; x < m_width; ++x) {
                        const std::size_t pixel = static_cast<std::size_t>(y) * m_width + x;
                        const std::uint32_t index = m_visible[pixel];
                        if (index == k_no_triangle) {
                            continue;
                        }
                        const std::array<float, 2>& stored = m_weights[pixel];
                        const std::array<float, 3> weights{stored[0], stored[1],
                                                           1.0f - stored[0] - stored[1]};
                        m_color[pixel] = shade(m_opaque[index], weights, view.eye, frame).color;
                    }
                }
            });
        }

        draw_blended(frame, view);
        draw_particles(frame, view);
        draw_lines(frame, view);
    }

    void prepare_lights(const Frame& frame, const View& view) {
        m_lights.clear();
        for (const Light& light : frame.lights) {
            LightData data{.light = light};
            data.light.direction = core::normalized(light.direction);
            data.cos_inner = std::cos(light.inner_angle);
            data.cos_outer = std::cos(std::max(light.outer_angle, light.inner_angle + 1e-3f));
            m_lights.push_back(data);
        }
        if (m_lights.size() > m_settings.max_lights) {
            std::ranges::sort(m_lights, [&](const LightData& a, const LightData& b) {
                return core::length_squared(a.light.position - view.eye) <
                       core::length_squared(b.light.position - view.eye);
            });
            m_stats.lights_dropped = static_cast<std::uint32_t>(m_lights.size() - m_settings.max_lights);
            m_lights.resize(m_settings.max_lights);
        }
        m_stats.lights = static_cast<std::uint32_t>(m_lights.size());
    }

    void draw_shadows(const Frame& frame, const View& view) {
        m_shadow.size = 0;
        if (m_settings.shadow_resolution == 0 || !frame.sun.has_value() ||
            !frame.sun->casts_shadows) {
            return;
        }
        DREAM_PROFILE_ZONE("Shadows");
        const std::optional<Mat4> clip_to_world = core::inverse(view.view_projection);
        if (!clip_to_world.has_value()) {
            return;
        }
        // The slice of the view the shadows cover, as a sphere, so the map doesn't swim as the
        // camera turns.
        const float far = std::min(view.z_far, view.z_near + m_settings.shadow_distance);
        const float far_depth = (far - view.z_near) * view.z_far / (far * (view.z_far - view.z_near));
        std::array<Vec3, 8> corners{};
        std::size_t count = 0;
        for (const float depth : {0.0f, std::clamp(far_depth, 0.0f, 1.0f)}) {
            for (const float y : {-1.0f, 1.0f}) {
                for (const float x : {-1.0f, 1.0f}) {
                    const Vec4 point = *clip_to_world * Vec4{x, y, depth, 1.0f};
                    corners[count++] = Vec3{point.x, point.y, point.z} * (1.0f / point.w);
                }
            }
        }
        Vec3 center{};
        for (const Vec3& corner : corners) {
            center += corner * 0.125f;
        }
        float radius = 0.0f;
        for (const Vec3& corner : corners) {
            radius = std::max(radius, core::length(corner - center));
        }
        radius = std::ceil(radius);
        const auto size = m_settings.shadow_resolution;
        const Vec3 direction = core::normalized(frame.sun->direction);
        const Vec3 up = std::abs(direction.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
        const float depth_range = radius * 2.0f + 100.0f;
        const Vec3 light_eye = center - direction * (radius + 100.0f);
        const Mat4 light_view = core::look_at(light_eye, center, up);
        // Snapped to whole texels, so the edges of shadows stay still as the camera moves.
        const float texel = radius * 2.0f / static_cast<float>(size);
        Vec3 snapped = core::transform_point(light_view, center);
        snapped.x = std::floor(snapped.x / texel) * texel;
        snapped.y = std::floor(snapped.y / texel) * texel;
        m_shadow.view_projection =
            core::orthographic(snapped.x - radius, snapped.x + radius, snapped.y - radius,
                               snapped.y + radius, 0.0f, depth_range + radius) *
            light_view;
        m_shadow.size = size;
        m_shadow.normal_offset = texel * 1.5f;
        m_shadow.depth.assign(std::size_t{size} * size, 1.0f);

        m_shadow_triangles.clear();
        m_target_width = static_cast<std::int32_t>(size);
        m_target_height = static_cast<std::int32_t>(size);
        const core::Frustum frustum = core::frustum_from(m_shadow.view_projection);
        std::uint32_t material = 0;
        for (const MeshDraw& mesh : frame.meshes) {
            if (!mesh.casts_shadows || mesh.material.alpha_mode == AlphaMode::Blend) {
                continue;
            }
            if (!mesh.bounds.empty() &&
                !core::intersects(frustum, core::transformed(mesh.model, mesh.bounds))) {
                ++m_stats.shadow_culled;
                continue;
            }
            ++m_stats.shadow_casters;
            // Materials of parts out of the camera's view aren't in m_materials, so masks are
            // ignored in the shadow map and parts cast whole shadows.
            prepare_part(mesh, m_shadow.view_projection, material, false, m_shadow_triangles);
        }
        m_target_width = m_width;
        m_target_height = m_height;

        const auto shadow_size = static_cast<std::int32_t>(size);
        m_pool.run([&](std::uint32_t band) {
            const auto [begin, end] = band_rows(band, shadow_size);
            for (const Triangle& triangle : m_shadow_triangles) {
                fill_triangle(triangle.screen, shadow_size, begin, end, 0, shadow_size,
                              [&](const Coverage& c) {
                                  float& stored =
                                      m_shadow.depth[static_cast<std::size_t>(c.y) * size + c.x];
                                  stored = std::min(stored, c.depth);
                              });
            }
        });
    }

    struct BlendedPart {
        const MeshDraw* mesh = nullptr;
        std::uint32_t material = 0;
        float distance = 0.0f;
    };

    // Blended surfaces, farthest first, over what is drawn, hidden behind it but leaving depth
    // alone.
    void draw_blended(const Frame& frame, const View& view) {
        if (m_blended_parts.empty()) {
            return;
        }
        DREAM_PROFILE_ZONE("Blended surfaces");
        for (BlendedPart& part : m_blended_parts) {
            const core::Aabb box = core::transformed(part.mesh->model, part.mesh->bounds);
            const Vec3 middle = box.empty() ? core::transform_point(part.mesh->model, Vec3{})
                                            : (box.min + box.max) * 0.5f;
            part.distance = core::length_squared(middle - view.eye);
        }
        std::ranges::stable_sort(m_blended_parts, [](const BlendedPart& a, const BlendedPart& b) {
            return a.distance > b.distance;
        });
        m_blended.clear();
        for (const BlendedPart& part : m_blended_parts) {
            prepare_part(*part.mesh, view.view_projection, part.material,
                         !part.mesh->material.double_sided, m_blended);
        }
        m_pool.run([&](std::uint32_t band) {
            const auto [begin, end] = band_rows(band, m_height);
            for (const Triangle& triangle : m_blended) {
                fill_triangle(triangle.screen, m_width, begin, end, 0, m_width, [&](const Coverage& c) {
                    const std::size_t pixel = static_cast<std::size_t>(c.y) * m_width + c.x;
                    if (c.depth >= m_depth[pixel] || c.depth < 0.0f) {
                        return;
                    }
                    const Shaded shaded = shade(triangle, c.weights, view.eye, frame);
                    m_color[pixel] = shaded.color + m_color[pixel] * (1.0f - shaded.coverage);
                });
            }
        });
    }

    void draw_particles(const Frame& frame, const View& view) {
        if (frame.particles.empty()) {
            return;
        }
        DREAM_PROFILE_ZONE("Particles");
        const std::optional<Mat4> clip_to_world = core::inverse(view.view_projection);
        if (!clip_to_world.has_value()) {
            return;
        }
        const auto unproject = [&](float x, float y) {
            const Vec4 point = *clip_to_world * Vec4{x, y, 0.5f, 1.0f};
            return Vec3{point.x, point.y, point.z} * (1.0f / point.w);
        };
        const Vec3 right = core::normalized(unproject(1.0f, 0.0f) - unproject(-1.0f, 0.0f));
        const Vec3 up = core::normalized(unproject(0.0f, 1.0f) - unproject(0.0f, -1.0f));

        constexpr std::size_t k_max_particles = 32768;
        std::size_t drawn = 0;
        for (const ParticleBatch& batch : frame.particles) {
            const SampledTexture* const sprite = texture(batch.texture);
            m_particle_order.clear();
            for (std::size_t index = 0; index < batch.particles.size(); ++index) {
                if (drawn + m_particle_order.size() >= k_max_particles) {
                    m_stats.particles_dropped +=
                        static_cast<std::uint32_t>(batch.particles.size() - index);
                    break;
                }
                m_particle_order.push_back(static_cast<std::uint32_t>(index));
            }
            if (batch.blend == ParticleBlend::Alpha) {
                std::ranges::sort(m_particle_order, [&](std::uint32_t a, std::uint32_t b) {
                    return core::length_squared(batch.particles[a].position - view.eye) >
                           core::length_squared(batch.particles[b].position - view.eye);
                });
            }
            drawn += m_particle_order.size();

            m_particle_triangles.clear();
            m_particle_colors.clear();
            for (const std::uint32_t index : m_particle_order) {
                const ParticleQuad& quad = batch.particles[index];
                const float half = quad.size * 0.5f;
                const float c = std::cos(quad.rotation) * half;
                const float s = std::sin(quad.rotation) * half;
                const Vec3 axis_x = right * c + up * s;
                const Vec3 axis_y = up * c - right * s;
                const std::array<Vec3, 4> corners{quad.position - axis_x - axis_y,
                                                  quad.position + axis_x - axis_y,
                                                  quad.position + axis_x + axis_y,
                                                  quad.position - axis_x + axis_y};
                const std::array<Vec2, 4> uvs{Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{1.0f, 0.0f},
                                              Vec2{0.0f, 0.0f}};
                std::array<ClipVertex, 4> vertices{};
                for (std::size_t corner = 0; corner < 4; ++corner) {
                    const Vec3& p = corners[corner];
                    vertices[corner] = ClipVertex{
                        .clip = view.view_projection * Vec4{p.x, p.y, p.z, 1.0f}, .world = p,
                        .uv = uvs[corner]};
                }
                const auto color_index = static_cast<std::uint32_t>(m_particle_colors.size());
                m_particle_colors.push_back(quad.color);
                for (const std::array<std::size_t, 3> corner_indices :
                     {std::array<std::size_t, 3>{0, 1, 2}, std::array<std::size_t, 3>{0, 2, 3}}) {
                    std::array<std::array<ClipVertex, 3>, 2> clipped{};
                    const std::uint32_t pieces = clip_near(
                        {vertices[corner_indices[0]], vertices[corner_indices[1]],
                         vertices[corner_indices[2]]},
                        clipped);
                    for (std::uint32_t piece = 0; piece < pieces; ++piece) {
                        Triangle triangle;
                        triangle.vertices = clipped[piece];
                        for (std::size_t corner = 0; corner < 3; ++corner) {
                            triangle.screen[corner] =
                                to_screen(clipped[piece][corner].clip, static_cast<float>(m_width),
                                          static_cast<float>(m_height));
                        }
                        triangle.material = color_index;
                        const float area = std::abs(signed_area(triangle.screen));
                        triangle.lod = 0.5f * std::log2(0.5f / std::max(area, 1e-6f));
                        m_particle_triangles.push_back(triangle);
                    }
                }
            }
            const bool additive = batch.blend == ParticleBlend::Additive;
            m_pool.run([&](std::uint32_t band) {
                const auto [begin, end] = band_rows(band, m_height);
                for (const Triangle& triangle : m_particle_triangles) {
                    const Vec4 tint = m_particle_colors[triangle.material];
                    fill_triangle(triangle.screen, m_width, begin, end, 0, m_width,
                                  [&](const Coverage& c) {
                        const std::size_t pixel = static_cast<std::size_t>(c.y) * m_width + c.x;
                        if (c.depth >= m_depth[pixel] || c.depth < 0.0f) {
                            return;
                        }
                        const Vec2 uv = blend3(c.weights, triangle.vertices[0].uv,
                                               triangle.vertices[1].uv, triangle.vertices[2].uv);
                        Vec4 texel{1.0f, 1.0f, 1.0f, 1.0f};
                        if (sprite != nullptr) {
                            texel = sprite->sample(uv, triangle.lod + sprite->lod_bias(), false);
                        } else {
                            // A soft round dot.
                            const float dx = uv.x * 2.0f - 1.0f;
                            const float dy = uv.y * 2.0f - 1.0f;
                            texel.w = 1.0f - smoothstep(0.0f, 1.0f, dx * dx + dy * dy);
                        }
                        const float alpha = saturate(texel.w * tint.w);
                        const Vec3 color{texel.x * tint.x, texel.y * tint.y, texel.z * tint.z};
                        m_color[pixel] = additive ? m_color[pixel] + color * alpha
                                                  : color * alpha + m_color[pixel] * (1.0f - alpha);
                    });
                }
            });
        }
        m_stats.particles = static_cast<std::uint32_t>(drawn);
    }

    void draw_lines(const Frame& frame, const View& view) {
        if (frame.lines.size() < 2) {
            return;
        }
        DREAM_PROFILE_ZONE("Debug lines");
        m_stats.lines = static_cast<std::uint32_t>(frame.lines.size() / 2);
        for (std::size_t index = 0; index + 1 < frame.lines.size(); index += 2) {
            const LineVertex& a = frame.lines[index];
            const LineVertex& b = frame.lines[index + 1];
            Vec4 clip_a = view.view_projection * Vec4{a.position.x, a.position.y, a.position.z, 1.0f};
            Vec4 clip_b = view.view_projection * Vec4{b.position.x, b.position.y, b.position.z, 1.0f};
            Vec4 color_a = a.color;
            Vec4 color_b = b.color;
            if (clip_a.z < 0.0f && clip_b.z < 0.0f) {
                continue;
            }
            if (clip_a.z < 0.0f || clip_b.z < 0.0f) {
                const float t = clip_a.z / (clip_a.z - clip_b.z);
                const Vec4 point = clip_a + (clip_b - clip_a) * t;
                const Vec4 color = color_a + (color_b - color_a) * t;
                if (clip_a.z < 0.0f) {
                    clip_a = point;
                    color_a = color;
                } else {
                    clip_b = point;
                    color_b = color;
                }
            }
            const ScreenVertex sa =
                to_screen(clip_a, static_cast<float>(m_width), static_cast<float>(m_height));
            const ScreenVertex sb =
                to_screen(clip_b, static_cast<float>(m_width), static_cast<float>(m_height));
            const float dx = sb.x - sa.x;
            const float dy = sb.y - sa.y;
            const float steps = std::min(std::max(std::abs(dx), std::abs(dy)), 16384.0f);
            const auto count = static_cast<std::int32_t>(std::ceil(steps));
            for (std::int32_t step = 0; step <= count; ++step) {
                const float t = count > 0 ? static_cast<float>(step) / static_cast<float>(count) : 0.0f;
                const auto x = static_cast<std::int32_t>(std::floor(sa.x + dx * t));
                const auto y = static_cast<std::int32_t>(std::floor(sa.y + dy * t));
                if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
                    continue;
                }
                const std::size_t pixel = static_cast<std::size_t>(y) * m_width + x;
                const float depth = sa.z + (sb.z - sa.z) * t;
                if (depth > m_depth[pixel] + 1e-4f) {
                    continue;
                }
                const Vec4 color = color_a + (color_b - color_a) * t;
                const float alpha = saturate(color.w);
                m_color[pixel] = Vec3{color.x, color.y, color.z} * alpha +
                                 m_color[pixel] * (1.0f - alpha);
            }
        }
    }

    // Tone mapping, the interface's layers, and the conversion to the window's sRGB bytes.
    void finish(const Frame& frame, const rhi::SoftwareTarget& target) {
        {
            DREAM_PROFILE_ZONE("Tone mapping");
            const float exposure = m_settings.exposure;
            const bool neutral = m_settings.neutral && frame.view.has_value();
            m_pool.run([&](std::uint32_t band) {
                const auto [begin, end] = band_rows(band, m_height);
                for (std::size_t pixel = static_cast<std::size_t>(begin) * m_width;
                     pixel < static_cast<std::size_t>(end) * m_width; ++pixel) {
                    Vec3 color = m_color[pixel] * exposure;
                    if (neutral) {
                        color = pbr_neutral(color);
                    }
                    m_color[pixel] = Vec3{saturate(color.x), saturate(color.y), saturate(color.z)};
                }
            });
        }
        draw_ui(frame);
        DREAM_PROFILE_ZONE("Encode");
        const ColorTables& tables = color_tables();
        m_pool.run([&](std::uint32_t band) {
            const auto [begin, end] = band_rows(band, m_height);
            for (std::size_t pixel = static_cast<std::size_t>(begin) * m_width;
                 pixel < static_cast<std::size_t>(end) * m_width; ++pixel) {
                std::byte* const out = target.pixels.data() + pixel * 4;
                out[0] = tables.encode(m_color[pixel].x);
                out[1] = tables.encode(m_color[pixel].y);
                out[2] = tables.encode(m_color[pixel].z);
                out[3] = std::byte{255};
            }
        });
    }

    void draw_ui(const Frame& frame) {
        if (frame.ui_layers.empty()) {
            return;
        }
        DREAM_PROFILE_ZONE("Interface");
        const ColorTables& tables = color_tables();
        struct UiDraw {
            const ui::DrawList* list;
            const ui::DrawCommand* command;
            Vec2 offset;
            const SampledTexture* texture;
        };
        std::vector<UiDraw> draws;
        for (const UiLayer& layer : frame.ui_layers) {
            if (layer.list == nullptr) {
                continue;
            }
            for (const ui::DrawCommand& command : layer.list->commands) {
                const SampledTexture* sampled = nullptr;
                if (command.texture != 0 && frame.ui_texture) {
                    sampled = texture(frame.ui_texture(command.texture));
                }
                draws.push_back(UiDraw{layer.list, &command, layer.offset, sampled});
                ++m_stats.ui_commands;
            }
        }
        m_pool.run([&](std::uint32_t band) {
            const auto [begin, end] = band_rows(band, m_height);
            for (const UiDraw& draw : draws) {
                const ui::DrawList& list = *draw.list;
                const ui::DrawCommand& command = *draw.command;
                std::int32_t clip_left = 0;
                std::int32_t clip_right = m_width;
                std::int32_t clip_top = begin;
                std::int32_t clip_bottom = end;
                if (command.clip.has_value()) {
                    const ui::ClipRect& clip = *command.clip;
                    const auto offset_x = static_cast<std::int32_t>(std::floor(draw.offset.x));
                    const auto offset_y = static_cast<std::int32_t>(std::floor(draw.offset.y));
                    clip_left = std::max(clip_left, clip.x + offset_x);
                    clip_right = std::min(clip_right, clip.x + offset_x + clip.width);
                    clip_top = std::max(clip_top, clip.y + offset_y);
                    clip_bottom = std::min(clip_bottom, clip.y + offset_y + clip.height);
                }
                if (clip_left >= clip_right || clip_top >= clip_bottom) {
                    continue;
                }
                const Vec2 shift = draw.offset + command.translation;
                for (std::uint32_t corner = 0; corner + 2 < command.index_count; corner += 3) {
                    std::array<const ui::Vertex*, 3> vertices{};
                    bool valid = true;
                    for (std::size_t index = 0; index < 3; ++index) {
                        const std::size_t at = std::size_t{command.first_index} + corner + index;
                        if (at >= list.indices.size() || list.indices[at] >= list.vertices.size()) {
                            valid = false;
                            break;
                        }
                        vertices[index] = &list.vertices[list.indices[at]];
                    }
                    if (!valid) {
                        continue;
                    }
                    std::array<ScreenVertex, 3> screen{};
                    std::array<Vec4, 3> colors{};
                    for (std::size_t index = 0; index < 3; ++index) {
                        const Vec2 position = vertices[index]->position + shift;
                        screen[index] = ScreenVertex{.x = position.x, .y = position.y};
                        const std::uint32_t packed = vertices[index]->color;
                        const auto channel = [&](std::uint32_t shift_bits) {
                            return static_cast<std::size_t>((packed >> shift_bits) & 0xffU);
                        };
                        const float alpha = tables.unorm[channel(24)];
                        Vec3 rgb{};
                        if (list.premultiplied_alpha) {
                            // Undoes the premultiplication, done in sRGB, then redoes it in
                            // linear, as ui.slang does.
                            const float safe = std::max(alpha, 1.0f / 255.0f);
                            rgb = Vec3{srgb_to_linear(std::min(1.0f, tables.unorm[channel(0)] / safe)),
                                       srgb_to_linear(std::min(1.0f, tables.unorm[channel(8)] / safe)),
                                       srgb_to_linear(std::min(1.0f, tables.unorm[channel(16)] / safe))} *
                                  alpha;
                        } else {
                            rgb = Vec3{tables.srgb_to_linear[channel(0)],
                                       tables.srgb_to_linear[channel(8)],
                                       tables.srgb_to_linear[channel(16)]};
                        }
                        colors[index] = Vec4{rgb.x, rgb.y, rgb.z, alpha};
                    }
                    fill_triangle(screen, m_width, clip_top, clip_bottom, clip_left, clip_right,
                                  [&](const Coverage& c) {
                        const std::size_t pixel = static_cast<std::size_t>(c.y) * m_width + c.x;
                        Vec4 color = blend3(c.weights, colors[0], colors[1], colors[2]);
                        if (draw.texture != nullptr) {
                            const Vec2 uv = blend3(c.weights, vertices[0]->uv, vertices[1]->uv,
                                                   vertices[2]->uv);
                            const Vec4 texel = draw.texture->sample(uv, 0.0f, false);
                            color = Vec4{color.x * texel.x, color.y * texel.y, color.z * texel.z,
                                         color.w * texel.w};
                        }
                        const float alpha = saturate(color.w);
                        const Vec3 rgb{color.x, color.y, color.z};
                        if (list.premultiplied_alpha) {
                            m_color[pixel] = rgb + m_color[pixel] * (1.0f - alpha);
                        } else {
                            m_color[pixel] = rgb * alpha + m_color[pixel] * (1.0f - alpha);
                        }
                    });
                }
            }
        });
    }

    Settings m_settings;
    BandPool m_pool;
    rhi::SoftwareAccess* m_access = nullptr;
    std::string m_describe;
    FrameStats m_stats;
    std::int32_t m_width = 0;
    std::int32_t m_height = 0;
    // The size of what prepare_part projects onto: the window, or the shadow map.
    std::int32_t m_target_width = 0;
    std::int32_t m_target_height = 0;

    // Linear light, then tone mapped, for every pixel.
    std::vector<Vec3> m_color;
    std::vector<float> m_depth;
    // The visibility buffer: each pixel's nearest opaque triangle, and the weights of its first two
    // vertices there.
    std::vector<std::uint32_t> m_visible;
    std::vector<std::array<float, 2>> m_weights;

    std::unordered_map<std::uint64_t, std::optional<SampledTexture>> m_textures;
    std::vector<Material> m_materials;
    std::vector<Triangle> m_opaque;
    std::vector<Triangle> m_blended;
    std::vector<BlendedPart> m_blended_parts;
    std::vector<Triangle> m_shadow_triangles;
    std::vector<Triangle> m_particle_triangles;
    std::vector<Vec4> m_particle_colors;
    std::vector<std::uint32_t> m_particle_order;
    std::vector<LightData> m_lights;
    ShadowMap m_shadow;

    std::vector<ClipVertex> m_transformed;
    std::vector<std::uint32_t> m_transformed_stamp;
    std::uint32_t m_stamp = 0;
};

core::Result<std::unique_ptr<SceneRenderer>> create(const core::Config& config) {
    core::Result<Settings> settings = read_settings(config);
    if (!settings.has_value()) {
        return settings.error();
    }
    return std::unique_ptr<SceneRenderer>{std::make_unique<SoftwareRenderer>(settings.value())};
}

} // namespace

[[nodiscard]] core::Result<void> register_renderer_software(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<SceneRenderer>{.name = "software", .create = &create});
}

} // namespace dream::modules
