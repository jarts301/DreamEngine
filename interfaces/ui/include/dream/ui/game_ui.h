#pragma once

#include "dream/core/handle.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/ui/draw_list.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dream::ui {

// The interface players see: menus and HUDs, written as documents and style sheets, such as RmlUi's
// HTML-like RML with CSS.

struct DocumentTag;
using DocumentHandle = core::Handle<DocumentTag>;

// For backends, which keep their own types in a core::HandlePool.
template <typename To, typename From>
[[nodiscard]] constexpr core::Handle<To> handle_cast(core::Handle<From> handle) noexcept {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

// Something the player did to an element with an id.
struct UiEvent {
    // Click: a press and release on the element. Change: a form control's value changed, as
    // `value`. Submit: a form was sent.
    enum class Kind : std::uint8_t { Click, Change, Submit };

    Kind kind = Kind::Click;
    DocumentHandle document;
    std::string element_id;
    std::string value;
};

class GameUi {
public:
    static constexpr std::string_view k_module_interface = "ui";

    virtual ~GameUi() = default;

    // Call once. `host` must outlive this object.
    [[nodiscard]] virtual core::Result<void> initialize(Host& host) = 0;

    // A TrueType or OpenType font, by virtual path. The first one loaded also stands in for
    // characters the others lack.
    [[nodiscard]] virtual core::Result<void> load_font(std::string_view path) = 0;

    // Loads a document, hidden until shown. Paths inside it, such as style sheets and images, are
    // relative to it.
    [[nodiscard]] virtual core::Result<DocumentHandle> load_document(std::string_view path) = 0;

    virtual void show(DocumentHandle document) = 0;
    virtual void hide(DocumentHandle document) = 0;
    virtual void close(DocumentHandle document) = 0;
    [[nodiscard]] virtual bool is_visible(DocumentHandle document) const = 0;

    // Replaces an element's contents with plain text. Fails if there is no such element.
    [[nodiscard]] virtual core::Result<void> set_text(DocumentHandle document,
                                                      std::string_view element_id,
                                                      std::string_view text) = 0;

    // Adds or removes a class, for style sheets to react to.
    [[nodiscard]] virtual core::Result<void> set_class(DocumentHandle document,
                                                       std::string_view element_id,
                                                       std::string_view name, bool enabled) = 0;

    // A form control's value, such as a slider's or a text field's.
    [[nodiscard]] virtual std::optional<std::string> value(DocumentHandle document,
                                                           std::string_view element_id) const = 0;

    // Sets a form control's value, such as a slider's, to show a saved setting. No Change event
    // follows, since the player didn't change it. Fails if there is no such element.
    [[nodiscard]] virtual core::Result<void> set_value(DocumentHandle document,
                                                       std::string_view element_id,
                                                       std::string_view value) = 0;

    // Returns true if the interface used the event, so the game can ignore it.
    virtual bool handle_event(const platform::Event& event) = 0;

    // Lays out and animates. Events from the input handled since the last update become
    // available from events().
    virtual void update(const FrameInfo& frame) = 0;

    [[nodiscard]] virtual std::span<const UiEvent> events() const = 0;

    // Whether the pointer is over, or holding, something in a visible document.
    [[nodiscard]] virtual bool wants_pointer() const = 0;

    // Whether a text field has focus, so typing belongs to the interface.
    [[nodiscard]] virtual bool wants_text() const = 0;

    // The triangles for the current state. Valid until the next call to any other function.
    [[nodiscard]] virtual const DrawList& render() = 0;
};

} // namespace dream::ui
