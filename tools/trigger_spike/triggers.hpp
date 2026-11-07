#pragma once

#include "pat/core/ids.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pat::trigger_spike {

enum class Scope : std::size_t { Character, Province };
enum class Method : std::size_t { Naive, Indexed, Sampled };
using EventId = core::StrongId<struct EventTag>;
// An index inside a Definition's checked scope, not an interchangeable game entity ID.
using SubjectId = core::StrongId<struct SubjectTag>;
inline constexpr std::size_t kActions = 16;
inline constexpr std::size_t kFields = 8;

struct Config {
    std::uint32_t characters = 5000;
    std::uint32_t provinces = 400;
    std::uint32_t events = 500;
    std::uint32_t changed_permille = 10;
    std::uint32_t sample_permille = 100;
    std::uint64_t seed = 7;
};

struct Definition {
    Scope scope = Scope::Character;
    bool monthly = false;
    std::uint16_t actions = 0;
    std::uint8_t priority = 0;
    std::array<std::uint8_t, 3> fields{};
    std::uint32_t lower = 0;
    std::uint32_t upper = 100;
    std::uint32_t flag = 1;
    std::size_t audit_offset = 0;
};

struct Notification {
    SubjectId subject;
    std::uint16_t actions = 0;
};

struct Table {
    std::array<std::vector<std::uint32_t>, kFields> fields;
    std::vector<std::uint16_t> actions;
    std::vector<SubjectId> permutation;
    std::vector<Notification> changed;
    std::size_t cursor = 0;
    [[nodiscard]] std::size_t size() const { return actions.size(); }
};

struct Index {
    std::array<std::array<std::vector<EventId>, kActions>, 2> action;
    std::array<std::vector<EventId>, 2> monthly;
};

struct Fixture {
    Fixture(sim::World w, const Config& c) : world(std::move(w)), config(c) {}

    sim::World world;
    Config config;
    std::array<Table, 2> tables;
    // Sorted by priority, then stable original order. EventId is the rank in this array.
    std::vector<Definition> definitions;
    Index index;
    std::uint64_t sample_key = 0;
    std::size_t audit_size = 0;
};

struct Hit {
    EventId event;
    SubjectId subject;
    [[nodiscard]] auto operator<=>(const Hit&) const = default;
};

struct Counts {
    std::uint64_t visited = 0;
    std::uint64_t predicates = 0;
    std::uint64_t duplicates = 0;
};

struct Evaluator {
    std::vector<Hit> actions;
    std::vector<Hit> pulses;
    std::vector<std::uint32_t> seen;
    std::uint32_t stamp = 0;
    Counts counts;
    explicit Evaluator(const Fixture& fixture);
    void run(const Fixture& fixture, Method method);
};

[[nodiscard]] core::Result<Fixture> make_fixture(const Config& config);
[[nodiscard]] Index build_index(const std::vector<Definition>& definitions);
void prepare_tick(Fixture& fixture);
[[nodiscard]] std::uint64_t hit_hash(const std::vector<Hit>& hits);

struct Audit {
    std::vector<int> full_first;
    std::vector<int> sampled_first;
    explicit Audit(std::size_t size) : full_first(size, -1), sampled_first(size, -1) {}
    void observe(const Fixture& fixture, const std::vector<Hit>& hits, int day, bool sampled);
};

}  // namespace pat::trigger_spike
