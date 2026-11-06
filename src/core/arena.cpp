#include "pat/core/arena.hpp"

#include "pat/core/assert.hpp"

#include <algorithm>

namespace pat::core {

Arena::Arena(std::size_t block_size) : block_size_(std::max<std::size_t>(block_size, 256)) {}

void* Arena::allocate(std::size_t size, std::size_t alignment) {
    PAT_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0,
               "Arena alignment must be a power of two");
    if (size == 0) {
        size = 1;  // always return a unique address
    }

    for (;;) {
        if (current_ < blocks_.size()) {
            Block& block = blocks_[current_];
            const auto base = reinterpret_cast<std::uintptr_t>(block.data.get());
            const std::uintptr_t aligned =
                (base + offset_ + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
            const std::size_t end = static_cast<std::size_t>(aligned - base) + size;
            if (end <= block.size) {
                offset_ = end;
                used_ += size;
                return reinterpret_cast<void*>(aligned);
            }
            // Does not fit in this block; try the next one (reused after reset) or add a new one.
            ++current_;
            offset_ = 0;
            continue;
        }
        const std::size_t needed = std::max(block_size_, size + alignment);
        blocks_.push_back(Block{std::make_unique<std::byte[]>(needed), needed});
        reserved_ += needed;
        // current_ == blocks_.size() - 1 now; loop retries with the fresh block
    }
}

void Arena::reset() noexcept {
    current_ = 0;
    offset_ = 0;
    used_ = 0;
}

}  // namespace pat::core
