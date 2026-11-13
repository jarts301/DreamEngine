#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/ui/game_ui.h"

#include <markup/markup.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace ui;

markup::Key to_markup(platform::Key key) {
    using platform::Key;
    switch (key) {
    case Key::Left:
        return markup::Key::Left;
    case Key::Right:
        return markup::Key::Right;
    case Key::Up:
        return markup::Key::Up;
    case Key::Down:
        return markup::Key::Down;
    case Key::Home:
        return markup::Key::Home;
    case Key::End:
        return markup::Key::End;
    case Key::Backspace:
        return markup::Key::Backspace;
    case Key::Delete:
        return markup::Key::Delete;
    case Key::Enter:
        return markup::Key::Enter;
    case Key::Tab:
        return markup::Key::Tab;
    case Key::Escape:
        return markup::Key::Escape;
    default:
        return markup::Key::Other;
    }
}

// Gives the library the engine's files and textures.
class Bridge final : public markup::Host {
public:
    // ui::Host by its full name: inside this class, plain Host names the base class.
    explicit Bridge(ui::Host& host) : m_host(host) {}

    std::optional<std::vector<std::byte>> read_file(std::string_view path) override {
        core::Result<std::vector<std::byte>> file = m_host.read_file(path);
        if (!file.has_value()) {
            return std::nullopt;
        }
        return std::move(file).value();
    }

    std::optional<markup::TextureId> create_texture(std::uint32_t width, std::uint32_t height,
                                                    std::span<const std::byte> rgba,
                                                    bool coverage) override {
        const core::Result<TextureId> texture = m_host.create_texture(
            width, height, rgba, coverage ? TextureKind::Coverage : TextureKind::Color);
        if (!texture.has_value()) {
            DREAM_LOG_WARNING("markup: {}", texture.error().message);
            return std::nullopt;
        }
        return texture.value();
    }

    std::optional<markup::Image> load_image(std::string_view path) override {
        const core::Result<LoadedTexture> loaded = m_host.load_texture(path, true);
        if (!loaded.has_value()) {
            DREAM_LOG_WARNING("markup: {}", loaded.error().message);
            return std::nullopt;
        }
        return markup::Image{loaded.value().id, loaded.value().width, loaded.value().height};
    }

    void release_texture(markup::TextureId texture) override {
        m_host.release_texture(texture);
    }

private:
    ui::Host& m_host;
};

DocumentHandle from_markup(markup::DocumentId document) {
    return DocumentHandle{.index = document.index, .generation = document.generation};
}

markup::DocumentId to_markup(DocumentHandle document) {
    return markup::DocumentId{document.index, document.generation};
}

