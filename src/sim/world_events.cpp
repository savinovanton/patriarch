// World-owned event engine tables (E11-T02, docs/dev/event-engine.md section 9): the storage API, the
// clean-up of a scope, the derived per-scope counters and the invariants E-1..E-12. No rules here: the
// dispatcher, the queue limits and the effects follow in E11-T05 and later; persistence is E11-T03.

#include "pat/sim/events/state.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "sorted_rows.hpp"

namespace pat::sim {
namespace {

using detail::find_row;
using detail::ordered_unique;
using detail::row_position;
using events::Scope;
using events::ScopeKind;

using detail::append_ring;
using detail::kMaxCounter;

// ---- keys of the tables -------------------------------------------------------------------------------

using ScopeKey = std::pair<events::Word, events::EventKey>;

template <class Row>
ScopeKey scope_key(const Row& row) {
    return {row.scope.word(), row.key};
}

/// The tables keyed by (scope, key): flags, variables, cooldowns, chains, modifiers.
template <class Rows>
const typename Rows::value_type* find_keyed(const Rows& rows, Scope scope, events::EventKey key) {
    return find_row(rows, ScopeKey{scope.word(), key}, [](const auto& row) { return scope_key(row); });
}

enum class Put { Full, Added, Replaced };

template <class Rows>
Put put_keyed(Rows& rows, const typename Rows::value_type& row, std::size_t bound) {
    const ScopeKey key = scope_key(row);
    const auto position = row_position(rows, key, [](const auto& r) { return scope_key(r); });
    if (position != rows.end() && scope_key(*position) == key) {
        *position = row;
        return Put::Replaced;
    }
    if (rows.size() >= bound) {
        return Put::Full;
    }
    rows.insert(position, row);
    return Put::Added;
}

template <class Rows>
bool erase_keyed(Rows& rows, Scope scope, events::EventKey key) {
    const ScopeKey wanted{scope.word(), key};
    const auto position = row_position(rows, wanted, [](const auto& r) { return scope_key(r); });
    if (position == rows.end() || scope_key(*position) != wanted) {
        return false;
    }
    rows.erase(position);
    return true;
}

/// Removes every row of `scope` from a table ordered by (scope, key); returns how many.
template <class Rows>
std::size_t erase_scope_rows(Rows& rows, Scope scope) {
    const auto first =
        std::ranges::lower_bound(rows, scope.word(), {}, [](const auto& row) { return row.scope.word(); });
    const auto last =
        std::ranges::upper_bound(rows, scope.word(), {}, [](const auto& row) { return row.scope.word(); });
    const auto removed = static_cast<std::size_t>(last - first);
    rows.erase(first, last);
    return removed;
}

// ---- value checks -------------------------------------------------------------------------------------

bool date_or_none(Date date) {
    return date == kNoDate || is_sane_date(date);
}

bool saved_scopes_shape(const events::SavedScopes& saved, std::uint8_t count) {
    if (count > saved.size()) {
        return false;
    }
    for (std::size_t i = 0; i < saved.size(); ++i) {
        if (i >= count) {
            if (!(saved[i] == events::SavedScope{})) {
                return false;  // hidden state beyond the counted rows
            }
            continue;
        }
        if (saved[i].name_key == 0) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (saved[j].name_key == saved[i].name_key) {
                return false;
            }
        }
    }
    return true;
}

bool instance_shape(const events::Instance& row) {
    const auto state = static_cast<std::uint8_t>(row.state);
    return row.event_key != 0 && state <= static_cast<std::uint8_t>(events::InstanceState::Deferred) &&
           (row.state != events::InstanceState::Chosen || row.option != events::kNoOption) &&
           row.avail_mask <= events::kAvailMaskBits && (row.flags & ~events::kInstanceFlagMask) == 0 &&
           (row.state == events::InstanceState::Deferred ? is_sane_date(row.defer_until)
                                                         : row.defer_until == kNoDate) &&
           is_sane_date(row.fired_on) && saved_scopes_shape(row.saved, row.saved_count);
}

bool history_shape(const events::HistoryEntry& row) {
    const auto outcome = static_cast<std::uint8_t>(row.outcome);
    return outcome >= 1 && outcome <= events::kLastHistoryOutcome && is_sane_date(row.date);
}

bool journal_shape(const events::JournalEntry& row) {
    const auto kind = static_cast<std::uint8_t>(row.kind);
    return kind >= 1 && kind <= events::kLastJournalKind && is_sane_date(row.date);
}

events::ScopeRows build_scope_rows(const events::State& state) {
    events::ScopeRows rows;
    const auto add_all = [&rows](const auto& table) {
        for (const auto& row : table) {
            rows.add(row.scope);
        }
    };
    add_all(state.flags);
    add_all(state.vars);
    add_all(state.timers);
    add_all(state.cooldowns);
    add_all(state.chains);
    add_all(state.modifiers);
    add_all(state.queue);
    return rows;
}

}  // namespace

