#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/tile_map_format/tile_map_format.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_tile_map_format_tiled(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::tile_map_format;
using doctest::Approx;

namespace {

std::unique_ptr<TileMapFormat> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_tile_map_format_tiled(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"tiled"};
    auto created = registry.create<TileMapFormat>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

// A 3 x 2 map, as Tiled 1.10 saves one, trimmed of what isn't read.
constexpr std::string_view k_map = R"({
  "type": "map", "version": "1.10", "orientation": "orthogonal", "infinite": false,
  "width": 3, "height": 2, "tilewidth": 16, "tileheight": 16,
  "properties": [{"name": "music", "type": "file", "value": "theme.ogg"}],
  "tilesets": [
    {"firstgid": 9, "source": "props.tsj"},
    {"firstgid": 1, "name": "ground", "image": "../sprites/ground.png",
     "imagewidth": 64, "imageheight": 32, "tilewidth": 16, "tileheight": 16,
     "columns": 4, "tilecount": 8, "margin": 0, "spacing": 0,
     "tiles": [
       {"id": 2, "animation": [{"tileid": 2, "duration": 250}, {"tileid": 3, "duration": 500}]},
       {"id": 0, "properties": [{"name": "solid", "type": "bool", "value": true}]},
       {"id": 1, "objectgroup": {"objects": [{"id": 1, "x": 0, "y": 0, "width": 16, "height": 16}]}}
     ]}
  ],
  "layers": [
    {"type": "imagelayer", "name": "sky", "image": "sky.png", "repeatx": true,
     "parallaxx": 0.25, "visible": true, "opacity": 1},
    {"type": "group", "name": "front", "offsetx": 4, "offsety": -2, "opacity": 0.5,
     "parallaxx": 0.5, "tintcolor": "#ff8000",
     "layers": [
       {"type": "tilelayer", "name": "ground", "width": 3, "height": 2, "opacity": 0.5,
        "offsetx": 1, "parallaxx": 0.5, "visible": true,
        "properties": [{"name": "layer", "type": "int", "value": 2}],
        "data": [0, 1, 2147483651, 2, 9, 1073741834]}
     ]},
    {"type": "tilelayer", "name": "base64", "width": 3, "height": 2, "visible": false,
     "encoding": "base64", "data": "AQAAAAIAAAADAAAABAAAAAUAAAAGAAAA"},
    {"type": "objectgroup", "name": "things", "visible": true,
     "objects": [
       {"id": 4, "name": "start", "type": "", "class": "spawn", "point": true, "x": 8, "y": 24,
        "width": 0, "height": 0, "rotation": 0, "visible": true},
       {"id": 5, "name": "door", "type": "trigger", "x": 32, "y": 0, "width": 16, "height": 32,
        "rotation": 90, "visible": true,
        "properties": [{"name": "to", "type": "string", "value": "cave"},
                       {"name": "speed", "type": "float", "value": 2.5}]},
       {"id": 6, "name": "tree", "gid": 10, "x": 16, "y": 32, "width": 16, "height": 16,
        "rotation": 0, "visible": true},
       {"id": 7, "name": "ledge", "x": 10, "y": 10, "rotation": 0, "visible": true,
        "polygon": [{"x": 0, "y": 0}, {"x": 20, "y": -4}, {"x": 6, "y": 8}]}
     ]}
  ]
})";

} // namespace

