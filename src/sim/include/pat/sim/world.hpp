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
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/entity_ids.hpp"
#include "pat/sim/entity_table.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/scheduled_queue.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pat::sim {

namespace economy {
struct LoanRules;
struct LoanSettlementReport;
}  // namespace economy

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
    core::Fixed32::from_int(50);                                       // [predl.] a freshly created dynasty
inline constexpr auto kDefaultPrestige = core::Fixed32::from_int(50);  // [predl.] neutral prestige
inline constexpr std::uint8_t kEducationFocusCount = 5;  // focus 0 = none, 1..5 = skill index + 1
inline constexpr std::uint8_t kMaxEducationLevel = 100;  // [predl.]
/// Age of majority (GDD 01 sections 3.1 and 6.3): on the 16th birthday a character comes of age (E7-T13).
inline constexpr int kComingOfAgeYears = 16;

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

/// Stable tags in the chronicle section and RecordChronicle payload.
enum class ChronicleKind : std::uint8_t { Birth = 1, Death = 2, HeadChanged = 3, Extinction = 4, Note = 5 };
inline constexpr std::size_t kMaxChronicleEntries = 256;

struct ChronicleEntry {
    Date date;
    ChronicleKind kind = ChronicleKind::Note;
    CharacterId a;
    CharacterId b;
    std::int32_t param = 0;

    [[nodiscard]] bool operator==(const ChronicleEntry&) const = default;
};

/// Code of the chronicle note (kind Note, `param`) that a character has come of age (E7-T13); the codes of
/// the health system are 101 and 102 (pat/sim/health.hpp).
inline constexpr std::int32_t kNoteComeOfAge = 103;

using OpinionReason = std::uint16_t;
inline constexpr OpinionReason kOtherOpinionReason = 0;
inline constexpr std::size_t kMaxOpinions = 30;
inline constexpr std::size_t kMaxOpinionSources = 3;
inline constexpr auto kOpinionLimit = core::Fixed32::from_int(100);

struct OpinionSource {
    OpinionReason reason = kOtherOpinionReason;
    core::Fixed32 value;

    [[nodiscard]] bool operator==(const OpinionSource&) const = default;
};

struct OpinionEntry {
    CharacterId target;
    std::array<OpinionSource, kMaxOpinionSources> sources{};

    [[nodiscard]] core::Fixed32 total() const noexcept;
    [[nodiscard]] bool operator==(const OpinionEntry&) const = default;
};

enum class StandingMetric : std::uint8_t { Honor = 1, Unity = 2, Prestige = 3 };
using StandingReason = std::uint16_t;
inline constexpr StandingReason kStandingReasonUnspecified = 0;
inline constexpr StandingReason kStandingReasonMonthlyDrift = 1;
inline constexpr StandingReason kStandingReasonLoanOverdue = 2;
inline constexpr StandingReason kStandingReasonBankruptcy = 3;
inline constexpr std::size_t kMaxStandingChanges = 32;

struct StandingChange {
    Date date;
    StandingMetric metric = StandingMetric::Honor;
    StandingReason reason = kStandingReasonUnspecified;
    core::Fixed32 delta;

    [[nodiscard]] bool operator==(const StandingChange&) const = default;
};

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
    std::vector<core::Fixed32> unity;     // 0..100, default kDefaultUnity
    std::vector<core::Fixed32> prestige;  // 0..100, default kDefaultPrestige
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
/// priority inside a day. Labour is not here: births have a queue of their own (see
/// World::take_due_labours), so that the system that handles the birthdays never sees them.
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

// ---- pregnancy (E7-T12, GDD 01 section 3.3) ---------------------------------------------------------
/// How a pregnancy is going to end, decided when it starts: at term (the labour then yields a living
/// child or a stillbirth) or early, by a miscarriage. Stored as one byte; the numbers are part of the
/// save format.
enum class PregnancyEnd : std::uint8_t { AtTerm, Miscarriage };
inline constexpr std::uint8_t kPregnancyEndCount = 2;

/// What a labour brought: a living child, a stillbirth, or (for a pregnancy that ended early) a
/// miscarriage. A command payload, not stored in the world.
enum class BirthOutcome : std::uint8_t { Live, Stillborn, Miscarriage };
inline constexpr std::uint8_t kBirthOutcomeCount = 3;

/// A pregnancy takes at most this many days (24 months of 31 days is the corridor of the rules).
inline constexpr std::int32_t kMaxPregnancyDays = 24 * 31;

