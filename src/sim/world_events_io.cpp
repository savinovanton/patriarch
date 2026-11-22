#include "pat/core/hash_stream.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "state_sections.hpp"

// E11-T03. Version-1 sections 48..57 of the extension block (docs/dev/event-engine.md section 9.1) and the
// extended hash parts 24..29 (section 9.2). A table that is empty is not written and its hash part is 0
// (ADR-0006 D6/D7), so a world without events saves and hashes exactly as before schema 13.
// The shared emitters define both the little-endian file fields and the FNV byte representation.
// Loading writes raw rows (the public mutators check today's state, whereas a save is a snapshot of
// an earlier day); only the shape of a row is checked here. The references to characters, dynasties
// and provinces, the counters and the derived index are checked by World::check_invariants() once every
// section has been read (E-1..E-12). Rows name events by EventKey only, never by text; the keys that no
// longer exist in the loaded set are cleaned by events::reconcile() (events/reconcile.hpp).

namespace pat::sim {
namespace {

// Smallest size of a row in the file (a row with saved scopes is longer).
constexpr std::size_t kFlagRowBytes = 1 + 4 + 8 + 4;
constexpr std::size_t kVarRowBytes = 1 + 4 + 8 + 8;
constexpr std::size_t kTimerRowBytes = 4 + 4 + 8 + 1 + 4 + 1 + 1;
constexpr std::size_t kCooldownRowBytes = 8 + 1 + 4 + 4;
constexpr std::size_t kChainRowBytes = 8 + 1 + 4 + 1 + 4 + 4;
constexpr std::size_t kModifierRowBytes = 1 + 4 + 8 + 4 + 4;
constexpr std::size_t kInstanceRowBytes = 4 + 8 + 4 + 1 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 1;
constexpr std::size_t kHistoryRowBytes = 4 + 4 + 8 + 1 + 1 + 4 + 1 + 2;
constexpr std::size_t kJournalRowBytes = 4 + 4 + 4 + 1 + 1 + 8 + 4 + 4 + 4 + 1;
constexpr std::size_t kMinMetaBytes = 1 + 8 + 4 * 5 + 2 + 1 + 1 + 4 + 2;

using events::EventKey;
using events::Scope;
using events::ScopeKind;

// ---- writers (file and hash) --------------------------------------------------------------------------

template <class Sink>
void write_scope(Sink& out, Scope scope) {
    out.u8(static_cast<std::uint8_t>(scope.kind));
    out.u32(scope.id);
}

template <class Sink>
void write_saved(Sink& out, const events::SavedScopes& saved, std::uint8_t count) {
    out.u8(count);
    for (std::size_t i = 0; i < count && i < saved.size(); ++i) {
        out.u32(saved[i].name_key);
        write_scope(out, saved[i].scope);
    }
}

template <class Sink>
void write_row(Sink& out, const events::Flag& row) {
    write_scope(out, row.scope);
    out.u64(row.key);
    out.i32(row.expires.days());
}

template <class Sink>
void write_row(Sink& out, const events::Var& row) {
    write_scope(out, row.scope);
    out.u64(row.key);
    out.i64(row.value.raw());
}

template <class Sink>
void write_row(Sink& out, const events::Timer& row) {
    out.u32(row.seq);
    out.i32(row.due.days());
    out.u64(row.event_key);
    write_scope(out, row.scope);
    out.u8(row.depth);
    write_saved(out, row.saved, row.saved_count);
}

template <class Sink>
void write_row(Sink& out, const events::Cooldown& row) {
    out.u64(row.key);
    write_scope(out, row.scope);
    out.i32(row.until.days());
}

template <class Sink>
void write_row(Sink& out, const events::Chain& row) {
    out.u64(row.key);
    write_scope(out, row.scope);
    out.u8(row.step);
    out.i32(row.started.days());
    out.i32(row.updated.days());
}

template <class Sink>
void write_row(Sink& out, const events::Modifier& row) {
    write_scope(out, row.scope);
    out.u64(row.key);
    out.i32(row.added.days());
    out.i32(row.expires.days());
}

template <class Sink>
void write_row(Sink& out, const events::Instance& row) {
    out.u32(row.instance);
    out.u64(row.event_key);
    out.u32(row.recipient.value());
    write_scope(out, row.scope);
    out.i32(row.fired_on.days());
    out.u8(static_cast<std::uint8_t>(row.state));
    out.u8(row.option);
    out.u8(row.avail_mask);
    out.u8(row.urgency);
    out.u8(row.priority);
    out.u8(row.flags);
    out.i32(row.defer_until.days());
    out.u8(row.defer_count);
    out.u8(row.depth);
    write_saved(out, row.saved, row.saved_count);
}

template <class Sink>
void write_row(Sink& out, const events::HistoryEntry& row) {
    out.u32(row.seq);
    out.i32(row.date.days());
    out.u64(row.event_key);
    out.u8(static_cast<std::uint8_t>(row.outcome));
    write_scope(out, row.scope);
    out.u8(row.option);
    out.u16(row.detail);
}

template <class Sink>
void write_row(Sink& out, const events::JournalEntry& row) {
    out.u32(row.seq);
    out.i32(row.date.days());
    out.u32(row.recipient.value());
    out.u8(static_cast<std::uint8_t>(row.kind));
    out.u8(row.urgency);
    out.u64(row.key);
    out.i32(row.a);
    out.i32(row.b);
    out.i32(row.param);
    out.u8(row.flags);
}

template <class Sink, class Rows>
void write_rows(Sink& out, const Rows& rows) {
    out.u32(static_cast<std::uint32_t>(rows.size()));
    for (const auto& row : rows) {
        write_row(out, row);
    }
}

template <class Sink>
void write_meta(Sink& out, const events::Meta& meta) {
    out.u8(meta.started);
    out.u64(meta.events_hash);
    out.u32(meta.next_instance_id);
    out.u32(meta.next_timer_seq);
    out.u32(meta.next_journal_seq);
    out.u32(meta.next_history_seq);
    out.u32(meta.incident_cursor);
    out.u16(meta.frequency_permille);
    out.u8(meta.anchors_enabled);
    out.u8(meta.player_count);
    for (std::size_t i = 0; i < meta.player_count && i < meta.players.size(); ++i) {
        out.u32(meta.players[i].value());
    }
    out.i32(meta.modal_month);
    out.u16(meta.modal_count);
}

template <class Sink>
void write_body(Sink& out, std::uint16_t id, const events::State& state) {
    switch (id) {
        case detail::kSectionEventsMeta:
            write_meta(out, state.meta);
            break;
        case detail::kSectionEventFlags:
            write_rows(out, state.flags);
            break;
        case detail::kSectionEventVars:
            write_rows(out, state.vars);
            break;
        case detail::kSectionEventTimers:
            write_rows(out, state.timers);
            break;
        case detail::kSectionEventCooldowns:
            write_rows(out, state.cooldowns);
            break;
        case detail::kSectionEventChains:
            write_rows(out, state.chains);
            break;
        case detail::kSectionEventModifiers:
            write_rows(out, state.modifiers);
            break;
        case detail::kSectionEventQueue:
            write_rows(out, state.queue);
            break;
        case detail::kSectionEventHistory:
            write_rows(out, state.history);
            break;
        case detail::kSectionEventJournal:
            write_rows(out, state.journal);
            break;
        default:
            break;
    }
}

/// `events_hash` says which set of events the world was made with; it is not part of the state hash.
events::Meta meta_for_hash(const events::State& state) {
    events::Meta meta = state.meta;
    meta.events_hash = 0;
    return meta;
}

// ---- readers ------------------------------------------------------------------------------------------

Scope read_scope(core::ByteReader& in) {
    const auto kind = static_cast<ScopeKind>(in.u8());
    const std::uint32_t id = in.u32();
    return Scope{kind, id};
}

bool read_saved(core::ByteReader& in, events::SavedScopes& saved, std::uint8_t& count) {
    count = in.u8();
    if (!in.ok() || count > saved.size() || !in.can_hold(count, 4 + 1 + 4)) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        saved[i].name_key = in.u32();
        saved[i].scope = read_scope(in);
    }
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Flag& row) {
    row.scope = read_scope(in);
    row.key = in.u64();
    row.expires = Date::from_days(in.i32());
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Var& row) {
    row.scope = read_scope(in);
    row.key = in.u64();
    row.value = core::Fixed64::from_raw(in.i64());
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Timer& row) {
    row.seq = in.u32();
    row.due = Date::from_days(in.i32());
    row.event_key = in.u64();
    row.scope = read_scope(in);
    row.depth = in.u8();
    return in.ok() && read_saved(in, row.saved, row.saved_count);
}

bool read_row(core::ByteReader& in, events::Cooldown& row) {
    row.key = in.u64();
    row.scope = read_scope(in);
    row.until = Date::from_days(in.i32());
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Chain& row) {
    row.key = in.u64();
    row.scope = read_scope(in);
    row.step = in.u8();
    row.started = Date::from_days(in.i32());
    row.updated = Date::from_days(in.i32());
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Modifier& row) {
    row.scope = read_scope(in);
    row.key = in.u64();
    row.added = Date::from_days(in.i32());
    row.expires = Date::from_days(in.i32());
    return in.ok();
}

bool read_row(core::ByteReader& in, events::Instance& row) {
    row.instance = in.u32();
    row.event_key = in.u64();
    row.recipient = DynastyId(in.u32());
    row.scope = read_scope(in);
    row.fired_on = Date::from_days(in.i32());
    row.state = static_cast<events::InstanceState>(in.u8());
    row.option = in.u8();
    row.avail_mask = in.u8();
    row.urgency = in.u8();
    row.priority = in.u8();
    row.flags = in.u8();
    row.defer_until = Date::from_days(in.i32());
    row.defer_count = in.u8();
    row.depth = in.u8();
    return in.ok() && read_saved(in, row.saved, row.saved_count);
}

bool read_row(core::ByteReader& in, events::HistoryEntry& row) {
    row.seq = in.u32();
    row.date = Date::from_days(in.i32());
    row.event_key = in.u64();
    row.outcome = static_cast<events::HistoryOutcome>(in.u8());
    row.scope = read_scope(in);
    row.option = in.u8();
    row.detail = in.u16();
    return in.ok();
}

bool read_row(core::ByteReader& in, events::JournalEntry& row) {
    row.seq = in.u32();
    row.date = Date::from_days(in.i32());
    row.recipient = DynastyId(in.u32());
    row.kind = static_cast<events::JournalKind>(in.u8());
    row.urgency = in.u8();
    row.key = in.u64();
    row.a = in.i32();
    row.b = in.i32();
    row.param = in.i32();
    row.flags = in.u8();
    return in.ok();
}

template <class Rows, class Projection>
bool read_rows(
    core::ByteReader& in, Rows& rows, std::size_t limit, std::size_t row_bytes, Projection projection) {
    return detail::read_ordered_rows(
        in, rows, limit, row_bytes, projection, [](core::ByteReader& reader, auto& row) {
            return read_row(reader, row);
        });
}

using ScopeKey = std::pair<events::Word, EventKey>;

template <class Row>
ScopeKey scope_key(const Row& row) {
    return {row.scope.word(), row.key};
}

bool read_meta(core::ByteReader& in, events::Meta& meta) {
    meta.started = in.u8();
    meta.events_hash = in.u64();
    meta.next_instance_id = in.u32();
    meta.next_timer_seq = in.u32();
    meta.next_journal_seq = in.u32();
    meta.next_history_seq = in.u32();
    meta.incident_cursor = in.u32();
    meta.frequency_permille = in.u16();
    meta.anchors_enabled = in.u8();
    meta.player_count = in.u8();
    if (!in.ok() || meta.player_count > events::kMaxPlayers) {
        return false;
    }
    for (std::size_t i = 0; i < meta.player_count; ++i) {
        meta.players[i] = DynastyId(in.u32());
    }
    meta.modal_month = in.i32();
    meta.modal_count = in.u16();
    return in.ok();
}

}  // namespace

