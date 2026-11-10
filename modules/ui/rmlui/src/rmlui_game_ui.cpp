#include "dream/core/clock.h"
#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/ui/game_ui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace ui;

Rml::Input::KeyIdentifier to_rml(platform::Key key) {
    using platform::Key;
    using namespace Rml::Input;
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<KeyIdentifier>(KI_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
    }
    if (key >= Key::Digit0 && key <= Key::Digit9) {
        return static_cast<KeyIdentifier>(KI_0 +
                                          (static_cast<int>(key) - static_cast<int>(Key::Digit0)));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<KeyIdentifier>(KI_F1 +
                                          (static_cast<int>(key) - static_cast<int>(Key::F1)));
    }
    switch (key) {
    case Key::Space:
        return KI_SPACE;
    case Key::Enter:
        return KI_RETURN;
    case Key::Escape:
        return KI_ESCAPE;
    case Key::Tab:
        return KI_TAB;
    case Key::Backspace:
        return KI_BACK;
    case Key::Delete:
        return KI_DELETE;
    case Key::Home:
        return KI_HOME;
    case Key::End:
        return KI_END;
    case Key::Left:
        return KI_LEFT;
    case Key::Right:
        return KI_RIGHT;
    case Key::Up:
        return KI_UP;
    case Key::Down:
        return KI_DOWN;
    case Key::LeftShift:
        return KI_LSHIFT;
    case Key::RightShift:
        return KI_RSHIFT;
    case Key::LeftControl:
        return KI_LCONTROL;
    case Key::RightControl:
        return KI_RCONTROL;
    case Key::LeftAlt:
        return KI_LMENU;
    case Key::RightAlt:
        return KI_RMENU;
    default:
        return KI_UNKNOWN;
    }
}

