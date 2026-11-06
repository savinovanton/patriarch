#pragma once

#include "pat/core/fixed.hpp"
#include "pat/core/ids.hpp"
#include "pat/core/rng.hpp"
#include "pat/core/sparse_set.hpp"
#include "pat/core/string_interner.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/entity_table.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pat::sim {

using CharacterId = core::StrongId<struct CharacterIdTag>;
using ProvinceId = core::StrongId<struct ProvinceIdTag>;
using DynastyId = core::StrongId<struct DynastyIdTag>;

enum class Sex : std::uint8_t { Male, Female };

inline constexpr std::int32_t kMinHonor = -1000;
inline constexpr std::int32_t kMaxHonor = 10000;

// ---- tables (structure of arrays) -------------------------------------------------------------
// Columns are public for fast read access in systems; anything that touches an indexed column
// (character dynasty, province owner, dynasty head) must go through World so indexes stay correct.

struct DynastyTable {
    std::vector<core::StringId> name;
    std::vector<CharacterId> head;
    std::vector<std::int32_t> honor;

    [[nodiscard]] std::size_t size() const noexcept { return name.size(); }
    [[nodiscard]] bool operator==(const DynastyTable&) const = default;
};

struct ProvinceTable {
    std::vector<core::StringId> name;
    std::vector<DynastyId> owner;
    std::vector<core::Fixed32> population;  // thousands of people
    std::vector<std::uint8_t> development;

    [[nodiscard]] std::size_t size() const noexcept { return name.size(); }
    [[nodiscard]] bool operator==(const ProvinceTable&) const = default;
};

struct CharacterTable {
    EntityTable<CharacterId> ids;
    std::vector<DynastyId> dynasty;
    std::vector<Date> birth_date;
    std::vector<Sex> sex;
    std::vector<core::Fixed32> health;  // 0..100
    std::vector<CharacterId> father;
    std::vector<CharacterId> mother;

    [[nodiscard]] bool operator==(const CharacterTable&) const = default;
};

struct CharacterInit {
    DynastyId dynasty;
    Date birth_date;
    Sex sex = Sex::Male;
    core::Fixed32 health = core::Fixed32::from_int(100);
    CharacterId father;
    CharacterId mother;
};

/// Hash of the whole state plus the parts it is built from (helps to locate a divergence).
struct WorldHash {
    std::uint64_t total = 0;
    std::uint64_t calendar = 0;
    std::uint64_t rng = 0;
    std::uint64_t dynasties = 0;
    std::uint64_t provinces = 0;
    std::uint64_t characters = 0;

    [[nodiscard]] bool operator==(const WorldHash&) const = default;
};

/// The complete simulation state. Plain data, copyable: a copy IS a snapshot.
class World {
public:
    World(Date start, std::uint64_t seed);

    // ---- time and randomness ----
    [[nodiscard]] Date today() const noexcept { return calendar_.today(); }
    [[nodiscard]] const TickCalendar& calendar() const noexcept { return calendar_; }
    /// Moves the clock one day forward (called by the simulation loop only).
    TickEvents advance_day() noexcept { return calendar_.advance(); }
    [[nodiscard]] core::RngStreams& rng() noexcept { return rng_; }
    [[nodiscard]] const core::RngStreams& rng() const noexcept { return rng_; }

    // ---- dynasties (never destroyed: an extinct dynasty keeps its record) ----
    DynastyId create_dynasty(core::StringId name);
    [[nodiscard]] bool exists(DynastyId id) const noexcept {
        return id.valid() && id.index() < dynasties_.size();
    }
    [[nodiscard]] const DynastyTable& dynasties() const noexcept { return dynasties_; }
    /// Head must be alive and a member of the dynasty (or invalid). Returns false otherwise.
    bool set_dynasty_head(DynastyId dynasty, CharacterId head);
    /// Adds `delta` to honor, clamped to [kMinHonor, kMaxHonor].
    void add_honor(DynastyId dynasty, std::int32_t delta);

    // ---- provinces (never destroyed) ----
    ProvinceId create_province(core::StringId name, DynastyId owner, core::Fixed32 population);
    [[nodiscard]] bool exists(ProvinceId id) const noexcept {
        return id.valid() && id.index() < provinces_.size();
    }
    [[nodiscard]] const ProvinceTable& provinces() const noexcept { return provinces_; }
    /// `owner` may be invalid (unowned).
    void set_province_owner(ProvinceId province, DynastyId owner);
    [[nodiscard]] std::span<core::Fixed32> province_population_mut() noexcept {
        return provinces_.population;
    }

    // ---- characters ----
    CharacterId create_character(const CharacterInit& init);
    /// Removes the character from all indexes (and from the head slot of its dynasty).
    bool destroy_character(CharacterId id);
    [[nodiscard]] bool is_alive(CharacterId id) const noexcept { return characters_.ids.is_alive(id); }
    [[nodiscard]] const CharacterTable& characters() const noexcept { return characters_; }
    void set_character_dynasty(CharacterId id, DynastyId dynasty);
    [[nodiscard]] std::span<core::Fixed32> character_health_mut() noexcept { return characters_.health; }

    // ---- indexes (maintained incrementally, deterministic order) ----
    [[nodiscard]] std::span<const CharacterId> members_of(DynastyId dynasty) const noexcept;
    [[nodiscard]] std::span<const ProvinceId> provinces_of(DynastyId dynasty) const noexcept;

    // ---- verification ----
    [[nodiscard]] WorldHash compute_hash() const;
    /// Empty when all invariants hold; otherwise human-readable descriptions of the violations.
    [[nodiscard]] std::vector<std::string> check_invariants() const;

    /// Immutable copy of the current state, safe to hand to other threads.
    [[nodiscard]] std::shared_ptr<const World> snapshot() const {
        return std::make_shared<const World>(*this);
    }

private:
    TickCalendar calendar_;
    core::RngStreams rng_;
    DynastyTable dynasties_;
    ProvinceTable provinces_;
    CharacterTable characters_;
    std::vector<core::SparseSet<CharacterId>> members_;         // per dynasty
    std::vector<core::SparseSet<ProvinceId>> owned_provinces_;  // per dynasty
};

}  // namespace pat::sim
