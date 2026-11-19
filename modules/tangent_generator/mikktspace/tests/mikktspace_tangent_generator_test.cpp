#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/tangent_generator/tangent_generator.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void>
register_tangent_generator_mikktspace(core::ModuleRegistry& registry);
} // namespace dream::modules

using namespace dream;
using namespace dream::tangent_generator;
using doctest::Approx;

namespace {

std::unique_ptr<TangentGenerator> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_tangent_generator_mikktspace(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"mikktspace"};
    auto created = registry.create<TangentGenerator>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

struct Vertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 2> uv;
};

// A square facing +Z, with the texture's top left at its top left, as glTF lays textures out: u
// grows along +X and v down, along -Y. `mirror_u` flips the texture across, as mirrored UVs do.
std::array<Vertex, 4> square(bool mirror_u) {
    const float left = mirror_u ? 1.0f : 0.0f;
    const float right = 1.0f - left;
    return {{
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {left, 0.0f}},
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {left, 1.0f}},
        {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {right, 1.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {right, 0.0f}},
    }};
}

constexpr std::array<std::uint32_t, 6> k_square_indices{0, 1, 2, 0, 2, 3};

Request request_for(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) {
    return Request{.vertices = std::as_bytes(vertices),
                   .stride = sizeof(Vertex),
                   .position_offset = offsetof(Vertex, position),
                   .normal_offset = offsetof(Vertex, normal),
                   .uv_offset = offsetof(Vertex, uv),
                   .indices = indices};
}

} // namespace

TEST_CASE("MikkTSpace tangents follow u, with the bitangent toward the texture's top") {
    const std::array vertices = square(false);
    const auto tangents = make()->generate(request_for(vertices, k_square_indices));
    REQUIRE(tangents.has_value());
    REQUIRE(tangents.value().size() == k_square_indices.size());
    for (const core::Vec4& tangent : tangents.value()) {
        CHECK(tangent.x == Approx(1.0f));
        CHECK(tangent.y == Approx(0.0f));
        CHECK(tangent.z == Approx(0.0f));
        // cross(+Z, +X) is +Y, the top of the texture.
        CHECK(tangent.w == 1.0f);
    }
}

TEST_CASE("Mirrored texture coordinates turn the tangent around and keep the bitangent") {
    const std::array vertices = square(true);
    const auto tangents = make()->generate(request_for(vertices, k_square_indices));
    REQUIRE(tangents.has_value());
    for (const core::Vec4& tangent : tangents.value()) {
        CHECK(tangent.x == Approx(-1.0f));
        // cross(+Z, -X) * -1 is +Y again.
        CHECK(tangent.w == -1.0f);
    }
}

TEST_CASE("Bad requests are refused") {
    const std::array vertices = square(false);
    const std::array<std::uint32_t, 3> outside{0, 1, 9};
    CHECK_FALSE(make()->generate(request_for(vertices, outside)).has_value());
    const std::array<std::uint32_t, 2> partial{0, 1};
    CHECK_FALSE(make()->generate(request_for(vertices, partial)).has_value());
}
