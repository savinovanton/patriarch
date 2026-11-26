#pragma once

#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/program.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/traits.hpp"
#include "pat/sim/work_budget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The dispatcher of the event engine (E11-T05, docs/dev/event-engine.md section 12): which events may start
// today, on which scope. It owns no persistent state: the cursors, the cooldowns, the history and the
// random streams live in the World (rule 3 of AGENTS.md); what it keeps (the index of the events, the copy
// of the owners of the provinces, scratch vectors) is derived and rebuilt from the set of events and the
// world.
//
// One pass (Dispatcher::run) is, in this order:
//   0. the timers that are due (section 11.1);
//   1. the occurrences of the on-actions of the day (the sources of section 12.2, in a fixed order);
//   2. the candidates (event, scope): the events subscribed to those actions, then the pulses of the day
//      (the monthly slot is `id % 28`, section 12.3); a candidate appears once per tick;
//   3. the gates, cheapest first: scope, cooldown / once, window, mean_time_to_happen / chance roll, trigger;
//   4. pools and the cap of pulse events per scope;
//   5. the launches: history, cooldown, the list Dispatcher::launches().
// What a launch does next (immediate, the queue of the player, the choice, the effects) is E11-T09..T12.
//
// The index is a pure optimisation: Mode::FullScan walks every event and every action and produces exactly
// the same candidates in the same order (the proof of "index = full scan", tests/event_dispatch_test.cpp).

namespace pat::sim {
class World;
}

namespace pat::sim::events {

inline constexpr std::size_t kActionCodes = 256;
inline constexpr std::size_t kScopeKinds = 5;  // None, Character, Dynasty, Province, Realm

/// Codes of the on-actions the dispatcher produces (section 8.1).
namespace action {
inline constexpr std::uint16_t kGameStart = 1;
inline constexpr std::uint16_t kBirth = 6;
inline constexpr std::uint16_t kPregnancyLost = 7;
inline constexpr std::uint16_t kDeath = 8;
inline constexpr std::uint16_t kMarriage = 9;
inline constexpr std::uint16_t kMarriageEnded = 10;
inline constexpr std::uint16_t kComingOfAge = 11;
inline constexpr std::uint16_t kConception = 12;
inline constexpr std::uint16_t kHeadChanged = 13;
inline constexpr std::uint16_t kTraitGained = 14;
inline constexpr std::uint16_t kTraitLost = 15;
inline constexpr std::uint16_t kStressThreshold = 16;
inline constexpr std::uint16_t kProvinceOwnerChanged = 17;
inline constexpr std::uint16_t kUnrestThreshold = 18;
inline constexpr std::uint16_t kRiot = 19;
inline constexpr std::uint16_t kUprising = 20;
inline constexpr std::uint16_t kAnarchy = 21;
inline constexpr std::uint16_t kBankruptcy = 22;
inline constexpr std::uint16_t kLoanOverdue = 23;
inline constexpr std::uint16_t kQuotaShort = 24;
inline constexpr std::uint16_t kBuildingCompleted = 25;
}  // namespace action

/// A subscription in the index: the event (index in EventSet::events) and the role of the action it runs on.
struct Subscriber {
    std::uint32_t event = 0;
    std::uint8_t role = 0;

    [[nodiscard]] friend constexpr bool operator==(const Subscriber&, const Subscriber&) noexcept = default;
};

/// The index of section 12.1: by event scope and action, by event scope and pulse, every list in the order
/// (priority descending, EventKey ascending). Built once for a set of events and a catalog.
class DispatchIndex {
public:
    DispatchIndex() = default;
    DispatchIndex(const EventSet& set, const Catalog& catalog);

