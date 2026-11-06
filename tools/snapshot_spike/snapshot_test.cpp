#include <doctest/doctest.h>

#include <array>
#include <thread>

#include "snapshots.hpp"

namespace ss = pat::snapshot_spike;

TEST_CASE("snapshot spike: skipped versions, delta overflow and retained frames stay exact") {
    auto generated = ss::make_fixture(false);
    REQUIRE(generated.has_value());
    for (const auto method : {ss::Method::Full, ss::Method::Diff, ss::Method::Pages}) {
        auto fixture = generated.value();
        ss::Frame retained;
        std::shared_ptr<ss::Memory> memory;
        {
            ss::Transport transport(method, fixture.source);
            memory = transport.memory();
            retained = transport.acquire();
            const auto original = ss::checksum(retained);
            // No acquisitions for >64 publications: Diff must replace the queue with a checkpoint;
            // Pages must catch up a stale bank and copy any page still owned by the old reader.
            for (int tick = 0; tick < 80; ++tick) {
                ss::advance(fixture, tick % 2 ? 10 : 1000);
                transport.publish(fixture.source, fixture.changes, fixture.page_versions);
            }
            const auto current = transport.acquire();
            CHECK(current.meta().version == fixture.source.meta.version);
            CHECK(ss::checksum(current) == fixture.source.meta.checksum);
            for (std::size_t row = 0; row < fixture.source.rows.size(); ++row) {
                for (std::size_t field = 0; field < ss::kFields; ++field) {
                    CHECK(current.at(row, field) == fixture.source.rows[row][field]);
                }
            }
            CHECK(transport.valid());
            CHECK(ss::checksum(retained) == original);
            if (method == ss::Method::Diff) {
                CHECK(transport.traffic().checkpoints == 1);
                CHECK(transport.traffic().queue_peak == ss::kQueueLimit);
                CHECK(transport.traffic().retained_cache_copies == 1);
            }
        }
        // A returned snapshot owns its storage independently of the transport lifetime.
        CHECK(ss::checksum(retained) == retained.meta().checksum);
        CHECK(memory->live.load() > 0);
        retained = {};
        CHECK(memory->live.load() == 0);
    }
}

TEST_CASE("snapshot spike: concurrent reader sees complete monotone immutable versions") {
    auto generated = ss::make_fixture(false);
    REQUIRE(generated.has_value());
    for (const auto method : {ss::Method::Full, ss::Method::Diff, ss::Method::Pages}) {
        auto fixture = generated.value();
        ss::Transport transport(method, fixture.source);
        std::atomic<bool> done{false};
        std::thread producer([&] {
            for (int tick = 0; tick < 200; ++tick) {
                ss::advance(fixture, 100);
                transport.publish(fixture.source, fixture.changes, fixture.page_versions);
            }
            done.store(true);
        });
        std::array<ss::Frame, 3> held;
        std::size_t cursor = 0;
        std::uint64_t previous = 0;
        do {
            const auto frame = transport.acquire();
            CHECK(frame.meta().version >= previous);
            CHECK(ss::checksum(frame) == frame.meta().checksum);
            previous = frame.meta().version;
            held[cursor++ % held.size()] = frame;
            for (const auto& old : held) {
                if (old.flat || old.paged)
                    CHECK(ss::checksum(old) == old.meta().checksum);
            }
        } while (!done.load());
        producer.join();
        CHECK(transport.valid());
        const auto last = transport.acquire();
        CHECK(last.meta().version == fixture.source.meta.version);
        CHECK(ss::checksum(last) == fixture.source.meta.checksum);
    }
}
