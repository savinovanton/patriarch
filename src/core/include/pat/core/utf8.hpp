#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

/// Upper case of one code point for the scripts the game text uses: Latin a-z, Cyrillic
/// U+0430..U+044F (-> U+0410..U+042F) and U+0450..U+045F (-> U+0400..U+040F, so yo -> Yo). Every other
/// code point is returned unchanged (no full Unicode case mapping, no locale).
[[nodiscard]] constexpr char32_t to_upper_cp(char32_t cp) noexcept {
    if (cp >= U'a' && cp <= U'z') {
        return cp - 0x20;
    }
    if (cp >= 0x0430 && cp <= 0x044F) {
        return cp - 0x20;
    }
    if (cp >= 0x0450 && cp <= 0x045F) {
        return cp - 0x50;
    }
    return cp;
}

/// to_upper_cp applied to a UTF-8 string. Only ASCII and two-byte (Cyrillic) sequences can change;
/// every other byte, including malformed input, is copied through unchanged.
[[nodiscard]] inline std::string to_upper_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0xC2U && c <= 0xDFU && i + 1 < text.size() &&
            (static_cast<unsigned char>(text[i + 1]) & 0xC0U) == 0x80U) {
            const char32_t cp = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(text[i + 1]) & 0x3FU);
            const char32_t upper = to_upper_cp(cp);
            out += static_cast<char>(0xC0U | (upper >> 6U));
            out += static_cast<char>(0x80U | (upper & 0x3FU));
            ++i;
        } else {
            out += static_cast<char>(c < 0x80U ? to_upper_cp(c) : c);
        }
    }
    return out;
}

}  // namespace pat::core
