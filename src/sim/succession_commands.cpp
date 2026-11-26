#include "pat/sim/succession_commands.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_negotiations.hpp"

#include <algorithm>
#include <type_traits>

namespace pat::sim {
namespace {

using Denial = SuccessionDenial;

bool disowned(const World& world, CharacterId id) {
    const auto* status = world.family_status(id);
    return status != nullptr && (status->flags & family::kPersonDisowned) != 0;
}

family::SuccessionState state_of(const World& world, DynastyId actor) {
    const auto* row = world.family_succession(actor);
    return row != nullptr ? *row : family::SuccessionState{actor, {}, {}, kNoDate};
}

void normalize(SuccessionDenials& reasons) {
    std::ranges::sort(reasons);
    const auto duplicates = std::ranges::unique(reasons);
    reasons.erase(duplicates.begin(), duplicates.end());
}

void check_heir(const World& world,
                const SuccessionRules& rules,
                DynastyId actor,
                CharacterId heir,
                SuccessionDenials& reasons) {
    if (!heir.valid()) {
        reasons.push_back(Denial::HeirRequired);
        return;
    }
    if (!world.is_alive(heir)) {
        reasons.push_back(Denial::HeirNotAlive);
        return;
    }
    const auto head = world.dynasties().head[actor.index()];
    if (world.characters().dynasty[heir.index()] != actor)
        reasons.push_back(Denial::HeirNotMember);
    if (heir == head)
        reasons.push_back(Denial::HeirIsHead);
    // Reuse the full resolver, including the female-policy appointment exception.
    const auto resolution = resolve_successor(world, rules, actor, head, SuccessionLaw::Appointment, heir);
    if (heir != head && world.characters().dynasty[heir.index()] == actor &&
        std::ranges::find(resolution.order, heir, &SuccessionCandidate::character) ==
            resolution.order.end()) {
        reasons.push_back(Denial::HeirIneligible);
    }
}

SuccessionDenials check_request(const World& world,
                                const SuccessionRules& rules,
                                DynastyId actor,
                                const SuccessionCommand& command) {
    SuccessionDenials reasons;
    if (!world.dynasty_mode())
        reasons.push_back(Denial::DynastyModeRequired);
    if (!world.exists(actor)) {
        reasons.push_back(Denial::NotYourHouse);
        normalize(reasons);
        return reasons;
    }
    if (!family_acting_head(world, actor).valid())
        reasons.push_back(Denial::NoActingHead);
    if (world.dynasty_extinct(actor))
        reasons.push_back(Denial::DynastyExtinct);
    if (world.family_regency(actor) != nullptr)
        reasons.push_back(Denial::RegencyActive);
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, cmd::SetHeir>) {
                if (world.dynasties().succession_law[actor.index()] != SuccessionLaw::Appointment)
                    reasons.push_back(Denial::LawNotAppointment);
                check_heir(world, rules, actor, payload.heir, reasons);
                if (payload.guardian.valid()) {
                    const auto id = payload.guardian;
                    if (!world.is_alive(id) || world.characters().dynasty[id.index()] != actor ||
                        age_years(world.characters().birth_date[id.index()], world.today()) <
                            rules.adult_age ||
                        id == payload.heir || world.is_tonsured(id) || disowned(world, id))
                        reasons.push_back(Denial::InvalidGuardian);
                }
            } else if constexpr (std::is_same_v<T, cmd::ChangeSuccessionLaw>) {
                if (world.family_dispute(actor) != nullptr)
                    reasons.push_back(Denial::DisputeOpen);
                if (world.dynasties().succession_law[actor.index()] == payload.law)
                    reasons.push_back(Denial::SameLaw);
                const auto law = static_cast<std::uint8_t>(payload.law);
                if (law >= rules.reform_selectable.size() || !rules.reform_selectable[law])
                    reasons.push_back(Denial::LawNotSelectable);
                const auto row = state_of(world, actor);
                if (row.law_changed_on != kNoDate &&
                    static_cast<std::int64_t>(world.today().days()) - row.law_changed_on.days() <
                        rules.reform_cooldown_days)
                    reasons.push_back(Denial::ReformCooldown);
                if (world.economy().meta.present != 0 &&
                    !economy::can_afford(world, economy::AccountRef::of(actor), rules.reform_money_kop))
                    reasons.push_back(Denial::InsufficientFunds);
                if (world.economy().meta.present != 0 &&
                    world.preview_money_posting(economy::AccountRef::of(actor),
                                                economy::LedgerCode::CourtUpkeep,
                                                -rules.reform_money_kop) != -rules.reform_money_kop)
                    reasons.push_back(Denial::MoneyPostingBlocked);
                if (payload.law == SuccessionLaw::Appointment)
                    check_heir(world, rules, actor, payload.heir, reasons);
            } else {
                const auto target = payload.character;
                if (!world.is_alive(target)) {
                    reasons.push_back(Denial::NotAlive);
                    return;
                }
                if (world.characters().dynasty[target.index()] != actor)
                    reasons.push_back(Denial::NotYourHouse);
                if (std::ranges::find(world.dynasties().head, target) != world.dynasties().head.end())
                    reasons.push_back(Denial::CannotDisownHead);
                if (disowned(world, target))
                    reasons.push_back(Denial::AlreadyDisowned);
                if (world.is_tonsured(target))
                    reasons.push_back(Denial::AlreadyTonsured);
                for (const auto& row : world.family().regencies)
                    if (row.regent == target || row.ward == target)
                        reasons.push_back(Denial::ActiveRole);
                for (const auto& row : world.family().disputes)
                    for (std::size_t i = 0; i < row.claimants_n; ++i)
                        if (row.claimants[i].character == target)
                            reasons.push_back(Denial::DisputeOpen);
            }
        },
        command);
    normalize(reasons);
    return reasons;
}