// ---- scopes -------------------------------------------------------------------------------------------

bool World::event_scope_ok(Scope scope, bool allow_dead) const noexcept {
    switch (scope.kind) {
        case ScopeKind::Character: {
            const CharacterId id(scope.id);
            return allow_dead ? exists(id) : is_alive(id);
        }
        case ScopeKind::Dynasty:
            return exists(DynastyId(scope.id));
        case ScopeKind::Province:
            return exists(ProvinceId(scope.id));
        case ScopeKind::Realm:
            return scope.id == 0;
        default:
            return false;
    }
}

bool World::event_scope_valid(Scope scope) const noexcept {
    return event_scope_ok(scope, false);
}

// ---- meta ---------------------------------------------------------------------------------------------

bool World::set_event_meta(const events::Meta& meta) {
    const events::Meta& old = events_.meta;
    bool ok = meta.started <= 1 && meta.anchors_enabled <= 1 && meta.frequency_permille >= 1 &&
              meta.frequency_permille <= events::kMaxFrequencyPermille && meta.modal_month >= 0 &&
              meta.modal_count <= events::kMaxModalCount && meta.player_count <= events::kMaxPlayers &&
              meta.next_instance_id >= old.next_instance_id && meta.next_timer_seq >= old.next_timer_seq &&
              meta.next_journal_seq >= old.next_journal_seq &&
              meta.next_history_seq >= old.next_history_seq &&
              meta.incident_cursor <= economy_.meta.next_incident_seq;
    for (std::size_t i = 0; ok && i < meta.players.size(); ++i) {
        if (i >= meta.player_count) {
            ok = !meta.players[i].valid();
            continue;
        }
        ok = exists(meta.players[i]) &&
             std::find(meta.players.begin(), meta.players.begin() + i, meta.players[i]) ==
                 meta.players.begin() + i;
    }
    if (!ok) {
        return false;
    }
    events_.meta = meta;
    return true;
}

// ---- flags, modifiers, variables, cooldowns, chains
// --------------------------------------------------------

bool World::set_event_flag(Scope scope, events::EventKey key, Date expires) {
    if (key == 0 || !event_scope_ok(scope, false) || !date_or_none(expires) ||
        (expires != kNoDate && expires < today())) {
        return false;
    }
    const events::Flag row{scope, key, expires};
    const Put put = put_keyed(events_.flags, row, events::kMaxFlags);
    if (put == Put::Full) {
        return false;
    }
    if (put == Put::Added) {
        event_scope_rows_.add(scope);
    }
    return true;
}

bool World::clear_event_flag(Scope scope, events::EventKey key) {
    if (!erase_keyed(events_.flags, scope, key)) {
        return false;
    }
    event_scope_rows_.remove(scope);
    return true;
}

const events::Flag* World::find_event_flag(Scope scope, events::EventKey key) const {
    return find_keyed(events_.flags, scope, key);
}

bool World::has_event_flag(Scope scope, events::EventKey key) const {
    const auto* row = find_event_flag(scope, key);
    return row != nullptr && (row->expires == kNoDate || row->expires >= today());
}

bool World::set_event_modifier(const events::Modifier& row) {
    if (row.key == 0 || !event_scope_ok(row.scope, false) || !is_sane_date(row.added) ||
        row.added > today() || !date_or_none(row.expires) ||
        (row.expires != kNoDate && row.expires < row.added)) {
        return false;
    }
    const Put put = put_keyed(events_.modifiers, row, events::kMaxModifiers);
    if (put == Put::Full) {
        return false;
    }
    if (put == Put::Added) {
        event_scope_rows_.add(row.scope);
    }
    return true;
}