    [[nodiscard]] std::span<const Subscriber> subscribers(ScopeKind kind,
                                                          std::uint16_t action) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> pulse(ScopeKind kind, PulseKind pulse) const noexcept;
    /// Every event, in the order (priority descending, EventKey ascending).
    [[nodiscard]] std::span<const std::uint32_t> ranked() const noexcept { return ranked_; }
    [[nodiscard]] std::uint32_t rank_of(std::uint32_t event) const noexcept { return rank_[event]; }
    /// The number of (event, action) and (event, pulse) subscriptions in the index.
    [[nodiscard]] std::size_t subscriptions() const noexcept { return subscriptions_; }

private:
    std::vector<std::uint32_t> ranked_;
    std::vector<std::uint32_t> rank_;
    std::array<std::array<std::vector<Subscriber>, kActionCodes>, kScopeKinds> by_action_;
    std::array<std::array<std::vector<std::uint32_t>, 5>, kScopeKinds> pulses_;
    std::size_t subscriptions_ = 0;
};

/// One happening of an on-action: the roots events may run on (in the order of OnActionEntry::roles), the
/// scopes the action provides to `scope:name` (OnActionEntry::provides) and the parameter.
struct Occurrence {
    static constexpr std::size_t kMaxRoots = 4;
    static constexpr std::size_t kMaxProvided = 2;

    std::uint16_t action = 0;
    std::uint8_t root_count = 0;
    std::array<Scope, kMaxRoots> roots{};
    std::array<Scope, kMaxProvided> provided{};
    std::int32_t param = 0;

    [[nodiscard]] friend constexpr bool operator==(const Occurrence&, const Occurrence&) noexcept = default;
};

/// An event that may start on a scope today, before the gates.
struct Candidate {
    std::uint32_t event = 0;
    Scope scope;
    PulseKind pulse = PulseKind::None;  // the pulse it came from; None: an action
    std::int32_t occurrence = -1;       // index in Dispatcher::occurrences(); -1 for a pulse
    bool rolled = false;                // the roll of mean_time_to_happen was won at generation

    [[nodiscard]] friend constexpr bool operator==(const Candidate&, const Candidate&) noexcept = default;
};

/// An event that started: what the next stages (E11-T09..T12) work with.
struct Launch {
    EventKey key = 0;
    std::uint32_t event = 0;
    Scope scope;
    std::array<Scope, limits::kMaxSavedScopes> saved{};
    std::uint8_t depth = 0;
    bool from_timer = false;
    std::int32_t param = 0;

    [[nodiscard]] friend constexpr bool operator==(const Launch&, const Launch&) noexcept = default;
};

/// Counters of one pass (the stages of the benchmark, the report of sim-events).
struct DispatchStats {
    std::uint32_t occurrences = 0;
    std::uint32_t action_candidates = 0;
    std::uint32_t pulse_candidates = 0;  // after the roll at generation (pulse_slots before it)
    std::uint32_t pulse_slots = 0;
    std::uint32_t duplicates = 0;
    std::uint32_t scope_rejected = 0;
    std::uint32_t cooldown_rejected = 0;
    std::uint32_t window_rejected = 0;
    std::uint32_t rolls = 0;
    std::uint32_t roll_rejected = 0;
    std::uint32_t vm_runs = 0;
    std::uint32_t vm_aborts = 0;
    std::uint32_t trigger_rejected = 0;
    std::uint32_t survivors = 0;
    std::uint32_t pool_losers = 0;
    std::uint32_t capped = 0;
    std::uint32_t timers_due = 0;
    std::uint32_t launches = 0;
    std::uint64_t wu = 0;

    [[nodiscard]] friend constexpr bool operator==(const DispatchStats&,
                                                   const DispatchStats&) noexcept = default;
};

enum class DispatchMode : std::uint8_t {
    Indexed,   // the index of section 12.1 (the game)
    FullScan,  // every event against every action and every entity (the reference of the tests)
};

struct DispatchConfig {
    /// The content that turns stored skills into effective ones and trait ids into slots (E7-T05, E11-T07);
    /// empty: the stored columns are used.
    std::shared_ptr<const TraitCatalog> traits;
    std::vector<std::uint16_t> trait_slots;
    /// Building registry index -> BuildingDef::slot (`has_building`, E11-T08); empty: the index is the slot.
    std::vector<std::uint8_t> building_slots;
    /// The economy rules (`unrest_state` needs the thresholds); empty: `unrest_state` has no data.
    std::shared_ptr<const economy::EconomyRules> economy;