TEST_CASE("Tiled maps are read with their tilesets, layers and objects") {
    const auto reader = make();
    CHECK(reader->reads("assets:/maps/level.tmj"));
    CHECK_FALSE(reader->reads("assets:/maps/level.tmx"));

    const core::Result<Map> read = reader->read_map(bytes(k_map), "level.tmj");
    if (!read.has_value()) {
        FAIL(read.error().message);
    }
    const Map& map = read.value();
    CHECK(map.columns == 3);
    CHECK(map.rows == 2);
    CHECK(map.tile_width == 16);
    REQUIRE(map.properties.size() == 1);
    CHECK(map.properties[0].value == "theme.ogg");

    // Sorted by their first tile.
    REQUIRE(map.tilesets.size() == 2);
    const Tileset& ground = map.tilesets[0];
    CHECK(ground.first_tile == 1);
    CHECK(ground.image == "../sprites/ground.png");
    CHECK(ground.columns == 4);
    CHECK(ground.tile_count == 8);
    REQUIRE(ground.tiles.size() == 3);
    CHECK(ground.tiles[0].id == 2);
    REQUIRE(ground.tiles[0].animation.size() == 2);
    CHECK(ground.tiles[0].animation[1].tile == 3);
    CHECK(ground.tiles[0].animation[1].seconds == Approx(0.5f));
    CHECK(ground.tiles[1].properties[0].name == "solid");
    CHECK(ground.tiles[1].properties[0].value == "true");
    CHECK(ground.tiles[2].has_collision);
    CHECK(map.tilesets[1].source == "props.tsj");
    CHECK(map.tilesets[1].first_tile == 9);

    REQUIRE(map.layers.size() == 4);
    const Layer& sky = map.layers[0];
    CHECK(sky.kind == LayerKind::Image);
    CHECK(sky.image == "sky.png");
    CHECK(sky.repeat_x);
    CHECK(sky.parallax_x == Approx(0.25f));

    // The group's offset, opacity, parallax and tint, with the layer's own.
    const Layer& front = map.layers[1];
    CHECK(front.kind == LayerKind::Tiles);
    CHECK(front.name == "front/ground");
    CHECK(front.offset_x == Approx(5.0f));
    CHECK(front.offset_y == Approx(-2.0f));
    CHECK(front.opacity == Approx(0.25f));
    CHECK(front.parallax_x == Approx(0.25f));
    CHECK(front.tint[0] == Approx(1.0f));
    CHECK(front.tint[1] == Approx(128.0f / 255.0f));
    CHECK(front.tint[2] == Approx(0.0f));
    CHECK(front.properties[0].value == "2");
    REQUIRE(front.tiles.size() == 6);
    CHECK(front.tiles[2] == (3u | k_flipped_horizontally));
    CHECK(front.tiles[5] == (10u | k_flipped_vertically));

    const Layer& encoded = map.layers[2];
    CHECK_FALSE(encoded.visible);
    CHECK(encoded.tiles == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6});

    const Layer& things = map.layers[3];
    CHECK(things.kind == LayerKind::Objects);
    REQUIRE(things.objects.size() == 4);
    CHECK(things.objects[0].shape == ObjectShape::Point);
    CHECK(things.objects[0].type == "spawn");
    CHECK(things.objects[1].type == "trigger");
    CHECK(things.objects[1].rotation == Approx(90.0f));
    REQUIRE(things.objects[1].properties.size() == 2);
    CHECK(things.objects[1].properties[1].value == "2.5");
    CHECK(things.objects[2].tile == 10);
    // A polygon as the rectangle around its points.
    CHECK(things.objects[3].shape == ObjectShape::Polygon);
    CHECK(things.objects[3].x == Approx(10.0f));
    CHECK(things.objects[3].y == Approx(6.0f));
    CHECK(things.objects[3].width == Approx(20.0f));
    CHECK(things.objects[3].height == Approx(12.0f));
}

TEST_CASE("Tiled tilesets in files of their own are read") {
    const auto reader = make();
    const core::Result<Tileset> read = reader->read_tileset(bytes(R"({
        "type": "tileset", "name": "props", "image": "props.png", "imagewidth": 32,
        "imageheight": 48, "tilewidth": 16, "tileheight": 24, "tilecount": 4,
        "margin": 1, "spacing": 2})"),
                                                            "props.tsj");
    if (!read.has_value()) {
        FAIL(read.error().message);
    }
    CHECK(read.value().name == "props");
    CHECK(read.value().tile_height == 24);
    // Not given: from the image's width.
    CHECK(read.value().columns == 2);
    CHECK(read.value().margin == 1);
    CHECK(read.value().spacing == 2);
}

TEST_CASE("Tiled files that can't be read say why") {
    const auto reader = make();
    const auto fails_with = [&](std::string_view json, std::string_view expected) {
        const core::Result<Map> read = reader->read_map(bytes(json), "bad.tmj");
        REQUIRE_FALSE(read.has_value());
        INFO(read.error().message);
        CHECK(read.error().message.find(expected) != std::string::npos);
    };
    const std::string base =
        R"("type": "map", "width": 1, "height": 1, "tilewidth": 8, "tileheight": 8)";
    fails_with("{ not json", "not JSON");
    fails_with(R"({"type": "tileset"})", "not a Tiled map");
    fails_with("{" + base + R"(, "orientation": "isometric"})", "only orthogonal");
    fails_with("{" + base + R"(, "orientation": "orthogonal", "infinite": true})", "infinite");
    fails_with("{" + base + R"(, "orientation": "orthogonal", "tilesets": [
        {"firstgid": 1, "source": "a.tsx"}]})",
               "export it as JSON");
    fails_with("{" + base + R"(, "orientation": "orthogonal", "tilesets": [
        {"firstgid": 1, "name": "bits", "tilewidth": 8, "tileheight": 8}]})",
               "collection of images");
    fails_with("{" + base + R"(, "orientation": "orthogonal", "layers": [
        {"type": "tilelayer", "name": "a", "width": 1, "height": 1, "encoding": "base64",
         "compression": "zlib", "data": "eJxjYGBgAAAABAAB"}]})",
               "compressed with zlib");
    fails_with("{" + base + R"(, "orientation": "orthogonal", "layers": [
        {"type": "tilelayer", "name": "a", "width": 2, "height": 1, "data": [1]}]})",
               "has 1 cells");
    CHECK_FALSE(reader->read_tileset(bytes(R"({"type": "map"})"), "x.tsj").has_value());
}