// RmlUi talks to the engine through three global interfaces. They forward to the one GameUi alive.
class Bridge final : public Rml::SystemInterface, public Rml::FileInterface,
                     public Rml::RenderInterface {
public:
    explicit Bridge(Host& host) : m_host(host) {}

    // --- System ---

    double GetElapsedTime() override {
        return m_clock.elapsed_seconds();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            DREAM_LOG_ERROR("RmlUi: {}", message);
            break;
        case Rml::Log::LT_WARNING:
            DREAM_LOG_WARNING("RmlUi: {}", message);
            break;
        default:
            break;
        }
        return true;
    }

    // Paths are virtual paths such as "assets:/ui/menu.rml". Relative paths are relative to the
    // document's folder.
    void JoinPath(Rml::String& translated, const Rml::String& document_path,
                  const Rml::String& path) override {
        if (path.find(":/") != Rml::String::npos || document_path.empty()) {
            translated = path;
            return;
        }
        const std::size_t slash = document_path.find_last_of('/');
        const std::string joined =
            (slash == Rml::String::npos ? std::string() : document_path.substr(0, slash + 1)) + path;

        // Resolves "." and ".." after the "assets:/" part, which the file system doesn't accept.
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
        translated = joined.substr(0, start);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            translated += (i == 0 ? "" : "/") + parts[i];
        }
    }

    // --- Files ---

    struct OpenFile {
        std::vector<std::byte> data;
        std::size_t position = 0;
    };

    Rml::FileHandle Open(const Rml::String& path) override {
        core::Result<std::vector<std::byte>> data = m_host.read_file(path);
        if (!data.has_value()) {
            return 0;
        }
        auto* const file = new OpenFile{std::move(data).value(), 0};
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle handle) override {
        delete reinterpret_cast<OpenFile*>(handle);
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override {
        auto* const file = reinterpret_cast<OpenFile*>(handle);
        const std::size_t count = std::min(size, file->data.size() - file->position);
        std::memcpy(buffer, file->data.data() + file->position, count);
        file->position += count;
        return count;
    }

    bool Seek(Rml::FileHandle handle, long offset, int origin) override {
        auto* const file = reinterpret_cast<OpenFile*>(handle);
        const auto base = static_cast<long long>(origin == SEEK_SET   ? 0
                                                 : origin == SEEK_CUR ? file->position
                                                                      : file->data.size());
        const long long target = base + offset;
        if (target < 0 || target > static_cast<long long>(file->data.size())) {
            return false;
        }
        file->position = static_cast<std::size_t>(target);
        return true;
    }

    size_t Tell(Rml::FileHandle handle) override {
        return reinterpret_cast<OpenFile*>(handle)->position;
    }

    size_t Length(Rml::FileHandle handle) override {
        return reinterpret_cast<OpenFile*>(handle)->data.size();
    }

    // --- Rendering, which records triangles into the draw list ---

    struct Geometry {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto* const geometry = new Geometry{
            std::vector<Rml::Vertex>(vertices.begin(), vertices.end()),
            std::vector<int>(indices.begin(), indices.end()),
        };
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        const auto* const geometry = reinterpret_cast<const Geometry*>(handle);
        const auto base = static_cast<std::uint32_t>(m_list.vertices.size());
        for (const Rml::Vertex& vertex : geometry->vertices) {
            const Rml::ColourbPremultiplied& c = vertex.colour;
            m_list.vertices.push_back(Vertex{
                .position = core::Vec2{vertex.position.x, vertex.position.y},
                .uv = core::Vec2{vertex.tex_coord.x, vertex.tex_coord.y},
                .color = std::uint32_t{c.red} | (std::uint32_t{c.green} << 8) |
                         (std::uint32_t{c.blue} << 16) | (std::uint32_t{c.alpha} << 24),
            });
        }
        const auto first = static_cast<std::uint32_t>(m_list.indices.size());
        for (const int index : geometry->indices) {
            m_list.indices.push_back(base + static_cast<std::uint32_t>(index));
        }
        m_list.commands.push_back(DrawCommand{
            .first_index = first,
            .index_count = static_cast<std::uint32_t>(geometry->indices.size()),
            .texture = static_cast<TextureId>(texture),
            .clip = m_scissor_enabled ? std::optional<ClipRect>(m_scissor) : std::nullopt,
            .translation = core::Vec2{translation.x, translation.y},
        });
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<Geometry*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        const core::Result<LoadedTexture> loaded = m_host.load_texture(source, true);
        if (!loaded.has_value()) {
            DREAM_LOG_WARNING("RmlUi: {}", loaded.error().message);
            return 0;
        }
        dimensions = Rml::Vector2i(static_cast<int>(loaded.value().width),
                                   static_cast<int>(loaded.value().height));
        return static_cast<Rml::TextureHandle>(loaded.value().id);
    }

    // RmlUi makes these for font glyphs and other generated images, already premultiplied.
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override {
        const core::Result<TextureId> created = m_host.create_texture(
            static_cast<std::uint32_t>(dimensions.x), static_cast<std::uint32_t>(dimensions.y),
            std::as_bytes(std::span(source.data(), source.size())), TextureKind::Coverage);
        return created.has_value() ? static_cast<Rml::TextureHandle>(created.value()) : 0;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        m_host.release_texture(static_cast<TextureId>(texture));
    }

    void EnableScissorRegion(bool enable) override {
        m_scissor_enabled = enable;
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        m_scissor = ClipRect{region.Left(), region.Top(), region.Width(), region.Height()};
    }

    DrawList& list() noexcept {
        return m_list;
    }

private:
    Host& m_host;
    core::Stopwatch m_clock;
    DrawList m_list;
    ClipRect m_scissor;
    bool m_scissor_enabled = false;
};

struct Document {
    Rml::ElementDocument* element = nullptr;
};

// RmlUi's state is global, so one GameUi may exist at a time.
bool g_in_use = false;

class RmlUiGameUi final : public GameUi, private Rml::EventListener {
public:
    RmlUiGameUi() {
        g_in_use = true;
    }

    ~RmlUiGameUi() override {
        if (m_context != nullptr) {
            Rml::RemoveContext(m_context->GetName());
        }
        if (m_initialised) {
            Rml::Shutdown();
        }
        g_in_use = false;
    }

    RmlUiGameUi(const RmlUiGameUi&) = delete;
    RmlUiGameUi& operator=(const RmlUiGameUi&) = delete;

