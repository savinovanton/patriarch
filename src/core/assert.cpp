#include "pat/core/assert.hpp"

#include "pat/core/log.hpp"

#include <atomic>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace pat::core {

namespace {

std::atomic<AssertHandler> g_handler{nullptr};
std::atomic<std::uint64_t> g_failure_count{0};

void default_handler(const AssertInfo& /*info*/) {
#if PAT_ASSERTS_ENABLED
#ifdef _WIN32
    if (IsDebuggerPresent() != FALSE) {
        __debugbreak();
    }
#endif
    std::abort();
#endif
}

}  // namespace

AssertHandler set_assert_handler(AssertHandler handler) noexcept {
    return g_handler.exchange(handler);
}

std::uint64_t assert_failure_count() noexcept {
    return g_failure_count.load();
}

void reset_assert_failure_count() noexcept {
    g_failure_count.store(0);
}

namespace detail {

void assert_fail(const char* expression, const char* file, int line, const std::string& message) {
    g_failure_count.fetch_add(1);

    Logger::instance().log(
        LogLevel::Fatal,
        "assert",
        std::format("assertion failed: {}{}{}", expression, message.empty() ? "" : " - ", message),
        file,
        line);

    const AssertInfo info{expression, file, line, message};
    if (AssertHandler handler = g_handler.load(); handler != nullptr) {
        handler(info);
    } else {
        default_handler(info);
    }
}

}  // namespace detail
}  // namespace pat::core
