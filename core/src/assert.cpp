#include "dream/core/assert.h"

#include "dream/core/log.h"

#include <atomic>

namespace dream::core {
namespace {

void default_assert_handler(const char* expression, const char* message,
                            const std::source_location& location) {
    DREAM_LOG_FATAL("Assertion failed: {} ({}) at {}:{}", expression, message, location.file_name(),
                    location.line());
#if defined(_MSC_VER)
    __debugbreak();
#else
    __builtin_trap();
#endif
}

std::atomic<AssertHandler> g_assert_handler{default_assert_handler};

} // namespace

void report_assert_failure(const char* expression, const char* message,
                           const std::source_location& location) {
    g_assert_handler.load()(expression, message, location);
}

AssertHandler set_assert_handler(AssertHandler handler) noexcept {
    return g_assert_handler.exchange(handler);
}

} // namespace dream::core
