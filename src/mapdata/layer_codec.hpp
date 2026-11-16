#pragma once

#include "pat/core/bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

// Small helpers shared by the PMD2 layer codecs (physical, provinces, nature). Private to src/mapdata:
// the strings of every chunk blob are `u16 length + bytes`, and every key is `[a-z0-9-]`.

namespace pat::mapdata::codec {

inline void put_str(core::ByteWriter& w, const std::string& s) {
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.bytes({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

inline std::string get_str(core::ByteReader& r) {
    const std::uint16_t n = r.u16();
    const auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

/// A key of 1..max_length characters from [a-z0-9-].
inline bool valid_ascii_key(const std::string& key, std::size_t max_length) {
    if (key.empty() || key.size() > max_length) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

}  // namespace pat::mapdata::codec
