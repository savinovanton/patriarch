#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// PAT_ASSERT(cond, "fmt {}", args...)  - debug-only check; compiled out in release builds
//                                         (the condition is NOT evaluated there).
// PAT_VERIFY(cond, "fmt {}", args...)  - always evaluates the condition. On failure: logs,
//                                         increments the failure counter and calls the handler.
//                                         The default handler stops the process in debug builds
//                                         and lets execution continue in release builds.
//
// Define PATRIARCH_FORCE_ASSERTS to keep PAT_ASSERT active in release builds.

#if !defined(NDEBUG) || defined(PATRIARCH_FORCE_ASSERTS)
#define PAT_ASSERTS_ENABLED 1
#else
#define PAT_ASSERTS_ENABLED 0
#endif

namespace pat::core {

struct AssertInfo {
    const char* expression = "";
    const char* file = "";
    int line = 0;
    std::string_view message;
};

using AssertHandler = void (*)(const AssertInfo&);

/// Installs a handler and returns the previous one. Passing nullptr restores the default handler.
AssertHandler set_assert_handler(AssertHandler handler) noexcept;

/// Number of failed PAT_ASSERT / PAT_VERIFY checks since start (or the last reset).
[[nodiscard]] std::uint64_t assert_failure_count() noexcept;
void reset_assert_failure_count() noexcept;

/// RAII helper (mostly for tests): installs a handler for the current scope.
class ScopedAssertHandler {
public:
    explicit ScopedAssertHandler(AssertHandler handler) noexcept : previous_(set_assert_handler(handler)) {}
    ~ScopedAssertHandler() { set_assert_handler(previous_); }
    ScopedAssertHandler(const ScopedAssertHandler&) = delete;
    ScopedAssertHandler& operator=(const ScopedAssertHandler&) = delete;

private:
    AssertHandler previous_;
};

namespace detail {

inline std::string format_message() {
    return {};
}

template <class... Args>
std::string format_message(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

void assert_fail(const char* expression, const char* file, int line, const std::string& message);

}  // namespace detail
}  // namespace pat::core

#define PAT_VERIFY(cond, ...)                                                                                \
    do {                                                                                                     \
        if (!(cond)) [[unlikely]] {                                                                          \
            ::pat::core::detail::assert_fail(                                                                \
                #cond, __FILE__, __LINE__, ::pat::core::detail::format_message(__VA_ARGS__));                \
        }                                                                                                    \
    } while (false)

#if PAT_ASSERTS_ENABLED
#define PAT_ASSERT(cond, ...) PAT_VERIFY(cond, __VA_ARGS__)
#else
#define PAT_ASSERT(cond, ...)                                                                                \
    do {                                                                                                     \
        (void)sizeof(!(cond));                                                                               \
    } while (false)
#endif
