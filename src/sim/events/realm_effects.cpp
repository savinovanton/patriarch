#include "pat/sim/events/realm_effects.hpp"

#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/realm_triggers.hpp"

#include <algorithm>

namespace pat::sim::events {

namespace {

/// A payment of an event in kopecks: the roubles of the argument, at least one kopeck and at most one
/// posting; nullopt for a non-positive or a too large sum.
[[nodiscard]] std::optional<economy::Kopecks> payment_of(Word w) {
    const economy::Kopecks kop = economy::roubles_to_kopecks(fixed_from_word(w));
    if (kop <= 0 || kop > economy::kMaxPostingKop) {
        return std::nullopt;
    }
    return kop;
}

EffectStatus
assign_administrator(ProvinceId province, std::span<const Word> args, std::vector<Command>& out) {
    // `dismiss = yes` removes the administrator; otherwise a person must be named (a path that led nowhere
    // does not dismiss anybody by accident).
    const std::optional<Scope> who = effect_scope_arg(args, 0, ScopeKind::Character);
    const bool dismiss = effect_arg(args, 1) != 0;
    if (!who || (dismiss != who->empty())) {
        return EffectStatus::BadArgument;
    }
    const CharacterId administrator = dismiss ? CharacterId() : CharacterId{who->id};
    out.push_back(Command{DynastyId(), cmd::AssignProvinceAdministrator{province, administrator}});
    return EffectStatus::Issued;
}

}  // namespace

std::optional<EffectStatus> realm_effect_commands(const EffectEnv& env,
                                                  std::uint16_t code,
                                                  Scope target,
                                                  std::span<const Word> args,
                                                  std::vector<Command>& out) {
    const auto issue = [&](CommandPayload payload) {
        out.push_back(Command{DynastyId(), std::move(payload)});
        return EffectStatus::Issued;
    };
    switch (code) {
        case kEffPay:
        case kEffReceive: {
            const DynastyId house = dynasty_of_scope(*env.triggers.world, target);
            if (target.kind != ScopeKind::Dynasty || !house.valid()) {
                return EffectStatus::BadScope;
            }
            const std::optional<economy::Kopecks> kop = payment_of(effect_arg(args, 0));
            if (!kop) {
                return EffectStatus::BadArgument;
            }
            return issue(cmd::PostEventMoney{house,
                                             *kop,
                                             code == kEffPay ? economy::LedgerCode::EventExpense
                                                             : economy::LedgerCode::EventIncome});
        }
        case kEffSetProvinceOwner:
        case kEffAssignAdministrator:
        case kEffChangeUnrest:
        case kEffChangeControl:
        case kEffChangeGroupMood:
        case kEffStartConstruction:
        case kEffConscript: {
            // An unknown province is not refused here: the command says so in the CommandLog.
            if (target.kind != ScopeKind::Province) {
                return EffectStatus::BadScope;
            }
            const ProvinceId province{target.id};
            switch (code) {
                case kEffSetProvinceOwner: {
                    const std::optional<Scope> owner = effect_scope_arg(args, 0, ScopeKind::Dynasty);
                    if (!owner || owner->empty()) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::SetProvinceOwner{province, DynastyId{owner->id}});
                }
                case kEffAssignAdministrator:
                    return assign_administrator(province, args, out);
                case kEffChangeUnrest:
                    return issue(cmd::AdjustProvinceUnrest{province, effect_delta(effect_arg(args, 0))});
                case kEffChangeControl:
                    return issue(cmd::AdjustProvinceControl{province, effect_delta(effect_arg(args, 0))});
                case kEffChangeGroupMood: {
                    const std::int32_t group = int_from_word(effect_arg(args, 0));
                    if (group < 0 || group >= static_cast<std::int32_t>(economy::kGroupCount)) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::AdjustGroupMood{
                        province, static_cast<std::uint8_t>(group), effect_delta(effect_arg(args, 1))});
                }
                case kEffStartConstruction: {
                    const std::uint8_t building =
                        building_slot_of(env.triggers, int_from_word(effect_arg(args, 0)));
                    if (building == 0) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::StartConstruction{province, building});
                }
                default: {  // kEffConscript
                    const std::int32_t count = int_from_word(effect_arg(args, 0));
                    if (count < 1) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::ConscriptRecruits{province, static_cast<std::uint32_t>(count)});
                }
            }
        }
        default:
            return std::nullopt;
    }
}

economy::Kopecks payment_shortfall(const World& world, DynastyId house, std::span<const Command> commands) {
    // The balance the payments see in turn; the least sum that keeps it from going below zero before any
    // payment is what the house lacks. Amounts of at most kMaxPostingKop and a run of at most
    // kMaxCommandsPerRun commands keep the sums far inside int64.
    economy::Kopecks balance = world.treasury(economy::AccountRef::of(house));
    economy::Kopecks lacking = 0;
    for (const Command& c : commands) {
        const auto* money = std::get_if<cmd::PostEventMoney>(&c.payload);
        if (money == nullptr || money->dynasty != house || money->amount_kop <= 0 ||
            money->amount_kop > economy::kMaxPostingKop) {
            continue;
        }
        if (money->ledger == economy::LedgerCode::EventIncome) {
            balance += money->amount_kop;
        } else if (money->ledger == economy::LedgerCode::EventExpense) {
            balance -= money->amount_kop;
            lacking = std::max(lacking, -balance);
        }
    }
    return lacking;
}

}  // namespace pat::sim::events
