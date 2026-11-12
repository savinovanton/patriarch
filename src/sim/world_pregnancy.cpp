#include "pat/core/hash_stream.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

// Pregnancies (E7-T12, ADR-0006 D4/D6 "Implementation S4"). The table `pregnancies_` is the state; the
// labour queue is an index over it that is rebuilt on load (World::rebuild_derived_indexes).

namespace pat::sim {

std::optional<Pregnancy> World::pregnancy_of(CharacterId mother) const {
    const auto row = std::ranges::lower_bound(pregnancies_, mother, {}, &Pregnancy::mother);
    if (row == pregnancies_.end() || row->mother != mother) {
        return std::nullopt;
    }
    return *row;
}

bool World::is_pregnant(CharacterId mother) const noexcept {
    const auto row = std::ranges::lower_bound(pregnancies_, mother, {}, &Pregnancy::mother);
    return row != pregnancies_.end() && row->mother == mother;
}

bool World::begin_pregnancy(const Pregnancy& pregnancy) {
    if (!dynasty_mode() || !is_alive(pregnancy.mother) || !is_alive(pregnancy.father) ||
        pregnancy.mother == pregnancy.father || is_pregnant(pregnancy.mother)) {
        return false;
    }
    if (characters_.sex[pregnancy.mother.index()] != Sex::Female ||
        characters_.sex[pregnancy.father.index()] != Sex::Male) {
        return false;
    }
    if (static_cast<std::uint8_t>(pregnancy.end) >= kPregnancyEndCount ||
        static_cast<std::uint8_t>(pregnancy.legitimacy) >= kLegitimacyCount) {
        return false;
    }
    if (!is_sane_date(pregnancy.conceived) || !is_sane_date(pregnancy.due) || !is_sane_date(today()) ||
        pregnancy.conceived > today() || pregnancy.due <= pregnancy.conceived ||
        pregnancy.due - pregnancy.conceived > kMaxPregnancyDays) {
        return false;
    }
    const auto row = std::ranges::lower_bound(pregnancies_, pregnancy.mother, {}, &Pregnancy::mother);
    pregnancies_.insert(row, pregnancy);
    labour_queue_.push(pregnancy.due, pregnancy.mother);
    return true;
}

std::optional<Pregnancy> World::end_pregnancy(CharacterId mother) {
    const auto row = std::ranges::lower_bound(pregnancies_, mother, {}, &Pregnancy::mother);
    if (row == pregnancies_.end() || row->mother != mother) {
        return std::nullopt;
    }
    const Pregnancy ended = *row;
    pregnancies_.erase(row);
    // The queue entry is left behind: it is recognised as stale when it comes due.
    return ended;
}

std::vector<DueLabour> World::take_due_labours(Date today) {
    std::vector<DueLabour> due_labours;
    labour_queue_.pop_due(today, [&](Date due, CharacterId mother) {
        const auto row = std::ranges::lower_bound(pregnancies_, mother, {}, &Pregnancy::mother);
        if (row != pregnancies_.end() && row->mother == mother && row->due == due) {
            due_labours.push_back({due, mother});
        }
    });
    std::sort(due_labours.begin(), due_labours.end(), [](const DueLabour& a, const DueLabour& b) {
        return std::tie(a.due, a.mother) < std::tie(b.due, b.mother);
    });
    return due_labours;
}

std::uint64_t World::pregnancy_hash() const {
    core::HashStream h;
    h.u64(pregnancies_.size());
    for (const Pregnancy& p : pregnancies_) {
        h.u32(p.mother.value());
        h.u32(p.father.value());
        h.i32(p.conceived.days());
        h.i32(p.due.days());
        h.u8(static_cast<std::uint8_t>(p.end));
        h.u8(static_cast<std::uint8_t>(p.legitimacy));
    }
    return h.value();
}

void World::check_pregnancy_invariants(std::vector<std::string>& bad) const {
    if (pregnancies_.empty()) {
        return;
    }
    if (!dynasty_mode()) {
        bad.emplace_back("pregnancies need dynasty mode");
        return;
    }
    CharacterId previous;
    for (const Pregnancy& p : pregnancies_) {
        if (previous.valid() && p.mother <= previous) {
            bad.emplace_back("pregnancies are not in ascending order of the mother");
            return;
        }
        previous = p.mother;
        if (!is_alive(p.mother) || characters_.sex[p.mother.index()] != Sex::Female) {
            bad.push_back(std::format("pregnancy of {}: the mother is not a living woman", p.mother.value()));
            continue;
        }
        if (!exists(p.father) || p.father == p.mother || characters_.sex[p.father.index()] != Sex::Male) {
            bad.push_back(std::format("pregnancy of {}: the father is not a man", p.mother.value()));
        }
        if (static_cast<std::uint8_t>(p.end) >= kPregnancyEndCount ||
            static_cast<std::uint8_t>(p.legitimacy) >= kLegitimacyCount) {
            bad.push_back(std::format("pregnancy of {}: unknown outcome or legitimacy", p.mother.value()));
        }
        if (!is_sane_date(p.conceived) || !is_sane_date(p.due) || p.conceived > today() ||
            p.due <= p.conceived || p.due - p.conceived > kMaxPregnancyDays) {
            bad.push_back(std::format("pregnancy of {}: invalid dates", p.mother.value()));
        }
    }
}

}  // namespace pat::sim
