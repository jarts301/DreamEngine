#include "dream/runtime/model.h"

#include "fake_device.h"

#include <doctest/doctest.h>

#include <cstring>
#include <format>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using doctest::Approx;
using dream::runtime::tests::FakeDevice;
using dream::runtime::tests::FakeDeviceState;

namespace {

// A real two-pixel PNG: one red pixel, then one blue pixel.
constexpr unsigned char k_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x7b, 0x40, 0xe8, 0xdd, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0xf8, 0xcf, 0x00, 0x04, 0xff, 0x01, 0x07, 0x00, 0x01, 0xff, 0x3d, 0x7d,
    0x8c, 0x49, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::span<const std::byte> png_file() {
    return std::as_bytes(std::span(k_png));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    const auto* const bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

// Packs JSON and binary data into a .glb file.
std::vector<std::byte> make_glb(std::string json, const std::vector<std::byte>& binary) {
    while (json.size() % 4 != 0) {
        json.push_back(' ');
    }
    std::vector<std::byte> padded = binary;
    while (padded.size() % 4 != 0) {
        padded.push_back(std::byte{0});
    }

    std::vector<std::byte> file;
    append_u32(file, 0x46546C67);
    append_u32(file, 2);
    append_u32(file, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + padded.size()));
    append_u32(file, static_cast<std::uint32_t>(json.size()));
    append_u32(file, 0x4E4F534A);
    const auto text = std::as_bytes(std::span(json));
    file.insert(file.end(), text.begin(), text.end());
    append_u32(file, static_cast<std::uint32_t>(padded.size()));
    append_u32(file, 0x004E4942);
    file.insert(file.end(), padded.begin(), padded.end());
    return file;
}

template <typename T> void append(std::vector<std::byte>& out, std::span<const T> values) {
    const auto bytes = std::as_bytes(values);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// One triangle in the XY plane, facing +Z.
constexpr float k_positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
constexpr float k_normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
constexpr float k_uvs[] = {0, 0, 1, 0, 0, 1};
constexpr std::uint16_t k_indices[] = {0, 1, 2};

std::vector<std::byte> triangle_binary() {
    std::vector<std::byte> binary;
    append<float>(binary, k_positions);
    append<float>(binary, k_normals);
    append<float>(binary, k_uvs);
    append<std::uint16_t>(binary, k_indices);
    return binary;
}

constexpr const char* k_views = R"("bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":36},
{"buffer":0,"byteOffset":36,"byteLength":36},
{"buffer":0,"byteOffset":72,"byteLength":24},
{"buffer":0,"byteOffset":96,"byteLength":6}])";

constexpr const char* k_accessors = R"("accessors":[
{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}])";

std::string model_json(const std::string& node, const std::string& attributes,
                       const std::string& extra = {}) {
    return std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{}],
"meshes":[{{"primitives":[{{"attributes":{},"indices":3,"material":0}}]}}],
"materials":[{{"pbrMetallicRoughness":{{"baseColorFactor":[0.5,0.25,0.75,1]}}}}],
"buffers":[{{"byteLength":104}}],{},{}{}}})",
        node, attributes, k_views, k_accessors, extra);
}

constexpr const char* k_attributes = R"({"POSITION":0,"NORMAL":1,"TEXCOORD_0":2})";

} // namespace

TEST_CASE("decode_image reads a PNG from the top row down") {
    const core::Result<ImageData> image = decode_image(png_file());
    REQUIRE(image.has_value());

    CHECK(image.value().width == 2);
    CHECK(image.value().height == 1);
    REQUIRE(image.value().pixels.size() == 8);
    CHECK(image.value().pixels[0] == std::byte{255});
    CHECK(image.value().pixels[2] == std::byte{0});
    CHECK(image.value().pixels[3] == std::byte{255});
    CHECK(image.value().pixels[4] == std::byte{0});
    CHECK(image.value().pixels[6] == std::byte{255});
}

