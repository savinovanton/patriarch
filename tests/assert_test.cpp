#include "pat/core/assert.hpp"
#include "pat/core/log.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace pat::core;

namespace {

// Returns its argument; the volatile read stops the compiler from treating it as a constant.
int opaque(int value) {
    volatile int v = value;
    return v;
}

int g_handler_calls = 0;
std::string g_last_expression;
std::string g_last_message;

void recording_handler(const AssertInfo& info) {
    ++g_handler_calls;
    g_last_expression = info.expression;
    g_last_message = std::string(info.message);
}

struct AssertFixture {
    AssertFixture() {
        g_handler_calls = 0;
        g_last_expression.clear();
        g_last_message.clear();
        reset_assert_failure_count();
    }
    ScopedLogCapture capture;  // keeps the console quiet and lets tests inspect log output
    ScopedAssertHandler handler{&recording_handler};
};

}  // namespace

TEST_CASE_FIXTURE(AssertFixture, "assert: PAT_VERIFY reports failures and continues") {
    const int two = opaque(2);  // runtime value: avoids C4127 (constant conditional) in the macro
    PAT_VERIFY(two + 1 == 5, "math is broken: {}", 42);

    CHECK(g_handler_calls == 1);
    CHECK(assert_failure_count() == 1);
    CHECK(g_last_expression == "two + 1 == 5");
    CHECK(g_last_message == "math is broken: 42");
    CHECK(capture.sink().contains("assertion failed: two + 1 == 5"));
}

TEST_CASE_FIXTURE(AssertFixture, "assert: PAT_VERIFY with a true condition is silent") {
    const int two = opaque(2);
    PAT_VERIFY(two > 1);
    CHECK(g_handler_calls == 0);
    CHECK(assert_failure_count() == 0);
}

TEST_CASE_FIXTURE(AssertFixture, "assert: PAT_VERIFY always evaluates its condition") {
    int evaluations = 0;
    PAT_VERIFY(++evaluations > 0);
    CHECK(evaluations == 1);
}

TEST_CASE_FIXTURE(AssertFixture, "assert: PAT_ASSERT follows the build configuration") {
    int evaluations = 0;
    PAT_ASSERT(++evaluations == 99, "expected to fail only in assert-enabled builds");

#if PAT_ASSERTS_ENABLED
    CHECK(evaluations == 1);
    CHECK(g_handler_calls == 1);
    CHECK(assert_failure_count() == 1);
#else
    CHECK(evaluations == 0);  // the condition is not evaluated in release builds
    CHECK(g_handler_calls == 0);
#endif
}

TEST_CASE("assert: handler can be restored") {
    const AssertHandler before = set_assert_handler(&recording_handler);
    CHECK(set_assert_handler(before) == &recording_handler);
}
