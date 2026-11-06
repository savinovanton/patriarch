#pragma once

#include "pat/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pat::core {

/// Compresses a byte buffer with zstd (level 3 is a good speed/size balance for saves).
[[nodiscard]] std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, int level = 3);

/// Decompresses a zstd frame whose original size is known. Fails (without throwing) on corrupt
/// data or when the frame does not decompress to exactly `expected_size` bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>, Error> decompress(std::span<const std::uint8_t> input,
                                                                  std::size_t expected_size);

}  // namespace pat::core
