#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal PNG writer (RGB8, "stored" deflate blocks, no compression): enough to eyeball generated maps
// without adding an image library dependency.

namespace pat::core {

inline std::uint32_t png_crc(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0xFFFFFFFFU) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            }
            t[i] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

inline void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24U));
    v.push_back(static_cast<std::uint8_t>(x >> 16U));
    v.push_back(static_cast<std::uint8_t>(x >> 8U));
    v.push_back(static_cast<std::uint8_t>(x));
}

inline void
put_chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, png_crc(body.data(), body.size()) ^ 0xFFFFFFFFU);
}

inline bool write_png_rgb(const std::string& path,
                          std::uint32_t width,
                          std::uint32_t height,
                          const std::vector<std::uint8_t>& rgb) {
    // Raw scanlines with filter type 0.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 3 + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const std::uint8_t* row = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 3);
    }

    // zlib stream made of stored deflate blocks.
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFFU));
        z.push_back(static_cast<std::uint8_t>(n >> 8U));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFFU));
        z.push_back(static_cast<std::uint8_t>((~n >> 8U) & 0xFFU));
        z.insert(z.end(),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) {
            break;
        }
    }
    put_be32(z, (b << 16U) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, deflate, no filter, no interlace
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace pat::core
