#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "markup/markup.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <ostream> // Needed for doctest to print std::string.
#include <set>
#include <string>
#include <vector>

using namespace markup;

namespace {

// Files from memory or disk, and textures that are only counted.
class TestHost final : public Host {
public:
    std::optional<std::vector<std::byte>> read_file(std::string_view path) override {
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return std::nullopt;
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(found->second.data());
        return std::vector<std::byte>(bytes, bytes + found->second.size());
    }

    std::optional<TextureId> create_texture(std::uint32_t width, std::uint32_t height,
                                            std::span<const std::byte> rgba, bool) override {
        if (rgba.size() != std::size_t{width} * height * 4) {
            return std::nullopt;
        }
        live.insert(++next);
        return next;
    }

    std::optional<Image> load_image(std::string_view path) override {
        if (!files.contains(std::string(path))) {
            return std::nullopt;
        }
        live.insert(++next);
        return Image{next, 40, 20};
    }

    void release_texture(TextureId texture) override {
        live.erase(texture);
    }

    void add_disk_file(const std::string& name, const std::string& disk_path) {
        std::ifstream stream(disk_path, std::ios::binary);
        files[name] = std::string(std::istreambuf_iterator<char>(stream), {});
    }

    std::map<std::string, std::string> files;
    std::set<TextureId> live;
    TextureId next = 0;
};

// A phone running the tests can point at its own copies of the fonts and sample files, since the
// built-in paths are on the build machine.
std::string test_path(const char* variable, const char* built_in) {
#if defined(_WIN32)
    static_cast<void>(variable);
    return built_in;
#else
    const char* const set = std::getenv(variable);
    return set != nullptr ? set : built_in;
#endif
}

struct Fixture {
    TestHost host;
    Context ui{host};

    Fixture() {
        host.add_disk_file("assets:/fonts/regular.ttf",
                           test_path("DREAM_TEST_FONT", DREAM_TEST_FONT));
        host.add_disk_file("assets:/fonts/bold.ttf",
                           test_path("DREAM_TEST_BOLD_FONT", DREAM_TEST_BOLD_FONT));
        REQUIRE(ui.load_font("assets:/fonts/regular.ttf"));
        REQUIRE(ui.load_font("assets:/fonts/bold.ttf"));
    }

    DocumentId load(const std::string& body, const std::string& style = {}) {
        host.files["assets:/ui/test.rml"] =
            "<rml><head><style>body { font-family: LatoLatin; font-size: 20px; } " + style +
            "</style></head><body>" + body + "</body></rml>";
        const auto document = ui.load_document("assets:/ui/test.rml");
        REQUIRE(document.has_value());
        ui.show(*document);
        frame();
        return *document;
    }

    void frame(float width = 800.0f, float height = 600.0f, float scale = 1.0f) {
        ui.update(width, height, 1.0f / 60.0f, scale);
    }

    void click(float x, float y) {
        ui.pointer_moved(x, y);
        ui.pointer_button(Button::Left, true);
        ui.pointer_button(Button::Left, false);
    }

