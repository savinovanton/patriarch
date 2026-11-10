#include "pat/core/hash_stream.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace pat::sim {
namespace {

using core::Fixed32;
constexpr auto kDecayPerMonth = Fixed32::from_int(1);

void pack_sources(OpinionEntry& entry) {
    std::ranges::sort(entry.sources, {}, [](const OpinionSource& source) {
        return std::pair{source.value == Fixed32{}, source.reason};
    });
}

void add_source(OpinionEntry& entry, OpinionReason reason, Fixed32 value) {
    auto& sources = entry.sources;
    auto slot = std::ranges::find_if(sources, [reason](const OpinionSource& source) {
        return source.value != Fixed32{} && source.reason == reason;
    });
    if (slot != sources.end()) {
        slot->value += value;
        return;
    }
    slot = std::ranges::find(sources, Fixed32{}, &OpinionSource::value);
    if (slot != sources.end()) {
        *slot = {reason, value};
    } else {
        // Reason zero sorts first. If it is absent, fold the first named source into it.
        sources.front().reason = kOtherOpinionReason;
        sources.front().value += value;
    }
    pack_sources(entry);
}

void change_sources(OpinionEntry& entry, Fixed32 delta, OpinionReason reason) {
    for (const bool matching : {true, false}) {
        for (auto& source : entry.sources) {
            if (delta == Fixed32{} || source.value == Fixed32{} || (source.reason == reason) != matching ||
                (source.value.raw() < 0) == (delta.raw() < 0)) {
                continue;
            }
            const auto magnitude = std::min(std::abs(source.value.raw()), std::abs(delta.raw()));
            const auto used = Fixed32::from_raw(delta.raw() < 0 ? -magnitude : magnitude);
            source.value += used;
            delta -= used;
            if (source.value == Fixed32{}) {
                source = {};
            }
        }
    }
    pack_sources(entry);
    if (delta != Fixed32{}) {
        add_source(entry, reason, delta);
    }
}

void decay_entry(OpinionEntry& entry) {
    const auto total = entry.total().raw();
    const auto magnitude = std::abs(total);
    const auto next = std::max(0, magnitude - kDecayPerMonth.raw());
    std::int32_t prefix = 0;
    std::int32_t assigned = 0;
    for (auto& source : entry.sources) {
        prefix += std::abs(source.value.raw());
        // Scale cumulative magnitudes to distribute rounding while keeping the exact new total.
        const auto scaled = static_cast<std::int32_t>(static_cast<std::int64_t>(prefix) * next / magnitude);
        const auto part = scaled - assigned;
        if (part == 0) {
            source = {};
        } else {
            source.value = Fixed32::from_raw(total < 0 ? -part : part);
        }
        assigned = scaled;
    }
    pack_sources(entry);
}

}  // namespace

Fixed32 OpinionEntry::total() const noexcept {
    Fixed32 sum;
    for (const auto& source : sources) {
        sum += source.value;
    }
    return sum;
}

bool World::add_opinion(CharacterId from, CharacterId to, Fixed32 delta, OpinionReason reason) {
    if (!dynasty_mode() || from == to || !is_alive(from) || !is_alive(to)) {
        return false;
    }
    if (delta == Fixed32{}) {
        return true;
    }

    auto row = std::ranges::lower_bound(opinions_, from, {}, &OpinionRow::holder);
    if (row == opinions_.end() || row->holder != from) {
        row = opinions_.insert(row, OpinionRow{from, {}});
    }
    auto& entries = row->entries;
    auto position = std::ranges::lower_bound(entries, to, {}, &OpinionEntry::target);
    if (position != entries.end() && position->target == to) {
        const Fixed32 old_value = position->total();
        const auto next_raw =
            std::clamp<std::int64_t>(static_cast<std::int64_t>(old_value.raw()) + delta.raw(),
                                     -kOpinionLimit.raw(),
                                     kOpinionLimit.raw());
        const auto next = Fixed32::from_raw(static_cast<std::int32_t>(next_raw));
        if (next == Fixed32{}) {
            entries.erase(position);
            if (entries.empty()) {
                opinions_.erase(row);
            }
        } else if (next != old_value) {
            change_sources(*position, next - old_value, reason);
        }
        return true;
    }

    const Fixed32 value = std::clamp(delta, -kOpinionLimit, kOpinionLimit);
    if (entries.size() == kMaxOpinions) {
        std::erase_if(entries, [&](const OpinionEntry& entry) { return !is_alive(entry.target); });
        position = std::ranges::lower_bound(entries, to, {}, &OpinionEntry::target);
    }
    if (entries.size() == kMaxOpinions) {
        const auto key = [](const OpinionEntry& entry) {
            return std::tuple{std::abs(entry.total().raw()), entry.target};
        };
        const auto victim = std::ranges::min_element(entries, {}, key);
        if (std::tuple{std::abs(value.raw()), to} < key(*victim)) {
            return true;
        }
        entries.erase(victim);
        position = std::ranges::lower_bound(entries, to, {}, &OpinionEntry::target);
    }
    OpinionEntry entry{to, {}};
    entry.sources.front() = {reason, value};
    entries.insert(position, entry);
    return true;
}