bool World::events_section_present(std::uint16_t id) const {
    switch (id) {
        case detail::kSectionEventsMeta:
            return !(events_.meta == events::Meta{});
        case detail::kSectionEventFlags:
            return !events_.flags.empty();
        case detail::kSectionEventVars:
            return !events_.vars.empty();
        case detail::kSectionEventTimers:
            return !events_.timers.empty();
        case detail::kSectionEventCooldowns:
            return !events_.cooldowns.empty();
        case detail::kSectionEventChains:
            return !events_.chains.empty();
        case detail::kSectionEventModifiers:
            return !events_.modifiers.empty();
        case detail::kSectionEventQueue:
            return !events_.queue.empty();
        case detail::kSectionEventHistory:
            return !events_.history.empty();
        case detail::kSectionEventJournal:
            return !events_.journal.empty();
        default:
            return false;
    }
}

void World::events_write_sections(std::vector<StateSection>& sections) const {
    for (std::uint16_t id = detail::kSectionEventsMeta; id <= detail::kSectionEventJournal; ++id) {
        if (events_section_present(id)) {
            StateSection section{id, 1, {}};
            write_body(section.body, id, events_);
            sections.push_back(std::move(section));
        }
    }
}

const char* World::events_read_section(std::uint16_t id, core::ByteReader& in) {
    switch (id) {
        case detail::kSectionEventsMeta: {
            events::Meta meta;
            if (!read_meta(in, meta)) {
                return "invalid event meta section";
            }
            events_.meta = meta;
            break;
        }
        case detail::kSectionEventFlags: {
            std::vector<events::Flag> rows;
            if (!read_rows(in, rows, events::kMaxFlags, kFlagRowBytes, [](const events::Flag& row) {
                    return scope_key(row);
                })) {
                return "invalid event flags section";
            }
            events_.flags = std::move(rows);
            break;
        }
        case detail::kSectionEventVars: {
            std::vector<events::Var> rows;
            if (!read_rows(in, rows, events::kMaxVars, kVarRowBytes, [](const events::Var& row) {
                    return scope_key(row);
                })) {
                return "invalid event variables section";
            }
            events_.vars = std::move(rows);
            break;
        }
        case detail::kSectionEventTimers: {
            std::vector<events::Timer> rows;
            if (!read_rows(in, rows, events::kMaxTimers, kTimerRowBytes, [](const events::Timer& row) {
                    return std::pair{row.due.days(), row.seq};
                })) {
                return "invalid event timers section";
            }
            events_.timers = std::move(rows);
            break;
        }
        case detail::kSectionEventCooldowns: {
            std::vector<events::Cooldown> rows;
            if (!read_rows(
                    in, rows, events::kMaxCooldowns, kCooldownRowBytes, [](const events::Cooldown& row) {
                        return scope_key(row);
                    })) {
                return "invalid event cooldowns section";
            }
            events_.cooldowns = std::move(rows);
            break;
        }
        case detail::kSectionEventChains: {
            std::vector<events::Chain> rows;
            if (!read_rows(in, rows, events::kMaxChains, kChainRowBytes, [](const events::Chain& row) {
                    return scope_key(row);
                })) {
                return "invalid event chains section";
            }
            events_.chains = std::move(rows);
            break;
        }
        case detail::kSectionEventModifiers: {
            std::vector<events::Modifier> rows;
            if (!read_rows(
                    in, rows, events::kMaxModifiers, kModifierRowBytes, [](const events::Modifier& row) {
                        return scope_key(row);
                    })) {
                return "invalid event modifiers section";
            }
            events_.modifiers = std::move(rows);
            break;
        }
        case detail::kSectionEventQueue: {
            std::vector<events::Instance> rows;
            if (!read_rows(in, rows, events::kMaxQueue, kInstanceRowBytes, &events::Instance::instance)) {
                return "invalid event queue section";
            }
            events_.queue = std::move(rows);
            break;
        }
        case detail::kSectionEventHistory: {
            std::vector<events::HistoryEntry> rows;
            if (!read_rows(
                    in, rows, events::kHistoryRingSize, kHistoryRowBytes, &events::HistoryEntry::seq)) {
                return "invalid event history section";
            }
            events_.history = std::move(rows);
            break;
        }
        case detail::kSectionEventJournal: {
            std::vector<events::JournalEntry> rows;
            if (!read_rows(
                    in, rows, events::kJournalRingSize, kJournalRowBytes, &events::JournalEntry::seq)) {
                return "invalid event journal section";
            }
            events_.journal = std::move(rows);
            break;
        }
        default:
            return "unknown event section";
    }
    return in.ok() && in.at_end() ? nullptr : "an event section is truncated or has extra bytes";
}

