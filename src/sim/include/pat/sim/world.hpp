#pragma once

#include "pat/core/bytes.hpp"
#include "pat/core/fixed.hpp"
#include "pat/core/ids.hpp"
#include "pat/core/result.hpp"
#include "pat/core/rng.hpp"
#include "pat/core/sparse_set.hpp"
#include "pat/core/string_interner.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/entity_ids.hpp"
#include "pat/sim/entity_table.hpp"
#include "pat/sim/scheduled_queue.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pat::sim {

enum class Sex : std::uint8_t { Male, Female };

/// How a person came into the world (GDD 01 section 2.1). Stored as one byte.
enum class Legitimacy : std::uint8_t { Legitimate, Illegitimate, Legitimized };
inline constexpr std::uint8_t kLegitimacyCount = 3;

/// Inheritance law of a dynasty (GDD 01 section 6). Majorat is the default.
enum class SuccessionLaw : std::uint8_t { Majorat, Minorat, Partition, FamilyCouncil, Appointment };
inline constexpr std::uint8_t kSuccessionLawCount = 5;

/// The five skills of GDD 01 section 2.1, in column order.
enum class Skill : std::uint8_t { Stewardship, Diplomacy, Martial, Intrigue, Learning };
inline constexpr std::size_t kSkillCount = 5;
inline constexpr std::size_t kTraitSlots = 8;

// ---- kinship limits (GDD 01 section 7.3, ADR-0006 D2) ---------------------------------------------
/// A person may enter at most three marriages (the fourth is forbidden); the same number bounds the
/// list of former spouses, so no marriage is ever forgotten.
inline constexpr std::uint8_t kMaxMarriages = 3;
inline constexpr std::size_t kMaxExSpouses = kMaxMarriages;
/// Reserved for godparents (open question 3, issue 135: M2). Stored and saved, written by nobody yet.
inline constexpr std::size_t kGodparentSlots = 2;

inline constexpr std::int32_t kMinHonor = -1000;
inline constexpr std::int32_t kMaxHonor = 10000;

// ---- bounds of the character and dynasty columns (GDD 01 section 2.1) -----------------------------
inline constexpr core::Fixed32 kSkillMax = core::Fixed32::from_int(20);  // skills: 0..20
inline constexpr core::Fixed32 kScaleMax =
    core::Fixed32::from_int(100);  // fertility, stress, piety, unity: 0..100
inline constexpr core::Fixed32 kSignedScaleMax =
    core::Fixed32::from_int(100);  // ambition, loyalties: -100..100
inline constexpr core::Fixed32 kDefaultUnity =
    core::Fixed32::from_int(50);                         // [predl.] a freshly created dynasty
inline constexpr std::uint8_t kEducationFocusCount = 5;  // focus 0 = none, 1..5 = skill index + 1
inline constexpr std::uint8_t kMaxEducationLevel = 100;  // [predl.]

/// Hard limit of character ids ever allocated (ADR-0006 D1): ids are never reused in dynasty mode.
inline constexpr std::uint32_t kMaxCharacterIds = 1'048'576;
/// A planned death lies within this many days after the birth (200 years).
inline constexpr std::int32_t kMaxLifeSpanDays = 200 * 366;
/// Dates read from a file or set through the API stay within +-1,000,000 days (about 2,700 years)
/// of the epoch; keeps all calendar arithmetic far from integer overflow.
inline constexpr std::int32_t kSaneDateLimit = 1'000'000;

[[nodiscard]] constexpr bool is_sane_date(Date d) noexcept {
    return d.days() >= -kSaneDateLimit && d.days() <= kSaneDateLimit;
}

/// A planned death is either absent or lies between the birth and kMaxLifeSpanDays after it.
[[nodiscard]] constexpr bool planned_death_is_valid(Date birth, Date planned) noexcept {
    if (planned == kNoDate) {
        return true;
    }
    return is_sane_date(birth) && is_sane_date(planned) && planned >= birth &&
           planned - birth <= kMaxLifeSpanDays;
}

/// Traits in canonical form: non-zero entries ascending without repeats, zeros at the end. Extra
/// entries beyond kTraitSlots are dropped.
[[nodiscard]] inline std::array<std::uint16_t, kTraitSlots>
sanitize_traits(const std::array<std::uint16_t, kTraitSlots>& traits) noexcept {
    std::array<std::uint16_t, kTraitSlots> sorted = traits;
    std::sort(sorted.begin(), sorted.end());
    std::array<std::uint16_t, kTraitSlots> out{};
    std::size_t n = 0;
    for (const std::uint16_t t : sorted) {
        if (t != 0 && (n == 0 || out[n - 1] != t)) {
            out[n++] = t;
        }
    }
    return out;
}

