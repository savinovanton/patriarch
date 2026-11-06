#pragma once

#include <string_view>

namespace pat::core {

/// Project version as declared in the top-level CMakeLists.txt.
[[nodiscard]] std::string_view version() noexcept;

/// True when the binary was built with floating-point-free simulation rules
/// enabled (placeholder for E1; always true for now).
[[nodiscard]] constexpr bool deterministic_build() noexcept {
    return true;
}

}  // namespace pat::core
