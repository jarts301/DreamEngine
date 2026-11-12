#include "dream/runtime/ui.h"

#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"

#include <type_traits>
#include <utility>

namespace dream::runtime {
namespace {

// Subscribes `forward` to every platform event an interface reacts to. Pointer positions are moved
// into the safe area's coordinates, which `origin` points to.
template <typename Forward>
std::vector<Subscription> subscribe_input(EventBus& events, const core::Vec2* origin,
                                          Forward forward) {
    std::vector<Subscription> subscriptions;
    const auto add = [&]<typename Event>() {
        subscriptions.push_back(
            events.subscribe<Event>([forward, origin](const Event& event) {
                if constexpr (std::is_same_v<Event, platform::PointerMoved>) {
                    forward(platform::Event{
                        platform::PointerMoved{event.x - origin->x, event.y - origin->y}});
                } else {
                    (void)origin;
                    forward(platform::Event{event});
                }
            }));
    };
    add.template operator()<platform::PointerMoved>();
    add.template operator()<platform::PointerButtonChanged>();
    add.template operator()<platform::PointerWheel>();
    add.template operator()<platform::KeyChanged>();
    add.template operator()<platform::TextEntered>();
    add.template operator()<platform::AppPaused>();
    add.template operator()<platform::AppResumed>();
    return subscriptions;
}

// Interfaces are laid out in the safe area, and `origin` is set to its corner.
ui::FrameInfo frame_info(App& app, float delta_seconds, core::Vec2& origin) {
    const platform::SurfaceRect area = app.safe_area();
    origin = core::Vec2{static_cast<float>(area.x), static_cast<float>(area.y)};
    return ui::FrameInfo{
        .size = platform::SurfaceSize{area.width, area.height},
        .delta_seconds = delta_seconds,
        .display_scale = app.display_scale(),
    };
}

} // namespace

// --- DebugOverlay -------------------------------------------------------------------------------

DebugOverlay::~DebugOverlay() {
    m_subscriptions.clear();
    if (m_renderer != nullptr) {
        m_renderer->clear_ui_layer(UiLayer::Debug);
    }
}

core::Result<void> DebugOverlay::initialize(App& app, Renderer& renderer) {
    core::Result<std::unique_ptr<debug_ui::DebugUi>> backend =
        app.create_module<debug_ui::DebugUi>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, renderer, std::move(backend).value());
}

core::Result<void> DebugOverlay::initialize(App& app, Renderer& renderer,
                                            std::unique_ptr<debug_ui::DebugUi> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "DebugOverlay needs a backend"};
    }
    if (core::Result<void> ready = backend->initialize(renderer.ui_host()); !ready.has_value()) {
        return ready;
    }
    m_backend = std::move(backend);
    m_renderer = &renderer;
    m_subscriptions = subscribe_input(app.events(), &m_origin, [this](const platform::Event& event) {
        if (m_visible) {
            m_backend->handle_event(event);
        }
    });
    return {};
}

debug_ui::DebugUi& DebugOverlay::begin_frame(App& app, float delta_seconds) {
    m_renderer->release_ui_textures();
    m_backend->begin_frame(frame_info(app, delta_seconds, m_origin));
    return *m_backend;
}

void DebugOverlay::end_frame() {
    const ui::DrawList& list = m_backend->end_frame();
    if (m_visible) {
        m_renderer->set_ui_layer(UiLayer::Debug, list, m_origin);
    } else {
        m_renderer->clear_ui_layer(UiLayer::Debug);
    }
}

bool DebugOverlay::wants_pointer() const {
    return m_visible && m_backend != nullptr && m_backend->wants_pointer();
}

bool DebugOverlay::wants_keyboard() const {
    return m_visible && m_backend != nullptr && m_backend->wants_keyboard();
}

// --- UserInterface ------------------------------------------------------------------------------

UserInterface::~UserInterface() {
    m_subscriptions.clear();
    if (m_renderer != nullptr) {
        m_renderer->clear_ui_layer(UiLayer::Game);
    }
}

core::Result<void> UserInterface::initialize(App& app, Renderer& renderer) {
    core::Result<std::unique_ptr<ui::GameUi>> backend = app.create_module<ui::GameUi>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, renderer, std::move(backend).value());
}

core::Result<void> UserInterface::initialize(App& app, Renderer& renderer,
                                             std::unique_ptr<ui::GameUi> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "UserInterface needs a backend"};
    }
    if (core::Result<void> ready = backend->initialize(renderer.ui_host()); !ready.has_value()) {
        return ready;
    }
    m_backend = std::move(backend);
    m_renderer = &renderer;
    m_subscriptions = subscribe_input(app.events(), &m_origin, [this](const platform::Event& event) {
        m_backend->handle_event(event);
    });
    return {};
}

void UserInterface::update(App& app, float delta_seconds) {
    DREAM_PROFILE_ZONE("Interface update");
    m_renderer->release_ui_textures();
    m_backend->update(frame_info(app, delta_seconds, m_origin));
    // Copied first, because a listener may change the interface.
    const std::vector<ui::UiEvent> events(m_backend->events().begin(), m_backend->events().end());
    for (const ui::UiEvent& event : events) {
        app.events().publish(UiEventPublished{event});
    }
    m_renderer->set_ui_layer(UiLayer::Game, m_backend->render(), m_origin);

    if (const bool wants_text = m_backend->wants_text(); wants_text != m_text_input) {
        m_text_input = wants_text;
        app.set_text_input(wants_text);
    }
}

bool UserInterface::wants_pointer() const {
    return m_backend != nullptr && m_backend->wants_pointer();
}

} // namespace dream::runtime