[[nodiscard]] inline bool
traits_are_canonical(const std::array<std::uint16_t, kTraitSlots>& traits) noexcept {
    return sanitize_traits(traits) == traits;
}

/// Why a character died (GDD 01 section 3, ADR-0006 D3). Stored as one byte in `cause_of_death`;
/// 0 means "alive". The numbers are part of the save format: only append.
enum class CauseOfDeath : std::uint8_t {
    None = 0,
    OldAge,
    Illness,
    Childbirth,
    War,
    Execution,
    Murder,
    Accident,
    Wounds,
    Grief,
};
inline constexpr std::uint8_t kCauseOfDeathCount = 10;  // values 0..9; Die accepts 1..9

/// Bits of `World::world_flags()`.
inline constexpr std::uint32_t kWorldFlagDynastyMode = 1U << 0;

// ---- tables (structure of arrays) -------------------------------------------------------------
// Columns are public for fast read access in systems; anything that touches an indexed column
// (character dynasty, planned death, province owner, dynasty head) must go through World so indexes
// stay correct. World hands out const references only; the setters below clamp to the bounds.

struct DynastyTable {
    std::vector<core::StringId> name;
    std::vector<CharacterId> head;
    std::vector<std::int32_t> honor;
    std::vector<core::Fixed32> unity;  // 0..100, default kDefaultUnity
    std::vector<SuccessionLaw> succession_law;

    [[nodiscard]] std::size_t size() const noexcept { return name.size(); }
    [[nodiscard]] bool operator==(const DynastyTable&) const = default;
};

/// Branches (sub-houses) of dynasties. Like dynasties they are never destroyed.
struct BranchTable {
    std::vector<core::StringId> name;
    std::vector<DynastyId> dynasty;

    [[nodiscard]] std::size_t size() const noexcept { return name.size(); }
    [[nodiscard]] bool operator==(const BranchTable&) const = default;
};

struct ProvinceTable {
    std::vector<core::StringId> name;
    std::vector<DynastyId> owner;
    std::vector<core::Fixed32> population;  // thousands of people
    std::vector<std::uint8_t> development;

    [[nodiscard]] std::size_t size() const noexcept { return name.size(); }
    [[nodiscard]] bool operator==(const ProvinceTable&) const = default;
};

/// One row per character id. Group K columns (identity and facts) survive the character's death;
/// group S columns (state of the living) are reset to their defaults when a character dies.
struct CharacterTable {
    EntityTable<CharacterId> ids;
    // E1 columns
    std::vector<DynastyId> dynasty;
    std::vector<Date> birth_date;
    std::vector<Sex> sex;
    std::vector<core::Fixed32> health;  // 0..100 (S)
    std::vector<CharacterId> father;
    std::vector<CharacterId> mother;
    // identity and facts (K)
    std::vector<core::StringId> name;     // invalid = unnamed
    std::vector<BranchId> branch;         // invalid = none; must belong to the character's dynasty
    std::vector<std::uint16_t> portrait;  // portrait pool id
    std::vector<Legitimacy> legitimacy;
    std::vector<std::uint8_t> cause_of_death;  // CauseOfDeath, 0 while alive
    std::vector<Date> death_date;              // kNoDate while alive
    // kinship (K, E7-T03). Father and mother are the E1 columns above. A marriage always sits on
    // both partners; ending it (for any reason, death included) clears `spouse` on both sides and
    // appends the partner to `ex_spouses` of the other. Mutate through World::marry/end_marriage.
    std::vector<CharacterId> spouse;      // current spouse, invalid = unmarried
    std::vector<std::uint8_t> marriages;  // marriages entered so far, the current one included
    std::vector<std::array<CharacterId, kMaxExSpouses>>
        ex_spouses;  // in order of ending, packed at the front, invalid = empty slot
    std::vector<std::array<CharacterId, kGodparentSlots>> godparent;  // reserved, see kGodparentSlots
    // Derived child lists (not saved, not hashed; dynasty mode only, empty in E1 mode). A person
    // heads one list: the children whose father is it (male) or whose mother is it (female), eldest
    // first by (birth_date, id). A child sits in the list of each of its parents and has one link per
    // parent. Read through the queries in pat/sim/kinship.hpp.
    std::vector<CharacterId> first_child;
    std::vector<CharacterId> next_by_father;  // next in the list of this child's father
    std::vector<CharacterId> next_by_mother;  // next in the list of this child's mother
    // state of the living (S)
    std::vector<std::array<core::Fixed32, kSkillCount>> skills;  // 0..20 each
    std::vector<core::Fixed32> fertility;                        // 0..100
    std::vector<core::Fixed32> stress;                           // 0..100
    std::vector<core::Fixed32> piety;                            // 0..100
    std::vector<core::Fixed32> ambition;                         // -100..100
    std::vector<core::Fixed32> loyalty_head;                     // -100..100
    std::vector<core::Fixed32> loyalty_emperor;                  // -100..100
    std::vector<std::array<std::uint16_t, kTraitSlots>>
        traits;                                 // sorted ascending, 0 = empty slot (at the end)
    std::vector<std::uint8_t> education_focus;  // 0 = none, 1..5 = skill + 1
    std::vector<CharacterId> education_tutor;   // invalid = none
    std::vector<std::uint8_t> education_level;  // 0..kMaxEducationLevel
    std::vector<Date> planned_death;            // kNoDate = none (indexed: use World)

