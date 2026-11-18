#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/entity_ids.hpp"
#include "pat/sim/events/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// World-owned state of the event engine (E11-T02, docs/dev/event-engine.md section 9). Ten tables:
// the meta record, flags, variables, timers, cooldowns, chains, modifiers, the queue of the player,
// and the rings of history and journal. They are sparse (empty in a world without events), sorted
// by key and bounded. World owns every mutation, bound and reference check; these types contain no
// rules. Persistence and hashing of the tables are E11-T03; the dispatcher and the queue rules are
// E11-T05 and T12.

namespace pat::sim::events {

/// 64-bit key of an event, a flag, a variable, a modifier or a chain: FNV-1a 64 of the id (D6).
/// 0 is not a valid key.
using EventKey = std::uint64_t;
using InstanceId = std::uint32_t;

// ---- bounds `[предл.]` (section 9.1) -----------------------------------------------------------------

inline constexpr std::size_t kMaxFlags = 32768;
inline constexpr std::size_t kMaxVars = 8192;
inline constexpr std::size_t kMaxTimers = 4096;
inline constexpr std::size_t kMaxCooldowns = 32768;
inline constexpr std::size_t kMaxChains = 1024;
inline constexpr std::size_t kMaxModifiers = 8192;
inline constexpr std::size_t kMaxQueue = 64;
inline constexpr std::size_t kHistoryRingSize = 512;
inline constexpr std::size_t kJournalRingSize = 256;
inline constexpr std::size_t kMaxPlayers = 4;
inline constexpr std::uint16_t kMaxModalCount = 1024;
inline constexpr std::uint16_t kMaxFrequencyPermille = 10000;
inline constexpr std::uint16_t kDefaultFrequencyPermille = 1000;

/// "No end": the `until` of a `once` cooldown.
inline constexpr Date kForever = Date::from_days(std::numeric_limits<std::int32_t>::max());
/// Bit 63 of a cooldown key marks a cooldown group (the key of the group, not of an event).
inline constexpr EventKey kCooldownGroupBit = EventKey{1} << 63;
/// `option` of a queue row or a history row while no option was chosen.
inline constexpr std::uint8_t kNoOption = 255;

// ---- rows ---------------------------------------------------------------------------------------------

/// A flag of a scope. `expires` is the last day it is present (kNoDate: unlimited).
struct Flag {
    Scope scope;
    EventKey key = 0;
    Date expires = kNoDate;

    [[nodiscard]] bool operator==(const Flag&) const = default;
};

/// A variable of a scope; an absent variable reads as 0.
struct Var {
    Scope scope;
    EventKey key = 0;
    core::Fixed64 value;

    [[nodiscard]] bool operator==(const Var&) const = default;
};

/// A scope the event stored under a name (`save_scope_as`); kept in timers and queue rows. A dead
/// character is an allowed archive reference here.
struct SavedScope {
    std::uint32_t name_key = 0;
    Scope scope;

    [[nodiscard]] bool operator==(const SavedScope&) const = default;
};
using SavedScopes = std::array<SavedScope, limits::kMaxSavedScopes>;

/// A delayed `trigger_event`: ordered by (due, seq); `scope` is where the event starts.
struct Timer {
    std::uint32_t seq = 0;
    Date due = kNoDate;
    EventKey event_key = 0;
    Scope scope;
    std::uint8_t depth = 0;
    std::uint8_t saved_count = 0;
    SavedScopes saved{};

    [[nodiscard]] bool operator==(const Timer&) const = default;
};

/// A pause of an event or of a cooldown group (bit 63 of `key`) on a scope. `until` is the first day
/// the pause no longer holds (kForever: `once`).
struct Cooldown {
    Scope scope;
    EventKey key = 0;
    Date until = kNoDate;

    [[nodiscard]] bool operator==(const Cooldown&) const = default;
};

/// A named "where are we in the story" record of a scope.
struct Chain {
    Scope scope;
    EventKey key = 0;
    std::uint8_t step = 0;
    Date started = kNoDate;
    Date updated = kNoDate;

    [[nodiscard]] bool operator==(const Chain&) const = default;
};

/// A named modifier of a scope with an end date (kNoDate: unlimited).
struct Modifier {
    Scope scope;
    EventKey key = 0;
    Date added = kNoDate;
    Date expires = kNoDate;

    [[nodiscard]] bool operator==(const Modifier&) const = default;
};

/// Numeric values are stable: the E11-T03 save codec stores them.
enum class InstanceState : std::uint8_t { Pending = 0, Chosen = 1, Deferred = 2 };

inline constexpr std::uint8_t kInstanceAutopause = 1U << 0;
inline constexpr std::uint8_t kInstanceCounted = 1U << 1;  ///< counted in the monthly modal limit
inline constexpr std::uint8_t kInstanceDemoted = 1U << 2;
inline constexpr std::uint8_t kInstanceFlagMask = kInstanceAutopause | kInstanceCounted | kInstanceDemoted;
inline constexpr std::uint8_t kAvailMaskBits = 0x0F;  ///< the window shows at most four options

/// An event that waits for the player (or has been chosen and not yet applied), keyed by `instance`.
struct Instance {
    InstanceId instance = 0;
    EventKey event_key = 0;
    DynastyId recipient;
    Scope scope;
    Date fired_on = kNoDate;
    InstanceState state = InstanceState::Pending;
    std::uint8_t option = kNoOption;
    std::uint8_t avail_mask = 0;
    std::uint8_t urgency = 0;
    std::uint8_t priority = 0;
    std::uint8_t flags = 0;
    Date defer_until = kNoDate;
    std::uint8_t defer_count = 0;
    std::uint8_t depth = 0;
    std::uint8_t saved_count = 0;
    SavedScopes saved{};

