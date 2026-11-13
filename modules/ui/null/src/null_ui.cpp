#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/ui/game_ui.h"

#include <memory>
#include <string>

namespace dream::modules {
namespace {

using namespace ui;

struct Document {
    bool visible = false;
};

// Draws nothing and never reacts to input, but loads documents through the host so missing files
// are still reported. For automated runs.
class NullUi final : public GameUi {
public:
    core::Result<void> initialize(Host& host) override {
        m_host = &host;
        return {};
    }

    core::Result<void> load_font(std::string_view path) override {
        const auto file = m_host->read_file(path);
        if (!file.has_value()) {
            return file.error();
        }
        return {};
    }

    core::Result<DocumentHandle> load_document(std::string_view path) override {
        const auto file = m_host->read_file(path);
        if (!file.has_value()) {
            return file.error();
        }
        return handle_cast<DocumentTag>(m_documents.create());
    }

    void show(DocumentHandle document) override {
        if (Document* const found = m_documents.get(handle_cast<Document>(document))) {
            found->visible = true;
        }
    }

    void hide(DocumentHandle document) override {
        if (Document* const found = m_documents.get(handle_cast<Document>(document))) {
            found->visible = false;
        }
    }

    void close(DocumentHandle document) override {
        m_documents.destroy(handle_cast<Document>(document));
    }

    bool is_visible(DocumentHandle document) const override {
        const Document* const found = m_documents.get(handle_cast<Document>(document));
        return found != nullptr && found->visible;
    }

    core::Result<void> set_text(DocumentHandle document, std::string_view /*element_id*/,
                                std::string_view /*text*/) override {
        return exists(document);
    }

    core::Result<void> set_class(DocumentHandle document, std::string_view /*element_id*/,
                                 std::string_view /*name*/, bool /*enabled*/) override {
        return exists(document);
    }

    std::optional<std::string> value(DocumentHandle /*document*/,
                                     std::string_view /*element_id*/) const override {
        return std::nullopt;
    }

    core::Result<void> set_value(DocumentHandle document, std::string_view /*element_id*/,
                                 std::string_view /*value*/) override {
        return exists(document);
    }

    bool handle_event(const platform::Event& /*event*/) override {
        return false;
    }

    void update(const FrameInfo& /*frame*/) override {}

    std::span<const UiEvent> events() const override {
        return {};
    }

    bool wants_pointer() const override {
        return false;
    }

    bool wants_text() const override {
        return false;
    }

    const DrawList& render() override {
        return m_list;
    }

private:
    core::Result<void> exists(DocumentHandle document) const {
        if (!m_documents.contains(handle_cast<Document>(document))) {
            return core::Error{core::ErrorCode::NotFound, "No such document"};
        }
        return {};
    }

    Host* m_host = nullptr;
    core::HandlePool<Document> m_documents;
    DrawList m_list;
};

core::Result<std::unique_ptr<GameUi>> create(const core::Config& /*config*/) {
    return std::unique_ptr<GameUi>{std::make_unique<NullUi>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_ui_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<GameUi>{.name = "null", .create = &create});
}

} // namespace dream::modules