TEST_CASE("decode_image rejects files that aren't images") {
    const std::array<std::byte, 4> junk{};
    CHECK_FALSE(decode_image(junk).has_value());
    CHECK_FALSE(decode_image({}).has_value());
}

TEST_CASE("parse_glb reads geometry and material") {
    const auto file = make_glb(model_json(R"({"mesh":0})", k_attributes), triangle_binary());

    const core::Result<ModelData> model = parse_glb(file);
    REQUIRE(model.has_value());

    REQUIRE(model.value().vertices.size() == 3);
    CHECK(model.value().vertices[1].position.x == Approx(1.0f));
    CHECK(model.value().vertices[2].normal.z == Approx(1.0f));
    CHECK(model.value().vertices[2].uv.y == Approx(1.0f));
    CHECK(model.value().indices == std::vector<std::uint32_t>{0, 1, 2});

    REQUIRE(model.value().parts.size() == 1);
    CHECK(model.value().parts[0].index_count == 3);

    REQUIRE(model.value().materials.size() == 1);
    CHECK(model.value().materials[0].base_color.y == Approx(0.25f));
    CHECK(model.value().materials[0].image == -1);
}

TEST_CASE("parse_glb bakes a node's transform into the vertices") {
    const auto file = make_glb(
        model_json(R"({"mesh":0,"translation":[10,0,0],"scale":[2,1,1]})", k_attributes),
        triangle_binary());

    const core::Result<ModelData> model = parse_glb(file);
    REQUIRE(model.has_value());

    CHECK(model.value().vertices[0].position.x == Approx(10.0f));
    CHECK(model.value().vertices[1].position.x == Approx(12.0f));
    CHECK(model.value().vertices[0].normal.z == Approx(1.0f));
}

TEST_CASE("parse_glb allows a missing texture coordinate set") {
    const auto file =
        make_glb(model_json(R"({"mesh":0})", R"({"POSITION":0,"NORMAL":1})"), triangle_binary());

    const core::Result<ModelData> model = parse_glb(file);
    REQUIRE(model.has_value());
    CHECK(model.value().vertices[1].uv.x == Approx(0.0f));
}

TEST_CASE("parse_glb asks for normals when they are missing") {
    const auto file =
        make_glb(model_json(R"({"mesh":0})", R"({"POSITION":0})"), triangle_binary());

    const core::Result<ModelData> model = parse_glb(file);
    REQUIRE_FALSE(model.has_value());
    CHECK(model.error().message.find("normals") != std::string::npos);
}

TEST_CASE("parse_glb rejects something that isn't a model") {
    const std::array<std::byte, 32> junk{};
    CHECK_FALSE(parse_glb(junk).has_value());
    CHECK_FALSE(parse_glb({}).has_value());
}

TEST_CASE("parse_glb rejects a file with no meshes") {
    const auto file = make_glb(R"({"asset":{"version":"2.0"}})", {});

    const core::Result<ModelData> model = parse_glb(file);
    REQUIRE_FALSE(model.has_value());
    CHECK(model.error().message.find("no meshes") != std::string::npos);
}

TEST_CASE("create_model_asset creates one texture per image and links materials to them") {
    ModelData data;
    data.vertices.resize(3);
    data.indices = {0, 1, 2};
    data.images.push_back(ImageData{2, 1, std::vector<std::byte>(8)});
    data.materials.push_back(ModelMaterialData{{1, 1, 1, 1}, 0});
    data.materials.push_back(ModelMaterialData{{1, 0, 0, 1}, -1});
    data.parts.push_back(ModelPart{0, 3, 1});

    FakeDeviceState state;
    FakeDevice device(state);

    const core::Result<ModelAsset> asset = create_model_asset(device, data);
    REQUIRE(asset.has_value());

    CHECK(state.calls == std::vector<std::string>{"texture 2x1"});
    REQUIRE(asset.value().materials.size() == 2);
    CHECK(asset.value().materials[0].texture.is_valid());
    CHECK_FALSE(asset.value().materials[1].texture.is_valid());
}