    [[nodiscard]] bool operator==(const CharacterTable&) const = default;
};

struct Education {
    std::uint8_t focus = 0;  // 0 = none, 1..5 = skill + 1
    CharacterId tutor;
    std::uint8_t level = 0;

    [[nodiscard]] bool operator==(const Education&) const = default;
};

/// Everything needed to create a character. All new fields default to "nothing special"; the
/// defaults are exactly what a schema-1 save is migrated to.
struct CharacterInit {
    DynastyId dynasty;
    Date birth_date;
    Sex sex = Sex::Male;
    core::Fixed32 health = core::Fixed32::from_int(100);
    CharacterId father;
    CharacterId mother;
    core::StringId name;
    BranchId branch;
    std::uint16_t portrait = 0;
    Legitimacy legitimacy = Legitimacy::Legitimate;
    std::array<core::Fixed32, kSkillCount> skills{};
    core::Fixed32 fertility;
    core::Fixed32 stress;
    core::Fixed32 piety;
    core::Fixed32 ambition;
    core::Fixed32 loyalty_head;
    core::Fixed32 loyalty_emperor;
    std::array<std::uint16_t, kTraitSlots> traits{};
    Education education;
    Date planned_death = kNoDate;

    [[nodiscard]] bool operator==(const CharacterInit&) const = default;
};

/// A dead person as the archive hands it out: a copy of the group K fields (ADR-0006 D3). Nothing
/// here refers into the world's storage, so the archive can later be packed without changing callers.
struct ArchivedPerson {
    CharacterId id;
    DynastyId dynasty;  // the house the person belonged to (the dead are not in its member list)
    BranchId branch;
    core::StringId name;
    std::uint16_t portrait = 0;
    Sex sex = Sex::Male;
    Legitimacy legitimacy = Legitimacy::Legitimate;
    Date birth_date;
    Date death_date = kNoDate;
    CauseOfDeath cause = CauseOfDeath::None;
    CharacterId father;
    CharacterId mother;
    std::uint8_t marriages = 0;
    /// Everybody the person was married to, in order of ending; the widow(er) is the last one. There
    /// is no current spouse: death ended the marriage.
    std::array<CharacterId, kMaxExSpouses> ex_spouses{};
    std::array<CharacterId, kGodparentSlots> godparent{};
    std::vector<CharacterId> children;  // eldest first, the dead among them

    [[nodiscard]] bool operator==(const ArchivedPerson&) const = default;
};

/// Reasons for entries of the life-event queue (ADR-0006 D4); the numeric value is also the
/// priority inside a day. Labour (3) is added by E7-T12.
enum class LifeEventKind : std::uint8_t { Birthday = 1, PlannedDeath = 2 };

struct LifeEvent {
    LifeEventKind kind = LifeEventKind::Birthday;
    CharacterId character;

    [[nodiscard]] bool operator==(const LifeEvent&) const = default;
};

struct DueLifeEvent {
    Date due;
    LifeEvent item;
    [[nodiscard]] bool operator==(const DueLifeEvent&) const = default;
};

