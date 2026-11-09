#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace dream::runtime {

namespace detail {

struct EventListener {
    std::uint64_t id = 0;
    std::function<void(const void*)> callback;
    bool active = true;
};

// A deque, because adding a listener must not move one that is currently running.
struct EventChannel {
    std::deque<EventListener> listeners;
    int dispatch_depth = 0;
};

struct EventBusState {
    std::map<const void*, EventChannel> channels;
    std::uint64_t next_id = 1;
};

// Identifies an event type without RTTI. Not const, so a linker can't merge two of them.
template <typename Event> inline char g_event_tag = 0;

} // namespace detail

// Keeps a listener connected. It disconnects when this object is destroyed, so a listener can't
// outlive whatever it captured. It is safe for the bus to be destroyed first.
class [[nodiscard]] Subscription {
public:
    Subscription() = default;
    Subscription(std::weak_ptr<detail::EventBusState> state, const void* type, std::uint64_t id)
        : m_state(std::move(state)), m_type(type), m_id(id) {}

    ~Subscription() {
        reset();
    }

    Subscription(Subscription&& other) noexcept
        : m_state(std::move(other.m_state)), m_type(other.m_type),
          m_id(std::exchange(other.m_id, 0)) {}

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            m_state = std::move(other.m_state);
            m_type = other.m_type;
            m_id = std::exchange(other.m_id, 0);
        }
        return *this;
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset();

private:
    std::weak_ptr<detail::EventBusState> m_state;
    const void* m_type = nullptr;
    std::uint64_t m_id = 0;
};

// Lets parts of a game talk without knowing about each other. Any copyable type can be an event.
//
// Listeners may publish, subscribe and unsubscribe from inside a callback. A listener added during
// a publish first hears the next event. Must be used from one thread.
class EventBus {
public:
    template <typename Event, typename Callback> Subscription subscribe(Callback callback) {
        const void* const type = &detail::g_event_tag<Event>;
        const std::uint64_t id = add_listener(
            type, [callback = std::move(callback)](const void* event) {
                callback(*static_cast<const Event*>(event));
            });
        return Subscription(m_state, type, id);
    }

    // Calls every listener now.
    template <typename Event> void publish(const Event& event) {
        dispatch(&detail::g_event_tag<Event>, &event);
    }

    // Saves the event for the next flush, which the main loop runs once per frame.
    template <typename Event> void enqueue(Event event) {
        m_queue.push_back([this, event = std::move(event)] { publish(event); });
    }

    // Events enqueued by a listener during a flush wait for the next one, so a pair of listeners
    // that keep triggering each other can't hang the frame.
    void flush();

private:
    using Callback = std::function<void(const void*)>;

    std::uint64_t add_listener(const void* type, Callback callback);
    void dispatch(const void* type, const void* event);

    std::shared_ptr<detail::EventBusState> m_state = std::make_shared<detail::EventBusState>();
    std::vector<std::function<void()>> m_queue;
};

} // namespace dream::runtime
