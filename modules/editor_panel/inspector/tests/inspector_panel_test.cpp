#include "dream/core/module_registry.h"
#include "fake_editor.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_editor_panel_inspector(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::editor_panel;
using namespace dream::editor_panel::tests;
using core::ConfigValue;
using core::FieldInfo;
using core::FieldKind;

namespace {

std::unique_ptr<EditorPanel> make_panel() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_editor_panel_inspector(registry).has_value());
    const std::vector<std::string> names{"inspector"};
    auto created = registry.create<EditorPanel>(names, core::Config::parse("").value());
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

FakeScene crate_scene() {
    FakeScene scene;
    scene.types = {
        core::ComponentInfo{
            .name = "transform",
            .fields = {FieldInfo{.name = "position",
                                 .kind = FieldKind::Vec3,
                                 .fallback = std::vector<double>{0.0, 0.0, 0.0}},
                       FieldInfo{.name = "scale",
                                 .kind = FieldKind::Vec3,
                                 .fallback = std::vector<double>{1.0, 1.0, 1.0}}}},
        core::ComponentInfo{
            .name = "mesh",
            .fields = {FieldInfo{.name = "model", .kind = FieldKind::Asset, .fallback = std::string()},
                       FieldInfo{.name = "casts_shadows", .kind = FieldKind::Flag, .fallback = true}}},
        core::ComponentInfo{
            .name = "sprite",
            .fields = {FieldInfo{.name = "filter",
                                 .kind = FieldKind::Choice,
                                 .fallback = std::string("pixel"),
                                 .choices = {"pixel", "smooth"}}}},
    };
    scene.scene = {
        {"crate",
         "",
         {{"transform", {{"position", std::vector<double>{1.0, 2.0, 3.0}}}},
          {"spin", {{"speed", 0.5}, {"axis", std::string("y")}, {"turns", std::int64_t{3}}}}}},
        {"ground", "", {}}};
    scene.select("crate");
    return scene;
}

} // namespace

TEST_CASE("The inspector shows described fields, defaults included, and edits them") {
    const std::unique_ptr<EditorPanel> panel = make_panel();
    CHECK(panel->title() == "Inspector");
    CHECK(panel->side() == EditorPanel::Side::Right);
    FakeScene scene = crate_scene();
    FakeUi ui;
    ui.drags["position##transform.position"] = {4.0f, 5.0f, 6.0f};
    ui.drags["scale##transform.scale"] = {2.0f, 2.0f, 2.0f};
    panel->draw(ui, scene);
    CHECK(ui.showed("(default)")); // The scale, which the scene leaves out.
    CHECK(scene.setting("crate", "transform", "position") ==
          ConfigValue{std::vector<double>{4.0, 5.0, 6.0}});
    CHECK(scene.setting("crate", "transform", "scale") ==
          ConfigValue{std::vector<double>{2.0, 2.0, 2.0}});
}

TEST_CASE("The inspector edits a game's own components by their values' types") {
    const std::unique_ptr<EditorPanel> panel = make_panel();
    FakeScene scene = crate_scene();
    FakeUi ui;
    ui.drags["speed##spin.speed"] = {0.30000001f};
    ui.typed["axis##spin.axis"] = "x";
    ui.whole_drags["turns##spin.turns"] = 7;
    panel->draw(ui, scene);
    CHECK(ui.showed("A game's own component, edited by its values"));
    // Rounded as typed, not as a float holds it.
    CHECK(scene.setting("crate", "spin", "speed") == ConfigValue{0.3});
    CHECK(scene.setting("crate", "spin", "axis") == ConfigValue{std::string("x")});
    CHECK(scene.setting("crate", "spin", "turns") == ConfigValue{std::int64_t{7}});

    FakeUi remove;
    remove.clicks.insert("x##remove.spin.axis");
    panel->draw(remove, scene);
    CHECK_FALSE(scene.setting("crate", "spin", "axis").has_value());
}

TEST_CASE("The inspector renames, reparents, and adds and removes components") {
    const std::unique_ptr<EditorPanel> panel = make_panel();
    FakeScene scene = crate_scene();
    FakeUi ui;
    ui.typed["Name"] = "box";
    panel->draw(ui, scene);
    CHECK(scene.selected() == "box");

    // Choices: (none), then the others, so 1 is the ground.
    FakeUi parent;
    parent.choices["Parent"] = 1;
    panel->draw(parent, scene);
    CHECK(scene.parent("box") == "ground");

    // Missing types, in order: mesh, then sprite.
    FakeUi add;
    add.choices["##add_component"] = 1;
    add.clicks.insert("Add component");
    panel->draw(add, scene);
    CHECK(scene.components("box").back() == "sprite");

    FakeUi choose;
    choose.choices["filter##sprite.filter"] = 1;
    panel->draw(choose, scene);
    CHECK(scene.setting("box", "sprite", "filter") == ConfigValue{std::string("smooth")});

    FakeUi remove;
    remove.clicks.insert("Remove spin##remove.spin");
    panel->draw(remove, scene);
    CHECK(scene.components("box") == std::vector<std::string>{"transform", "sprite"});

    FakeUi nothing;
    scene.select("");
    panel->draw(nothing, scene);
    CHECK(nothing.showed("Select an entity to see its components"));
}