bool World::clear_event_modifier(Scope scope, events::EventKey key) {
    if (!erase_keyed(events_.modifiers, scope, key)) {
        return false;
    }
    event_scope_rows_.remove(scope);
    return true;
}

const events::Modifier* World::find_event_modifier(Scope scope, events::EventKey key) const {
    return find_keyed(events_.modifiers, scope, key);
}

bool World::has_event_modifier(Scope scope, events::EventKey key) const {
    const auto* row = find_event_modifier(scope, key);
    return row != nullptr && (row->expires == kNoDate || row->expires >= today());
}

bool World::set_event_var(Scope scope, events::EventKey key, core::Fixed64 value) {
    if (key == 0 || !event_scope_ok(scope, false)) {
        return false;
    }
    const events::Var row{scope, key, value};
    const Put put = put_keyed(events_.vars, row, events::kMaxVars);
    if (put == Put::Full) {
        return false;
    }
    if (put == Put::Added) {
        event_scope_rows_.add(scope);
    }
    return true;
}

bool World::clear_event_var(Scope scope, events::EventKey key) {
    if (!erase_keyed(events_.vars, scope, key)) {
        return false;
    }
    event_scope_rows_.remove(scope);
    return true;
}

const events::Var* World::find_event_var(Scope scope, events::EventKey key) const {
    return find_keyed(events_.vars, scope, key);
}

core::Fixed64 World::event_var(Scope scope, events::EventKey key) const {
    const auto* row = find_event_var(scope, key);
    return row != nullptr ? row->value : core::Fixed64{};
}

bool World::set_event_cooldown(const events::Cooldown& row) {
    if (row.key == 0 || !event_scope_ok(row.scope, false) ||
        !(row.until == events::kForever || is_sane_date(row.until))) {
        return false;
    }
    const Put put = put_keyed(events_.cooldowns, row, events::kMaxCooldowns);
    if (put == Put::Full) {
        return false;
    }
    if (put == Put::Added) {
        event_scope_rows_.add(row.scope);
    }
    return true;
}

bool World::clear_event_cooldown(Scope scope, events::EventKey key) {
    if (!erase_keyed(events_.cooldowns, scope, key)) {
        return false;
    }
    event_scope_rows_.remove(scope);
    return true;
}

const events::Cooldown* World::find_event_cooldown(Scope scope, events::EventKey key) const {
    return find_keyed(events_.cooldowns, scope, key);
}

bool World::event_on_cooldown(Scope scope, events::EventKey key) const {
    const auto* row = find_event_cooldown(scope, key);
    return row != nullptr && (row->until == events::kForever || row->until > today());
}

bool World::set_event_chain(const events::Chain& row) {
    if (row.key == 0 || !event_scope_ok(row.scope, false) || !is_sane_date(row.started) ||
        !is_sane_date(row.updated) || row.updated < row.started) {
        return false;
    }
    const Put put = put_keyed(events_.chains, row, events::kMaxChains);
    if (put == Put::Full) {
        return false;
    }
    if (put == Put::Added) {
        event_scope_rows_.add(row.scope);
    }
    return true;
}

bool World::clear_event_chain(Scope scope, events::EventKey key) {
    if (!erase_keyed(events_.chains, scope, key)) {
        return false;
    }
    event_scope_rows_.remove(scope);
    return true;
}

const events::Chain* World::find_event_chain(Scope scope, events::EventKey key) const {
    return find_keyed(events_.chains, scope, key);
}

// ---- timers -------------------------------------------------------------------------------------------

std::optional<std::uint32_t> World::add_event_timer(events::Timer row) {
    if (row.event_key == 0 || !event_scope_ok(row.scope, false) || !is_sane_date(row.due) ||
        row.due < today() || !saved_scopes_shape(row.saved, row.saved_count) ||
        events_.timers.size() >= events::kMaxTimers || events_.meta.next_timer_seq == kMaxCounter) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < row.saved_count; ++i) {
        if (!event_scope_ok(row.saved[i].scope, true)) {
            return std::nullopt;
        }
    }
    row.seq = events_.meta.next_timer_seq++;
    // The new seq is the largest: it goes after every timer due on or before the same day.
    const auto position = std::ranges::upper_bound(
        events_.timers, row.due.days(), {}, [](const events::Timer& t) { return t.due.days(); });
    events_.timers.insert(position, row);
    event_scope_rows_.add(row.scope);
    return row.seq;
}

