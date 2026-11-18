#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// Small checks shared by the validators of rule sets (E12-T04; court_rules.cpp and marriage_rules.cpp keep
// their older private copies, see docs/dev/code-duplication-audit.md). `Error` is an aggregate
// `{std::string message; std::string field;}`.

namespace pat::sim::rules_check {

template <class Error>
using Check = core::Result<void, Error>;

template <class Error>
Check<Error> fail(std::string message, std::string field) {
    return Check<Error>::err(Error{std::move(message), std::move(field)});
}

template <class Error>
Check<Error> in_range(std::string_view name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail<Error>(std::format("{} = {} is out of range {}...{}", name, value, lo, hi),
                           std::string(name));
    }
    return Check<Error>::ok();
}

}  // namespace pat::sim::rules_check

#define PAT_RULES_CHECK(expr)                                                                                \
    do {                                                                                                     \
        if (auto res_ = (expr); !res_)                                                                       \
            return res_;                                                                                     \
    } while (false)
