#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void>
register_mesh_simplifier_meshoptimizer(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::mesh_simplifier;

namespace {

std::unique_ptr<MeshSimplifier> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_mesh_simplifier_meshoptimizer(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"meshoptimizer"};
    auto created = registry.create<MeshSimplifier>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

// A position, then a normal and texture coordinates the simplifier must skip over.
struct Vertex {
    std::array<float, 3> position;
    std::array<float, 5> rest;
};

// A grid of `size` by `size` quads, gently curved, so it can lose most of its triangles.
struct Grid {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

Grid grid(std::uint32_t size, float bend) {
    Grid result;
    for (std::uint32_t y = 0; y <= size; ++y) {
        for (std::uint32_t x = 0; x <= size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            result.vertices.push_back(
                Vertex{{u * 10.0f, bend * std::sin(u * 3.0f) * std::sin(v * 3.0f), v * 10.0f}, {}});
        }
    }
    const std::uint32_t row = size + 1;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint32_t a = y * row + x;
            for (const std::uint32_t index : {a, a + row, a + 1, a + 1, a + row, a + row + 1}) {
                result.indices.push_back(index);
            }
        }
    }
    return result;
}

Request request_for(const Grid& mesh, std::size_t target) {
    return Request{.vertices = std::as_bytes(std::span(mesh.vertices)),
                   .stride = sizeof(Vertex),
                   .indices = mesh.indices,
                   .target_index_count = target,
                   .max_relative_error = 0.2f,
                   .keep_borders = false};
}

} // namespace

TEST_CASE("meshoptimizer simplifies a mesh toward the size asked for, and says how far it strays") {
    const auto simplifier = make();
    const Grid flat = grid(16, 0.0f);

    const Simplified half = simplifier->simplify(request_for(flat, flat.indices.size() / 2)).value();
    CHECK(half.indices.size() % 3 == 0);
    CHECK(half.indices.size() <= flat.indices.size() / 2);
    // A flat grid loses nothing of its shape.
    CHECK(half.error == doctest::Approx(0.0f).epsilon(1e-3));

    const Grid bent = grid(16, 1.0f);
    const Simplified coarse = simplifier->simplify(request_for(bent, 60)).value();
    CHECK(coarse.indices.size() < bent.indices.size() / 4);
    CHECK(coarse.error > 0.0f);
    // In the mesh's own units: the grid is ten across, and the error stays below a fifth of it.
    CHECK(coarse.error < 2.0f);
    for (const std::uint32_t index : coarse.indices) {
        CHECK(index < bent.vertices.size());
    }
}

TEST_CASE("meshoptimizer keeps a mesh's open edges in place when asked") {
    const auto simplifier = make();
    const Grid bent = grid(8, 1.0f);
    Request request = request_for(bent, 6);
    request.keep_borders = true;
    const Simplified kept = simplifier->simplify(request).value();
    // Every vertex on the grid's edge is still used.
    for (std::uint32_t x = 0; x <= 8; ++x) {
        CHECK(std::ranges::find(kept.indices, x) != kept.indices.end());
    }
}

TEST_CASE("meshoptimizer refuses meshes it can't read") {
    const auto simplifier = make();
    const Grid flat = grid(2, 0.0f);
    Request request = request_for(flat, 6);
    request.stride = 4;
    CHECK_FALSE(simplifier->simplify(request).has_value());

    std::vector<std::uint32_t> broken{0, 1, 999};
    request = request_for(flat, 3);
    request.indices = broken;
    CHECK_FALSE(simplifier->simplify(request).has_value());
}