    // The bounds of everything drawn, to check where things went.
    struct Bounds {
        float left = 1e9f;
        float top = 1e9f;
        float right = -1e9f;
        float bottom = -1e9f;
    };
    Bounds drawn() {
        const DrawList& list = ui.render();
        Bounds bounds;
        for (const Vertex& v : list.vertices) {
            bounds.left = std::min(bounds.left, v.x);
            bounds.top = std::min(bounds.top, v.y);
            bounds.right = std::max(bounds.right, v.x);
            bounds.bottom = std::max(bounds.bottom, v.y);
        }
        return bounds;
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "markup draws shown documents only, as valid premultiplied triangles") {
    const DocumentId document = load(R"(<div id="box">Hello world</div>)",
                                     "div { display: block; background-color: #ff000080; }");
    const DrawList& list = ui.render();
    REQUIRE_FALSE(list.commands.empty());
    CHECK_FALSE(host.live.empty());
    for (const DrawCommand& command : list.commands) {
        CHECK(command.first_index + command.index_count <= list.indices.size());
    }
    for (const std::uint32_t index : list.indices) {
        CHECK(index < list.vertices.size());
    }
    // Half-transparent red, premultiplied.
    CHECK(list.vertices.front().color == 0x80000080u);

    ui.hide(document);
    frame();
    CHECK(ui.render().commands.empty());
}

TEST_CASE_FIXTURE(Fixture, "markup places absolute boxes, in pixels or dp") {
    load(R"(<div id="a"></div><div id="b"></div>)",
         "div { position: absolute; background-color: #fff; } "
         "#a { left: 100px; top: 50px; width: 200px; height: 40px; } "
         "#b { right: 10dp; bottom: 10dp; width: 20dp; height: 20dp; }");
    Bounds bounds = drawn();
    CHECK(bounds.left == doctest::Approx(100.0f));
    CHECK(bounds.top == doctest::Approx(50.0f));
    CHECK(bounds.right == doctest::Approx(800.0f - 10.0f));
    CHECK(bounds.bottom == doctest::Approx(600.0f - 10.0f));

    // At two pixels per dp, #b is twice as big and twice as far from the corner.
    frame(800.0f, 600.0f, 2.0f);
    bounds = drawn();
    CHECK(bounds.right == doctest::Approx(800.0f - 20.0f));
    CHECK(bounds.bottom == doctest::Approx(600.0f - 20.0f));
}

TEST_CASE_FIXTURE(Fixture, "markup stacks blocks with margins, borders and padding") {
    load(R"(<div id="one"></div><div id="two"></div>)",
         "div { display: block; height: 30px; margin: 10px 20px; padding: 5px; "
         "border: 2px #00ff00; background-color: #fff; } body { padding: 0; }");
    // Each block: 10 margin, 2 border, 5 padding, 30 content, 5 padding, 2 border, 10 margin.
    const Bounds bounds = drawn();
    CHECK(bounds.left == doctest::Approx(20.0f));
    CHECK(bounds.right == doctest::Approx(780.0f));
    CHECK(bounds.top == doctest::Approx(10.0f));
    CHECK(bounds.bottom == doctest::Approx(10.0f + 44.0f + 20.0f + 44.0f));
}

TEST_CASE_FIXTURE(Fixture, "markup wraps text to its box and aligns it") {
    load(R"(<p id="text">one two three four five six seven eight nine ten</p>)",
         "p { display: block; width: 120px; text-align: right; }");
    const Bounds bounds = drawn();
    // Several lines, each ending at the right edge.
    CHECK(bounds.bottom - bounds.top > 3.0f * 20.0f);
    // Glyphs may reach a pixel past their advance.
    CHECK(bounds.right <= 120.0f + 1.5f);
    CHECK(bounds.right > 110.0f);
}

TEST_CASE_FIXTURE(Fixture, "markup cascades by specificity, and reacts to classes and hover") {
    const DocumentId document =
        load(R"(<div id="target" class="plain">x</div>)",
             "div { display: block; width: 100px; height: 100px; background-color: #0000ff; } "
             "#target { background-color: #00ff00; } "
             "div.plain { background-color: #ff0000; } "
             "#target.hot { background-color: #ffffff; } "
             "#target:hover { background-color: #000000; }");
    // The id outranks the class, which outranks the tag.
    CHECK((ui.render().vertices.front().color & 0xffffffu) == 0x00ff00u);

    REQUIRE(ui.set_class(document, "target", "hot", true));
    frame();
    CHECK((ui.render().vertices.front().color & 0xffffffu) == 0xffffffu);

    ui.pointer_moved(50.0f, 50.0f);
    CHECK(ui.wants_pointer());
    frame();
    CHECK((ui.render().vertices.front().color & 0xffffffu) == 0x000000u);
    CHECK_FALSE(ui.set_class(document, "missing", "hot", true));
}

TEST_CASE_FIXTURE(Fixture, "markup reports clicks by the nearest element with an id") {
    const DocumentId document =
        load(R"(<div id="button"><span>press</span></div><div class="anonymous">nothing</div>)",
             "div { display: block; height: 50px; }");
    click(5.0f, 10.0f);
    frame();
    REQUIRE(ui.events().size() == 1);
    CHECK(ui.events()[0].kind == Event::Kind::Click);
    CHECK(ui.events()[0].element_id == "button");
    CHECK(ui.events()[0].document == document);

    // Clicks are delivered once, and clicks on elements with no named ancestor are not.
    frame();
    CHECK(ui.events().empty());
    click(5.0f, 60.0f);
    frame();
    CHECK(ui.events().empty());

    // A press that is released somewhere else is no click.
    ui.pointer_moved(5.0f, 10.0f);
    ui.pointer_button(Button::Left, true);
    ui.pointer_moved(5.0f, 400.0f);
    ui.pointer_button(Button::Left, false);
    frame();
    CHECK(ui.events().empty());
}

TEST_CASE_FIXTURE(Fixture, "markup checkboxes toggle and report their value") {
    const DocumentId document =
        load(R"(<input id="sound" type="checkbox" checked/>)",
             "input.checkbox { display: block; width: 20px; height: 20px; } "
             "input:checked { background-color: #ffffff; }");
    CHECK(ui.value(document, "sound") == "on");
    click(10.0f, 10.0f);
    frame();
    CHECK(ui.value(document, "sound") == "");
    bool changed = false;
    for (const Event& event : ui.events()) {
        changed = changed || (event.kind == Event::Kind::Change && event.element_id == "sound" &&
                              event.value.empty());
    }
    CHECK(changed);
}

TEST_CASE_FIXTURE(Fixture, "markup sliders follow the pointer in steps") {
    const DocumentId document =
        load(R"(<input id="volume" type="range" min="0" max="1" step="0.25" value="0.5"/>)",
             "input.range { display: block; width: 200px; height: 20px; } "
             "input.range sliderbar { width: 20px; height: 20px; background-color: #fff; } "
             "input.range slidertrack { height: 4px; margin-top: 8px; background-color: #888; }");
    CHECK(ui.value(document, "volume") == "0.5");

    // The bar's center travels from 10 to 190 pixels.
    click(190.0f, 10.0f);
    frame();
    CHECK(ui.value(document, "volume") == "1");
    bool changed = false;
    for (const Event& event : ui.events()) {
        changed = changed || (event.kind == Event::Kind::Change && event.value == "1");
    }
    CHECK(changed);

    ui.pointer_moved(190.0f, 10.0f);
    ui.pointer_button(Button::Left, true);
    ui.pointer_moved(60.0f, 10.0f);
    CHECK(ui.value(document, "volume") == "0.25");
    ui.pointer_moved(-100.0f, 10.0f);
    ui.pointer_button(Button::Left, false);
    CHECK(ui.value(document, "volume") == "0");
}

TEST_CASE_FIXTURE(Fixture, "markup text fields take typing where the caret is") {
    const DocumentId document =
        load(R"(<input id="name" type="text" value="Ann"/><input id="secret" type="password"/>)",
             "input { display: block; width: 200px; height: 30px; }");
    CHECK_FALSE(ui.wants_text());
    click(150.0f, 15.0f);
    CHECK(ui.wants_text());
    CHECK(ui.key(Key::End, true));
    CHECK(ui.text("e"));
    CHECK(ui.value(document, "name") == "Anne");
    CHECK(ui.key(Key::Home, true));
    CHECK(ui.key(Key::Delete, true));
    CHECK(ui.text("J"));
    CHECK(ui.value(document, "name") == "Jnne");
    CHECK(ui.key(Key::Backspace, true));
    CHECK(ui.value(document, "name") == "nne");

    // Typing goes nowhere once another field, or nothing, has focus.
    click(150.0f, 45.0f);
    CHECK(ui.text("pw"));
    CHECK(ui.value(document, "secret") == "pw");
    CHECK(ui.value(document, "name") == "nne");
    click(500.0f, 500.0f);
    CHECK_FALSE(ui.wants_text());
    CHECK_FALSE(ui.text("x"));
}

TEST_CASE_FIXTURE(Fixture, "markup replaces text and reads missing elements as missing") {
    const DocumentId document = load(R"(<div id="score">0</div>)", "div { display: block; }");
    CHECK(ui.set_text(document, "score", "42 <points>"));
    frame();
    CHECK_FALSE(ui.set_text(document, "missing", "x"));
    CHECK_FALSE(ui.value(document, "missing").has_value());
    CHECK(ui.value(document, "score") == "");

    ui.close(document);
    ui.close(document);
    CHECK_FALSE(ui.is_visible(document));
    CHECK_FALSE(ui.set_text(document, "score", "1"));
}

TEST_CASE_FIXTURE(Fixture, "markup refuses missing files and documents without a body") {
    CHECK_FALSE(ui.load_document("assets:/ui/missing.rml").has_value());
    CHECK_FALSE(ui.load_font("assets:/fonts/missing.ttf"));
    host.files["assets:/fonts/broken.ttf"] = "not a font";
    CHECK_FALSE(ui.load_font("assets:/fonts/broken.ttf"));
    host.files["assets:/ui/empty.rml"] = "<rml><head></head></rml>";
    CHECK_FALSE(ui.load_document("assets:/ui/empty.rml").has_value());
    CHECK_FALSE(ui.last_error().empty());
}

TEST_CASE_FIXTURE(Fixture, "markup shows images at their size") {
    host.files["assets:/ui/logo.png"] = "png";
    load(R"(<img src="logo.png"/>)");
    const Bounds bounds = drawn();
    CHECK(bounds.right - bounds.left == doctest::Approx(40.0f));
    CHECK(bounds.bottom - bounds.top == doctest::Approx(20.0f));
}

// The arcade demo's own interface files, read from the samples, must lay out as their style sheet
// says and react to the pointer as RmlUi does with them.
TEST_CASE_FIXTURE(Fixture, "markup lays out the arcade demo's interface") {
    const std::string ui_folder =
        test_path("DREAM_TEST_SAMPLES", DREAM_TEST_SAMPLES) + "/arcade_demo/assets/ui/";
    host.add_disk_file("assets:/ui/hud.rml", ui_folder + "hud.rml");
    host.add_disk_file("assets:/ui/menu.rml", ui_folder + "menu.rml");
    host.add_disk_file("assets:/ui/style.rcss", ui_folder + "style.rcss");
    REQUIRE(host.files["assets:/ui/hud.rml"].find("score_box") != std::string::npos);

    const auto hud = ui.load_document("assets:/ui/hud.rml");
    const auto menu = ui.load_document("assets:/ui/menu.rml");
    REQUIRE(hud.has_value());
    REQUIRE(menu.has_value());
    ui.show(*hud);
    frame(1280.0f, 720.0f);
    const Bounds hud_bounds = drawn();
    // #score_box is 16 dp from the top-left corner, and #menu 16 dp from the top-right.
    CHECK(hud_bounds.left == doctest::Approx(16.0f));
    CHECK(hud_bounds.top == doctest::Approx(16.0f));
    CHECK(hud_bounds.right == doctest::Approx(1280.0f - 16.0f));

    // The menu button, at the top right, takes clicks.
    click(1280.0f - 40.0f, 30.0f);
    frame(1280.0f, 720.0f);
    REQUIRE(ui.events().size() == 1);
    CHECK(ui.events()[0].element_id == "menu");

    ui.show(*menu);
    frame(1280.0f, 720.0f);
    CHECK(ui.value(*menu, "music") == "0.6");
    CHECK(ui.value(*menu, "debug") == "");

    // The panel is centered: 300 dp wide plus 20 dp padding, moved left 170 dp from the middle.
    // Its first button, Resume, is under its title.
    click(640.0f, 360.0f - 200.0f + 20.0f + 60.0f);
    frame(1280.0f, 720.0f);
    REQUIRE_FALSE(ui.events().empty());
    CHECK(ui.events().back().element_id == "resume");

    // The debug checkbox toggles.
    bool toggled = false;
    for (float y = 360.0f; y < 560.0f && !toggled; y += 4.0f) {
        click(640.0f - 170.0f + 20.0f + 11.0f, y);
        frame(1280.0f, 720.0f);
        for (const Event& event : ui.events()) {
            toggled = toggled || (event.element_id == "debug" && event.kind == Event::Kind::Change);
        }
    }
    CHECK(toggled);
    CHECK(ui.value(*menu, "debug") == "on");
}
