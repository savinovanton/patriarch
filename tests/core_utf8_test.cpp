#include "pat/core/utf8.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace pat;

namespace {

// Reference decoder written independently of core::utf8_sequence_length (decodes the scalar value and
// re-encodes it; accepts exactly the canonical encodings of non-surrogate code points <= U+10FFFF).
std::size_t reference_length(std::string_view s) {
    if (s.empty()) {
        return 0;
    }
    const auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i])); };
    const std::uint32_t c = b(0);
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) {
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (s.size() < len) {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        if ((b(i) & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b(i) & 0x3F);
    }
    const std::size_t canonical = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (canonical != len || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return len;
}

}  // namespace

TEST_CASE("utf8_sequence_length: known sequences") {
    CHECK(core::utf8_sequence_length("") == 0);
    CHECK(core::utf8_sequence_length("a") == 1);
    CHECK(core::utf8_sequence_length(std::string_view("\0", 1)) == 1);
    CHECK(core::utf8_sequence_length("\xD0\x96") == 2);          // Zhe
    CHECK(core::utf8_sequence_length("\xE2\x82\xAC") == 3);      // euro sign
    CHECK(core::utf8_sequence_length("\xF0\x9F\x98\x80") == 4);  // emoji
    CHECK(core::utf8_sequence_length("\xD0") == 0);              // truncated
    CHECK(core::utf8_sequence_length("\x80") == 0);              // stray continuation
    CHECK(core::utf8_sequence_length("\xC0\x80") == 0);          // overlong NUL
    CHECK(core::utf8_sequence_length("\xC1\xBF") == 0);          // overlong
    CHECK(core::utf8_sequence_length("\xE0\x80\x80") == 0);      // overlong
    CHECK(core::utf8_sequence_length("\xED\xA0\x80") == 0);      // surrogate U+D800
    CHECK(core::utf8_sequence_length("\xED\x9F\xBF") == 3);      // U+D7FF
    CHECK(core::utf8_sequence_length("\xEE\x80\x80") == 3);      // U+E000
    CHECK(core::utf8_sequence_length("\xF4\x8F\xBF\xBF") == 4);  // U+10FFFF
    CHECK(core::utf8_sequence_length("\xF4\x90\x80\x80") == 0);  // U+110000
    CHECK(core::utf8_sequence_length("\xF0\x80\x80\x80") == 0);  // overlong
    CHECK(core::utf8_sequence_length("\xF5\x80\x80\x80") == 0);
    CHECK(core::utf8_sequence_length("\xFF") == 0);
}

TEST_CASE("utf8_sequence_length: agrees with the reference decoder") {
    // All lead bytes x all second bytes x a sample of third/fourth bytes, plus truncations.
    const std::uint8_t tails[] = {0x00, 0x80, 0xBF, 0xC0};
    for (unsigned b0 = 0; b0 < 256; ++b0) {
        for (unsigned b1 = 0; b1 < 256; ++b1) {
            for (const std::uint8_t b2 : tails) {
                for (const std::uint8_t b3 : tails) {
                    const char buf[4] = {static_cast<char>(b0),
                                         static_cast<char>(b1),
                                         static_cast<char>(b2),
                                         static_cast<char>(b3)};
                    for (std::size_t n = 1; n <= 4; ++n) {
                        const std::string_view s(buf, n);
                        if (core::utf8_sequence_length(s) != reference_length(s)) {
                            FAIL("mismatch for bytes " << b0 << " " << b1 << " " << unsigned(b2) << " "
                                                       << unsigned(b3) << " n=" << n);
                        }
                    }
                }
            }
        }
    }
}
