#pragma once

#include <cstddef>
#include <cstdint>

namespace dream::core {

// Keeps a block of memory aside and gives it back when an allocation fails, so the allocation can
// succeed and the program can carry on long enough to free memory, instead of crashing. See
// docs/adr/0018-memory-budgets.md.
//
// While it exists, it is the new-handler, which operator new calls when it can't allocate. The
// first failure frees the reserve and lets the allocation try again. A failure after that is
// reported on stderr, and the handler steps aside: an allocation that may fail, such as
// `new (std::nothrow)`, then returns null, and any other ends the program, as it would have
// without a handler. refill() takes the reserve back and arms the handler again.
//
// Only one may exist at a time. Its functions are safe from any thread.
class MemoryReserve {
public:
    // With zero bytes there is no reserve, but failures are still reported.
    explicit MemoryReserve(std::size_t bytes);
    ~MemoryReserve();

    MemoryReserve(const MemoryReserve&) = delete;
    MemoryReserve& operator=(const MemoryReserve&) = delete;

    // Whether an allocation failure has used the reserve up since it was last filled.
    [[nodiscard]] bool is_spent() const noexcept;

    // Allocations that failed, in total, each counted once per attempt the handler saw.
    [[nodiscard]] std::uint64_t failed_allocations() const noexcept;

    // Takes the reserve again if it was spent, and arms the handler. Returns false if the memory
    // isn't there yet, in which case it can be tried again later.
    bool refill() noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return m_bytes;
    }

private:
    std::size_t m_bytes;
};

} // namespace dream::core