/// Stable numbers of the extended hash parts (ADR-0006 D7). Part `n` lives in `WorldHash::ext[n]`.
/// A part that has no content is 0 and is not mixed into `total`, so a world that does not use a
/// feature has the same `total` as before the feature existed.
enum class ExtPart : std::uint8_t {
    Mode = 0,          // world flags
    CharacterExt = 1,  // new character columns (living rows that differ from the defaults)
    Archive = 2,       // E7-T04: the dead (sum of the hashes of their rows, and their number)
    DynastyExt = 3,    // unity, succession law, branches
    Pregnancy = 4,     // E7-T12
    Chronicle = 5,     // E7-T16
    Opinion = 6,       // E7-T17
    Kinship = 7,       // E7-T03: spouses, marriage counters, former spouses, godparents
    Economy = 16,      // E9-T03 (numbers 16..23 are E9's): hash of the nine economy sub-hashes
};
inline constexpr std::size_t kExtPartCount = 64;

/// Name of an extended part for diagnostics ("ext[7]" for numbers without a name).
[[nodiscard]] std::string ext_part_name(std::size_t number);

/// Hash of the whole state plus the parts it is built from (helps to locate a divergence).
struct WorldHash {
    std::uint64_t total = 0;
    std::uint64_t calendar = 0;
    std::uint64_t rng = 0;
    std::uint64_t dynasties = 0;
    std::uint64_t provinces = 0;
    std::uint64_t characters = 0;
    std::array<std::uint64_t, kExtPartCount> ext{};

    [[nodiscard]] bool operator==(const WorldHash&) const = default;
    /// Compares only `total` and the five fixed parts, i.e. the fields a replay checkpoint records.
    [[nodiscard]] bool same_recorded_parts(const WorldHash& other) const noexcept {
        return total == other.total && calendar == other.calendar && rng == other.rng &&
               dynasties == other.dynasties && provinces == other.provinces && characters == other.characters;
    }
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

    // ---- mode ----
    /// Switches to "dynasty mode" (ADR-0006 D1): character ids are never reused, the old destroy
    /// operation is forbidden and every living character has an entry in the life-event queue.
    /// Fails (false) only when the free list of character ids is not empty. Idempotent.
    bool enable_dynasty_mode();
    [[nodiscard]] bool dynasty_mode() const noexcept { return (flags_ & kWorldFlagDynastyMode) != 0; }
    [[nodiscard]] std::uint32_t world_flags() const noexcept { return flags_; }

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
    /// Sets the unity, clamped to 0..100.
    void set_dynasty_unity(DynastyId dynasty, core::Fixed32 unity);
    void set_dynasty_succession_law(DynastyId dynasty, SuccessionLaw law);

    // ---- branches (never destroyed) ----
    /// Creates a branch of an existing dynasty.
    BranchId create_branch(DynastyId dynasty, core::StringId name);
    [[nodiscard]] bool exists(BranchId id) const noexcept {
        return id.valid() && id.index() < branches_.size();
    }
    [[nodiscard]] const BranchTable& branches() const noexcept { return branches_; }

