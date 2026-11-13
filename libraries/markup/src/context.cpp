#include "markup/markup.h"

#include "dom.h"
#include "fonts.h"
#include "layout.h"
#include "parser.h"
#include "render.h"
#include "style.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace markup {

using namespace detail;

namespace {

// Joins a path found in a document to the document's own, as "assets:/ui/../styles/x.rcss" becomes
// "assets:/styles/x.rcss". Paths with a scheme, such as "assets:/...", are already whole.
std::string join_path(std::string_view document, std::string_view path) {
    if (path.find(":/") != std::string_view::npos || document.empty()) {
        return std::string(path);
    }
    const std::size_t slash = document.find_last_of('/');
    const std::string joined =
        (slash == std::string_view::npos ? std::string()
                                         : std::string(document.substr(0, slash + 1))) +
        std::string(path);
    const std::size_t root = joined.find(":/");
    const std::size_t start = root == std::string::npos ? 0 : root + 2;
    std::vector<std::string> parts;
    std::size_t begin = start;
    while (begin <= joined.size()) {
        const std::size_t end = std::min(joined.find('/', begin), joined.size());
        const std::string part = joined.substr(begin, end - begin);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        begin = end + 1;
    }
    std::string result = joined.substr(0, start);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        result += (i == 0 ? "" : "/") + parts[i];
    }
    return result;
}

