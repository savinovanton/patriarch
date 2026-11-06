#include "pat/core/result.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace pat::core;

namespace {

Result<int> parse_positive(int v) {
    if (v <= 0) {
        return Result<int>::err(Error{7, "not positive"});
    }
    return Result<int>::ok(v);
}

Result<void> check_even(int v) {
    if (v % 2 != 0) {
        return Result<void>::err(Error{1, "odd"});
    }
    return Result<void>::ok();
}

}  // namespace

TEST_CASE("result: ok value") {
    const auto r = parse_positive(5);
    REQUIRE(r.has_value());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 5);
    CHECK(r.value_or(-1) == 5);
}

TEST_CASE("result: error value") {
    const auto r = parse_positive(-3);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 7);
    CHECK(r.error().message == "not positive");
    CHECK(r.value_or(-1) == -1);
}

TEST_CASE("result: move-only payloads work") {
    auto r = Result<std::unique_ptr<int>>::ok(std::make_unique<int>(9));
    REQUIRE(r.has_value());
    auto p = std::move(r).value();
    CHECK(*p == 9);
}

TEST_CASE("result: same type for value and error") {
    auto ok = Result<std::string, std::string>::ok("fine");
    auto bad = Result<std::string, std::string>::err("broken");
    CHECK(ok.has_value());
    CHECK(ok.value() == "fine");
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error() == "broken");
}

TEST_CASE("result: void specialization") {
    CHECK(check_even(4).has_value());
    const auto bad = check_even(3);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().message == "odd");
}