/// One woman carries at most one child at a time. The row lives from the Conceive command until the
/// Birth command (or the mother's death), and it is the whole state: the labour queue is derived from
/// `due`. The father is the man the child belongs to; he may die before the birth.
struct Pregnancy {
    CharacterId mother;
    CharacterId father;
    Date conceived;
    Date due;  // when the labour (or the miscarriage) takes place
    PregnancyEnd end = PregnancyEnd::AtTerm;
    Legitimacy legitimacy = Legitimacy::Legitimate;

    [[nodiscard]] bool operator==(const Pregnancy&) const = default;
};

/// A labour that has come due (World::take_due_labours).
struct DueLabour {
    Date due;
    CharacterId mother;
    [[nodiscard]] bool operator==(const DueLabour&) const = default;
};

/// A character who has come of age (World::take_due_adulthoods).
struct DueAdulthood {
    Date due;  // the 16th birthday (or, for an entry taken late, the day it was due)
    CharacterId character;
    [[nodiscard]] bool operator==(const DueAdulthood&) const = default;
};

/// The 16th birthday of somebody born on `birth` (29 February counts as 28 February in common years).
[[nodiscard]] constexpr Date adulthood_date(Date birth) noexcept {
    return anniversary_in_year(birth, birth.year() + kComingOfAgeYears);
}

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
    Negotiation = 8,   // E8-T03: counters, negotiations and their outcome ring
    Betrothal = 9,     // E8-T03
    MarriageRecord = 10,
    PersonStatus = 11,
    Succession = 12,  // E8-T03: choices, grants and godparent cursor
    Regency = 13,
    Dispute = 14,
    Economy = 16,  // E9-T03 (numbers 16..23 are E9's): hash of the nine economy sub-hashes
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
    void add_honor(DynastyId dynasty, std::int32_t delta, StandingReason reason = kStandingReasonUnspecified);
    /// Sets the unity, clamped to 0..100.
    void set_dynasty_unity(DynastyId dynasty,
                           core::Fixed32 unity,
                           StandingReason reason = kStandingReasonUnspecified);
    void add_unity(DynastyId dynasty, core::Fixed32 delta, StandingReason reason);
    void add_prestige(DynastyId dynasty, core::Fixed32 delta, StandingReason reason);
    /// The last applied nonzero changes, oldest first, in dynasty mode.
    [[nodiscard]] std::span<const StandingChange> standing_changes(DynastyId dynasty) const;
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
    /// Reserves room for `extra` more character slots in every column, so that the next births do
    /// not reallocate each of the ~30 columns (a copied or loaded world has no spare capacity).
    /// Capacity only: no state, hash or save changes.
    void reserve_character_headroom(std::size_t extra);
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

    /// Retained facts and notes, oldest first and in insertion order within a day.
    [[nodiscard]] std::span<const ChronicleEntry> chronicle(DynastyId dynasty) const;
    [[nodiscard]] std::vector<ChronicleEntry> query_chronicle(
        DynastyId dynasty, Date from, Date through, std::optional<ChronicleKind> kind = std::nullopt) const;
    bool record_chronicle_note(DynastyId dynasty, CharacterId a, CharacterId b, std::int32_t param);

    /// A directed change for two living dynasty-mode characters. Returns false for invalid endpoints.
    bool add_opinion(CharacterId from, CharacterId to, core::Fixed32 delta, OpinionReason reason);
    [[nodiscard]] core::Fixed32 get_opinion(CharacterId from, CharacterId to) const;
    [[nodiscard]] std::span<const OpinionSource> opinion_sources(CharacterId from, CharacterId to) const;
    /// Stored rows, sorted by target. A dead target can remain here until this holder's monthly slot.
    [[nodiscard]] std::span<const OpinionEntry> opinions_from(CharacterId holder) const;
    /// Process this day's monthly slot and return the number of relations visited for WU accounting.
    std::size_t decay_opinions(Date date);
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
    void set_character_health(CharacterId id, core::Fixed32 value);
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
    /// The cache of the province modifiers (E9-T17, economy/modifiers.hpp): DERIVED data, not part of the
    /// save, the hash or any copy-comparison; empty (everything dirty) in a new or a loaded world. The
    /// mutators below mark it where its inputs change; the functions of modifiers.hpp read and refill it.
    [[nodiscard]] const economy::ModifierCache& modifier_cache() const noexcept { return modifier_cache_; }
    [[nodiscard]] economy::ModifierCache& modifier_cache_mut() noexcept { return modifier_cache_; }
    /// Marks the modifiers of one province / of every province dirty. For writers the World cannot see:
    /// the `tax_policy` / `recruit_policy` columns of a PROVINCE written through economy_columns_mut(), a
    /// change of the rules. No effect on the state or the hash.
    void invalidate_province_modifiers(ProvinceId province) noexcept;
    void invalidate_all_province_modifiers() noexcept;
    /// Hash of the rules the world was built with. Saved with the economy, but not hashed.
    void set_economy_rules_hash(std::uint64_t rules_hash) noexcept;
    /// The number of levels of the tax and of the recruit policy in the rules of this world (E9-T12):
    /// the range of SetTaxPolicy and SetRecruitPolicy. Call it where the rules are known (scenario
    /// builders; the invariant E-R1 checks it against the rules). Without it only the structural limit
    /// economy::kMaxPolicyLevels holds. False (nothing changed) without economy or for more levels
    /// than the limit.
    bool set_economy_policy_levels(std::uint8_t tax_levels, std::uint8_t recruit_levels) noexcept;
    /// The policy of a dynasty for all its provinces (E9-T12, the effect of SetTaxPolicy /
    /// SetRecruitPolicy). The level must be below the number of levels set by set_economy_policy_levels.
    /// False (nothing changed) for an unknown dynasty, a level out of range or without economy.
    bool set_dynasty_tax_policy(DynastyId dynasty, std::uint8_t level);
    bool set_dynasty_recruit_policy(DynastyId dynasty, std::uint8_t level);
    /// Conscription (E9-T13, the effect of ConscriptRecruits): takes `count` people out of the recruit
    /// pool of a province, out of the peasants, cossacks and townsfolk (economy::split_conscription),
    /// and adds the same number to the manpower reserve of the owner dynasty. Returns the number really
    /// conscripted: `count`, or less when the pool, the people of the recruit groups or the room in the
    /// reserve are fewer; 0 without economy, for an unknown province or one that no dynasty owns.
    std::uint32_t conscript_recruits(ProvinceId province, std::uint32_t count);
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
    /// every province (invariant E-P4). A province that changes hands (set_province_owner,
    /// set_province_owner_kind) loses its administrator unless he is of the house of the new dynasty owner.
    bool set_province_administrator(ProvinceId province, CharacterId administrator);
    /// Puts a building (0 = empty) into a slot of a province; false when the slot is not below
    /// `slots_total` or the province is unknown.
    bool set_slot_building(ProvinceId province, std::size_t slot, std::uint8_t building);
    /// Construction (E9-T16, economy-model sections 4.6 and 9 F15; the effects of StartConstruction,
    /// CancelConstruction and of the system `economy.construction`). The rules of WHO may build WHAT
    /// WHERE live in economy/construction.hpp; these three only keep the tables consistent.
    ///
    /// Starts building `building` (a type number, not 0) in the lowest slot below `slots_total` that
    /// holds no building and no other construction: puts an entry into the queue (kept in the order
    /// `(finish_date, seq)`, `finish_date = today + build_days`, `seq` from `next_construction_seq`),
    /// and posts `-cost_kop` to the account of `payer` as ONE posting of the code Construction. Returns
    /// the slot. nullopt (nothing changed) without economy, for an unknown province or payer, a zero
    /// `building`, `build_days < 1` or a cost outside 0...kMaxPostingKop, when no slot is free, or when
    /// the payer holds less than `cost_kop` (a balance in debt affords nothing that costs).
    std::optional<std::uint8_t> begin_construction(ProvinceId province,
                                                   std::uint8_t building,
                                                   DynastyId payer,
                                                   economy::Kopecks cost_kop,
                                                   std::int32_t build_days);
    /// Cancels the construction that occupies `slot` of a province: removes the entry, frees the slot
    /// and posts `refund_permille` of the paid cost (kopecks truncated toward zero) back to the payer
    /// as a positive posting of the code Construction. Returns the refund; nullopt (nothing changed)
    /// when there is no such entry, the permille is outside 0...1000 or without economy.
    std::optional<economy::Kopecks>
    abort_construction(ProvinceId province, std::uint8_t slot, std::int32_t refund_permille);
    /// Finishes every construction whose `finish_date` is not after today (they stand at the head of
    /// the queue): the building goes into its slot, the entry leaves the queue, and the incident log
    /// gets BuildingCompleted (province, payer, `param` = the type). Returns how many were finished;
    /// O(1) when the queue is empty or nothing is due.
    std::size_t finish_due_constructions();

    /// Issues a crown loan after checking the rules and the room on both ledger sides. The amount
    /// moves in full or nothing changes. Terms are fixed at issue; ids increase and are never reused.
    std::optional<std::uint32_t>
    take_loan(DynastyId borrower, economy::Kopecks amount_kop, const economy::LoanRules& rules);
    /// Repays principal from the borrower's treasury. Rejects a non-positive amount, overpayment,
    /// insufficient funds or an unrepresentable transfer. Full repayment removes the row stably.
    std::optional<economy::Kopecks>
    repay_loan(std::uint32_t loan_id, economy::Kopecks amount_kop, const economy::LoanRules& rules);
    /// Called by monthly settlement before rotating the ledger: posts one month's interest for
    /// loans issued before `month_start`, marks first overdue dates and refreshes bankruptcy flags.
    /// The caller must rotate the ledger afterwards; calling twice before rotation charges twice.
    economy::LoanSettlementReport settle_loans(const economy::LoanRules& rules, Date month_start);

    /// Row of an account in the account table: the crown is row 0 (or the row of the crown dynasty
    /// in the "one treasury" mode), dynasty `d` is row `1 + d`. nullopt without economy or for an
    /// unknown dynasty.
    [[nodiscard]] std::optional<std::size_t> resolve_account(economy::AccountRef account) const noexcept;
    /// Balance of an account in kopecks (0 for an account that does not exist).
    [[nodiscard]] economy::Kopecks treasury(economy::AccountRef account) const noexcept;
    /// Scenario bootstrap: sets both the opening balance and treasury of a fresh account without
    /// recording income. False (nothing changed) without economy, for an unknown account, after
    /// the first tick or month closure, for a nonzero account or any open posting, or when the
    /// amount is outside +-kMaxTreasuryKop. Existing gameplay money goes through post_money/transfer.
    bool set_initial_treasury(economy::AccountRef account, economy::Kopecks amount_kop);
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
    /// True for an existing dynasty in dynasty mode with no living members.
    [[nodiscard]] bool dynasty_extinct(DynastyId dynasty) const noexcept;
    [[nodiscard]] std::span<const ProvinceId> provinces_of(DynastyId dynasty) const noexcept;
    /// Ids of all living characters in strictly ascending order.
    [[nodiscard]] std::span<const CharacterId> living() const noexcept { return living_; }
    /// The life-event queue (ADR-0006 D4): a derived index over birth and planned-death dates,
    /// empty outside dynasty mode. Not saved and not hashed; rebuilt on load.
    [[nodiscard]] const ScheduledQueue<LifeEvent>& life_queue() const noexcept { return life_queue_; }

    // ---- family tables (E8-T02; rules, systems and persistence follow in E8) ----
    [[nodiscard]] const family::State& family() const noexcept { return family_; }
    /// Creation assigns the next monotone id; the supplied id must be zero. Rejected mutations
    /// leave every family row and counter unchanged. These operations apply structural checks only.
    [[nodiscard]] std::optional<family::NegotiationId> create_family_negotiation(family::Negotiation row);
    bool update_family_negotiation(family::NegotiationId id, const family::Negotiation& row);
    bool close_family_negotiation(family::NegotiationId id);
    [[nodiscard]] std::optional<family::BetrothalId> create_family_betrothal(family::Betrothal row);
    bool update_family_betrothal(family::BetrothalId id, const family::Betrothal& row);
    bool close_family_betrothal(family::BetrothalId id);
    /// An Active record describes an existing World::marry pair; this does not marry the people.
    [[nodiscard]] std::optional<family::MarriageId> create_family_marriage(family::Marriage row);
    bool update_family_marriage(family::MarriageId id, const family::Marriage& row);
    [[nodiscard]] std::optional<family::OutcomeSequence>
    append_family_outcome(family::NegotiationOutcome row);
    bool set_family_status(const family::PersonStatus& row);
    bool clear_family_status(CharacterId character);
    bool set_family_succession(const family::SuccessionState& row);
    bool clear_family_succession(DynastyId dynasty);
    bool set_family_regency(const family::Regency& row);
    bool clear_family_regency(DynastyId dynasty);
    bool set_family_dispute(const family::Dispute& row);
    bool clear_family_dispute(DynastyId dynasty);
    /// With economy enabled, the holder must already be the province administrator.
    bool set_family_land_grant(const family::LandGrant& row);
    bool clear_family_land_grant(ProvinceId province);
    bool set_family_godparent_cursor(std::uint32_t cursor);
    [[nodiscard]] const family::Negotiation* find_family_negotiation(family::NegotiationId id) const;
    [[nodiscard]] const family::Betrothal* find_family_betrothal(family::BetrothalId id) const;
    [[nodiscard]] const family::Marriage* find_family_marriage(family::MarriageId id) const;
    [[nodiscard]] const family::NegotiationOutcome* find_family_outcome(family::OutcomeSequence seq) const;
    [[nodiscard]] const family::PersonStatus* family_status(CharacterId character) const;
    [[nodiscard]] const family::SuccessionState* family_succession(DynastyId dynasty) const;
    [[nodiscard]] const family::Regency* family_regency(DynastyId dynasty) const;
    [[nodiscard]] const family::Dispute* family_dispute(DynastyId dynasty) const;
    [[nodiscard]] const family::LandGrant* family_land_grant(ProvinceId province) const;
    [[nodiscard]] const family::Negotiation* family_negotiation_of(CharacterId character) const;
    [[nodiscard]] const family::Betrothal* family_betrothal_of(CharacterId character) const;
    [[nodiscard]] const family::Marriage* family_marriage_of(CharacterId character) const;
    /// Computed indexes: 0 none, 1 negotiation, 2 betrothal. No mutable derived family state.
    [[nodiscard]] std::uint8_t engagement_of(CharacterId character) const;
    /// The first living adult in the saved order, otherwise the first living minor (#491).
    /// No final title is assigned.
    [[nodiscard]] CharacterId family_caretaker(DynastyId dynasty) const;

    // ---- pregnancies (E7-T12) ----
    /// Pregnancies by ascending mother id. Dynasty mode only (empty otherwise).
    [[nodiscard]] std::span<const Pregnancy> pregnancies() const noexcept { return pregnancies_; }
    [[nodiscard]] std::optional<Pregnancy> pregnancy_of(CharacterId mother) const;
    [[nodiscard]] bool is_pregnant(CharacterId mother) const noexcept;
    /// Starts a pregnancy and queues its labour. False (nothing changed) outside dynasty mode, when
    /// the mother is not a living woman, already carries a child, the father is not a living man (the
    /// father must exist at conception), or the dates are not sane: `conceived` not after today, `due`
    /// after `conceived` and at most kMaxPregnancyDays later.
    bool begin_pregnancy(const Pregnancy& pregnancy);
    /// Ends the pregnancy of `mother` (the Birth command, or the mother's death) and hands the row out.
    std::optional<Pregnancy> end_pregnancy(CharacterId mother);
    /// Takes the labours with `due <= today` out of the queue, oldest first and by mother id inside a
    /// day, dropping stale entries (the pregnancy ended or has another due date). A labour that was
    /// taken is not offered again: the caller issues the Birth command, and until it is applied the row
    /// stays. After loading a saved world the queue is rebuilt from the rows.
    [[nodiscard]] std::vector<DueLabour> take_due_labours(Date today);

    // ---- coming of age (E7-T13) ----
    /// Takes the characters whose 16th birthday has come (`due <= today`) out of the queue, by (due, id),
    /// dropping the dead. Each living character is offered exactly once: the caller issues the ComeOfAge
    /// command. The queue is derived (not saved, not hashed): it holds the living characters younger than
    /// 16 and is rebuilt on load for a state at a tick boundary (everything up to today is processed).
    [[nodiscard]] std::vector<DueAdulthood> take_due_adulthoods(Date today);

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
    ///   6  + section 5 "chronicle" (E7-T16); older files start with empty chronicles
    ///   7  + section 6 "opinion" (E7-T17); older files start without stored opinions
    ///   8  + section 7 "dynasty_standing" (E7-T14); older files start with default prestige
    ///   9  + section 40 "economy policy" (E9-T12): policies of the dynasties; older files: all default
    ///  10  loan interest carries use denominator 12,000,000; older carries are multiplied by 12
    ///  11  + section 4 "pregnancy" (E7-T12); older files start without pregnancies
    ///  12  + sections 16..23 (E8 family), coded ledger additions 16 and 112
    static constexpr std::uint32_t kStateSchema = 12;
    /// Writes the complete state, including the order of the index lists, so that a loaded world
    /// has the same hash and behaves identically in the future.
    void save_state(core::ByteWriter& out) const;
    /// Reads a state written by save_state() of any supported schema and migrates it to the current
    /// one. Never throws and never trusts counts or ids: bad input produces an error, not a crash.
    /// The result satisfies check_invariants().
    [[nodiscard]] static core::Result<World, core::Error> load_state(core::ByteReader& in);

