#include "pat/sim/dynasty_standing.hpp"

#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim {
namespace {

using core::Fixed32;
constexpr auto kUnityDrift = Fixed32::from_int(1);

Fixed32 add_scale(Fixed32 current, Fixed32 delta) {
    const auto value = static_cast<std::int64_t>(current.raw()) + delta.raw();
    return Fixed32::from_raw(static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, kScaleMax.raw())));
}

class DynastyStandingSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return kDynastyStandingSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        if (!world.dynasty_mode()) {
            return;
        }
        const auto& dynasties = world.dynasties();
        for (std::uint32_t id = 0; id < dynasties.size(); ++id) {
            if (is_monthly_slot(id, context.date)) {
                context.budget.spend(1);
                const auto delta = std::clamp(kDefaultUnity - dynasties.unity[id], -kUnityDrift, kUnityDrift);
                if (delta != Fixed32{}) {
                    world.add_unity(DynastyId(id), delta, kStandingReasonMonthlyDrift);
                }
            }
        }
    }
};

}  // namespace

std::unique_ptr<System> make_dynasty_standing_system() {
    return std::make_unique<DynastyStandingSystem>();
}

void World::add_unity(DynastyId dynasty, Fixed32 delta, StandingReason reason) {
    if (exists(dynasty)) {
        set_dynasty_unity(dynasty, add_scale(dynasties_.unity[dynasty.index()], delta), reason);
    }
}

void World::add_prestige(DynastyId dynasty, Fixed32 delta, StandingReason reason) {
    if (!dynasty_mode() || !exists(dynasty)) {
        return;
    }
    auto& prestige = dynasties_.prestige[dynasty.index()];
    const auto previous = prestige;
    prestige = add_scale(previous, delta);
    append_standing_change(dynasty, StandingMetric::Prestige, reason, prestige - previous);
}

std::span<const StandingChange> World::standing_changes(DynastyId dynasty) const {
    const auto row = std::ranges::lower_bound(standing_history_, dynasty, {}, &StandingHistory::dynasty);
    if (row == standing_history_.end() || row->dynasty != dynasty) {
        return {};
    }
    return row->changes;
}

void World::append_standing_change(DynastyId dynasty,
                                   StandingMetric metric,
                                   StandingReason reason,
                                   Fixed32 delta) {
    if (delta == Fixed32{} || !is_sane_date(today())) {
        return;
    }
    auto row = std::ranges::lower_bound(standing_history_, dynasty, {}, &StandingHistory::dynasty);
    if (row == standing_history_.end() || row->dynasty != dynasty) {
        row = standing_history_.insert(row, StandingHistory{dynasty, {}});
    }
    auto& changes = row->changes;
    if (changes.size() == kMaxStandingChanges) {
        changes.erase(changes.begin());
    }
    changes.push_back({today(), metric, reason, delta});
}

std::uint64_t World::standing_hash() const {
    core::HashStream hash;
    const auto prestige_count =
        std::ranges::count_if(dynasties_.prestige, [](Fixed32 value) { return value != kDefaultPrestige; });
    if (prestige_count == 0 && standing_history_.empty()) {
        return 0;
    }
    hash.u64(static_cast<std::uint64_t>(prestige_count));
    for (std::size_t id = 0; id < dynasties_.size(); ++id) {
        const auto prestige = dynasties_.prestige[id];
        if (prestige != kDefaultPrestige) {
            hash.u32(static_cast<std::uint32_t>(id));
            hash.fixed(prestige);
        }
    }
    hash.u64(standing_history_.size());
    for (const auto& [dynasty, changes] : standing_history_) {
        hash.u32(dynasty.value());
        hash.u64(changes.size());
        for (const auto& change : changes) {
            hash.i32(change.date.days());
            hash.u8(static_cast<std::uint8_t>(change.metric));
            hash.u16(change.reason);
            hash.fixed(change.delta);
        }
    }
    const auto value = hash.value();
    return value == 0 ? 1 : value;
}

void World::check_standing_invariants(std::vector<std::string>& errors) const {
    DynastyId previous;
    for (const auto& [dynasty, changes] : standing_history_) {
        if (!dynasty_mode() || !exists(dynasty) || (previous.valid() && dynasty <= previous) ||
            changes.empty() || changes.size() > kMaxStandingChanges ||
            !std::ranges::is_sorted(changes, {}, &StandingChange::date)) {
            errors.emplace_back("invalid dynasty standing history");
            return;
        }
        previous = dynasty;
        for (const auto& change : changes) {
            const auto bound =
                change.metric == StandingMetric::Honor ? Fixed32::from_int(kMaxHonor - kMinHonor) : kScaleMax;
            if (!is_sane_date(change.date) || change.date > today() ||
                change.metric < StandingMetric::Honor || change.metric > StandingMetric::Prestige ||
                change.delta == Fixed32{} || change.delta < -bound || change.delta > bound) {
                errors.emplace_back("invalid dynasty standing change");
                return;
            }
        }
    }
}

}  // namespace pat::sim