std::vector<CharacterId> ranked(const SuccessionResult& result) {
    std::vector<CharacterId> ids;
    for (const auto& row : result.order)
        ids.push_back(row.character);
    const auto winner = std::ranges::find(ids, result.winner);
    if (winner != ids.end())
        std::rotate(ids.begin(), winner, winner + 1);
    return ids;
}

CharacterId skipped_son(const World& world, const SuccessionRules& rules, DynastyId actor, CharacterId heir) {
    const auto head = world.dynasties().head[actor.index()];
    if (!head.valid())
        return {};
    const auto order = resolve_successor(world, rules, actor, head, SuccessionLaw::Majorat, {});
    for (const auto& row : order.order) {
        const auto i = row.character.index();
        if (!row.minor && world.characters().sex[i] == Sex::Male &&
            (world.characters().father[i] == head || world.characters().mother[i] == head) &&
            world.characters().legitimacy[i] != Legitimacy::Illegitimate)
            return row.character == heir ? CharacterId{} : row.character;
    }
    return {};
}

void explain_cost(const LawChangePreview& preview, economy::Breakdown* points, economy::Breakdown* money) {
    if (points != nullptr) {
        std::int64_t opinion = 0;
        for (const auto& row : preview.losers)
            opinion += row.opinion_delta;
        constexpr std::int64_t scale = 65536;
        points->begin(succession_term::kReformCost,
                      economy::Unit::Points,
                      (preview.cost.honor_delta + preview.cost.unity_delta + opinion) * scale);
        (void)points->add_term(
            economy::kRootNode, succession_term::kReformHonor, preview.cost.honor_delta * scale);
        (void)points->add_term(
            economy::kRootNode, succession_term::kReformUnity, preview.cost.unity_delta * scale);
        (void)points->add_term(economy::kRootNode, succession_term::kReformOpinions, opinion * scale);
    }
    if (money != nullptr) {
        money->begin(succession_term::kReformCost, economy::Unit::Kopecks, -preview.cost.money_kop);
        (void)money->add_term(economy::kRootNode, succession_term::kReformMoney, -preview.cost.money_kop);
    }
}

}  // namespace

SuccessionDenials check_succession_command(const World& world,
                                           const SuccessionRules& rules,
                                           DynastyId actor,
                                           const SuccessionCommand& command) {
    return check_request(world, rules, actor, command);
}

core::Result<void, CommandError> validate_succession_command(const World& world,
                                                             const SuccessionRules& rules,
                                                             DynastyId actor,
                                                             const SuccessionCommand& command) {
    const auto reasons = check_request(world, rules, actor, command);
    if (reasons.empty())
        return core::Result<void, CommandError>::ok();
    const auto first = reasons.front();
    auto rejection = Rejection::InvalidState;
    if (first == Denial::NotYourHouse || first == Denial::HeirNotMember)
        rejection = Rejection::NotAuthorized;
    if (first == Denial::HeirNotAlive || first == Denial::NotAlive)
        rejection = Rejection::UnknownEntity;
    if (first == Denial::InvalidGuardian || first == Denial::LawNotSelectable ||
        first == Denial::HeirRequired)
        rejection = Rejection::InvalidArgument;
    return core::Result<void, CommandError>::err(
        {rejection, succession_denial_key(first).data(), static_cast<std::uint16_t>(first)});
}