private:
    struct StandingHistory {
        DynastyId dynasty;
        std::vector<StandingChange> changes;
    };
    void append_standing_change(DynastyId dynasty,
                                StandingMetric metric,
                                StandingReason reason,
                                core::Fixed32 delta);
    [[nodiscard]] std::uint64_t standing_hash() const;
    void check_standing_invariants(std::vector<std::string>& errors) const;
    struct OpinionRow {
        CharacterId holder;
        std::vector<OpinionEntry> entries;
    };
    void erase_opinion_holder(CharacterId holder);
    [[nodiscard]] std::uint64_t opinion_hash() const;
    void check_opinion_invariants(std::vector<std::string>& errors) const;
    void append_chronicle(DynastyId dynasty, ChronicleEntry entry);
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
    void check_pregnancy_invariants(std::vector<std::string>& bad) const;
    [[nodiscard]] std::uint64_t pregnancy_hash() const;
    void family_on_death(CharacterId character);
    void check_family_invariants(std::vector<std::string>& bad) const;
    // One section of the extension block of the save file (world_io.cpp, world_economy_io.cpp).
    struct StateSection {
        std::uint16_t id = 0;
        std::uint16_t version = 1;
        core::ByteWriter body;
    };
    [[nodiscard]] bool family_section_present(std::uint16_t id) const;
    /// Appends nondefault family sections 16..23 in ascending order.
    void family_write_sections(std::vector<StateSection>& sections) const;
    /// Reads raw rows; the caller checks cross-table invariants after every section is loaded.
    [[nodiscard]] const char* family_read_section(std::uint16_t id, core::ByteReader& in);
    /// Parts 8..14 in order; default content yields zero and contributes nothing to the total.
    [[nodiscard]] std::array<std::uint64_t, 7> family_hash_parts() const;
    /// Appends the sections of the economy (nothing when it is off), ascending by id.
    void economy_write_sections(std::vector<StateSection>& sections) const;
    /// Reads one economy section into the tables. `closed_months` etc. come from the section 32 read
    /// before (ids ascend). Returns nullptr on success, otherwise a description of the damage; the
    /// invariants are checked by the caller once everything has been read.
    [[nodiscard]] const char* economy_read_section(std::uint16_t id, core::ByteReader& in);
    /// Migration 8 -> 9 (E9-T12): gives every dynasty the policy "default"; nothing without economy.
    void economy_migrate_policies();
    // Economy helpers (world_economy.cpp).
    void economy_add_dynasty_rows();
    /// Appends the economy rows of province `index`, which exists in ProvinceTable already: the
    /// people come from its population cache, the owner kind from its owner.
    void economy_add_province_rows(std::size_t index);
    void economy_remove_administrator(CharacterId id) noexcept;
    /// A skill or a trait of a character changed: the provinces he administers get their modifiers marked
    /// dirty (E9-T17; his profile may have changed).
    void economy_note_character_change(CharacterId id) noexcept;
    /// After the owner (or the kind of ownership) of a province changed: drops the administrator unless he
    /// belongs to the house of the new dynasty owner (E9 #242; the rule of AssignProvinceAdministrator).
    void economy_release_foreign_administrator(ProvinceId province) noexcept;
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
    std::vector<Pregnancy> pregnancies_;                        // by ascending mother id
    ScheduledQueue<CharacterId> labour_queue_;                  // derived: (due, mother) of the rows above
    ScheduledQueue<CharacterId> adult_queue_;                   // derived: 16th birthdays of the living
    std::uint64_t archive_sum_ = 0;                             // derived: sum of the hashes of archived rows
    std::map<DynastyId, std::vector<ChronicleEntry>> chronicles_;
    std::vector<OpinionRow> opinions_;               // nonempty holders, ascending id
    std::vector<StandingHistory> standing_history_;  // nonempty histories, ascending dynasty id
    family::State family_;                           // E8-T03: sparse family tables and counters
    economy::EconomyState economy_;                  // E9-T02: empty unless enable_economy() was called
    economy::ModifierCache modifier_cache_;          // E9-T17: derived, never saved, hashed or compared
};

}  // namespace pat::sim