std::span<const OpinionEntry> World::opinions_from(CharacterId holder) const {
    const auto row = std::ranges::lower_bound(opinions_, holder, {}, &OpinionRow::holder);
    if (row == opinions_.end() || row->holder != holder) {
        return {};
    }
    return row->entries;
}

std::span<const OpinionSource> World::opinion_sources(CharacterId from, CharacterId to) const {
    if (!is_alive(to)) {
        return {};
    }
    const auto entries = opinions_from(from);
    const auto entry = std::ranges::lower_bound(entries, to, {}, &OpinionEntry::target);
    if (entry == entries.end() || entry->target != to) {
        return {};
    }
    const auto end = std::ranges::find(entry->sources, Fixed32{}, &OpinionSource::value);
    return {entry->sources.begin(), end};
}

Fixed32 World::get_opinion(CharacterId from, CharacterId to) const {
    Fixed32 result;
    for (const auto& source : opinion_sources(from, to)) {
        result += source.value;
    }
    return result;
}

std::size_t World::decay_opinions(Date date) {
    std::size_t visited = 0;
    for (auto& [holder, entries] : opinions_) {
        if (!is_monthly_slot(holder.value(), date)) {
            continue;
        }
        visited += entries.size();
        for (auto& entry : entries) {
            if (is_alive(entry.target)) {
                decay_entry(entry);
            } else {
                entry.sources = {};
            }
        }
        std::erase_if(entries,
                      [](const OpinionEntry& entry) { return entry.sources.front().value == Fixed32{}; });
    }
    if (visited != 0) {
        std::erase_if(opinions_, [](const OpinionRow& row) { return row.entries.empty(); });
    }
    return visited;
}

void World::erase_opinion_holder(CharacterId holder) {
    const auto row = std::ranges::lower_bound(opinions_, holder, {}, &OpinionRow::holder);
    if (row != opinions_.end() && row->holder == holder) {
        opinions_.erase(row);
    }
}

std::uint64_t World::opinion_hash() const {
    core::HashStream h;
    h.u64(opinions_.size());
    for (const auto& [holder, entries] : opinions_) {
        h.u32(holder.value());
        h.u64(entries.size());
        for (const auto& entry : entries) {
            h.u32(entry.target.value());
            for (const auto& source : entry.sources) {
                h.u16(source.reason);
                h.fixed(source.value);
            }
        }
    }
    return h.value();
}

void World::check_opinion_invariants(std::vector<std::string>& errors) const {
    CharacterId previous_holder;
    for (const auto& [holder, entries] : opinions_) {
        if (!dynasty_mode() || !is_alive(holder) || (previous_holder.valid() && holder <= previous_holder) ||
            entries.empty() || entries.size() > kMaxOpinions) {
            errors.emplace_back("invalid opinion holder");
            return;
        }
        previous_holder = holder;
        CharacterId previous_target;
        for (const auto& entry : entries) {
            if (!exists(entry.target) || entry.target == holder ||
                (previous_target.valid() && entry.target <= previous_target)) {
                errors.emplace_back("invalid opinion target");
                return;
            }
            previous_target = entry.target;
            std::int64_t sum = 0;
            OpinionReason previous_reason = 0;
            bool empty_slot = false;
            for (const auto& source : entry.sources) {
                const auto raw = source.value.raw();
                if (raw == 0) {
                    if (source.reason != kOtherOpinionReason) {
                        errors.emplace_back("invalid empty opinion source");
                        return;
                    }
                    empty_slot = true;
                    continue;
                }
                if (empty_slot ||
                    (sum != 0 && (source.reason <= previous_reason || (sum < 0) != (raw < 0)))) {
                    errors.emplace_back("invalid opinion source order or sign");
                    return;
                }
                sum += raw;
                previous_reason = source.reason;
            }
            if (sum == 0 || sum < -kOpinionLimit.raw() || sum > kOpinionLimit.raw()) {
                errors.emplace_back("invalid opinion total");
                return;
            }
        }
    }
}

}  // namespace pat::sim
