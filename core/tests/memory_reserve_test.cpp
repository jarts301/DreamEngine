#include "dream/core/memory_reserve.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <new>

using namespace dream::core;

// Sanitizers replace operator new with their own, whose nothrow form returns null without asking
// the new-handler, unlike the standard library's.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DREAM_TEST_SANITIZED_NEW
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DREAM_TEST_SANITIZED_NEW
#endif
#endif

namespace {

void test_handler() {}

// Stands in for operator new running out: it calls the installed new-handler.
void fail_an_allocation() {
    const std::new_handler handler = std::get_new_handler();
    REQUIRE(handler != nullptr);
    handler();
}

} // namespace

TEST_CASE("a memory reserve is the new-handler while it exists, then gives the old one back") {
    std::set_new_handler(&test_handler);
    {
        const MemoryReserve reserve(1024);
        CHECK(std::get_new_handler() != &test_handler);
        CHECK(std::get_new_handler() != nullptr);
        CHECK_FALSE(reserve.is_spent());
        CHECK(reserve.size() == 1024);
    }
    CHECK(std::get_new_handler() == &test_handler);
    std::set_new_handler(nullptr);
}

TEST_CASE("a failed allocation spends the reserve, and refilling takes it back") {
    MemoryReserve reserve(1024 * 1024);
    fail_an_allocation();
    CHECK(reserve.is_spent());
    CHECK(reserve.failed_allocations() == 1);
    // Still armed: the next failure is reported, then the handler steps aside.
    CHECK(std::get_new_handler() != nullptr);

    CHECK(reserve.refill());
    CHECK_FALSE(reserve.is_spent());
    CHECK(std::get_new_handler() != nullptr);
}

TEST_CASE("with the reserve spent, the handler reports the failure and steps aside") {
    MemoryReserve reserve(1024);
    fail_an_allocation();
    fail_an_allocation();
    CHECK(reserve.failed_allocations() == 2);
    // The handler before the reserve, which was none.
    CHECK(std::get_new_handler() == nullptr);

    CHECK(reserve.refill());
    CHECK(std::get_new_handler() != nullptr);
}

TEST_CASE("an allocation that may fail still returns null when memory runs out") {
    MemoryReserve reserve(1024 * 1024);
    // Far more than any machine has. volatile keeps the compiler from rejecting it outright.
    volatile std::size_t huge = std::numeric_limits<std::size_t>::max() / 4;
    char* const block = new (std::nothrow) char[huge];
    const bool failed = block == nullptr;
    delete[] block;

    CHECK(failed);
#if !defined(DREAM_TEST_SANITIZED_NEW)
    // The reserve was given back for the first attempt, which still didn't fit, and the failure
    // after that was reported.
    CHECK(reserve.is_spent());
    CHECK(reserve.failed_allocations() >= 2);
#endif
    CHECK(reserve.refill());
}
