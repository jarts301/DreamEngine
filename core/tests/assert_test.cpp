#include "dream/core/assert.h"

#include <doctest/doctest.h>

using dream::core::AssertHandler;
using dream::core::set_assert_handler;

namespace {

int g_failures = 0;

void count_failure(const char*, const char*, const std::source_location&) {
    ++g_failures;
}

class ScopedCountingHandler {
public:
    ScopedCountingHandler() : m_previous(set_assert_handler(count_failure)) {
        g_failures = 0;
    }
    ~ScopedCountingHandler() {
        set_assert_handler(m_previous);
    }

private:
    AssertHandler m_previous;
};

} // namespace

TEST_CASE("DREAM_VERIFY reports only false conditions") {
    const ScopedCountingHandler handler;
    bool condition = true;

    DREAM_VERIFY(condition, "should pass");
    CHECK(g_failures == 0);

    condition = false;
    DREAM_VERIFY(condition, "should fail");
    CHECK(g_failures == 1);
}

TEST_CASE("DREAM_ASSERT follows the build configuration") {
    const ScopedCountingHandler handler;
    const bool condition = false;
    const int expected_failures = DREAM_ASSERTS_ENABLED;

    DREAM_ASSERT(condition, "only reported when asserts are enabled");
    CHECK(g_failures == expected_failures);
}