    /// What the evaluators of the catalog read (character_triggers.hpp).
    [[nodiscard]] TriggerEnv trigger_env(const World& world) const {
        TriggerEnv env;
        env.world = &world;
        env.traits = traits.get();
        env.trait_slots = trait_slots;
        env.building_slots = building_slots;
        env.economy = economy.get();
        return env;
    }
};

class Dispatcher {
public:
    Dispatcher(std::shared_ptr<const EventSet> set, Catalog catalog, DispatchConfig config = {});
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;
    ~Dispatcher() = default;

    void set_mode(DispatchMode mode) noexcept { mode_ = mode; }
    /// An observer for the benchmark: with a clock (nanoseconds), the time spent in the machine during the
    /// gates is summed into vm_ns(). It never influences the pass (no decision reads it).
    using Clock = std::uint64_t (*)() noexcept;
    void set_clock(Clock clock) noexcept { clock_ = clock; }
    [[nodiscard]] std::uint64_t vm_ns() const noexcept { return vm_ns_; }
    [[nodiscard]] DispatchMode mode() const noexcept { return mode_; }

    /// Copies the owners of the provinces: the "old owner" of the next SetProvinceOwner. Call after the
    /// world was loaded or replaced (the commands of the next tick are not applied yet).
    void rebaseline(const World& world);

    /// One whole pass over the tick that has just finished its commands: `applied` are the commands the tick
    /// processed (CommandLog entries of this tick).
    void run(World& world, std::span<const LoggedCommand> applied, WorkBudget* budget = nullptr);

    // The stages of run(), public for the benchmark and the tests (call in this order).
    void begin(World& world);                                                        // resets the pass
    void fire_timers(World& world);                                                  // stage 0
    void collect_occurrences(World& world, std::span<const LoggedCommand> applied);  // stage 1
    void collect_candidates(World& world);                                           // stage 2 (rolls)
    void gate(World& world);                                                         // stage 3
    void fire(World& world);                                                         // stages 4 and 5
    void finish(World& world);                                                       // cursors, owners

    [[nodiscard]] const DispatchIndex& index() const noexcept { return index_; }
    [[nodiscard]] const EventSet& set() const noexcept { return *set_; }
    [[nodiscard]] const std::vector<Occurrence>& occurrences() const noexcept { return occurrences_; }
    [[nodiscard]] const std::vector<Candidate>& candidates() const noexcept { return candidates_; }
    [[nodiscard]] const std::vector<Candidate>& survivors() const noexcept { return survivors_; }
    [[nodiscard]] const std::vector<Launch>& launches() const noexcept { return launches_; }
    [[nodiscard]] const DispatchStats& stats() const noexcept { return stats_; }

private:
    /// A sort key of the dedup, the pools and the cap: (a, b, position in the vector).
    struct SortKey {
        std::uint64_t a = 0;
        Word b = 0;
        std::uint32_t position = 0;
        [[nodiscard]] bool operator<(const SortKey& o) const noexcept {
            if (a != o.a) {
                return a < o.a;
            }
            if (b != o.b) {
                return b < o.b;
            }
            return position < o.position;
        }
    };

    void launch_now(World& world, const EventDef& def, const Launch& launch);

    std::shared_ptr<const EventSet> set_;
    Catalog catalog_;
    DispatchConfig config_;
    DispatchIndex index_;
    DispatchMode mode_ = DispatchMode::Indexed;
    std::array<const OnActionEntry*, kActionCodes> actions_{};
    Vm vm_;
    std::vector<DynastyId> prev_owner_;
    bool baselined_ = false;
    std::vector<std::uint8_t> multi_;  // per event: more than one subscription (can repeat a candidate)

    std::vector<Occurrence> occurrences_;
    std::vector<Candidate> candidates_;
    std::vector<Candidate> survivors_;
    std::vector<Launch> launches_;
    DispatchStats stats_;
    bool state_full_ = false;
    Clock clock_ = nullptr;
    std::uint64_t vm_ns_ = 0;

    // scratch, reused between passes
    std::vector<std::uint32_t> slot_ids_;
    std::vector<SortKey> keys_;
    std::vector<std::uint8_t> drop_;
    std::vector<std::uint32_t> weights_;
};

}  // namespace pat::sim::events