void World::erase_event_timer_at(std::size_t index) {
    event_scope_rows_.remove(events_.timers[index].scope);
    events_.timers.erase(events_.timers.begin() + static_cast<std::ptrdiff_t>(index));
}

bool World::cancel_event_timer(std::uint32_t seq) {
    const auto position = std::ranges::find(events_.timers, seq, &events::Timer::seq);
    if (position == events_.timers.end()) {
        return false;
    }
    erase_event_timer_at(static_cast<std::size_t>(position - events_.timers.begin()));
    return true;
}

const events::Timer* World::find_event_timer(std::uint32_t seq) const {
    const auto position = std::ranges::find(events_.timers, seq, &events::Timer::seq);
    return position != events_.timers.end() ? &*position : nullptr;
}

// ---- the queue of the player ---------------------------------------------------------------------------

bool World::event_instance_ok(const events::Instance& row) const noexcept {
    if (!instance_shape(row) || !exists(row.recipient) || !event_scope_ok(row.scope, true)) {
        return false;
    }
    for (std::size_t i = 0; i < row.saved_count; ++i) {
        if (!event_scope_ok(row.saved[i].scope, true)) {
            return false;
        }
    }
    return true;
}

std::optional<events::InstanceId> World::enqueue_event_instance(events::Instance row) {
    if (!event_instance_ok(row) || events_.queue.size() >= events::kMaxQueue ||
        events_.meta.next_instance_id == kMaxCounter) {
        return std::nullopt;
    }
    row.instance = events_.meta.next_instance_id++;
    events_.queue.push_back(row);  // ids only grow: the table stays ordered
    event_scope_rows_.add(row.scope);
    return row.instance;
}

bool World::update_event_instance(const events::Instance& row) {
    const auto position = std::ranges::find(events_.queue, row.instance, &events::Instance::instance);
    if (position == events_.queue.end() || position->event_key != row.event_key || !event_instance_ok(row)) {
        return false;
    }
    event_scope_rows_.remove(position->scope);
    *position = row;
    event_scope_rows_.add(row.scope);
    return true;
}

bool World::remove_event_instance(events::InstanceId instance) {
    const auto position = std::ranges::find(events_.queue, instance, &events::Instance::instance);
    if (position == events_.queue.end()) {
        return false;
    }
    event_scope_rows_.remove(position->scope);
    events_.queue.erase(position);
    return true;
}

const events::Instance* World::find_event_instance(events::InstanceId instance) const {
    const auto position = std::ranges::find(events_.queue, instance, &events::Instance::instance);
    return position != events_.queue.end() ? &*position : nullptr;
}

// ---- rings ----------------------------------------------------------------------------------------------

std::optional<std::uint32_t> World::append_event_history(events::HistoryEntry entry) {
    const bool scope_ok = entry.scope.empty() || event_scope_ok(entry.scope, true);
    if (!history_shape(entry) || !scope_ok) {
        return std::nullopt;
    }
    return append_ring(events_.history, events::kHistoryRingSize, events_.meta.next_history_seq, entry);
}

std::optional<std::uint32_t> World::append_event_journal(events::JournalEntry entry) {
    if (!journal_shape(entry) || !exists(entry.recipient)) {
        return std::nullopt;
    }
    return append_ring(events_.journal, events::kJournalRingSize, events_.meta.next_journal_seq, entry);
}

// ---- clean-up -------------------------------------------------------------------------------------------

