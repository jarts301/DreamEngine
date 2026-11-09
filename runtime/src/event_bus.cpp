#include "dream/runtime/event_bus.h"

#include <algorithm>
#include <cstddef>

namespace dream::runtime {

void Subscription::reset() {
    if (m_id == 0) {
        return;
    }
    const std::uint64_t id = std::exchange(m_id, 0);

    const std::shared_ptr<detail::EventBusState> state = m_state.lock();
    if (state == nullptr) {
        return;
    }
    const auto channel = state->channels.find(m_type);
    if (channel == state->channels.end()) {
        return;
    }

    auto& listeners = channel->second.listeners;
    const auto listener = std::ranges::find_if(
        listeners, [id](const detail::EventListener& entry) { return entry.id == id; });
    if (listener == listeners.end()) {
        return;
    }

    // While dispatching it is only marked, because the listener could be the one running now.
    if (channel->second.dispatch_depth > 0) {
        listener->active = false;
    } else {
        listeners.erase(listener);
    }
}

std::uint64_t EventBus::add_listener(const void* type, Callback callback) {
    const std::uint64_t id = m_state->next_id++;
    m_state->channels[type].listeners.push_back(
        detail::EventListener{id, std::move(callback), true});
    return id;
}

void EventBus::dispatch(const void* type, const void* event) {
    const auto found = m_state->channels.find(type);
    if (found == m_state->channels.end()) {
        return;
    }
    detail::EventChannel& channel = found->second;

    ++channel.dispatch_depth;
    const std::size_t count = channel.listeners.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (channel.listeners[index].active) {
            channel.listeners[index].callback(event);
        }
    }
    --channel.dispatch_depth;

    if (channel.dispatch_depth == 0) {
        std::erase_if(channel.listeners,
                      [](const detail::EventListener& entry) { return !entry.active; });
    }
}

void EventBus::flush() {
    const std::vector<std::function<void()>> pending = std::exchange(m_queue, {});
    for (const auto& deliver : pending) {
        deliver();
    }
}

} // namespace dream::runtime