    // ---- provinces (never destroyed) ----
    /// With the economy on, `population` (thousands) is converted to persons (truncated) and put into
    /// the group of peasants; the cache is then refreshed from the groups (see enable_economy).
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
    /// Out-of-range values in `init` are clamped (skills, scales), unknown references are cleared
    /// (dynasty, branch, tutor) and traits are sorted; use cmd::CreatePerson to have them rejected
    /// instead. In dynasty mode a parent is kept only when it exists, has the matching sex and was
    /// born before the child; otherwise the reference is cleared. Returns an invalid id when the table
    /// is full (kMaxCharacterIds).
    CharacterId create_character(const CharacterInit& init);
    /// Removes the character from all indexes (and from the head slot of its dynasty). Only
    /// available outside dynasty mode: there the dead stay in the table (see die()).
    bool destroy_character(CharacterId id);
    /// Kills a living character and moves its row to the archive (E7-T04, ADR-0006 D3), in one step:
    ///   * the death date (today) and the cause are recorded;
    ///   * a marriage ends (end_marriage): the spouse is a widow(er), the dead one keeps the partner
    ///     in `ex_spouses`;
    ///   * the person leaves the member list of the dynasty and the list of the living and stops
    ///     being head of the dynasty (the successor is chosen by E7-T15, not here);
    ///   * the group S columns (health, skills, scales, traits, education, planned death) are reset
    ///     and the entries of the life-event queue are dropped;
    ///   * the slot becomes archived; the id is never handed out again.
    /// Identity, parents, children and marriages stay: they are facts of the family tree. Dynasty
    /// mode only. False (nothing changed) when the character is not alive, the cause is None or
    /// unknown, or the person is not born yet.
    bool die(CharacterId id, CauseOfDeath cause);
    [[nodiscard]] bool is_alive(CharacterId id) const noexcept { return characters_.ids.is_alive(id); }
    /// True when the character is dead and its row sits in the archive (dynasty mode).
    [[nodiscard]] bool is_archived(CharacterId id) const noexcept {
        return id.valid() && id.index() < characters_.ids.capacity() &&
               characters_.ids.alive_flags()[id.index()] == 2;
    }
    /// True when the row of the character exists: it is alive or archived. In dynasty mode an id
    /// that exists never refers to anybody else later.
    [[nodiscard]] bool exists(CharacterId id) const noexcept {
        return id.valid() && id.index() < characters_.ids.capacity() &&
               characters_.ids.alive_flags()[id.index()] != 0;
    }
    /// The archived record of a dead character, copied by value; nullopt for anybody who is not in
    /// the archive (alive, unknown, or E1 mode).
    [[nodiscard]] std::optional<ArchivedPerson> archived_person(CharacterId id) const;
    /// Number of rows in the archive.
    [[nodiscard]] std::size_t archived_count() const noexcept { return characters_.ids.retired_count(); }
    /// True when another character can be created now.
    [[nodiscard]] bool can_create_character() const noexcept {
        return characters_.ids.capacity() < kMaxCharacterIds || !characters_.ids.free_list().empty();
    }
    [[nodiscard]] const CharacterTable& characters() const noexcept { return characters_; }
    void set_character_dynasty(CharacterId id, DynastyId dynasty);
    [[nodiscard]] std::span<core::Fixed32> character_health_mut() noexcept { return characters_.health; }

    // Setters of the columns of the living (E7-T02). Values are clamped to their bounds; a
    // character that is not alive is ignored.
    void set_character_name(CharacterId id, core::StringId name);
    void set_character_portrait(CharacterId id, std::uint16_t portrait);
    void set_character_legitimacy(CharacterId id, Legitimacy legitimacy);
    /// False when the branch does not exist or belongs to another dynasty than the character.
    bool set_character_branch(CharacterId id, BranchId branch);
    void set_character_skill(CharacterId id, Skill skill, core::Fixed32 value);
    void set_character_fertility(CharacterId id, core::Fixed32 value);
    void set_character_stress(CharacterId id, core::Fixed32 value);
    void set_character_piety(CharacterId id, core::Fixed32 value);
    void set_character_ambition(CharacterId id, core::Fixed32 value);
    void set_character_loyalty_head(CharacterId id, core::Fixed32 value);
    void set_character_loyalty_emperor(CharacterId id, core::Fixed32 value);
    /// Stores the traits sorted without gaps. False (nothing changed) when there are more than
    /// kTraitSlots non-zero entries or a trait appears twice.
    bool set_character_traits(CharacterId id, std::span<const std::uint16_t> traits);
    /// False (nothing changed) when the focus or level is out of range or the tutor is unknown.
    bool set_character_education(CharacterId id, const Education& education);
    /// `kNoDate` clears it. False (nothing changed) when the date lies outside
    /// [birth_date, birth_date + kMaxLifeSpanDays]. In dynasty mode this also schedules the event.
    bool set_character_planned_death(CharacterId id, Date date);

    // ---- kinship primitives (E7-T03; the rules of marriage belong to E8) ----
    // Dynasty mode only: outside it the ids of the dead are reused and a stored spouse would
    // silently point to a stranger. Both return false and change nothing when a precondition fails.
    /// Marries two living, unmarried characters. Fails when `a == b`, one of them is dead, already
    /// married or has used kMaxMarriages marriages, or they have been married to each other before.
    /// Sex, age, blood relationship and the Church's consent are rules for E8, not checked here.
    bool marry(CharacterId a, CharacterId b);
    /// Ends the marriage of a living, married character with a living spouse, for any reason
    /// (annulment, tonsure, widowhood: Die calls this for the deceased before it is archived). Both
    /// partners lose `spouse` and gain each other in `ex_spouses`.
    bool end_marriage(CharacterId a);
    /// Stores godparents (reserved for M2). Both must exist, differ from the character and from each
    /// other, or be invalid. False (nothing changed) otherwise.
    bool set_character_godparents(CharacterId id, const std::array<CharacterId, kGodparentSlots>& godparents);