float number(const std::string& text, float fallback) {
    float value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

std::string format_number(float value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

Element* find_tag(Element& element, std::string_view tag) {
    if (element.tag == tag) {
        return &element;
    }
    for (const auto& child : element.children) {
        if (Element* const found = find_tag(*child, tag)) {
            return found;
        }
    }
    return nullptr;
}

Element* find_id(Element& element, std::string_view id) {
    if (!element.internal && !element.is_text() && element.id == id) {
        return &element;
    }
    for (const auto& child : element.children) {
        if (Element* const found = find_id(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

bool contains(const Element& ancestor, const Element* element) {
    for (; element != nullptr; element = element->parent) {
        if (element == &ancestor) {
            return true;
        }
    }
    return false;
}

// The form control an element belongs to: itself, or the control it is a part of.
Element* control_for(Element* element) {
    for (; element != nullptr; element = element->parent) {
        if (control_of(*element) != Control::None) {
            return element;
        }
        if (!element->internal) {
            return nullptr;
        }
    }
    return nullptr;
}

std::unique_ptr<Element> internal_part(const char* tag) {
    auto part = std::make_unique<Element>();
    part->tag = tag;
    part->internal = true;
    return part;
}

// Gives controls what RmlUi gives them: their type as a class, their first value, and a slider's
// parts.
void prepare_controls(Element& element) {
    const Control control = control_of(element);
    if (control != Control::None && element.tag == "input") {
        const std::string type = lower(element.attribute("type", "text"));
        if (!element.has_class(type)) {
            element.classes.push_back(type);
        }
        element.checked = element.attributes.contains("checked");
        element.value = control == Control::Range
                            ? element.attribute("value", element.attribute("min", "0"))
                            : element.attribute("value");
        element.caret = element.value.size();
        if (control == Control::Range) {
            for (const char* part :
                 {"sliderarrowdec", "slidertrack", "sliderbar", "sliderarrowinc"}) {
                element.add_child(internal_part(part));
            }
        }
    } else if (element.tag == "textarea") {
        std::string text;
        for (const auto& child : element.children) {
            text += child->text;
        }
        element.children.clear();
        element.value = text;
        element.caret = text.size();
    }
    for (const auto& child : element.children) {
        prepare_controls(*child);
    }
}

struct Document {
    std::uint32_t generation = 1;
    bool alive = false;
    std::string path;
    std::unique_ptr<Element> root;
    Element* body = nullptr;
    StyleSheet sheet;
    bool visible = false;
    std::uint64_t front = 0;
};

} // namespace

struct Context::Impl {
    explicit Impl(Host& host_) : host(host_) {}

    Host& host;
    Fonts fonts;
    std::string error;
    std::vector<Document> documents;
    std::vector<Event> pending;
    std::vector<Event> events;
    DrawList list;
    TextureId atlas = 0;
    std::uint64_t shown = 0;
    float scale = 1.0f;

    float pointer_x = -1.0f;
    float pointer_y = -1.0f;
    Element* hover = nullptr;
    Element* pressed = nullptr;
    Element* focus = nullptr;
    Element* dragging = nullptr;

    Document* find(DocumentId id) {
        if (id.index >= documents.size() || !documents[id.index].alive ||
            documents[id.index].generation != id.generation) {
            return nullptr;
        }
        return &documents[id.index];
    }

    DocumentId id_of(const Element* element) const {
        const Element* root = element;
        while (root != nullptr && root->parent != nullptr) {
            root = root->parent;
        }
        for (std::uint32_t i = 0; i < documents.size(); ++i) {
            if (documents[i].alive && documents[i].root.get() == root) {
                return DocumentId{i, documents[i].generation};
            }
        }
        return DocumentId{};
    }

    // Visible documents, back to front.
    std::vector<Document*> visible() {
        std::vector<Document*> result;
        for (Document& document : documents) {
            if (document.alive && document.visible) {
                result.push_back(&document);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const Document* a, const Document* b) { return a->front < b->front; });
        return result;
    }

    // Drops pointers into a part of a tree that is about to go.
    void forget(const Element& subtree) {
        for (Element** pointer : {&hover, &pressed, &focus, &dragging}) {
            if (*pointer != nullptr && contains(subtree, *pointer)) {
                *pointer = nullptr;
            }
        }
    }

    void release_images(Element& element) {
        if (element.image.texture != 0) {
            host.release_texture(element.image.texture);
            element.image = Image{};
        }
        for (const auto& child : element.children) {
            release_images(*child);
        }
    }

    // --- Hit testing and states ---

    static Element* hit(Element& element, float x, float y) {
        if (element.style.display == Display::None || element.is_text()) {
            return nullptr;
        }
        for (auto it = element.children.rbegin(); it != element.children.rend(); ++it) {
            if (Element* const found = hit(**it, x, y)) {
                return found;
            }
        }
        if (!element.style.visible) {
            return nullptr;
        }
        if (!element.line_boxes.empty()) {
            for (const Rect& box : element.line_boxes) {
                if (box.contains(x, y)) {
                    return &element;
                }
            }
            return nullptr;
        }
        return element.box.contains(x, y) ? &element : nullptr;
    }

    Element* target_at(float x, float y) {
        const std::vector<Document*> order = visible();
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            if (Element* const found = hit(*(*it)->body, x, y)) {
                return found;
            }
        }
        return nullptr;
    }

    static void set_chain(Element* element, bool Element::* flag, bool value) {
        for (; element != nullptr; element = element->parent) {
            element->*flag = value;
        }
    }

    void update_hover() {
        Element* const target = target_at(pointer_x, pointer_y);
        if (target != hover) {
            set_chain(hover, &Element::hover, false);
            hover = target;
            set_chain(hover, &Element::hover, true);
        }
    }

    // --- Events ---

    void emit(Event::Kind kind, Element* target, std::string value = {}) {
        // Named after the nearest element with an id, stopping at the body.
        Element* element = target;
        while (element != nullptr && element->tag != "body" &&
               (element->internal || element->id.empty())) {
            element = element->parent;
        }
        if (element == nullptr || element->id.empty()) {
            return;
        }
        pending.push_back(Event{kind, id_of(element), element->id, std::move(value)});
    }

    void set_slider(Element& slider, float x) {
        const float low = number(slider.attribute("min", "0"), 0.0f);
        const float high = number(slider.attribute("max", "100"), 100.0f);
        const float step = number(slider.attribute("step", "1"), 1.0f);
        float bar = 0.0f;
        for (const auto& part : slider.children) {
            if (part->tag == "sliderbar") {
                bar = part->box.width;
            }
        }
        const float left = slider.box.x + slider.style.border[k_left] + slider.paddings[k_left];
        const float travel = slider.box.width - slider.style.border[k_left] -
                             slider.style.border[k_right] - slider.paddings[k_left] -
                             slider.paddings[k_right] - bar;
        const float fraction =
            travel > 0.0f ? std::clamp((x - left - 0.5f * bar) / travel, 0.0f, 1.0f) : 0.0f;
        float value = low + fraction * (high - low);
        if (step > 0.0f) {
            value = low + std::round((value - low) / step) * step;
        }
        value = std::clamp(value, std::min(low, high), std::max(low, high));
        const std::string text = format_number(value);
        if (text != slider.value) {
            slider.value = text;
            emit(Event::Kind::Change, &slider, text);
        }
    }

    // Where in a text field's value a point falls, as a byte offset.
    std::size_t caret_at(const Element& field, float x) {
        if (field.runs.empty()) {
            return field.value.size();
        }
        const int face = font_of(field.style, fonts);
        const std::string text = shown_text(field);
        float best = std::abs(x - field.runs.front().x);
        std::size_t best_characters = 0;
        std::size_t characters = 0;
        for (std::size_t i = 0; i < text.size();) {
            (void)next_codepoint(text, i);
            ++characters;
            const float edge =
                field.runs.front().x +
                fonts.measure(face, field.style.font_size, std::string_view(text).substr(0, i));
            if (std::abs(x - edge) < best) {
                best = std::abs(x - edge);
                best_characters = characters;
            }
        }
        std::size_t offset = 0;
        for (std::size_t c = 0; c < best_characters && offset < field.value.size(); ++c) {
            (void)next_codepoint(field.value, offset);
        }
        return offset;
    }

    void set_focus(Element* element) {
        if (focus == element) {
            return;
        }
        if (focus != nullptr) {
            focus->focus = false;
        }
        focus = element;
        if (focus != nullptr) {
            focus->focus = true;
        }
    }

    [[nodiscard]] bool text_field_focused() const {
        if (focus == nullptr) {
            return false;
        }
        const Control control = control_of(*focus);
        return control == Control::Text || control == Control::Password;
    }

    void submit(Element* from) {
        Element* form = from;
        while (form != nullptr && form->tag != "form") {
            form = form->parent;
        }
        emit(Event::Kind::Submit, form != nullptr ? form : from);
    }

    void activate(Element& target) {
        Element* const control = control_for(&target);
        if (control != nullptr) {
            switch (control_of(*control)) {
            case Control::Checkbox:
                control->checked = !control->checked;
                emit(Event::Kind::Change, control,
                     control->checked ? control->attribute("value", "on") : std::string());
                break;
            case Control::Radio:
                if (!control->checked) {
                    const std::string name = control->attribute("name");
                    Element* root = control;
                    while (root->parent != nullptr) {
                        root = root->parent;
                    }
                    uncheck_radios(*root, name);
                    control->checked = true;
                    emit(Event::Kind::Change, control, control->attribute("value", "on"));
                }
                break;
            case Control::Submit:
                submit(control);
                break;
            default:
                break;
            }
        }
        emit(Event::Kind::Click, &target);
    }

    static void uncheck_radios(Element& element, const std::string& name) {
        if (control_of(element) == Control::Radio && element.attribute("name") == name) {
            element.checked = false;
        }
        for (const auto& child : element.children) {
            uncheck_radios(*child, name);
        }
    }

    void edit(Element& field, std::string value, std::size_t caret) {
        field.value = std::move(value);
        field.caret = std::min(caret, field.value.size());
        emit(Event::Kind::Change, &field, field.value);
    }

    // The start of the character before `offset`.
    static std::size_t previous_character(const std::string& text, std::size_t offset) {
        if (offset == 0) {
            return 0;
        }
        --offset;
        while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0u) == 0x80u) {
            --offset;
        }
        return offset;
    }
};

Context::Context(Host& host) : m_impl(std::make_unique<Impl>(host)) {}

Context::~Context() {
    for (Document& document : m_impl->documents) {
        if (document.alive) {
            m_impl->release_images(*document.root);
        }
    }
    if (m_impl->atlas != 0) {
        m_impl->host.release_texture(m_impl->atlas);
    }
}

const std::string& Context::last_error() const {
    return m_impl->error;
}

bool Context::load_font(std::string_view path) {
    std::optional<std::vector<std::byte>> file = m_impl->host.read_file(path);
    if (!file.has_value()) {
        m_impl->error = "Could not read the font " + std::string(path);
        return false;
    }
    std::string reason;
    if (!m_impl->fonts.add(std::move(*file), reason)) {
        m_impl->error = reason + ": " + std::string(path);
        return false;
    }
    return true;
}

std::optional<DocumentId> Context::load_document(std::string_view path) {
    Impl& impl = *m_impl;
    const std::optional<std::vector<std::byte>> file = impl.host.read_file(path);
    if (!file.has_value()) {
        impl.error = "Could not read the document " + std::string(path);
        return std::nullopt;
    }
    Document document;
    document.path = std::string(path);
    document.root =
        parse_markup(std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
    document.body = find_tag(*document.root, "body");
    if (document.body == nullptr) {
        impl.error = "The document has no body: " + std::string(path);
        return std::nullopt;
    }

    // Style sheets, linked and inline, in the order the head lists them.
    if (Element* const head = find_tag(*document.root, "head")) {
        for (const auto& child : head->children) {
            if (child->tag == "link") {
                const std::string type = lower(child->attribute("type"));
                const bool sheet = type.find("css") != std::string::npos ||
                                   lower(child->attribute("rel")) == "stylesheet";
                if (!sheet) {
                    continue;
                }
                const std::string sheet_path = join_path(path, child->attribute("href"));
                const std::optional<std::vector<std::byte>> text = impl.host.read_file(sheet_path);
                if (!text.has_value()) {
                    impl.error = "Could not read the style sheet " + sheet_path;
                    return std::nullopt;
                }
                parse_css(
                    std::string_view(reinterpret_cast<const char*>(text->data()), text->size()),
                    document.sheet);
            } else if (child->tag == "style") {
                for (const auto& text : child->children) {
                    parse_css(text->text, document.sheet);
                }
            }
        }
    }
    prepare_controls(*document.body);

    std::uint32_t index = 0;
    const auto free = std::find_if(impl.documents.begin(), impl.documents.end(),
                                   [](const Document& d) { return !d.alive; });
    if (free == impl.documents.end()) {
        index = static_cast<std::uint32_t>(impl.documents.size());
        impl.documents.emplace_back();
    } else {
        index = static_cast<std::uint32_t>(free - impl.documents.begin());
    }
    document.generation = impl.documents[index].generation;
    document.alive = true;
    impl.documents[index] = std::move(document);
    return DocumentId{index, impl.documents[index].generation};
}

void Context::show(DocumentId id) {
    if (Document* const document = m_impl->find(id)) {
        document->visible = true;
        document->front = ++m_impl->shown;
    }
}

void Context::hide(DocumentId id) {
    if (Document* const document = m_impl->find(id)) {
        document->visible = false;
        m_impl->forget(*document->root);
    }
}

void Context::close(DocumentId id) {
    Document* const document = m_impl->find(id);
    if (document == nullptr) {
        return;
    }
    m_impl->forget(*document->root);
    m_impl->release_images(*document->root);
    const std::uint32_t generation = document->generation + 1;
    *document = Document{};
    document->generation = generation;
}

bool Context::is_visible(DocumentId id) const {
    const Document* const document = m_impl->find(id);
    return document != nullptr && document->visible;
}

bool Context::set_text(DocumentId id, std::string_view element_id, std::string_view text) {
    Document* const document = m_impl->find(id);
    Element* const element = document != nullptr ? find_id(*document->body, element_id) : nullptr;
    if (element == nullptr) {
        m_impl->error = "No element with id '" + std::string(element_id) + "'";
        return false;
    }
    for (const auto& child : element->children) {
        m_impl->forget(*child);
    }
    element->children.clear();
    auto node = std::make_unique<Element>();
    node->text = std::string(text);
    element->add_child(std::move(node));
    return true;
}

bool Context::set_class(DocumentId id, std::string_view element_id, std::string_view name,
                        bool enabled) {
    Document* const document = m_impl->find(id);
    Element* const element = document != nullptr ? find_id(*document->body, element_id) : nullptr;
    if (element == nullptr) {
        m_impl->error = "No element with id '" + std::string(element_id) + "'";
        return false;
    }
    const auto found = std::find(element->classes.begin(), element->classes.end(), name);
    if (enabled && found == element->classes.end()) {
        element->classes.emplace_back(name);
    } else if (!enabled && found != element->classes.end()) {
        element->classes.erase(found);
    }
    return true;
}

std::optional<std::string> Context::value(DocumentId id, std::string_view element_id) const {
    Document* const document = m_impl->find(id);
    const Element* const element =
        document != nullptr ? find_id(*document->body, element_id) : nullptr;
    if (element == nullptr) {
        return std::nullopt;
    }
    switch (control_of(*element)) {
    case Control::Checkbox:
    case Control::Radio:
        return element->checked ? element->attribute("value", "on") : std::string();
    case Control::Range:
    case Control::Text:
    case Control::Password:
        return element->value;
    default:
        return element->attribute("value");
    }
}

bool Context::set_value(DocumentId id, std::string_view element_id, std::string_view value) {
    Document* const document = m_impl->find(id);
    Element* const element = document != nullptr ? find_id(*document->body, element_id) : nullptr;
    if (element == nullptr) {
        m_impl->error = "No element with id '" + std::string(element_id) + "'";
        return false;
    }
    switch (control_of(*element)) {
    case Control::Checkbox:
    case Control::Radio:
        element->checked = !value.empty();
        break;
    case Control::Range: {
        const float low = number(element->attribute("min", "0"), 0.0f);
        const float high = number(element->attribute("max", "100"), 100.0f);
        const float step = number(element->attribute("step", "1"), 1.0f);
        float number_value = number(std::string(value), low);
        if (step > 0.0f) {
            number_value = low + std::round((number_value - low) / step) * step;
        }
        element->value =
            format_number(std::clamp(number_value, std::min(low, high), std::max(low, high)));
        break;
    }
    case Control::Text:
    case Control::Password:
        element->value = value;
        break;
    default:
        element->attributes["value"] = std::string(value);
        break;
    }
    return true;
}

bool Context::pointer_moved(float x, float y) {
    Impl& impl = *m_impl;
    impl.pointer_x = x;
    impl.pointer_y = y;
    impl.update_hover();
    if (impl.dragging != nullptr) {
        impl.set_slider(*impl.dragging, x);
        return true;
    }
    return wants_pointer();
}

bool Context::pointer_button(Button button, bool pressed) {
    Impl& impl = *m_impl;
    if (button != Button::Left) {
        return wants_pointer();
    }
    impl.update_hover();
    Element* const target = impl.hover;
    if (pressed) {
        impl.pressed = target;
        Impl::set_chain(target, &Element::active, true);
        Element* const control = control_for(target);
        const Control kind = control != nullptr ? control_of(*control) : Control::None;
        if (kind == Control::Text || kind == Control::Password) {
            impl.set_focus(control);
            control->caret = impl.caret_at(*control, impl.pointer_x);
        } else {
            impl.set_focus(nullptr);
        }
        if (kind == Control::Range) {
            impl.dragging = control;
            impl.set_slider(*control, impl.pointer_x);
        }
        return target != nullptr && wants_pointer();
    }

    Element* const was_pressed = impl.pressed;
    const bool was_dragging = impl.dragging != nullptr;
    impl.dragging = nullptr;
    impl.pressed = nullptr;
    Impl::set_chain(was_pressed, &Element::active, false);
    if (was_pressed != nullptr && target != nullptr && contains(*was_pressed, target)) {
        impl.activate(*was_pressed);
    }
    return was_dragging || wants_pointer();
}

bool Context::wheel(float /*dx*/, float /*dy*/) {
    return wants_pointer();
}

bool Context::key(Key key, bool pressed) {
    Impl& impl = *m_impl;
    if (!impl.text_field_focused()) {
        return false;
    }
    if (!pressed) {
        return true;
    }
    Element& field = *impl.focus;
    std::string value = field.value;
    std::size_t caret = std::min(field.caret, value.size());
    switch (key) {
    case Key::Left:
        field.caret = Impl::previous_character(value, caret);
        break;
    case Key::Right:
        if (caret < value.size()) {
            (void)next_codepoint(value, caret);
        }
        field.caret = caret;
        break;
    case Key::Home:
    case Key::Up:
        field.caret = 0;
        break;
    case Key::End:
    case Key::Down:
        field.caret = value.size();
        break;
    case Key::Backspace:
        if (caret > 0) {
            const std::size_t start = Impl::previous_character(value, caret);
            value.erase(start, caret - start);
            impl.edit(field, std::move(value), start);
        }
        break;
    case Key::Delete:
        if (caret < value.size()) {
            std::size_t end = caret;
            (void)next_codepoint(value, end);
            value.erase(caret, end - caret);
            impl.edit(field, std::move(value), caret);
        }
        break;
    case Key::Enter:
        impl.submit(&field);
        break;
    case Key::Escape:
        impl.set_focus(nullptr);
        break;
    default:
        break;
    }
    return true;
}

bool Context::text(std::string_view utf8) {
    Impl& impl = *m_impl;
    if (!impl.text_field_focused()) {
        return false;
    }
    std::string typed;
    for (const char c : utf8) {
        // Control characters, such as a newline from Enter, aren't text here.
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            typed += c;
        }
    }
    if (!typed.empty()) {
        Element& field = *impl.focus;
        std::string value = field.value;
        const std::size_t caret = std::min(field.caret, value.size());
        value.insert(caret, typed);
        impl.edit(field, std::move(value), caret + typed.size());
    }
    return true;
}

void Context::update(float width, float height, float /*delta_seconds*/, float scale) {
    Impl& impl = *m_impl;
    impl.events.swap(impl.pending);
    impl.pending.clear();
    impl.scale = scale > 0.0f ? scale : 1.0f;
    impl.fonts.trim();

    LayoutServices services{impl.fonts, impl.scale, {}};
    for (Document& document : impl.documents) {
        if (!document.alive || !document.visible) {
            continue;
        }
        services.load_image = [&impl, &document](const Element& image) {
            const std::optional<Image> loaded =
                impl.host.load_image(join_path(document.path, image.attribute("src")));
            return loaded.value_or(Image{});
        };
        compute_styles(*document.body, document.sheet, impl.scale);
        layout_document(*document.body, width, height, services);
    }
    impl.update_hover();
}

std::span<const Event> Context::events() const {
    return m_impl->events;
}

bool Context::wants_pointer() const {
    const Element* const hover = m_impl->hover;
    return hover != nullptr && hover->tag != "body";
}

bool Context::wants_text() const {
    return m_impl->text_field_focused();
}

const DrawList& Context::render() {
    Impl& impl = *m_impl;
    impl.list.clear();
    const std::vector<Document*> order = impl.visible();
    if (order.empty()) {
        return impl.list;
    }
    if (impl.fonts.atlas_changed() || impl.atlas == 0) {
        // The glyph masks become premultiplied white, so text takes its color from its vertices.
        const int size = impl.fonts.atlas_size();
        const std::vector<std::uint8_t>& coverage = impl.fonts.atlas();
        std::vector<std::byte> rgba(coverage.size() * 4);
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            std::memset(&rgba[i * 4], coverage[i], 4);
        }
        const std::optional<TextureId> texture = impl.host.create_texture(
            static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), rgba, true);
        if (texture.has_value()) {
            if (impl.atlas != 0) {
                impl.host.release_texture(impl.atlas);
            }
            impl.atlas = *texture;
            impl.fonts.atlas_taken();
        }
    }
    Painter painter{impl.list, impl.fonts, impl.atlas, impl.scale};
    for (const Document* document : order) {
        painter.element(*document->body);
    }
    return impl.list;
}

} // namespace markup
