#pragma once

#include "pat/sim/world.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace pat::snapshot_spike {

using Clock = std::chrono::steady_clock;
inline constexpr std::size_t kFields = 8;
inline constexpr std::size_t kRowsPerPage = 128;
inline constexpr std::size_t kQueueLimit = 64;
using Row = std::array<std::uint32_t, kFields>;
enum class Method { Full, Diff, Pages };

struct Meta {
    std::uint64_t version = 0;
    std::uint64_t checksum = 0;
    Clock::time_point ready{};
    std::uint32_t provinces = 0;
    std::uint32_t dynasties = 0;
};
struct Data {
    Meta meta;
    std::vector<Row> rows;
};
struct Cell {
    std::uint32_t row = 0;
    std::uint32_t field = 0;
    std::uint32_t value = 0;
};
struct Page {
    std::array<Row, kRowsPerPage> rows{};
    std::uint64_t version = 0;
};
struct Directory {
    Meta meta;
    std::vector<std::shared_ptr<const Page>> pages;
};
struct Frame {
    std::shared_ptr<const Data> flat;
    std::shared_ptr<const Directory> paged;
    [[nodiscard]] const Meta& meta() const { return flat ? flat->meta : paged->meta; }
    [[nodiscard]] std::uint32_t at(std::size_t row, std::size_t field) const {
        return flat ? flat->rows[row][field]
                    : paged->pages[row / kRowsPerPage]->rows[row % kRowsPerPage][field];
    }
};

// Logical owned object/vector storage, not allocator/control-block overhead or process RSS.
struct Memory {
    std::atomic<std::uint64_t> live{0};
    std::atomic<std::uint64_t> peak{0};
    std::atomic<std::uint64_t> allocations{0};
    void add(std::size_t bytes);
};
struct Traffic {
    std::uint64_t payload_bytes = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t queue_peak = 0;
    std::uint64_t reused_pages = 0;
    std::uint64_t consumer_bytes = 0;
    std::uint64_t retained_cache_copies = 0;
};

// A single producer and a single UI consumer. Returned Frames may outlive the transport.
// Diff owns a UI cache; it copies that cache only when an older Frame still owns it.
// Pages keeps two banks, with copy-on-write when a reader still owns a stale page.
class Transport {
public:
    Transport(Method method, const Data& initial);
    void
    publish(const Data& source, std::span<const Cell> changes, std::span<const std::uint64_t> page_versions);
    [[nodiscard]] Frame acquire();
    [[nodiscard]] bool valid() const { return valid_.load(); }
    [[nodiscard]] const Traffic& traffic() const { return traffic_; }
    [[nodiscard]] std::shared_ptr<Memory> memory() const { return memory_; }

private:
    struct Packet {
        Meta meta;
        std::uint64_t base_version = 0;
        std::vector<Cell> cells;
        std::shared_ptr<const Data> checkpoint;
    };
    Method method_;
    std::shared_ptr<Memory> memory_ = std::make_shared<Memory>();
    Traffic traffic_;
    std::atomic<bool> valid_{true};
    std::atomic<std::shared_ptr<const Data>> full_;
    std::atomic<std::shared_ptr<const Directory>> directory_;
    std::array<std::vector<std::shared_ptr<Page>>, 2> banks_;
    std::size_t back_ = 0;
    std::mutex mutex_;
    std::vector<std::shared_ptr<const Packet>> pending_;
    std::vector<std::shared_ptr<const Packet>> batch_;  // UI thread only
    std::shared_ptr<Data> cache_;                       // UI thread only
    std::uint64_t producer_version_ = 0;
};

struct Fixture {
    sim::World world;
    Data source;
    std::vector<std::uint32_t> permutation;
    std::vector<std::uint64_t> page_versions;
    std::vector<Cell> changes;
    std::size_t cursor = 0;
};

[[nodiscard]] core::Result<Fixture> make_fixture(bool triple);
void advance(Fixture& fixture, std::uint32_t changed_permille);
// UI workload: all eight fields of at most 400 provinces and 100 dynasties.
[[nodiscard]] std::uint64_t checksum(const Frame& frame);
[[nodiscard]] std::uint64_t checksum(const Data& data);

}  // namespace pat::snapshot_spike
