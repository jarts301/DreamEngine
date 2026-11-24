#include "dream/core/module_registry.h"
#include "fake_editor.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_editor_panel_hierarchy(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::editor_panel;
using namespace dream::editor_panel::tests;

namespace {

std::unique_ptr<EditorPanel> make_panel() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_editor_panel_hierarchy(registry).has_value());
    const std::vector<std::string> names{"hierarchy"};
    auto created = registry.create<EditorPanel>(names, core::Config::parse("").value());
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

FakeScene yard() {
    FakeScene scene;
    scene.scene = {{"camera", "", {}}, {"crate", "", {}}, {"lid", "crate", {}}, {"sun", "", {}}};
    return scene;
}

} // namespace

TEST_CASE("The entity tree shows children under their parents, and selects what is clicked") {
    const std::unique_ptr<EditorPanel> panel = make_panel();
    CHECK(panel->title() == "Entities");
    FakeScene scene = yard();
    FakeUi ui;
    ui.clicks.insert("lid");
    panel->draw(ui, scene);
    CHECK(scene.selected() == "lid");
    // Depth first: the lid right after its crate.
    std::vector<std::string> order;
    for (const std::string& label : ui.shown) {
        if (label == "camera" || label == "crate" || label == "lid" || label == "sun") {
            order.push_back(label);
        }
    }
    CHECK(order == std::vector<std::string>{"camera", "crate", "lid", "sun"});

    // A closed node hides its children, which don't show at the top either.
    FakeUi closed;
    closed.closed.insert("crate");
    panel->draw(closed, scene);
    CHECK_FALSE(closed.showed("lid"));
}

TEST_CASE("The entity tree adds entities inside the selected one, and removes them") {
    const std::unique_ptr<EditorPanel> panel = make_panel();
    FakeScene scene = yard();
    scene.select("crate");
    FakeUi ui;
    ui.clicks.insert("Add##entity");
    panel->draw(ui, scene);
    REQUIRE(scene.find("entity") != nullptr);
    CHECK(scene.parent("entity") == "crate");
    CHECK(scene.components("entity") == std::vector<std::string>{"transform"});
    CHECK(scene.selected() == "entity");

    scene.select("crate");
    FakeUi remove;
    remove.clicks.insert("Remove##entity");
    panel->draw(remove, scene);
    CHECK(scene.find("crate") == nullptr);
    CHECK(scene.parent("lid").empty());

    // Entities in a loop of parents still show, so they can be fixed.
    FakeScene looped;
    looped.scene = {{"a", "b", {}}, {"b", "a", {}}};
    FakeUi loop_ui;
    panel->draw(loop_ui, looped);
    CHECK(loop_ui.showed("a"));
    CHECK(loop_ui.showed("b"));
}
