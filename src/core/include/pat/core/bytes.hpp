#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace pat::core {

/// Appends fixed-width little-endian values to a byte buffer (file formats, replays, saves).
class ByteWriter {
public:
    void u8(std::uint8_t v) { data_.push_back(v); }
    void u16(std::uint16_t v) { put(v, 2); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u64(std::uint64_t v) { put(v, 8); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void boolean(bool v) { u8(v ? 1U : 0U); }
    void bytes(std::span<const std::uint8_t> b) { data_.insert(data_.end(), b.begin(), b.end()); }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(data_); }

private:
    void put(std::uint64_t v, int width) {
        for (int i = 0; i < width; ++i) {
            data_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    }
    std::vector<std::uint8_t> data_;
};

/// Reads values written by ByteWriter. Reading past the end returns 0 and puts the reader into a
/// failed state (check ok() once after a group of reads) - no exceptions, no out-of-bounds access.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    [[nodiscard]] std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(take(1)); }
    [[nodiscard]] std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(take(2)); }
    [[nodiscard]] std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(take(4)); }
    [[nodiscard]] std::uint64_t u64() noexcept { return take(8); }
    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    [[nodiscard]] std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    [[nodiscard]] bool boolean() noexcept { return u8() != 0; }

    /// Next `count` bytes (empty span and failed state if fewer remain).
    [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t count) noexcept {
        if (failed_ || count > remaining()) {
            failed_ = true;
            return {};
        }
        const auto out = data_.subspan(pos_, count);
        pos_ += count;
        return out;
    }

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    void fail() noexcept { failed_ = true; }

private:
    [[nodiscard]] std::uint64_t take(int width) noexcept {
        if (failed_ || static_cast<std::size_t>(width) > remaining()) {
            failed_ = true;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < width; ++i) {
            v |= static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        }
        pos_ += static_cast<std::size_t>(width);
        return v;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace pat::core
