#include "dream/core/module_registry.h"
#include "dream/debug_ui/debug_ui.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace debug_ui;

// Shows nothing: windows are collapsed and no widget is ever used. For builds without debug tools,
// such as a game's release, chosen in the project configuration.
class NullDebugUi final : public DebugUi {
public:
    core::Result<void> initialize(ui::Host& /*host*/) override {
        return {};
    }
    bool handle_event(const platform::Event& /*event*/) override {
        return false;
    }
    void begin_frame(const ui::FrameInfo& /*frame*/) override {}
    bool begin_window(std::string_view /*title*/, bool* /*open*/) override {
        return false;
    }
    void end_window() override {}
    void text(std::string_view /*text*/) override {}
    bool button(std::string_view /*label*/) override {
        return false;
    }
    bool checkbox(std::string_view /*label*/, bool& /*value*/) override {
        return false;
    }
    bool slider(std::string_view /*label*/, float& /*value*/, float /*minimum*/,
                float /*maximum*/) override {
        return false;
    }
    bool slider(std::string_view /*label*/, int& /*value*/, int /*minimum*/,
                int /*maximum*/) override {
        return false;
    }
    void plot(std::string_view /*label*/, std::span<const float> /*values*/, float /*minimum*/,
              float /*maximum*/) override {}
    void separator() override {}
    void same_line() override {}
    const ui::DrawList& end_frame() override {
        return m_list;
    }
    bool wants_pointer() const override {
        return false;
    }
    bool wants_keyboard() const override {
        return false;
    }

private:
    ui::DrawList m_list;
};

core::Result<std::unique_ptr<DebugUi>> create(const core::Config& /*config*/) {
    return std::unique_ptr<DebugUi>{std::make_unique<NullDebugUi>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_debug_ui_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<DebugUi>{.name = "null", .create = &create});
}

} // namespace dream::modules