    core::Result<void> initialize(Host& host) override {
        if (m_bridge != nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "Already initialized"};
        }
        m_bridge = std::make_unique<Bridge>(host);
        Rml::SetSystemInterface(m_bridge.get());
        Rml::SetFileInterface(m_bridge.get());
        Rml::SetRenderInterface(m_bridge.get());
        if (!Rml::Initialise()) {
            return core::Error{core::ErrorCode::Unknown, "RmlUi could not start"};
        }
        m_initialised = true;
        m_context = Rml::CreateContext("main", Rml::Vector2i(1, 1));
        if (m_context == nullptr) {
            return core::Error{core::ErrorCode::Unknown, "RmlUi could not create a context"};
        }
        return {};
    }

    core::Result<void> load_font(std::string_view path) override {
        if (!Rml::LoadFontFace(std::string(path), !m_has_font)) {
            return core::Error{core::ErrorCode::NotFound,
                               std::string("Could not load the font ") + std::string(path)};
        }
        m_has_font = true;
        return {};
    }

    core::Result<DocumentHandle> load_document(std::string_view path) override {
        Rml::ElementDocument* const element = m_context->LoadDocument(std::string(path));
        if (element == nullptr) {
            return core::Error{core::ErrorCode::NotFound,
                               std::string("Could not load the document ") + std::string(path)};
        }
        const auto handle = m_documents.create(Document{element});
        element->AddEventListener(Rml::EventId::Click, this);
        element->AddEventListener(Rml::EventId::Change, this);
        element->AddEventListener(Rml::EventId::Submit, this);
        m_by_element[element] = handle_cast<DocumentTag>(handle);
        return handle_cast<DocumentTag>(handle);
    }

    void show(DocumentHandle document) override {
        if (Document* const found = find(document)) {
            found->element->Show();
        }
    }

    void hide(DocumentHandle document) override {
        if (Document* const found = find(document)) {
            found->element->Hide();
        }
    }

    void close(DocumentHandle document) override {
        if (Document* const found = find(document)) {
            m_by_element.erase(found->element);
            found->element->RemoveEventListener(Rml::EventId::Click, this);
            found->element->RemoveEventListener(Rml::EventId::Change, this);
            found->element->RemoveEventListener(Rml::EventId::Submit, this);
            found->element->Close();
            m_documents.destroy(handle_cast<Document>(document));
        }
    }

    bool is_visible(DocumentHandle document) const override {
        const Document* const found = m_documents.get(handle_cast<Document>(document));
        return found != nullptr && found->element->IsVisible();
    }

    core::Result<void> set_text(DocumentHandle document, std::string_view element_id,
                                std::string_view text) override {
        Rml::Element* const element = find_element(document, element_id);
        if (element == nullptr) {
            return missing(element_id);
        }
        element->SetInnerRML(Rml::StringUtilities::EncodeRml(std::string(text)));
        return {};
    }

    core::Result<void> set_class(DocumentHandle document, std::string_view element_id,
                                 std::string_view name, bool enabled) override {
        Rml::Element* const element = find_element(document, element_id);
        if (element == nullptr) {
            return missing(element_id);
        }
        element->SetClass(std::string(name), enabled);
        return {};
    }

    std::optional<std::string> value(DocumentHandle document,
                                     std::string_view element_id) const override {
        Rml::Element* const element = find_element(document, element_id);
        if (element == nullptr) {
            return std::nullopt;
        }
        if (auto* const control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element)) {
            return control->GetValue();
        }
        return element->GetAttribute<Rml::String>("value", "");
    }

    bool handle_event(const platform::Event& event) override {
        if (m_context == nullptr) {
            return false;
        }
        return std::visit(
            [&](const auto& value) -> bool {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, platform::PointerMoved>) {
                    m_context->ProcessMouseMove(static_cast<int>(value.x),
                                                static_cast<int>(value.y), m_modifiers);
                    return wants_pointer();
                } else if constexpr (std::is_same_v<Type, platform::PointerButtonChanged>) {
                    const int button = value.button == platform::PointerButton::Right    ? 1
                                       : value.button == platform::PointerButton::Middle ? 2
                                                                                         : 0;
                    if (value.pressed) {
                        m_context->ProcessMouseButtonDown(button, m_modifiers);
                    } else {
                        m_context->ProcessMouseButtonUp(button, m_modifiers);
                    }
                    return wants_pointer();
                } else if constexpr (std::is_same_v<Type, platform::PointerWheel>) {
                    // RmlUi scrolls down for positive values.
                    m_context->ProcessMouseWheel(Rml::Vector2f(value.x, -value.y), m_modifiers);
                    return wants_pointer();
                } else if constexpr (std::is_same_v<Type, platform::KeyChanged>) {
                    update_modifiers(value);
                    const Rml::Input::KeyIdentifier key = to_rml(value.key);
                    if (key == Rml::Input::KI_UNKNOWN) {
                        return false;
                    }
                    return value.pressed ? !m_context->ProcessKeyDown(key, m_modifiers)
                                         : !m_context->ProcessKeyUp(key, m_modifiers);
                } else if constexpr (std::is_same_v<Type, platform::TextEntered>) {
                    return !m_context->ProcessTextInput(value.text);
                } else {
                    return false;
                }
            },
            event);
    }

    void update(const FrameInfo& frame) override {
        m_events.swap(m_pending);
        m_pending.clear();
        if (m_context == nullptr) {
            return;
        }
        m_context->SetDimensions(Rml::Vector2i(static_cast<int>(frame.size.width),
                                               static_cast<int>(frame.size.height)));
        m_context->SetDensityIndependentPixelRatio(std::clamp(frame.display_scale, 0.5f, 6.0f));
        m_context->Update();
    }

    std::span<const UiEvent> events() const override {
        return m_events;
    }

    // Over an element inside a document, rather than a document's own background, which often
    // fills the screen.
    bool wants_pointer() const override {
        if (m_context == nullptr) {
            return false;
        }
        const Rml::Element* const hover = m_context->GetHoverElement();
        return hover != nullptr && hover != hover->GetOwnerDocument() &&
               hover->GetOwnerDocument() != nullptr;
    }

    bool wants_text() const override {
        if (m_context == nullptr) {
            return false;
        }
        Rml::Element* const focus = m_context->GetFocusElement();
        if (focus == nullptr) {
            return false;
        }
        const Rml::String& tag = focus->GetTagName();
        if (tag == "textarea") {
            return true;
        }
        if (tag != "input") {
            return false;
        }
        const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password";
    }

    const DrawList& render() override {
        DrawList& list = m_bridge->list();
        list.clear();
        list.premultiplied_alpha = true;
        if (m_context != nullptr) {
            m_context->Render();
        }
        return list;
    }

