#include "pat/core/compress.hpp"

#include <zstd.h>

#include <utility>

namespace pat::core {

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, int level) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(input.size()));
    const std::size_t written = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
    if (ZSTD_isError(written) != 0) {
        return {};  // cannot happen with a correctly sized output buffer
    }
    out.resize(written);
    return out;
}

Result<std::vector<std::uint8_t>, Error> decompress(std::span<const std::uint8_t> input,
                                                    std::size_t expected_size) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    std::vector<std::uint8_t> out(expected_size);
    const std::size_t produced = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
    if (ZSTD_isError(produced) != 0) {
        return R::err(Error{1, ZSTD_getErrorName(produced)});
    }
    if (produced != expected_size) {
        return R::err(Error{2, "decompressed size differs from the size recorded in the header"});
    }
    return R::ok(std::move(out));
}

}  // namespace pat::core