static LawChangePreview law_change_effects(const World& world,
                                           const SuccessionRules& rules,
                                           DynastyId actor,
                                           SuccessionLaw law,
                                           CharacterId heir,
                                           economy::Breakdown* points,
                                           economy::Breakdown* money) {
    LawChangePreview preview;
    preview.to = law;
    preview.needs_heir = law == SuccessionLaw::Appointment && !heir.valid();
    preview.cost = {-rules.reform_honor,
                    -rules.reform_unity,
                    world.economy().meta.present != 0 ? rules.reform_money_kop : 0};
    if (world.exists(actor) && world.dynasty_mode()) {
        preview.from = world.dynasties().succession_law[actor.index()];
        const auto head = world.dynasties().head[actor.index()];
        const auto designated = state_of(world, actor).designated_heir;
        preview.before = resolve_successor(world, rules, actor, head);
        if (static_cast<std::uint8_t>(law) < kSuccessionLawCount)
            preview.after = resolve_successor(
                world, rules, actor, head, law, law == SuccessionLaw::Appointment ? heir : designated);
        const auto old_order = ranked(preview.before);
        const auto new_order = ranked(preview.after);
        const auto loser_limit = std::min<std::size_t>(rules.reform_max_losers, 8);
        for (std::size_t i = 0; i < old_order.size() && preview.losers.size() < loser_limit; ++i) {
            const auto position = std::ranges::find(new_order, old_order[i]);
            if (position == new_order.end() || static_cast<std::size_t>(position - new_order.begin()) > i)
                preview.losers.push_back(
                    {old_order[i], -(i == 0 ? rules.reform_first_opinion : rules.reform_other_opinion)});
        }
    }
    explain_cost(preview, points, money);
    return preview;
}

LawChangePreview preview_law_change(const World& world,
                                    const SuccessionRules& rules,
                                    DynastyId actor,
                                    SuccessionLaw law,
                                    CharacterId heir,
                                    economy::Breakdown* points,
                                    economy::Breakdown* money) {
    auto preview = law_change_effects(world, rules, actor, law, heir, points, money);
    preview.denials = check_request(world, rules, actor, cmd::ChangeSuccessionLaw{law, heir});
    return preview;
}

void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::SetHeir& command) {
    auto state = state_of(world, actor);
    const auto skipped = state.designated_heir == command.heir
                             ? CharacterId{}
                             : skipped_son(world, rules, actor, command.heir);
    state.designated_heir = command.heir;
    if (command.guardian.valid())
        state.guardian = command.guardian;
    PAT_VERIFY(world.set_family_succession(state));
    if (skipped.valid()) {
        world.add_unity(
            actor, core::Fixed32::from_int(-rules.appointment_skip_unity), kStandingReasonAppointmentSkip);
        add_family_head_opinion(world,
                                skipped,
                                family_acting_head(world, actor),
                                -rules.appointment_skip_opinion,
                                kOpinionReasonLawReformLoser);
    }
    PAT_VERIFY(world.record_chronicle_note(actor, command.heir, family_acting_head(world, actor), 215));
}

void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::ChangeSuccessionLaw& command) {
    const auto preview = law_change_effects(world, rules, actor, command.law, command.heir, nullptr, nullptr);
    const auto head = family_acting_head(world, actor);
    auto state = state_of(world, actor);
    state.law_changed_on = world.today();
    if (command.law == SuccessionLaw::Appointment)
        state.designated_heir = command.heir;
    PAT_VERIFY(world.set_family_succession(state));
    world.set_dynasty_succession_law(actor, command.law);
    world.add_honor(actor, preview.cost.honor_delta, kStandingReasonLawReform);
    world.add_unity(actor, core::Fixed32::from_int(preview.cost.unity_delta), kStandingReasonLawReform);
    if (preview.cost.money_kop != 0)
        PAT_VERIFY(world.post_money(economy::AccountRef::of(actor),
                                    economy::LedgerCode::CourtUpkeep,
                                    -preview.cost.money_kop) == -preview.cost.money_kop);
    for (const auto& loser : preview.losers)
        add_family_head_opinion(
            world, loser.character, head, loser.opinion_delta, kOpinionReasonLawReformLoser);
    PAT_VERIFY(
        world.record_chronicle_note(actor, head, command.heir, 210 + static_cast<std::uint8_t>(command.law)));
}

