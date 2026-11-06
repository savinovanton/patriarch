#include "snapshots.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/stress/world_generator.hpp"

#include <algorithm>
#include <utility>

namespace pat::snapshot_spike {
namespace {

template <class T>
std::shared_ptr<T> counted(T value, std::size_t bytes, const std::shared_ptr<Memory>& memory) {
    auto* object = new T(std::move(value));
    memory->add(bytes);
    return std::shared_ptr<T>(object, [memory, bytes](T* p) {
        delete p;
        memory->live.fetch_sub(bytes);
    });
}

std::shared_ptr<Data> copy_data(const Data& source, const std::shared_ptr<Memory>& memory) {
    Data copy = source;
    const auto bytes = sizeof(Data) + copy.rows.capacity() * sizeof(Row);
    return counted(std::move(copy), bytes, memory);
}

template <class Access>
std::uint64_t read_checksum(const Meta& meta, Access at) {
    core::HashStream hash;
    hash.u64(meta.version);
    auto scope = [&](std::uint32_t start, std::uint32_t size, std::uint32_t limit) {
        const auto count = std::min(size, limit);
        for (std::uint32_t i = 0; i < count; ++i) {
            // Spread the viewport over the full source, including the last page.
            const auto row = start + (count == 1 ? 0 : i * (size - 1) / (count - 1));
            for (std::size_t field = 0; field < kFields; ++field) {
                hash.u32(at(row, field));
            }
        }
    };
    scope(0, meta.provinces, 400);
    scope(meta.provinces, meta.dynasties, 100);
    return hash.value();
}

}  // namespace

void Memory::add(std::size_t bytes) {
    allocations.fetch_add(1);
    const auto current = live.fetch_add(bytes) + bytes;
    auto previous = peak.load();
    while (previous < current && !peak.compare_exchange_weak(previous, current)) {
    }
}

Transport::Transport(Method method, const Data& initial)
    : method_(method), producer_version_(initial.meta.version) {
    if (method == Method::Full) {
        full_.store(copy_data(initial, memory_));
    } else if (method == Method::Diff) {
        cache_ = copy_data(initial, memory_);
        pending_.reserve(kQueueLimit);
        batch_.reserve(kQueueLimit);
    } else {
        const auto count = (initial.rows.size() + kRowsPerPage - 1) / kRowsPerPage;
        for (auto& bank : banks_) {
            for (std::size_t p = 0; p < count; ++p) {
                Page page;
                page.version = initial.meta.version;
                const auto start = p * kRowsPerPage;
                std::copy_n(initial.rows.begin() + static_cast<std::ptrdiff_t>(start),
                            std::min(kRowsPerPage, initial.rows.size() - start),
                            page.rows.begin());
                bank.push_back(counted(std::move(page), sizeof(Page), memory_));
            }
        }
        Directory dir{initial.meta, {}};
        dir.pages.assign(banks_[1].begin(), banks_[1].end());
        const auto bytes = sizeof(Directory) + dir.pages.capacity() * sizeof(std::shared_ptr<const Page>);
        directory_.store(counted(std::move(dir), bytes, memory_));
    }
}

void Transport::publish(const Data& source,
                        std::span<const Cell> changes,
                        std::span<const std::uint64_t> page_versions) {
    if (method_ == Method::Full) {
        full_.store(copy_data(source, memory_));
        traffic_.payload_bytes += source.rows.size() * sizeof(Row);
    } else if (method_ == Method::Diff) {
        Packet packet{source.meta, producer_version_, {}, {}};
        packet.cells.assign(changes.begin(), changes.end());
        std::lock_guard lock(mutex_);
        if (pending_.size() == kQueueLimit) {
            // Latest-state semantics: replace an overflowing delta chain with a full checkpoint.
            // No lost base versions, unbounded queue, or waiting for a UI frame.
            pending_.clear();
            packet.cells.clear();
            packet.cells.shrink_to_fit();
            packet.checkpoint = copy_data(source, memory_);
            ++traffic_.checkpoints;
            traffic_.payload_bytes += source.rows.size() * sizeof(Row);
        } else {
            traffic_.payload_bytes += changes.size_bytes();
        }
        const auto bytes = sizeof(Packet) + packet.cells.capacity() * sizeof(Cell);
        pending_.push_back(counted(std::move(packet), bytes, memory_));
        traffic_.queue_peak = std::max(traffic_.queue_peak, static_cast<std::uint64_t>(pending_.size()));
        producer_version_ = source.meta.version;
    } else {
        auto& bank = banks_[back_];
        Directory dir{source.meta, {}};
        dir.pages.reserve(bank.size());
        for (std::size_t p = 0; p < bank.size(); ++p) {
            if (bank[p]->version != page_versions[p]) {
                if (bank[p].use_count() > 1) {
                    bank[p] = counted(Page{}, sizeof(Page), memory_);
                } else {
                    ++traffic_.reused_pages;
                }
                const auto start = p * kRowsPerPage;
                const auto count = std::min(kRowsPerPage, source.rows.size() - start);
                std::copy_n(
                    source.rows.begin() + static_cast<std::ptrdiff_t>(start), count, bank[p]->rows.begin());
                bank[p]->version = page_versions[p];
                traffic_.payload_bytes += count * sizeof(Row);
            }
            dir.pages.push_back(bank[p]);
        }
        const auto bytes = sizeof(Directory) + dir.pages.capacity() * sizeof(std::shared_ptr<const Page>);
        directory_.store(counted(std::move(dir), bytes, memory_));
        back_ ^= 1;
    }
}

Frame Transport::acquire() {
    if (method_ == Method::Full) {
        return {full_.load(), {}};
    }
    if (method_ == Method::Pages) {
        return {{}, directory_.load()};
    }
    {
        std::lock_guard lock(mutex_);
        batch_.swap(pending_);
    }
    if (!batch_.empty()) {
        if (cache_.use_count() > 1) {
            cache_ = copy_data(*cache_, memory_);
            traffic_.consumer_bytes += cache_->rows.size() * sizeof(Row);
            ++traffic_.retained_cache_copies;
        }
        for (const auto& packet : batch_) {
            if (packet->checkpoint) {
                *cache_ = *packet->checkpoint;
                traffic_.consumer_bytes += cache_->rows.size() * sizeof(Row);
            } else {
                if (packet->base_version != cache_->meta.version) {
                    valid_.store(false);
                }
                for (const auto& cell : packet->cells) {
                    cache_->rows[cell.row][cell.field] = cell.value;
                    traffic_.consumer_bytes += sizeof(cell.value);
                }
                cache_->meta = packet->meta;
            }
        }
        batch_.clear();
    }
    return {cache_, {}};
}

core::Result<Fixture> make_fixture(bool triple) {
    stress::WorldConfig config;
    config.seed = 7;
    config.characters = triple ? 15000 : 5000;
    config.provinces = triple ? 1200 : 400;
    config.dynasties = triple ? 300 : 100;
    auto generated = stress::generate_world(config);
    if (!generated) {
        return core::Result<Fixture>::err(generated.error());
    }
    Fixture fixture{std::move(generated).value(), {}, {}, {}, {}, 0};
    fixture.source.meta.provinces = config.provinces;
    fixture.source.meta.dynasties = config.dynasties;
    fixture.source.rows.resize(config.provinces + config.dynasties);
    for (std::uint32_t p = 0; p < config.provinces; ++p) {
        const auto& t = fixture.world.provinces();
        fixture.source.rows[p] = {t.name[p].value(),
                                  t.owner[p].value(),
                                  static_cast<std::uint32_t>(t.population[p].raw()),
                                  t.development[p],
                                  p,
                                  p * 3,
                                  p * 7,
                                  p * 11};
    }
    for (std::uint32_t d = 0; d < config.dynasties; ++d) {
        const auto& t = fixture.world.dynasties();
        const sim::DynastyId id(d);
        fixture.source.rows[config.provinces + d] = {
            t.name[d].value(),
            t.head[d].value(),
            static_cast<std::uint32_t>(t.honor[d]),
            static_cast<std::uint32_t>(fixture.world.members_of(id).size()),
            static_cast<std::uint32_t>(fixture.world.provinces_of(id).size()),
            d,
            d * 5,
            d * 13};
    }
    for (std::uint32_t i = 0; i < fixture.source.rows.size(); ++i) {
        fixture.permutation.push_back(i);
    }
    fixture.world.rng().stream("snapshot.fixture").shuffle<std::uint32_t>(fixture.permutation);
    fixture.page_versions.resize((fixture.source.rows.size() + kRowsPerPage - 1) / kRowsPerPage, 0);
    fixture.changes.reserve(fixture.source.rows.size());
    fixture.source.meta.checksum = checksum(fixture.source);
    fixture.source.meta.ready = Clock::now();
    return core::Result<Fixture>::ok(std::move(fixture));
}

void advance(Fixture& fixture, std::uint32_t changed_permille) {
    auto& source = fixture.source;
    ++source.meta.version;
    auto& rng = fixture.world.rng().stream("snapshot.changes");
    const auto count = (source.rows.size() * changed_permille + 999) / 1000;
    fixture.changes.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const auto row = fixture.permutation[(fixture.cursor + i) % source.rows.size()];
        const auto field = static_cast<std::uint32_t>((source.meta.version + row) % kFields);
        const auto value = source.rows[row][field] + 1 + rng.bounded(100);
        source.rows[row][field] = value;
        fixture.changes.push_back({row, field, value});
        fixture.page_versions[row / kRowsPerPage] = source.meta.version;
    }
    fixture.cursor = (fixture.cursor + count) % source.rows.size();
    source.meta.checksum = checksum(source);
    source.meta.ready = Clock::now();
}

std::uint64_t checksum(const Data& data) {
    return read_checksum(data.meta,
                         [&](std::size_t row, std::size_t field) { return data.rows[row][field]; });
}

std::uint64_t checksum(const Frame& frame) {
    if (frame.flat) {
        return checksum(*frame.flat);
    }
    return read_checksum(frame.meta(), [&](std::size_t row, std::size_t field) {
        return frame.paged->pages[row / kRowsPerPage]->rows[row % kRowsPerPage][field];
    });
}

}  // namespace pat::snapshot_spike
