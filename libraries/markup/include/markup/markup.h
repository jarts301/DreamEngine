#pragma once

// markup: a small user interface library for games that reads documents in an HTML-like markup
// with CSS style sheets, the RML and RCSS that RmlUi reads, so the same files work with both.
//
// It supports a useful subset: block, inline, inline-block and absolutely positioned boxes;
// margins, borders, padding and rounded corners; colors and opacity; TrueType text with wrapping
// and alignment; selectors by tag, id, class, descendant and child, with :hover, :active, :focus
// and :checked; and buttons, sliders, checkboxes and text fields. It has no scrolling, animation,
// transforms or tables.
//
// It draws nothing itself: it describes triangles in a DrawList for the embedder to draw, and gets
// files and textures through a Host. Not thread-safe: use a context from one thread at a time.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markup {

// Chosen by the host. Zero means no texture: plain color.
using TextureId = std::uint64_t;

struct Vertex {
    // In pixels from the top-left corner.
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;

    // RGBA, eight bits each, red in the lowest byte, as sRGB, multiplied by alpha.
    std::uint32_t color = 0xffffffff;
};

struct ClipRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct DrawCommand {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    TextureId texture = 0;
    std::optional<ClipRect> clip;
};

// Triangles in drawing order. Colors, and texture colors, are premultiplied by alpha.
struct DrawList {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<DrawCommand> commands;

    void clear() noexcept {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
};

struct Image {
    TextureId texture = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// What the library needs from the program that embeds it.
class Host {
public:
    [[nodiscard]] virtual std::optional<std::vector<std::byte>>
    read_file(std::string_view path) = 0;

    // `rgba` holds width * height pixels of four bytes, row by row from the top. Coverage textures
    // hold data, such as glyph masks; others hold sRGB colors.
    [[nodiscard]] virtual std::optional<TextureId> create_texture(std::uint32_t width,
                                                                  std::uint32_t height,
                                                                  std::span<const std::byte> rgba,
                                                                  bool coverage) = 0;

    // A PNG or JPEG file as a texture of colors premultiplied by alpha.
    [[nodiscard]] virtual std::optional<Image> load_image(std::string_view path) = 0;

    virtual void release_texture(TextureId texture) = 0;

protected:
    ~Host() = default;
};

struct DocumentId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const DocumentId&, const DocumentId&) = default;
};

// Something the player did to an element with an id.
struct Event {
    // Click: a press and release on the element. Change: a form control's value changed, to
    // `value`. Submit: a form was sent.
    enum class Kind : std::uint8_t { Click, Change, Submit };

    Kind kind = Kind::Click;
    DocumentId document;
    std::string element_id;
    std::string value;
};

enum class Key : std::uint8_t {
    Other,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Backspace,
    Delete,
    Enter,
    Tab,
    Escape,
};

enum class Button : std::uint8_t { Left, Right, Middle };

class Context {
public:
    explicit Context(Host& host);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // The reason the last failing call failed.
    [[nodiscard]] const std::string& last_error() const;

    // A TrueType or OpenType font. Fonts are chosen by the family name and weight they record; the
    // first one loaded stands in for any family not loaded.
    [[nodiscard]] bool load_font(std::string_view path);

    // Loads a document, hidden until shown. Style sheets and images it names are relative to it.
    [[nodiscard]] std::optional<DocumentId> load_document(std::string_view path);

    // Showing a document also brings it in front of the others.
    void show(DocumentId document);
    void hide(DocumentId document);
    void close(DocumentId document);
    [[nodiscard]] bool is_visible(DocumentId document) const;

    // Replaces an element's contents with plain text. False if there is no such element.
    [[nodiscard]] bool set_text(DocumentId document, std::string_view element_id,
                                std::string_view text);

    [[nodiscard]] bool set_class(DocumentId document, std::string_view element_id,
                                 std::string_view name, bool enabled);

    // A form control's value, or another element's value attribute.
    [[nodiscard]] std::optional<std::string> value(DocumentId document,
                                                   std::string_view element_id) const;

    // Input. Each returns true if the interface used it.
    bool pointer_moved(float x, float y);
    bool pointer_button(Button button, bool pressed);
    bool wheel(float dx, float dy);
    bool key(Key key, bool pressed);
    bool text(std::string_view utf8);

    // Styles and lays out the documents for a view of this size, in pixels. `scale` is the number
    // of pixels per dp, the style sheets' density-independent unit. Events from the input since the
    // last update become available from events().
    void update(float width, float height, float delta_seconds, float scale);

    [[nodiscard]] std::span<const Event> events() const;

    // Whether the pointer is over an element of a visible document, other than its background.
    [[nodiscard]] bool wants_pointer() const;

    // Whether a text field has focus.
    [[nodiscard]] bool wants_text() const;

    // The triangles for the layout of the last update. Valid until the next call.
    [[nodiscard]] const DrawList& render();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace markup