    // ---- economy (E9-T02, docs/dev/economy-model.md) ----
    // The economy tables are absent by default: a world that never calls enable_economy() behaves
    // exactly as before and check_invariants() requires every economy table to be empty. Once on,
    // they are part of the hash (ExtPart::Economy, 0 and outside `total` while the economy is off),
    // of the save file (sections 32..39, written only when the economy is on) and of a copy of the
    // World (snapshot).

    /// Switches the economy on: builds a default row for every dynasty and province that exists and
    /// from now on for every new one. Each province keeps its population: the persons go into the
    /// group of peasants (see create_province). With a valid `crown_dynasty` the crown's money is held
    /// on the account of that dynasty ("one treasury" mode of open question Q7); without it the crown
    /// has its own account (row 0). Idempotent for the same argument; false (nothing changed) when
    /// `crown_dynasty` does not exist, or when the economy is on already with another one.
    bool enable_economy(DynastyId crown_dynasty = DynastyId());
    [[nodiscard]] bool economy_present() const noexcept { return economy_.meta.present != 0; }
    [[nodiscard]] const economy::EconomyState& economy() const noexcept { return economy_; }
    /// Writable views of the plain numeric columns for systems; empty spans without economy. Values
    /// are not clamped here, see economy::EconomyColumnsMut.
    [[nodiscard]] economy::EconomyColumnsMut economy_columns_mut() noexcept;
    /// Hash of the rules the world was built with. Saved with the economy, but not hashed.
    void set_economy_rules_hash(std::uint64_t rules_hash) noexcept;
    /// The sub-hashes of the economy (economy::EconomyHashPart order); all 0 without economy. A
    /// diagnostic: `compute_hash().ext[ExtPart::Economy]` is the hash of these nine numbers.
    [[nodiscard]] std::array<std::uint64_t, economy::kEconomyHashPartCount> economy_hash_parts() const;

    /// Development of a province, clamped to 1..economy::kMaxDevelopment (GDD 03 section 8.3).
    void set_province_development(ProvinceId province, std::uint8_t development);
    /// Gives a province to the crown, the Church or nobody (any kind except Dynasty, which is set by
    /// set_province_owner together with the owner). The dynasty owner, if any, is dropped. Needs the
    /// economy; false (nothing changed) otherwise or for an unknown province or the kind Dynasty.
    bool set_province_owner_kind(ProvinceId province, economy::OwnerKind kind);
    /// Sets the number of people of a group and refreshes the population cache of the province. The
    /// total of the six groups is capped at economy::kMaxProvincePersons; returns the stored value
    /// (0 for an unknown province or group or without economy).
    std::uint32_t set_group_size(ProvinceId province, economy::Group group, std::uint32_t persons);
    /// People in the six groups of a province (0 without economy).
    [[nodiscard]] std::uint64_t population_persons(ProvinceId province) const noexcept;
    /// `ProvinceTable::population` := total persons / 1000 (the cache of section 3.1 of the spec).
    void refresh_population_cache(ProvinceId province) noexcept;
    /// The administrator must be alive or invalid. False (nothing changed) otherwise, for an
    /// unknown province or without economy. A character who dies or is destroyed is removed from
    /// every province (invariant E-P4).
    bool set_province_administrator(ProvinceId province, CharacterId administrator);
    /// Puts a building (0 = empty) into a slot of a province; false when the slot is not below
    /// `slots_total` or the province is unknown.
    bool set_slot_building(ProvinceId province, std::size_t slot, std::uint8_t building);

