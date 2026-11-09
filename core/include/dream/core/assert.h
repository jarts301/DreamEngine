#pragma once

#include <source_location>

#if !defined(DREAM_ASSERTS_ENABLED)
#if defined(NDEBUG)
#define DREAM_ASSERTS_ENABLED 0
#else
#define DREAM_ASSERTS_ENABLED 1
#endif
#endif

namespace dream::core {

using AssertHandler = void (*)(const char* expression, const char* message,
                               const std::source_location& location);

// Calls the current handler. The default handler logs the failure and stops the program.
void report_assert_failure(const char* expression, const char* message,
                           const std::source_location& location);

// Replaces the handler and returns the previous one. A handler that returns lets execution
// continue past the failed check, so only test code should install one that does.
AssertHandler set_assert_handler(AssertHandler handler) noexcept;

} // namespace dream::core

// Always checked, in every build type.
#define DREAM_VERIFY(condition, message)                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ::dream::core::report_assert_failure(#condition, message,                              \
                                                 std::source_location::current());                 \
        }                                                                                          \
    } while (false)

// Checked only when DREAM_ASSERTS_ENABLED is 1. The condition is not evaluated otherwise.
#if DREAM_ASSERTS_ENABLED
#define DREAM_ASSERT(condition, message) DREAM_VERIFY(condition, message)
#else
#define DREAM_ASSERT(condition, message) static_cast<void>(sizeof(!(condition)))
#endif
