#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Private helper of the map layers: labelling of 4-connected regions in a raster.

namespace pat::mapdata::raster {

/// Labels the 4-connected components of pixels with equal non-zero `klass` (pixels of different
/// classes never join) in raster order of their first pixel: label[i] = 1..N, 0 where klass is 0.
/// Returns N; `overflow` is set (and labelling stops) if there are more than a u16 can number.
inline std::size_t label_components(std::size_t width,
                                    std::span<const std::uint8_t> klass,
                                    std::vector<std::uint16_t>& label,
                                    bool& overflow) {
    const std::size_t n = klass.size();
    label.assign(n, 0);
    overflow = false;
    std::size_t count = 0;
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < n; ++start) {
        const std::uint8_t k = klass[start];
        if (k == 0 || label[start] != 0) {
            continue;
        }
        if (count == std::numeric_limits<std::uint16_t>::max()) {
            overflow = true;
            return count;
        }
        const auto id = static_cast<std::uint16_t>(++count);
        label[start] = id;
        stack.push_back(start);
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const auto visit = [&](std::size_t j) {
                if (klass[j] == k && label[j] == 0) {
                    label[j] = id;
                    stack.push_back(j);
                }
            };
            const std::size_t x = i % width;
            if (x > 0) {
                visit(i - 1);
            }
            if (x + 1 < width) {
                visit(i + 1);
            }
            if (i >= width) {
                visit(i - width);
            }
            if (i + width < n) {
                visit(i + width);
            }
        }
    }
    return count;
}

}  // namespace pat::mapdata::raster