private:
    // Rml::EventListener: turns clicks and changes into UiEvents, named after the nearest element
    // with an id.
    void ProcessEvent(Rml::Event& event) override {
        Rml::ElementDocument* const owner = event.GetCurrentElement()->GetOwnerDocument();
        const auto document = m_by_element.find(owner);
        if (document == m_by_element.end()) {
            return;
        }
        // Stops at the document, whose parent is RmlUi's root, named after the context.
        Rml::Element* element = event.GetTargetElement();
        while (element != nullptr && element != owner && element->GetId().empty()) {
            element = element->GetParentNode();
        }
        if (element == nullptr || element->GetId().empty()) {
            return;
        }

        UiEvent result{.document = document->second, .element_id = element->GetId()};
        switch (event.GetId()) {
        case Rml::EventId::Click:
            result.kind = UiEvent::Kind::Click;
            break;
        case Rml::EventId::Change:
            result.kind = UiEvent::Kind::Change;
            result.value = event.GetParameter<Rml::String>("value", "");
            break;
        case Rml::EventId::Submit:
            result.kind = UiEvent::Kind::Submit;
            break;
        default:
            return;
        }
        m_pending.push_back(std::move(result));
    }

    void update_modifiers(const platform::KeyChanged& event) {
        using platform::Key;
        const auto set = [&](Key left, Key right, int flag) {
            if (event.key == left || event.key == right) {
                m_modifiers = event.pressed ? (m_modifiers | flag) : (m_modifiers & ~flag);
            }
        };
        set(Key::LeftControl, Key::RightControl, Rml::Input::KM_CTRL);
        set(Key::LeftShift, Key::RightShift, Rml::Input::KM_SHIFT);
        set(Key::LeftAlt, Key::RightAlt, Rml::Input::KM_ALT);
    }

    Document* find(DocumentHandle document) {
        return m_documents.get(handle_cast<Document>(document));
    }

    Rml::Element* find_element(DocumentHandle document, std::string_view id) const {
        const Document* const found = m_documents.get(handle_cast<Document>(document));
        return found == nullptr ? nullptr : found->element->GetElementById(std::string(id));
    }

    static core::Error missing(std::string_view id) {
        return core::Error{core::ErrorCode::NotFound,
                           std::string("No element with id '") + std::string(id) + "'"};
    }

    std::unique_ptr<Bridge> m_bridge;
    Rml::Context* m_context = nullptr;
    core::HandlePool<Document> m_documents;
    std::unordered_map<Rml::ElementDocument*, DocumentHandle> m_by_element;
    std::vector<UiEvent> m_pending;
    std::vector<UiEvent> m_events;
    int m_modifiers = 0;
    bool m_initialised = false;
    bool m_has_font = false;
};

core::Result<std::unique_ptr<GameUi>> create(const core::Config& /*config*/) {
    if (g_in_use) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "RmlUi supports one game interface at a time"};
    }
    return std::unique_ptr<GameUi>{std::make_unique<RmlUiGameUi>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_ui_rmlui(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<GameUi>{.name = "rmlui", .create = &create});
}

} // namespace dream::modules
