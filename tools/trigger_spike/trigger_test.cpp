#include <doctest/doctest.h>

#include <algorithm>

#include "triggers.hpp"

namespace ts = pat::trigger_spike;

TEST_CASE("trigger spike: indexed dispatch and complete pulses match full scan") {
    ts::Config config;
    config.characters = 64;
    config.provinces = 12;
    config.events = 50;
    config.changed_permille = 1000;
    config.sample_permille = 1000;
    auto generated = ts::make_fixture(config);
    REQUIRE(generated.has_value());
    auto& fixture = generated.value();
    ts::Evaluator full(fixture), index(fixture), sample(fixture);
    for (int day = 0; day < 35; ++day) {
        ts::prepare_tick(fixture);
        full.run(fixture, ts::Method::Naive);
        for (auto& table : fixture.tables) {
            std::reverse(table.changed.begin(), table.changed.end());
        }
        index.run(fixture, ts::Method::Indexed);
        sample.run(fixture, ts::Method::Sampled);
        CHECK(full.actions == index.actions);
        CHECK(full.pulses == index.pulses);
        CHECK(full.actions == sample.actions);
        CHECK(full.pulses == sample.pulses);
        CHECK(full.counts.predicates == index.counts.predicates);
        CHECK(std::adjacent_find(index.actions.begin(), index.actions.end()) == index.actions.end());
        if (day >= 28 && day <= 30) {
            CHECK(full.pulses.empty());
        }
        fixture.world.advance_day();
    }
}

TEST_CASE("trigger spike: sampled pulses are a subset and action delivery stays exact") {
    ts::Config config;
    config.characters = 128;
    config.provinces = 16;
    config.events = 80;
    config.changed_permille = 100;
    auto generated = ts::make_fixture(config);
    REQUIRE(generated.has_value());
    auto fixture = generated.value();
    auto duplicate = generated.value();
    ts::Evaluator full(fixture), sampled(fixture), replay(duplicate);
    ts::Audit audit(fixture.audit_size);
    for (int day = 0; day < 35; ++day) {
        ts::prepare_tick(fixture);
        ts::prepare_tick(duplicate);
        full.run(fixture, ts::Method::Indexed);
        sampled.run(fixture, ts::Method::Sampled);
        replay.run(duplicate, ts::Method::Sampled);
        CHECK(full.actions == sampled.actions);
        CHECK(sampled.pulses == replay.pulses);
        CHECK(std::includes(
            full.pulses.begin(), full.pulses.end(), sampled.pulses.begin(), sampled.pulses.end()));
        audit.observe(fixture, full.pulses, day, false);
        audit.observe(fixture, sampled.pulses, day, true);
        fixture.world.advance_day();
        duplicate.world.advance_day();
    }
    for (std::size_t i = 0; i < audit.full_first.size(); ++i) {
        if (audit.sampled_first[i] >= 0) {
            CHECK(audit.full_first[i] >= 0);
            CHECK(audit.sampled_first[i] >= audit.full_first[i]);
        }
    }
    // Explicit multi-action/scope case: an event subscribed to both bits must fire only once.
    for (auto& table : fixture.tables) {
        table.changed.clear();
        std::fill(table.actions.begin(), table.actions.end(), 0);
        for (auto& field : table.fields) {
            std::fill(field.begin(), field.end(), 1);
        }
        table.actions[0] = 3;
        table.changed.push_back({ts::SubjectId(0), 3});
    }
    fixture.definitions.resize(2);
    fixture.definitions[0] = {ts::Scope::Character, false, 3, 0, {0, 1, 2}, 0, 100, 1};
    fixture.definitions[1] = {ts::Scope::Province, false, 3, 0, {0, 1, 2}, 0, 100, 1};
    fixture.index = ts::build_index(fixture.definitions);
    ts::Evaluator exact(fixture);
    exact.run(fixture, ts::Method::Indexed);
    REQUIRE(exact.actions.size() == 2);
    CHECK(exact.actions[0].event == ts::EventId(0));
    CHECK(exact.actions[1].event == ts::EventId(1));
    CHECK(exact.counts.duplicates == 2);
}
