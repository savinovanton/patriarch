#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pat::core {

/// Length in bytes of the strictly valid UTF-8 sequence at the start of `s`; 0 if `s` is empty or the
/// sequence is malformed (bad lead or continuation byte, overlong form, surrogate, beyond U+10FFFF,
/// truncated). A NUL byte is a valid one-byte sequence: callers that forbid it check it themselves.
[[nodiscard]] constexpr std::size_t utf8_sequence_length(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    const auto b0 = static_cast<std::uint8_t>(s[0]);
    if (b0 < 0x80U) {
        return 1;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    std::uint32_t min_cp = 0;
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        len = 2;
        cp = b0 & 0x1FU;
        min_cp = 0x80U;
    } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
        len = 3;
        cp = b0 & 0x0FU;
        min_cp = 0x800U;
    } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
        len = 4;
        cp = b0 & 0x07U;
        min_cp = 0x10000U;
    } else {
        return 0;
    }
    if (s.size() < len) {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        const auto b = static_cast<std::uint8_t>(s[i]);
        if ((b & 0xC0U) != 0x80U) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3FU);
    }
    if (cp < min_cp || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return 0;
    }
    return len;
}

}  // namespace pat::core