class MarkupGameUi final : public GameUi {
public:
    core::Result<void> initialize(Host& host) override {
        if (m_context != nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "Already initialized"};
        }
        m_bridge = std::make_unique<Bridge>(host);
        m_context = std::make_unique<markup::Context>(*m_bridge);
        return {};
    }

    core::Result<void> load_font(std::string_view path) override {
        if (!m_context->load_font(path)) {
            return core::Error{core::ErrorCode::NotFound, m_context->last_error()};
        }
        return {};
    }

    core::Result<DocumentHandle> load_document(std::string_view path) override {
        const std::optional<markup::DocumentId> document = m_context->load_document(path);
        if (!document.has_value()) {
            return core::Error{core::ErrorCode::NotFound, m_context->last_error()};
        }
        return from_markup(*document);
    }

    void show(DocumentHandle document) override {
        m_context->show(to_markup(document));
    }

    void hide(DocumentHandle document) override {
        m_context->hide(to_markup(document));
    }

    void close(DocumentHandle document) override {
        m_context->close(to_markup(document));
    }

    bool is_visible(DocumentHandle document) const override {
        return m_context->is_visible(to_markup(document));
    }

    core::Result<void> set_text(DocumentHandle document, std::string_view element_id,
                                std::string_view text) override {
        if (!m_context->set_text(to_markup(document), element_id, text)) {
            return core::Error{core::ErrorCode::NotFound, m_context->last_error()};
        }
        return {};
    }

    core::Result<void> set_class(DocumentHandle document, std::string_view element_id,
                                 std::string_view name, bool enabled) override {
        if (!m_context->set_class(to_markup(document), element_id, name, enabled)) {
            return core::Error{core::ErrorCode::NotFound, m_context->last_error()};
        }
        return {};
    }

    std::optional<std::string> value(DocumentHandle document,
                                     std::string_view element_id) const override {
        return m_context->value(to_markup(document), element_id);
    }

    core::Result<void> set_value(DocumentHandle document, std::string_view element_id,
                                 std::string_view value) override {
        if (!m_context->set_value(to_markup(document), element_id, value)) {
            return core::Error{core::ErrorCode::NotFound, m_context->last_error()};
        }
        return {};
    }

    bool handle_event(const platform::Event& event) override {
        if (m_context == nullptr) {
            return false;
        }
        return std::visit(
            [&](const auto& value) -> bool {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, platform::PointerMoved>) {
                    return m_context->pointer_moved(value.x, value.y);
                } else if constexpr (std::is_same_v<Type, platform::PointerButtonChanged>) {
                    const markup::Button button =
                        value.button == platform::PointerButton::Right    ? markup::Button::Right
                        : value.button == platform::PointerButton::Middle ? markup::Button::Middle
                                                                          : markup::Button::Left;
                    return m_context->pointer_button(button, value.pressed);
                } else if constexpr (std::is_same_v<Type, platform::PointerWheel>) {
                    return m_context->wheel(value.x, value.y);
                } else if constexpr (std::is_same_v<Type, platform::KeyChanged>) {
                    return m_context->key(to_markup(value.key), value.pressed);
                } else if constexpr (std::is_same_v<Type, platform::TextEntered>) {
                    return m_context->text(value.text);
                } else {
                    return false;
                }
            },
            event);
    }

    void update(const FrameInfo& frame) override {
        m_context->update(static_cast<float>(frame.size.width),
                          static_cast<float>(frame.size.height), frame.delta_seconds,
                          std::clamp(frame.display_scale, 0.5f, 6.0f));
        m_events.clear();
        for (const markup::Event& event : m_context->events()) {
            const UiEvent::Kind kind =
                event.kind == markup::Event::Kind::Click    ? UiEvent::Kind::Click
                : event.kind == markup::Event::Kind::Change ? UiEvent::Kind::Change
                                                            : UiEvent::Kind::Submit;
            m_events.push_back(
                UiEvent{kind, from_markup(event.document), event.element_id, event.value});
        }
    }

    std::span<const UiEvent> events() const override {
        return m_events;
    }

    bool wants_pointer() const override {
        return m_context != nullptr && m_context->wants_pointer();
    }

    bool wants_text() const override {
        return m_context != nullptr && m_context->wants_text();
    }

    const DrawList& render() override {
        m_list.clear();
        m_list.premultiplied_alpha = true;
        if (m_context == nullptr) {
            return m_list;
        }
        const markup::DrawList& drawn = m_context->render();
        m_list.vertices.reserve(drawn.vertices.size());
        for (const markup::Vertex& vertex : drawn.vertices) {
            m_list.vertices.push_back(Vertex{core::Vec2{vertex.x, vertex.y},
                                             core::Vec2{vertex.u, vertex.v}, vertex.color});
        }
        m_list.indices.assign(drawn.indices.begin(), drawn.indices.end());
        for (const markup::DrawCommand& command : drawn.commands) {
            std::optional<ClipRect> clip;
            if (command.clip.has_value()) {
                clip = ClipRect{command.clip->x, command.clip->y, command.clip->width,
                                command.clip->height};
            }
            m_list.commands.push_back(DrawCommand{command.first_index, command.index_count,
                                                  command.texture, clip, core::Vec2{}});
        }
        return m_list;
    }

private:
    // The context goes before the bridge it uses.
    std::unique_ptr<Bridge> m_bridge;
    std::unique_ptr<markup::Context> m_context;
    std::vector<UiEvent> m_events;
    DrawList m_list;
};

core::Result<std::unique_ptr<GameUi>> create(const core::Config& /*config*/) {
    return std::unique_ptr<GameUi>{std::make_unique<MarkupGameUi>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_ui_markup(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<GameUi>{.name = "markup", .create = &create});
}

} // namespace dream::modules
