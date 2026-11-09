#pragma once

#include "dream/core/assert.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace dream::core {

// A safe reference to an object in a HandlePool. A default-constructed handle is invalid, and a
// handle stops resolving once its object is destroyed, even if the slot is reused.
template <typename T> struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return generation != 0;
    }

    constexpr auto operator<=>(const Handle&) const = default;
};

// Stores objects in reusable slots addressed by Handle<T>. Not thread-safe. Pointers returned by
// get() are invalidated by create(); handles are not.
template <typename T> class HandlePool {
public:
    template <typename... Args> [[nodiscard]] Handle<T> create(Args&&... args) {
        std::uint32_t index = 0;
        if (m_free_slots.empty()) {
            DREAM_VERIFY(m_slots.size() < std::numeric_limits<std::uint32_t>::max(),
                         "HandlePool is full");
            index = static_cast<std::uint32_t>(m_slots.size());
            m_slots.emplace_back();
        } else {
            index = m_free_slots.back();
            m_free_slots.pop_back();
        }

        Slot& slot = m_slots[index];
        slot.value.emplace(std::forward<Args>(args)...);
        ++m_size;
        return Handle<T>{.index = index, .generation = slot.generation};
    }

    // Returns false if the handle no longer refers to a live object.
    bool destroy(Handle<T> handle) {
        Slot* const slot = find_slot(handle);
        if (slot == nullptr) {
            return false;
        }

        slot->value.reset();
        --m_size;
        ++slot->generation;
        // When the generation wraps to zero the slot is retired, so no old handle can match again.
        if (slot->generation != 0) {
            m_free_slots.push_back(handle.index);
        }
        return true;
    }

    [[nodiscard]] T* get(Handle<T> handle) noexcept {
        Slot* const slot = find_slot(handle);
        return slot == nullptr ? nullptr : &*slot->value;
    }

    [[nodiscard]] const T* get(Handle<T> handle) const noexcept {
        return const_cast<HandlePool*>(this)->get(handle);
    }

    [[nodiscard]] bool contains(Handle<T> handle) const noexcept {
        return get(handle) != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };

    [[nodiscard]] Slot* find_slot(Handle<T> handle) noexcept {
        if (handle.index >= m_slots.size()) {
            return nullptr;
        }
        Slot& slot = m_slots[handle.index];
        const bool is_live = slot.generation == handle.generation && slot.value.has_value();
        return is_live ? &slot : nullptr;
    }

    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free_slots;
    std::size_t m_size = 0;
};

} // namespace dream::core
