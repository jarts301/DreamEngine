#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/debug_ui/debug_ui.h"
#include "tests/fake_host.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_debug_ui_imgui(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::debug_ui;

namespace {

std::unique_ptr<DebugUi> make_ui(tests::FakeHost& host) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_debug_ui_imgui(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"imgui"};
    auto created = registry.create<DebugUi>(preference, config);
    REQUIRE(created.has_value());
    REQUIRE(created.value().instance->initialize(host).has_value());
    return std::move(created.value().instance);
}

constexpr ui::FrameInfo k_frame{.size = {800, 600}, .delta_seconds = 1.0f / 60.0f};

// One frame with a window of every widget.
const ui::DrawList& draw_panel(DebugUi& tools, float& value, bool& flag, float scale = 1.0f) {
    ui::FrameInfo frame = k_frame;
    frame.display_scale = scale;
    tools.begin_frame(frame);
    if (tools.begin_window("Stats")) {
        tools.text("Hello");
        tools.button("Reset");
        tools.same_line();
        tools.checkbox("Flag", flag);
        tools.slider("Value", value, 0.0f, 10.0f);
        int count = 3;
        tools.slider("Count", count, 0, 5);
        tools.separator();
        const std::array<float, 4> times{1.0f, 2.0f, 1.5f, 3.0f};
        tools.plot("Times", times, 0.0f, 4.0f);
    }
    tools.end_window();
    return tools.end_frame();
}

} // namespace

TEST_CASE("ImGui draws a window with every widget, using a font texture it asked for") {
    tests::FakeHost host;
    float value = 2.0f;
    bool flag = false;
    {
        const auto tools = make_ui(host);
        // ImGui hides a new window for its first frame, while it measures it.
        draw_panel(*tools, value, flag);
        const ui::DrawList& list = draw_panel(*tools, value, flag);

        CHECK_FALSE(list.premultiplied_alpha);
        CHECK(list.vertices.size() > 100);
        REQUIRE_FALSE(list.commands.empty());
        CHECK(host.live.size() == 1);
        for (const ui::DrawCommand& command : list.commands) {
            CHECK(command.first_index + command.index_count <= list.indices.size());
            CHECK(command.clip.has_value());
        }
        for (const std::uint32_t index : list.indices) {
            CHECK(index < list.vertices.size());
        }
    }
    // Destroying it releases the texture.
    CHECK(host.live.empty());
}

TEST_CASE("ImGui wants the pointer while it is over a window") {
    tests::FakeHost host;
    const auto tools = make_ui(host);
    float value = 2.0f;
    bool flag = false;
    draw_panel(*tools, value, flag);

    tools->handle_event(platform::PointerMoved{700.0f, 550.0f});
    draw_panel(*tools, value, flag);
    CHECK_FALSE(tools->wants_pointer());

    // New windows open near the top-left corner.
    tools->handle_event(platform::PointerMoved{80.0f, 80.0f});
    draw_panel(*tools, value, flag);
    CHECK(tools->wants_pointer());
    CHECK(tools->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, true}));
    tools->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, false});
    tools->handle_event(platform::PointerWheel{0.0f, 1.0f});
    tools->handle_event(platform::KeyChanged{platform::Key::LeftControl, true});
    tools->handle_event(platform::TextEntered{"abc"});
    draw_panel(*tools, value, flag);
}

TEST_CASE("ImGui grows its windows with the display scale") {
    tests::FakeHost host;
    const auto tools = make_ui(host);
    float value = 2.0f;
    bool flag = false;

    const auto widest = [](const ui::DrawList& list) {
        float right = 0.0f;
        for (const ui::Vertex& vertex : list.vertices) {
            right = std::max(right, vertex.position.x);
        }
        return right;
    };
    const float normal = widest(draw_panel(*tools, value, flag));
    draw_panel(*tools, value, flag, 2.0f);
    const float doubled = widest(draw_panel(*tools, value, flag, 2.0f));
    CHECK(doubled > normal * 1.3f);
}

TEST_CASE("ImGui's text field keeps what is typed until it is left, then hands it over") {
    tests::FakeHost host;
    const auto tools = make_ui(host);
    std::string name = "crate";
    std::string other = "lid";
    bool committed = false;
    const auto frame = [&] {
        tools->begin_frame(k_frame);
        tools->next_window_placement(core::Vec2{0.0f, 0.0f}, core::Vec2{400.0f, 200.0f});
        if (tools->begin_window("Inspector")) {
            committed = tools->input_text("Name", name) || committed;
            // Another field in the same window doesn't disturb the one being typed in.
            tools->input_text("Other", other);
        }
        tools->end_window();
        static_cast<void>(tools->end_frame());
    };
    frame();
    frame();
    // The first field, just under the title bar.
    tools->handle_event(platform::PointerMoved{40.0f, 32.0f});
    frame();
    tools->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, true});
    frame();
    tools->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, false});
    frame();
    REQUIRE(tools->wants_keyboard());
    tools->handle_event(platform::TextEntered{"_box"});
    frame();
    frame();
    // Nothing yet: the field is still being typed in.
    CHECK_FALSE(committed);
    CHECK(name == "crate");
    tools->handle_event(platform::KeyChanged{platform::Key::Enter, true});
    frame();
    tools->handle_event(platform::KeyChanged{platform::Key::Enter, false});
    frame();
    frame();
    CHECK(committed);
    // Typed where the click put the cursor, keeping the rest.
    CAPTURE(name);
    CHECK(name.size() == std::string("crate_box").size());
    CHECK(name.find("_box") != std::string::npos);
    CHECK(name.starts_with("c"));
    CHECK(other == "lid");
    CHECK_FALSE(tools->wants_keyboard());
}

TEST_CASE("ImGui draws the editor's widgets") {
    tests::FakeHost host;
    const auto tools = make_ui(host);
    for (int pass = 0; pass < 2; ++pass) {
        tools->begin_frame(k_frame);
        if (tools->begin_window("Widgets")) {
            bool clicked = false;
            if (tools->tree_node("root", false, true, clicked)) {
                tools->tree_node("leaf", true, false, clicked);
                tools->tree_pop();
                tools->tree_pop();
            }
            tools->selectable("row", false);
            if (tools->collapsing_header("transform")) {
                std::array<float, 3> position{1.0f, 2.0f, 3.0f};
                tools->drag("position", position, 0.01f);
                int count = 2;
                tools->drag("count", count, 0.1f);
                std::array<float, 3> rgb{1.0f, 0.5f, 0.0f};
                tools->color("color", rgb);
                const std::array<std::string, 2> items{"pixel", "smooth"};
                int chosen = 0;
                tools->combo("filter", chosen, items);
                tools->tooltip("help");
                tools->text_disabled("(default)");
            }
        }
        tools->end_window();
        const ui::DrawList& list = tools->end_frame();
        if (pass == 1) {
            CHECK(list.vertices.size() > 100);
        }
    }
}
