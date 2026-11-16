#pragma once

#include "pat/core/bytes.hpp"
#include "pat/core/result.hpp"
#include "pat/mapdata/pmd2_container.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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

/// The error value every mapdata module returns: a stable numeric code plus a message.
inline core::Error make_error(int code, std::string message) {
    return core::Error{code, std::move(message)};
}

/// End-of-blob check shared by the PMD2 layer decoders: the reader must not have run out of bytes
/// ("<CHUNK>: truncated") and, unless `allow_trailing`, must be at the end ("<CHUNK>: trailing bytes").
inline core::Result<void, core::Error>
finish_blob(const core::ByteReader& r, bool allow_trailing, Pmd2ChunkId id, int bad_chunk_code) {
    using R = core::Result<void, core::Error>;
    if (!r.ok()) {
        return R::err(make_error(bad_chunk_code, pmd2_chunk_id_string(id) + ": truncated"));
    }
    if (!allow_trailing && !r.at_end()) {
        return R::err(make_error(bad_chunk_code, pmd2_chunk_id_string(id) + ": trailing bytes"));
    }
    return R::ok();
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