std::array<std::uint64_t, 6> World::events_hash_parts() const {
    // Part 24..28: the sections they group (event-engine.md section 9.2); 29: the meta record without
    // `events_hash`. A group without a row is 0 and stays out of `total`.
    struct Group {
        std::uint16_t first;
        std::uint16_t second;  // 0: the group has one section
    };
    constexpr std::array<Group, 5> groups = {{{detail::kSectionEventFlags, detail::kSectionEventVars},
                                              {detail::kSectionEventTimers, detail::kSectionEventCooldowns},
                                              {detail::kSectionEventChains, detail::kSectionEventModifiers},
                                              {detail::kSectionEventQueue, 0},
                                              {detail::kSectionEventHistory, detail::kSectionEventJournal}}};
    std::array<std::uint64_t, 6> parts{};
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const Group group = groups[i];
        if (events_section_present(group.first) ||
            (group.second != 0 && events_section_present(group.second))) {
            core::HashStream hash;
            write_body(hash, group.first, events_);
            if (group.second != 0) {
                write_body(hash, group.second, events_);
            }
            parts[i] = detail::nonzero(hash.value());
        }
    }
    const events::Meta meta = meta_for_hash(events_);
    if (!(meta == events::Meta{})) {
        core::HashStream hash;
        write_meta(hash, meta);
        parts[5] = detail::nonzero(hash.value());
    }
    return parts;
}

}  // namespace pat::sim