    /// Row of an account in the account table: the crown is row 0 (or the row of the crown dynasty
    /// in the "one treasury" mode), dynasty `d` is row `1 + d`. nullopt without economy or for an
    /// unknown dynasty.
    [[nodiscard]] std::optional<std::size_t> resolve_account(economy::AccountRef account) const noexcept;
    /// Balance of an account in kopecks (0 for an account that does not exist).
    [[nodiscard]] economy::Kopecks treasury(economy::AccountRef account) const noexcept;
    /// THE way to change money (economy-model section 5): adds `amount_kop` to the balance of the
    /// account and to its line `code` of the open month, both by the same applied amount, so that
    /// `treasury == opening + sum(lines)` holds (E-M1). Income is positive. A posting beyond
    /// +-kMaxPostingKop, or one that would take the balance beyond +-kMaxTreasuryKop or a line
    /// beyond +-kMaxLedgerLineKop, saturates; returns the amount really applied. Zero (nothing
    /// changes) for an unissued code, a code of a transfer pair (those go through transfer()), a
    /// missing account or without economy.
    economy::Kopecks
    post_money(economy::AccountRef account, economy::LedgerCode code, economy::Kopecks amount_kop);
    /// THE way to move money between two accounts: `amount_kop` (> 0) leaves `from` under `out_code`
    /// and enters `to` under `in_code`; the pair must be one of economy::kLedgerPairs. If either side
    /// would saturate, the smaller amount moves on both sides, so the pair sums to zero (E-M3).
    /// Returns the amount moved; 0 for a non-positive amount, an unknown pair or account.
    economy::Kopecks transfer(economy::AccountRef from,
                              economy::AccountRef to,
                              economy::LedgerCode out_code,
                              economy::LedgerCode in_code,
                              economy::Kopecks amount_kop);
    /// Closes the open month of every account (economy-model section 6.3, step 3): the open lines
    /// move to the front of the 12-month ring (the oldest month drops out), `opening := treasury`,
    /// the open lines restart at zero and `open_month_start := new_month_start`. The new start must
    /// be the first day of a month after the current one. False (nothing changed) otherwise.
    bool close_ledger_month(Date new_month_start);
    /// Appends to the incident log (a ring of economy::kIncidentLogSize entries) and returns the
    /// sequence number. `province` and `dynasty` are cleared when they do not exist. nullopt without
    /// economy or for a kind outside 1..economy::kMaxIncidentKind (nothing is logged then).
    std::optional<std::uint32_t>
    log_incident(economy::IncidentKind kind, ProvinceId province, DynastyId dynasty, std::int32_t param);

    // ---- indexes (maintained incrementally, deterministic order) ----
    [[nodiscard]] std::span<const CharacterId> members_of(DynastyId dynasty) const noexcept;
    [[nodiscard]] std::span<const ProvinceId> provinces_of(DynastyId dynasty) const noexcept;
    /// Ids of all living characters in strictly ascending order.
    [[nodiscard]] std::span<const CharacterId> living() const noexcept { return living_; }
    /// The life-event queue (ADR-0006 D4): a derived index over birth and planned-death dates,
    /// empty outside dynasty mode. Not saved and not hashed; rebuilt on load.
    [[nodiscard]] const ScheduledQueue<LifeEvent>& life_queue() const noexcept { return life_queue_; }

    /// Drain due events in (date, kind, character) order, discarding stale entries. Reschedule
    /// birthdays and keep active death plans indexed until the next boundary applies Die.
    [[nodiscard]] std::vector<DueLifeEvent> take_due_life_events(Date today);
    /// Rebuilds every derived index (living list, child lists, life-event queue) from the columns. The queue
    /// is rebuilt for a state at a tick boundary: all events up to today are considered processed.
    void rebuild_derived_indexes();

    // ---- verification ----
    [[nodiscard]] WorldHash compute_hash() const;
    /// Empty when all invariants hold; otherwise human-readable descriptions of the violations.
    [[nodiscard]] std::vector<std::string> check_invariants() const;

    /// Immutable copy of the current state, safe to hand to other threads.
    [[nodiscard]] std::shared_ptr<const World> snapshot() const {
        return std::make_shared<const World>(*this);
    }