std::size_t World::clear_event_scope(Scope scope, bool cancel_instances) {
    if (event_scope_rows_.count(scope) == 0) {
        return 0;
    }
    std::size_t removed = 0;
    const auto drop = [&](std::size_t n) {
        removed += n;
        for (std::size_t i = 0; i < n; ++i) {
            event_scope_rows_.remove(scope);
        }
    };
    drop(erase_scope_rows(events_.flags, scope));
    drop(erase_scope_rows(events_.vars, scope));
    drop(erase_scope_rows(events_.cooldowns, scope));
    drop(erase_scope_rows(events_.chains, scope));
    drop(erase_scope_rows(events_.modifiers, scope));
    for (std::size_t i = events_.timers.size(); i-- > 0;) {
        if (events_.timers[i].scope == scope) {
            erase_event_timer_at(i);
            ++removed;
        }
    }
    if (cancel_instances) {
        for (std::size_t i = 0; i < events_.queue.size();) {
            const events::Instance row = events_.queue[i];
            if (row.scope != scope || row.state == events::InstanceState::Chosen) {
                ++i;
                continue;
            }
            events_.queue.erase(events_.queue.begin() + static_cast<std::ptrdiff_t>(i));
            event_scope_rows_.remove(scope);
            ++removed;
            events::HistoryEntry entry;
            entry.date = today();
            entry.event_key = row.event_key;
            entry.outcome = events::HistoryOutcome::Cancelled;
            entry.scope = scope;
            entry.option = row.option;
            (void)append_event_history(entry);
        }
    }
    return removed;
}

void World::rebuild_event_indexes() {
    event_scope_rows_ = build_scope_rows(events_);
}

// ---- invariants -------------------------------------------------------------------------------------------