    [[nodiscard]] bool operator==(const Instance&) const = default;
};

enum class HistoryOutcome : std::uint8_t {
    Fired = 1,
    Aborted = 2,
    ChainDepthExceeded = 3,
    EffectRejected = 4,
    Demoted = 5,
    Cancelled = 6,
    Disabled = 7,
    StateFull = 8,
};
inline constexpr std::uint8_t kLastHistoryOutcome = 8;

/// One entry of the ring of fired events (statistics, the AI, the dev panel). `seq` is assigned by
/// World and ascends.
struct HistoryEntry {
    std::uint32_t seq = 0;
    Date date = kNoDate;
    EventKey event_key = 0;
    HistoryOutcome outcome = HistoryOutcome::Fired;
    Scope scope;
    std::uint8_t option = kNoOption;
    std::uint16_t detail = 0;

    [[nodiscard]] bool operator==(const HistoryEntry&) const = default;
};

enum class JournalKind : std::uint8_t { Notify = 1, Rumor = 2, System = 3, Demoted = 4 };
inline constexpr std::uint8_t kLastJournalKind = 4;

/// One entry of the notification journal; the text is built by the UI bridge from the key and the three
/// numbers. `seq` is assigned by World and ascends.
struct JournalEntry {
    std::uint32_t seq = 0;
    Date date = kNoDate;
    DynastyId recipient;
    JournalKind kind = JournalKind::Notify;
    std::uint8_t urgency = 0;
    EventKey key = 0;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t param = 0;
    std::uint8_t flags = 0;

    [[nodiscard]] bool operator==(const JournalEntry&) const = default;
};

/// The single record of counters and settings (section 48). Default = all zero except the two settings
/// that have a documented default, so a world without events has `meta == Meta{}`.
struct Meta {
    std::uint8_t started = 0;
    std::uint64_t events_hash = 0;  ///< informational: which set of events the world was made with
    InstanceId next_instance_id = 0;
    std::uint32_t next_timer_seq = 0;
    std::uint32_t next_journal_seq = 0;
    std::uint32_t next_history_seq = 0;
    /// The next seq of the economy IncidentLog the dispatcher has not read yet.
    std::uint32_t incident_cursor = 0;
    std::uint16_t frequency_permille = kDefaultFrequencyPermille;
    std::uint8_t anchors_enabled = 1;
    std::uint8_t player_count = 0;
    std::array<DynastyId, kMaxPlayers> players{};
    /// Month the modal counter belongs to: year * 12 + (month - 1); 0 before any window was shown.
    std::int32_t modal_month = 0;
    std::uint16_t modal_count = 0;

    [[nodiscard]] bool operator==(const Meta&) const = default;
};

/// Primary state only; the per-scope row counters are derived by World.
struct State {
    Meta meta;
    std::vector<Flag> flags;            ///< by (scope, key)
    std::vector<Var> vars;              ///< by (scope, key)
    std::vector<Timer> timers;          ///< by (due, seq)
    std::vector<Cooldown> cooldowns;    ///< by (scope, key)
    std::vector<Chain> chains;          ///< by (scope, key)
    std::vector<Modifier> modifiers;    ///< by (scope, key)
    std::vector<Instance> queue;        ///< by instance
    std::vector<HistoryEntry> history;  ///< ring, ascending seq
    std::vector<JournalEntry> journal;  ///< ring, ascending seq

    [[nodiscard]] bool operator==(const State&) const = default;
};

/// Derived index: how many rows of the tables name each scope (the root scope for the queue, not the
/// saved scopes). It lets the clean-up after a death skip the scopes that never had event state, which
/// is almost all of them. Never saved or hashed; World rebuilds it on load and keeps it in step.
class ScopeRows {
public:
    void add(Scope scope) {
        const auto position = find(scope.word());
        if (position != rows_.end() && position->first == scope.word()) {
            ++position->second;
        } else {
            rows_.insert(position, {scope.word(), 1});
        }
    }
    void remove(Scope scope) noexcept {
        const auto position = find(scope.word());
        if (position == rows_.end() || position->first != scope.word()) {
            return;
        }
        if (--position->second == 0) {
            rows_.erase(position);
        }
    }
    [[nodiscard]] std::uint32_t count(Scope scope) const noexcept {
        const auto position = find(scope.word());
        return position != rows_.end() && position->first == scope.word() ? position->second : 0;
    }
    void clear() noexcept { rows_.clear(); }
    [[nodiscard]] std::size_t scopes() const noexcept { return rows_.size(); }
    [[nodiscard]] bool operator==(const ScopeRows&) const = default;

private:
    [[nodiscard]] std::vector<std::pair<Word, std::uint32_t>>::const_iterator find(Word word) const noexcept {
        return std::lower_bound(
            rows_.begin(), rows_.end(), word, [](const auto& row, Word w) { return row.first < w; });
    }
    [[nodiscard]] std::vector<std::pair<Word, std::uint32_t>>::iterator find(Word word) noexcept {
        return std::lower_bound(
            rows_.begin(), rows_.end(), word, [](const auto& row, Word w) { return row.first < w; });
    }
    std::vector<std::pair<Word, std::uint32_t>> rows_;  ///< ascending scope word
};

}  // namespace pat::sim::events