    // ---- persistence (world_io.cpp) ----
    /// Version of the byte layout written by save_state(); load_state() reads every schema from 1 to
    /// this one. Bump when the layout changes and add the migration step to load_state (the table
    /// of steps is in ADR-0006 D6).
    ///   1  the E1 layout (clock, random streams, dynasties, provinces, characters)
    ///   2  + extension block: world flags and id-versioned sections (E7-T02: sections 1 and 2)
    ///   3  + section 3 "kin": spouses, marriage counters, former spouses, godparents (E7-T03)
    ///   4  the slot table may hold archived rows (slot state 2), dynasty mode only (E7-T04)
    ///   5  + the economy: sections 32..39 of the extension block (E9-T03); older files have none
    static constexpr std::uint32_t kStateSchema = 5;
    /// Writes the complete state, including the order of the index lists, so that a loaded world
    /// has the same hash and behaves identically in the future.
    void save_state(core::ByteWriter& out) const;
    /// Reads a state written by save_state() of any supported schema and migrates it to the current
    /// one. Never throws and never trusts counts or ids: bad input produces an error, not a crash.
    /// The result satisfies check_invariants().
    [[nodiscard]] static core::Result<World, core::Error> load_state(core::ByteReader& in);

private:
    // Defaults of the columns added in E7-T02 and helpers that keep them in sync.
    void grow_character_columns();
    void reset_character_extension(std::size_t index);
    [[nodiscard]] bool character_extension_is_default(std::size_t index) const;
    /// Queues the birthday (due `birthday_due`) and, when set, the planned death of a character.
    void push_life_events(CharacterId id, Date birthday_due);
    void insert_living(CharacterId id);
    // Member lists (one vector per dynasty, swap-remove) and the position of every member in its list.
    static constexpr std::uint32_t kNoMember = 0xFFFFFFFFU;
    void member_add(DynastyId dynasty, CharacterId id);
    bool member_remove(DynastyId dynasty, CharacterId id);
    [[nodiscard]] bool member_contains(DynastyId dynasty, CharacterId id) const noexcept;
    // Archive (E7-T04): the group S columns of a dead row are at their defaults; the hash of the
    // archive is the sum of the hashes of its rows, kept in `archive_sum_` (derived, rebuilt on load).
    void reset_living_state(std::size_t index);
    [[nodiscard]] bool living_state_is_default(std::size_t index) const;
    [[nodiscard]] std::uint64_t archive_row_hash(std::size_t index) const;
    [[nodiscard]] std::uint64_t compute_archive_sum() const;
    void check_archive_invariants(std::vector<std::string>& bad) const;
    // Kinship helpers (world.cpp). The child lists are derived from the father/mother columns.
    [[nodiscard]] bool kin_is_default(std::size_t index) const;
    void reset_kinship(std::size_t index);
    [[nodiscard]] bool parent_link_is_valid(CharacterId child, CharacterId parent, Sex role) const noexcept;
    void link_child(CharacterId child);
    void rebuild_child_lists();
    void check_kinship_invariants(std::vector<std::string>& bad) const;
    // One section of the extension block of the save file (world_io.cpp, world_economy_io.cpp).
    struct StateSection {
        std::uint16_t id = 0;
        std::uint16_t version = 1;
        core::ByteWriter body;
    };
    /// Appends the sections of the economy (nothing when it is off), ascending by id.
    void economy_write_sections(std::vector<StateSection>& sections) const;
    /// Reads one economy section into the tables. `closed_months` etc. come from the section 32 read
    /// before (ids ascend). Returns nullptr on success, otherwise a description of the damage; the
    /// invariants are checked by the caller once everything has been read.
    [[nodiscard]] const char* economy_read_section(std::uint16_t id, core::ByteReader& in);
    // Economy helpers (world_economy.cpp).
    void economy_add_dynasty_rows();
    /// Appends the economy rows of province `index`, which exists in ProvinceTable already: the
    /// people come from its population cache, the owner kind from its owner.
    void economy_add_province_rows(std::size_t index);
    void economy_remove_administrator(CharacterId id) noexcept;
    /// What a posting of `amount` would really change on line `line` of `account` (see post_money).
    [[nodiscard]] economy::Kopecks
    preview_posting(std::size_t account, std::size_t line, economy::Kopecks amount) const noexcept;
    /// Applies a posting to line `line` of account row `row` (what post_money and transfer end in).
    economy::Kopecks post_line(std::size_t row, std::size_t line, economy::Kopecks amount_kop);
    void check_economy_invariants(std::vector<std::string>& bad) const;

    TickCalendar calendar_;
    core::RngStreams rng_;
    std::uint32_t flags_ = 0;
    DynastyTable dynasties_;
    BranchTable branches_;
    ProvinceTable provinces_;
    CharacterTable characters_;
    std::vector<std::vector<CharacterId>> members_;             // per dynasty
    std::vector<std::uint32_t> member_pos_;                     // per character id, derived from members_
    std::vector<core::SparseSet<ProvinceId>> owned_provinces_;  // per dynasty
    std::vector<CharacterId> living_;                           // derived, ascending
    ScheduledQueue<LifeEvent> life_queue_;                      // derived
    std::uint64_t archive_sum_ = 0;                             // derived: sum of the hashes of archived rows
    economy::EconomyState economy_;  // E9-T02: empty unless enable_economy() was called
};

}  // namespace pat::sim