void World::check_event_invariants(std::vector<std::string>& bad) const {
    const auto fail = [&bad](std::string message) { bad.push_back(std::move(message)); };
    const events::Meta& meta = events_.meta;

    // E-1: sizes and order.
    if (events_.flags.size() > events::kMaxFlags || events_.vars.size() > events::kMaxVars ||
        events_.timers.size() > events::kMaxTimers || events_.cooldowns.size() > events::kMaxCooldowns ||
        events_.chains.size() > events::kMaxChains || events_.modifiers.size() > events::kMaxModifiers ||
        events_.queue.size() > events::kMaxQueue || events_.history.size() > events::kHistoryRingSize ||
        events_.journal.size() > events::kJournalRingSize) {
        fail("E-1: an event table exceeds its bound");
    }
    const auto by_key = [](const auto& row) { return scope_key(row); };
    const auto by_due = [](const events::Timer& row) { return std::pair{row.due.days(), row.seq}; };
    if (!ordered_unique(events_.flags, by_key) || !ordered_unique(events_.vars, by_key) ||
        !ordered_unique(events_.cooldowns, by_key) || !ordered_unique(events_.chains, by_key) ||
        !ordered_unique(events_.modifiers, by_key) || !ordered_unique(events_.timers, by_due) ||
        !ordered_unique(events_.queue, &events::Instance::instance) ||
        !ordered_unique(events_.history, &events::HistoryEntry::seq) ||
        !ordered_unique(events_.journal, &events::JournalEntry::seq)) {
        fail("E-1: event table keys are not unique and ordered");
        return;  // the lookups below require ordered tables
    }

    // E-2, E-3: a row names an existing scope of a supported kind. A character who died not more than
    // a day ago is allowed: the dispatcher cleans the scope at the end of the tick of the death.
    const auto scope_problem = [&](Scope scope, bool allow_dead) -> const char* {
        switch (scope.kind) {
            case ScopeKind::Character: {
                const CharacterId id(scope.id);
                if (!exists(id)) {
                    return "E-2: names an unknown character";
                }
                if (!allow_dead && !is_alive(id) && !(characters_.death_date[id.index()] >= today() - 1)) {
                    return "E-2: names a character who died more than a day ago";
                }
                return nullptr;
            }
            case ScopeKind::Dynasty:
                return exists(DynastyId(scope.id)) ? nullptr : "E-2: names an unknown dynasty";
            case ScopeKind::Province:
                return exists(ProvinceId(scope.id)) ? nullptr : "E-2: names an unknown province";
            case ScopeKind::Realm:
                return scope.id == 0 ? nullptr : "E-3: the realm has id 0";
            default:
                return "E-3: unsupported scope kind";
        }
    };
    const auto check_scope = [&](const char* table, Scope scope, bool allow_dead) {
        if (const char* problem = scope_problem(scope, allow_dead)) {
            fail(std::format("{} row: {}", table, problem));
        }
    };
    for (const auto& row : events_.flags) {
        check_scope("event flag", row.scope, false);
        if (row.key == 0 || !date_or_none(row.expires)) {
            fail("event flag row: bad key or date");
        }
    }
    for (const auto& row : events_.vars) {
        check_scope("event variable", row.scope, false);
        if (row.key == 0) {
            fail("event variable row: zero key");
        }
    }
    for (const auto& row : events_.cooldowns) {
        check_scope("event cooldown", row.scope, false);
        // E-9
        if (row.key == 0 || !(row.until == events::kForever || is_sane_date(row.until))) {
            fail("E-9: event cooldown has a bad key or end");
        }
    }
    for (const auto& row : events_.chains) {
        check_scope("event chain", row.scope, false);
        // E-9
        if (row.key == 0 || !is_sane_date(row.started) || !is_sane_date(row.updated) ||
            row.updated < row.started) {
            fail("E-9: event chain has a bad key or dates");
        }
    }
    for (const auto& row : events_.modifiers) {
        check_scope("event modifier", row.scope, false);
        if (row.key == 0 || !is_sane_date(row.added) || !date_or_none(row.expires) ||
            (row.expires != kNoDate && row.expires < row.added)) {
            fail("event modifier row: bad key or dates");
        }
    }

    // E-4: timers. `due` not before the last pass of the dispatcher is checked by the dispatcher (T05).
    std::vector<std::uint32_t> seqs;
    seqs.reserve(events_.timers.size());
    for (const auto& row : events_.timers) {
        check_scope("event timer", row.scope, false);
        if (row.event_key == 0 || !is_sane_date(row.due) || row.seq >= meta.next_timer_seq) {
            fail("E-4: event timer has a bad key, date or seq");
        }
        // E-6
        if (!saved_scopes_shape(row.saved, row.saved_count)) {
            fail("E-6: event timer saved scopes");
        }
        for (std::size_t i = 0; i < row.saved_count && i < row.saved.size(); ++i) {
            check_scope("event timer saved", row.saved[i].scope, true);
        }
        seqs.push_back(row.seq);
    }
    std::ranges::sort(seqs);
    if (std::ranges::adjacent_find(seqs) != seqs.end()) {
        fail("E-4: duplicate event timer seq");
    }

    // E-5, E-6, E-12: the queue. The root scope of a row may be dead (the on_death event).
    for (const auto& row : events_.queue) {
        check_scope("event instance", row.scope, true);
        if (!instance_shape(row) || row.instance >= meta.next_instance_id || !exists(row.recipient)) {
            fail("E-5: invalid event instance");
        }
        for (std::size_t i = 0; i < row.saved_count && i < row.saved.size(); ++i) {
            check_scope("event instance saved", row.saved[i].scope, true);
        }
    }

    // The rings.
    for (const auto& row : events_.history) {
        if (!history_shape(row) || row.seq >= meta.next_history_seq) {
            fail("invalid event history entry");
        }
        if (!row.scope.empty()) {
            check_scope("event history", row.scope, true);
        }
    }
    for (const auto& row : events_.journal) {
        if (!journal_shape(row) || row.seq >= meta.next_journal_seq || !exists(row.recipient)) {
            fail("invalid event journal entry");
        }
    }

    // E-7, E-8, E-10: the meta record.
    if (meta.started > 1 || meta.anchors_enabled > 1 || meta.frequency_permille < 1 ||
        meta.frequency_permille > events::kMaxFrequencyPermille) {
        fail("invalid event settings");
    }
    if (meta.player_count > events::kMaxPlayers) {
        fail("E-7: too many player dynasties");
    } else {
        for (std::size_t i = 0; i < meta.players.size(); ++i) {
            const DynastyId player = meta.players[i];
            if (i >= meta.player_count
                    ? player.valid()
                    : (!exists(player) || std::find(meta.players.begin(), meta.players.begin() + i, player) !=
                                              meta.players.begin() + i)) {
                fail("E-7: invalid player dynasty list");
            }
        }
    }
    if (meta.incident_cursor > economy_.meta.next_incident_seq) {
        fail("E-8: incident cursor beyond the incident log");
    }
    if (meta.modal_month < 0 || meta.modal_count > events::kMaxModalCount) {
        fail("E-10: invalid modal counter");
    }

    // E-11: the derived index equals a rebuild.
    if (!(build_scope_rows(events_) == event_scope_rows_)) {
        fail("E-11: the per-scope event row counters differ from the tables");
    }
}

}  // namespace pat::sim