void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::Disown& command) {
    const auto target = command.character;
    // Revoke only the disowned person's grants; ownership, balances and ordinary offices survive.
    for (std::size_t i = world.family().land_grants.size(); i > 0; --i) {
        const auto grant = world.family().land_grants[i - 1];
        if (grant.holder == target) {
            PAT_VERIFY(world.clear_family_land_grant(grant.province));
            if (world.economy().meta.present != 0)
                PAT_VERIFY(world.set_province_administrator(grant.province, {}));
        }
    }
    auto status =
        world.family_status(target) != nullptr ? *world.family_status(target) : family::PersonStatus{};
    status.character = target;
    status.flags |= family::kPersonDisowned;
    status.disowned_on = world.today();
    PAT_VERIFY(world.set_family_status(status));
    world.add_unity(actor, core::Fixed32::from_int(-rules.disown_unity), kStandingReasonDisown);
    add_family_head_opinion(
        world, target, family_acting_head(world, actor), -rules.disown_opinion, kOpinionReasonDisowned);
    world.set_character_loyalty_head(target,
                                     world.characters().loyalty_head[target.index()] -
                                         core::Fixed32::from_int(rules.disown_loyalty));
    PAT_VERIFY(world.record_chronicle_note(actor, target, family_acting_head(world, actor), 216));
}

std::string_view succession_denial_key(SuccessionDenial denial) noexcept {
    switch (denial) {
#define PAT_SUCCESSION_KEY(name, key)                                                                        \
    case Denial::name:                                                                                       \
        return "succession.denial." key
        PAT_SUCCESSION_KEY(NoActingHead, "no_acting_head");
        PAT_SUCCESSION_KEY(NotYourHouse, "not_your_house");
        PAT_SUCCESSION_KEY(DisputeOpen, "dispute_open");
        PAT_SUCCESSION_KEY(RegencyActive, "regency_active");
        PAT_SUCCESSION_KEY(DynastyExtinct, "dynasty_extinct");
        PAT_SUCCESSION_KEY(LawNotAppointment, "law_not_appointment");
        PAT_SUCCESSION_KEY(SameLaw, "same_law");
        PAT_SUCCESSION_KEY(LawNotSelectable, "law_not_selectable");
        PAT_SUCCESSION_KEY(ReformCooldown, "reform_cooldown");
        PAT_SUCCESSION_KEY(InsufficientFunds, "insufficient_funds");
        PAT_SUCCESSION_KEY(HeirNotMember, "heir_not_member");
        PAT_SUCCESSION_KEY(HeirNotAlive, "heir_not_alive");
        PAT_SUCCESSION_KEY(HeirIneligible, "heir_ineligible");
        PAT_SUCCESSION_KEY(HeirIsHead, "heir_is_head");
        PAT_SUCCESSION_KEY(HeirRequired, "heir_required");
        PAT_SUCCESSION_KEY(CannotDisownHead, "cannot_disown_head");
        PAT_SUCCESSION_KEY(AlreadyDisowned, "already_disowned");
        PAT_SUCCESSION_KEY(AlreadyTonsured, "already_tonsured");
        PAT_SUCCESSION_KEY(NoRegency, "no_regency");
        PAT_SUCCESSION_KEY(NoSupport, "no_support");
        PAT_SUCCESSION_KEY(ProvinceNotOwned, "province_not_owned");
        PAT_SUCCESSION_KEY(HolderNotAdult, "holder_not_adult");
        PAT_SUCCESSION_KEY(HolderIneligible, "holder_ineligible");
        PAT_SUCCESSION_KEY(ProvinceHasNoEconomy, "province_has_no_economy");
        PAT_SUCCESSION_KEY(TableFull, "table_full");
        PAT_SUCCESSION_KEY(InvalidGuardian, "invalid_guardian");
        PAT_SUCCESSION_KEY(ActiveRole, "active_role");
        PAT_SUCCESSION_KEY(NotAlive, "not_alive");
        PAT_SUCCESSION_KEY(DynastyModeRequired, "dynasty_mode_required");
        PAT_SUCCESSION_KEY(MoneyPostingBlocked, "money_posting_blocked");
#undef PAT_SUCCESSION_KEY
    }
    return "succession.denial.unknown";
}

}  // namespace pat::sim
