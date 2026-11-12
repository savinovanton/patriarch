#include "pat/sim/command.hpp"

#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/construction.hpp"
#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/recruits.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/traits.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace pat::sim {

namespace {

using core::Result;

constexpr core::Fixed32 kZeroFixed = core::Fixed32::zero();

[[nodiscard]] Result<void, CommandError> reject(Rejection reason, const char* detail) {
    return Result<void, CommandError>::err(CommandError{reason, detail});
}
[[nodiscard]] Result<void, CommandError> accept() {
    return Result<void, CommandError>::ok();
}

Result<void, CommandError> trait_verdict(TraitStatus status) {
    switch (status) {
        case TraitStatus::Ok:
            return accept();
        case TraitStatus::NotAlive:
            return reject(Rejection::UnknownEntity, "unknown or dead character");
        case TraitStatus::Unknown:
            return reject(Rejection::UnknownEntity, "unknown trait");
        case TraitStatus::AlreadyPresent:
            return reject(Rejection::InvalidState, "the character already has this trait");
        case TraitStatus::Missing:
            return reject(Rejection::InvalidState, "the character does not have this trait");
        case TraitStatus::SlotsFull:
            return reject(Rejection::InvalidState, "the character has no free trait slot");
        case TraitStatus::Opposite:
            return reject(Rejection::InvalidArgument, "the character has the opposite trait");
        case TraitStatus::CongenitalLimit:
            return reject(Rejection::InvalidArgument, "the congenital trait limit is three");
        case TraitStatus::InvalidSet:
            return reject(Rejection::InvalidArgument, "the trait set is not valid");
    }
    return reject(Rejection::InvalidArgument, "unknown trait status");
}

Result<void, CommandError> loan_verdict(economy::LoanStatus status) {
    using economy::LoanStatus;
    switch (status) {
        case LoanStatus::Ok:
            return accept();
        case LoanStatus::NoEconomy:
            return reject(Rejection::InvalidState, "the world has no economy");
        case LoanStatus::UnknownDynasty:
            return reject(Rejection::UnknownEntity, "unknown borrower");
        case LoanStatus::UnknownLoan:
            return reject(Rejection::UnknownEntity, "unknown loan");
        case LoanStatus::InvalidAmount:
            return reject(Rejection::InvalidArgument, "the loan amount is invalid");
        case LoanStatus::LimitExceeded:
            return reject(Rejection::InvalidArgument, "the amount exceeds the remaining loan limit");
        case LoanStatus::TooManyLoans:
            return reject(Rejection::InvalidState, "the borrower has the maximum number of loans");
        case LoanStatus::Bankrupt:
            return reject(Rejection::InvalidState, "a bankrupt dynasty cannot borrow");
        case LoanStatus::InsufficientFunds:
            return reject(Rejection::InvalidState, "the borrower cannot afford the repayment");
        case LoanStatus::InvalidState:
            return reject(Rejection::InvalidState, "the loan transaction cannot be applied");
    }
    return reject(Rejection::InvalidState, "unknown loan status");
}

/// The system (invalid actor) may do anything that is otherwise valid; a dynasty may act only for itself.
[[nodiscard]] bool may_act_for(DynastyId actor, DynastyId owner) noexcept {
    return !actor.valid() || actor == owner;
}

struct Validator {
    const World& world;
    DynastyId actor;
    const TraitCatalog* traits;
    const economy::EconomyRules* economy;

    Result<void, CommandError> operator()(const cmd::SetProvinceOwner& c) const {
        if (!world.exists(c.province)) {
            return reject(Rejection::UnknownEntity, "unknown province");
        }
        if (c.new_owner.valid() && !world.exists(c.new_owner)) {
            return reject(Rejection::UnknownEntity, "unknown new owner");
        }
        if (!c.new_owner.valid() && actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may leave a province unowned");
        }
        const DynastyId current = world.provinces().owner[c.province.index()];
        if (actor.valid() && actor != current) {
            return reject(Rejection::NotAuthorized, "the actor does not own the province");
        }
        if (current == c.new_owner) {
            return reject(Rejection::InvalidState, "the province already has this owner");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::CreateCharacter& c) const {
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!may_act_for(actor, c.dynasty)) {
            return reject(Rejection::NotAuthorized, "the actor may not create characters of another dynasty");
        }
        if (c.birth_date > world.today()) {
            return reject(Rejection::InvalidArgument, "birth date is in the future");
        }
        if (!world.can_create_character()) {
            return reject(Rejection::InvalidState, "the character table is full");
        }
        return check_parents(c.father, c.mother, c.birth_date);
    }

    Result<void, CommandError> check_parents(CharacterId father, CharacterId mother, Date birth_date) const {
        if (world.dynasty_mode()) {
            // The family tree of dynasty mode needs real ancestors (GDD 01 section 13.3).
            for (const CharacterId parent : {father, mother}) {
                if (parent.valid() && world.is_alive(parent) &&
                    world.characters().birth_date[parent.index()] >= birth_date) {
                    return reject(Rejection::InvalidArgument, "a parent must be born before the child");
                }
            }
        }
        if (father.valid()) {
            if (!world.is_alive(father)) {
                return reject(Rejection::UnknownEntity, "father does not exist");
            }
            if (world.characters().sex[father.index()] != Sex::Male) {
                return reject(Rejection::InvalidArgument, "father must be male");
            }
        }
        if (mother.valid()) {
            if (!world.is_alive(mother)) {
                return reject(Rejection::UnknownEntity, "mother does not exist");
            }
            if (world.characters().sex[mother.index()] != Sex::Female) {
                return reject(Rejection::InvalidArgument, "mother must be female");
            }
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::CreatePerson& c) const {
        const CharacterInit& init = c.init;
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may create persons directly");
        }
        if (!world.exists(init.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!is_sane_date(init.birth_date) || init.birth_date > world.today()) {
            return reject(Rejection::InvalidArgument, "birth date is in the future or implausible");
        }
        if (!world.can_create_character()) {
            return reject(Rejection::InvalidState, "the character table is full");
        }
        if (init.sex != Sex::Male && init.sex != Sex::Female) {
            return reject(Rejection::InvalidArgument, "unknown sex");
        }
        if (static_cast<std::uint8_t>(init.legitimacy) >= kLegitimacyCount) {
            return reject(Rejection::InvalidArgument, "unknown legitimacy");
        }
        if (init.health < kZeroFixed || init.health > kScaleMax) {
            return reject(Rejection::InvalidArgument, "health must be within 0..100");
        }
        for (const core::Fixed32 skill : init.skills) {
            if (skill < kZeroFixed || skill > kSkillMax) {
                return reject(Rejection::InvalidArgument, "skills must be within 0..20");
            }
        }
        for (const core::Fixed32 scale : {init.fertility, init.stress, init.piety}) {
            if (scale < kZeroFixed || scale > kScaleMax) {
                return reject(Rejection::InvalidArgument,
                              "fertility, stress and piety must be within 0..100");
            }
        }
        for (const core::Fixed32 value : {init.ambition, init.loyalty_head, init.loyalty_emperor}) {
            if (value < -kSignedScaleMax || value > kSignedScaleMax) {
                return reject(Rejection::InvalidArgument, "ambition and loyalties must be within -100..100");
            }
        }
        if (!traits_are_canonical(init.traits)) {
            return reject(Rejection::InvalidArgument, "traits must be sorted, unique and packed");
        }
        if (traits != nullptr) {
            const auto verdict = trait_verdict(check_trait_set(*traits, init.traits));
            if (!verdict) {
                return verdict;
            }
        }
        if (init.education.focus > kEducationFocusCount || init.education.level > kMaxEducationLevel) {
            return reject(Rejection::InvalidArgument, "education focus or level out of range");
        }
        if (init.education.tutor.valid() && !world.is_alive(init.education.tutor)) {
            return reject(Rejection::UnknownEntity, "the tutor does not exist");
        }
        if (init.branch.valid() &&
            (!world.exists(init.branch) || world.branches().dynasty[init.branch.index()] != init.dynasty)) {
            return reject(Rejection::InvalidArgument, "the branch does not belong to the dynasty");
        }
        if (!planned_death_is_valid(init.birth_date, init.planned_death)) {
            return reject(Rejection::InvalidArgument, "planned death lies outside the lifetime");
        }
        return check_parents(init.father, init.mother, init.birth_date);
    }

    Result<void, CommandError> trait_command() const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may change traits directly");
        }
        if (!world.dynasty_mode() || traits == nullptr) {
            return reject(Rejection::InvalidState, "trait commands need dynasty mode and a trait catalogue");
        }
        return accept();
    }
    Result<void, CommandError> operator()(const cmd::GrantTrait& c) const {
        const auto allowed = trait_command();
        return allowed ? trait_verdict(check_grant_trait(world, *traits, c.character, c.trait)) : allowed;
    }
    Result<void, CommandError> operator()(const cmd::RevokeTrait& c) const {
        const auto allowed = trait_command();
        return allowed ? trait_verdict(check_revoke_trait(world, *traits, c.character, c.trait)) : allowed;
    }

    Result<void, CommandError> operator()(const cmd::Marry& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may marry characters");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "marriages need dynasty mode");
        }
        if (!world.is_alive(c.a) || !world.is_alive(c.b)) {
            return reject(Rejection::UnknownEntity, "unknown or dead character");
        }
        if (c.a == c.b) {
            return reject(Rejection::InvalidArgument, "a character cannot marry itself");
        }
        const CharacterTable& t = world.characters();
        if (t.spouse[c.a.index()].valid() || t.spouse[c.b.index()].valid()) {
            return reject(Rejection::InvalidState, "already married");
        }
        if (t.marriages[c.a.index()] >= kMaxMarriages || t.marriages[c.b.index()] >= kMaxMarriages) {
            return reject(Rejection::InvalidState, "a person may marry at most three times");
        }
        const auto& ex = t.ex_spouses[c.a.index()];
        if (std::find(ex.begin(), ex.end(), c.b) != ex.end()) {
            return reject(Rejection::InvalidState, "these two have been married to each other before");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::EndMarriage& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may end a marriage");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "marriages need dynasty mode");
        }
        if (!world.is_alive(c.a)) {
            return reject(Rejection::UnknownEntity, "unknown or dead character");
        }
        if (static_cast<std::uint8_t>(c.reason) >= kMarriageEndCount) {
            return reject(Rejection::InvalidArgument, "unknown reason to end a marriage");
        }
        if (!world.is_alive(world.characters().spouse[c.a.index()])) {
            return reject(Rejection::InvalidState, "the character is not married");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::Die& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may let a character die");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "Die needs dynasty mode (use KillCharacter in E1 mode)");
        }
        if (!world.exists(c.character)) {
            return reject(Rejection::UnknownEntity, "unknown character");
        }
        if (!world.is_alive(c.character)) {
            return reject(Rejection::InvalidState, "the character is already dead");
        }
        if (c.cause == CauseOfDeath::None || static_cast<std::uint8_t>(c.cause) >= kCauseOfDeathCount) {
            return reject(Rejection::InvalidArgument, "unknown cause of death");
        }
        if (world.characters().birth_date[c.character.index()] > world.today()) {
            return reject(Rejection::InvalidState, "the character is not born yet");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::PlanDeath& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may plan a death");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "PlanDeath needs dynasty mode");
        }
        if (!world.exists(c.character)) {
            return reject(Rejection::UnknownEntity, "unknown character");
        }
        if (!world.is_alive(c.character)) {
            return reject(Rejection::InvalidState, "the character is already dead");
        }
        if (!planned_death_is_valid(world.characters().birth_date[c.character.index()], c.date)) {
            return reject(Rejection::InvalidArgument, "invalid planned death date");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::RecordChronicle& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may write a chronicle note");
        }
        if (!world.dynasty_mode() || !is_sane_date(world.today())) {
            return reject(Rejection::InvalidState, "RecordChronicle needs dynasty mode and a valid date");
        }
        if (c.kind != ChronicleKind::Note) {
            return reject(Rejection::InvalidArgument, "RecordChronicle accepts editorial notes only");
        }
        if (!world.exists(c.dynasty) || (c.a.valid() && !world.exists(c.a)) ||
            (c.b.valid() && !world.exists(c.b))) {
            return reject(Rejection::UnknownEntity, "unknown chronicle dynasty or participant");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::AdjustOpinion& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may adjust an opinion");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "opinions need dynasty mode");
        }
        if (!world.is_alive(c.from) || !world.is_alive(c.to)) {
            return reject(Rejection::UnknownEntity, "unknown or dead character");
        }
        if (c.from == c.to) {
            return reject(Rejection::InvalidArgument, "an opinion needs two different characters");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::AssignProvinceAdministrator& c) const {
        if (!world.economy_present()) {
            return reject(Rejection::InvalidState, "the world has no economy");
        }
        if (!world.exists(c.province)) {
            return reject(Rejection::UnknownEntity, "unknown province");
        }
        if (c.administrator.valid()) {
            if (!world.exists(c.administrator)) {
                return reject(Rejection::UnknownEntity, "unknown character");
            }
            if (!world.is_alive(c.administrator)) {
                return reject(Rejection::InvalidState, "the character is dead");
            }
        }
        const std::size_t p = c.province.index();
        if (actor.valid()) {
            const bool owns = world.economy().provinces.owner_kind[p] ==
                                  static_cast<std::uint8_t>(economy::OwnerKind::Dynasty) &&
                              world.provinces().owner[p] == actor;
            if (!owns) {
                return reject(Rejection::NotAuthorized, "the actor does not own the province");
            }
            if (c.administrator.valid() && world.characters().dynasty[c.administrator.index()] != actor) {
                return reject(Rejection::NotAuthorized,
                              "the administrator must belong to the house of the owner");
            }
        }
        if (world.economy().provinces.administrator[p] == c.administrator) {
            return reject(Rejection::InvalidState, "the province already has this administrator");
        }
        return accept();
    }

    Result<void, CommandError> policy(DynastyId dynasty, std::uint8_t level, std::uint8_t levels) const {
        if (!world.economy_present()) {
            return reject(Rejection::InvalidState, "the world has no economy");
        }
        if (!world.exists(dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!may_act_for(actor, dynasty)) {
            return reject(Rejection::NotAuthorized, "a dynasty sets only its own policy");
        }
        // Without the number of levels of the rules (0, never set) only the structural limit holds.
        const std::size_t limit = levels != 0 ? levels : economy::kMaxPolicyLevels;
        if (level >= limit) {
            return reject(Rejection::InvalidArgument, "the policy has no such level");
        }
        return accept();
    }
    Result<void, CommandError> operator()(const cmd::SetTaxPolicy& c) const {
        return policy(c.dynasty, c.level, world.economy().meta.tax_levels);
    }
    Result<void, CommandError> operator()(const cmd::SetRecruitPolicy& c) const {
        return policy(c.dynasty, c.level, world.economy().meta.recruit_levels);
    }

    Result<void, CommandError> operator()(const cmd::ConscriptRecruits& c) const {
        if (!world.economy_present()) {
            return reject(Rejection::InvalidState, "the world has no economy");
        }
        if (!world.exists(c.province)) {
            return reject(Rejection::UnknownEntity, "unknown province");
        }
        const std::size_t p = c.province.index();
        const bool held = world.economy().provinces.owner_kind[p] ==
                              static_cast<std::uint8_t>(economy::OwnerKind::Dynasty) &&
                          world.provinces().owner[p].valid();
        if (actor.valid() && !(held && world.provinces().owner[p] == actor)) {
            return reject(Rejection::NotAuthorized, "the actor does not own the province");
        }
        if (!held) {
            return reject(Rejection::InvalidState, "no dynasty holds the province");
        }
        if (c.count == 0) {
            return reject(Rejection::InvalidArgument, "nobody to conscript");
        }
        if (c.count > world.economy().provinces.recruit_pool[p]) {
            return reject(Rejection::InvalidState, "more than the recruit pool of the province");
        }
        std::array<std::uint32_t, economy::kGroupCount> size{};
        for (std::size_t g = 0; g < economy::kGroupCount; ++g) {
            size[g] = world.economy().groups.size[p * economy::kGroupCount + g];
        }
        if (c.count > economy::recruit_population(size)) {
            return reject(Rejection::InvalidState, "fewer people in the recruit groups than asked for");
        }
        const std::uint32_t reserve =
            world.economy().dynasties.manpower_reserve[world.provinces().owner[p].index()];
        if (c.count > std::numeric_limits<std::uint32_t>::max() - reserve) {
            return reject(Rejection::InvalidState, "the manpower reserve cannot hold that many");
        }
        return accept();
    }

    /// The verdict of the construction rules as a rejection (the refusal texts are static strings).
    static Result<void, CommandError> construction_verdict(economy::ConstructionRefusal refusal) {
        using economy::ConstructionRefusal;
        const char* text = economy::refusal_text(refusal);
        switch (refusal) {
            case ConstructionRefusal::None:
                return accept();
            case ConstructionRefusal::UnknownProvince:
            case ConstructionRefusal::NoConstruction:
                return reject(Rejection::UnknownEntity, text);
            case ConstructionRefusal::UnknownBuilding:
                return reject(Rejection::InvalidArgument, text);
            case ConstructionRefusal::NotOwner:
            case ConstructionRefusal::NotPayer:
                return reject(Rejection::NotAuthorized, text);
            case ConstructionRefusal::NoEconomy:
            case ConstructionRefusal::NoRules:
            case ConstructionRefusal::NotHeld:
            case ConstructionRefusal::WrongKind:
            case ConstructionRefusal::TooLittleDevelopment:
            case ConstructionRefusal::TooMany:
            case ConstructionRefusal::NoFreeSlot:
            case ConstructionRefusal::NoMoney:
                return reject(Rejection::InvalidState, text);
        }
        return reject(Rejection::InvalidState, text);
    }

    Result<void, CommandError> operator()(const cmd::StartConstruction& c) const {
        if (!world.economy_present()) {
            return construction_verdict(economy::ConstructionRefusal::NoEconomy);
        }
        if (economy == nullptr) {
            return construction_verdict(economy::ConstructionRefusal::NoRules);
        }
        return construction_verdict(
            economy::check_start_construction(world, *economy, actor, c.province, c.building).refusal);
    }

    Result<void, CommandError> operator()(const cmd::CancelConstruction& c) const {
        return construction_verdict(economy::check_cancel_construction(world, actor, c.province, c.slot));
    }

    Result<void, CommandError> operator()(const cmd::TakeLoan& c) const {
        if (!world.economy_present()) {
            return loan_verdict(economy::LoanStatus::NoEconomy);
        }
        if (economy == nullptr) {
            return reject(Rejection::InvalidState, "the simulation has no economy rules");
        }
        if (!actor.valid()) {
            return reject(Rejection::NotAuthorized, "taking a loan requires a borrower dynasty");
        }
        return loan_verdict(economy::check_take_loan(world, economy->loans, actor, c.amount_kop));
    }

    Result<void, CommandError> operator()(const cmd::RepayLoan& c) const {
        if (!world.economy_present()) {
            return loan_verdict(economy::LoanStatus::NoEconomy);
        }
        if (economy == nullptr) {
            return reject(Rejection::InvalidState, "the simulation has no economy rules");
        }
        const auto row = economy::find_loan(world, c.loan_id);
        if (!row) {
            return loan_verdict(economy::LoanStatus::UnknownLoan);
        }
        if (!may_act_for(actor, world.economy().loans.borrower[*row])) {
            return reject(Rejection::NotAuthorized, "only the borrower may repay the loan");
        }
        return loan_verdict(economy::check_repay_loan(world, c.loan_id, c.amount_kop));
    }

    Result<void, CommandError> operator()(const cmd::KillCharacter& c) const {
        if (world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "characters do not disappear in dynasty mode (use Die)");
        }
        if (!world.is_alive(c.character)) {
            return reject(Rejection::UnknownEntity, "unknown or already dead character");
        }
        if (!may_act_for(actor, world.characters().dynasty[c.character.index()])) {
            return reject(Rejection::NotAuthorized, "the actor does not control this character");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::SetDynastyHead& c) const {
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!may_act_for(actor, c.dynasty)) {
            return reject(Rejection::NotAuthorized, "the actor may not change another dynasty's head");
        }
        if (c.head.valid()) {
            if (!world.is_alive(c.head)) {
                return reject(Rejection::UnknownEntity, "unknown or dead character");
            }
            if (world.characters().dynasty[c.head.index()] != c.dynasty) {
                return reject(Rejection::InvalidState, "the new head is not a member of the dynasty");
            }
        }
        return accept();
    }

    Result<void, CommandError> standing_change(DynastyId dynasty) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may adjust dynasty standing");
        }
        if (!world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "standing changes need dynasty mode");
        }
        if (!world.exists(dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::AdjustUnity& c) const {
        return standing_change(c.dynasty);
    }
    Result<void, CommandError> operator()(const cmd::AdjustPrestige& c) const {
        return standing_change(c.dynasty);
    }
    Result<void, CommandError> operator()(const cmd::AdjustHonorWithReason& c) const {
        return standing_change(c.dynasty);
    }

    Result<void, CommandError> operator()(const cmd::AdjustHonor& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may adjust honor directly");
        }
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        return accept();
    }
};

struct Applier {
    World& world;
    DynastyId actor;
    const TraitCatalog* traits;
    const economy::EconomyRules* economy;

    void operator()(const cmd::GrantTrait& c) const {
        if (traits != nullptr) {
            (void)grant_trait(world, *traits, c.character, c.trait);
        }
    }
    void operator()(const cmd::RevokeTrait& c) const {
        if (traits != nullptr) {
            (void)revoke_trait(world, *traits, c.character, c.trait);
        }
    }

    void operator()(const cmd::SetProvinceOwner& c) const {
        world.set_province_owner(c.province, c.new_owner);
    }
    void operator()(const cmd::CreateCharacter& c) const {
        CharacterInit init;
        init.dynasty = c.dynasty;
        init.birth_date = c.birth_date;
        init.sex = c.sex;
        init.father = c.father;
        init.mother = c.mother;
        (void)world.create_character(init);
    }
    void operator()(const cmd::KillCharacter& c) const { (void)world.destroy_character(c.character); }
    void operator()(const cmd::SetDynastyHead& c) const { (void)world.set_dynasty_head(c.dynasty, c.head); }
    void operator()(const cmd::AdjustHonor& c) const { world.add_honor(c.dynasty, c.delta); }
    void operator()(const cmd::AdjustUnity& c) const { world.add_unity(c.dynasty, c.delta, c.reason); }
    void operator()(const cmd::AdjustPrestige& c) const { world.add_prestige(c.dynasty, c.delta, c.reason); }
    void operator()(const cmd::AdjustHonorWithReason& c) const {
        world.add_honor(c.dynasty, c.delta, c.reason);
    }
    void operator()(const cmd::CreatePerson& c) const { (void)world.create_character(c.init); }
    void operator()(const cmd::Die& c) const { (void)world.die(c.character, c.cause); }
    void operator()(const cmd::PlanDeath& c) const {
        (void)world.set_character_planned_death(c.character, c.date);
    }
    void operator()(const cmd::RecordChronicle& c) const {
        (void)world.record_chronicle_note(c.dynasty, c.a, c.b, c.param);
    }
    void operator()(const cmd::AdjustOpinion& c) const {
        (void)world.add_opinion(c.from, c.to, c.delta, c.reason);
    }
    void operator()(const cmd::AssignProvinceAdministrator& c) const {
        (void)world.set_province_administrator(c.province, c.administrator);
    }
    void operator()(const cmd::SetTaxPolicy& c) const {
        (void)world.set_dynasty_tax_policy(c.dynasty, c.level);
    }
    void operator()(const cmd::SetRecruitPolicy& c) const {
        (void)world.set_dynasty_recruit_policy(c.dynasty, c.level);
    }
    void operator()(const cmd::ConscriptRecruits& c) const {
        (void)world.conscript_recruits(c.province, c.count);
    }
    void operator()(const cmd::StartConstruction& c) const {
        if (economy != nullptr) {
            (void)economy::start_construction(world, *economy, actor, c.province, c.building);
        }
    }
    void operator()(const cmd::CancelConstruction& c) const {
        if (economy != nullptr) {
            (void)economy::cancel_construction(world, *economy, actor, c.province, c.slot);
        }
    }
    void operator()(const cmd::TakeLoan& c) const {
        if (economy != nullptr) {
            (void)world.take_loan(actor, c.amount_kop, economy->loans);
        }
    }
    void operator()(const cmd::RepayLoan& c) const {
        if (economy != nullptr) {
            (void)world.repay_loan(c.loan_id, c.amount_kop, economy->loans);
        }
    }
    void operator()(const cmd::Marry& c) const { (void)world.marry(c.a, c.b); }
    void operator()(const cmd::EndMarriage& c) const { (void)world.end_marriage(c.a); }
};

// ---- stable tags ------------------------------------------------------------------------------
// One specialisation per payload type. A new command without one does not compile; the tag of an
// existing command must never change (it is written to replays and saves).

template <class T>
struct CommandTag;  // intentionally undefined for unknown payload types

template <>
struct CommandTag<cmd::SetProvinceOwner> {
    static constexpr CommandType value = CommandType::SetProvinceOwner;
};
template <>
struct CommandTag<cmd::CreateCharacter> {
    static constexpr CommandType value = CommandType::CreateCharacter;
};
template <>
struct CommandTag<cmd::KillCharacter> {
    static constexpr CommandType value = CommandType::KillCharacter;
};
template <>
struct CommandTag<cmd::SetDynastyHead> {
    static constexpr CommandType value = CommandType::SetDynastyHead;
};
template <>
struct CommandTag<cmd::AdjustHonor> {
    static constexpr CommandType value = CommandType::AdjustHonor;
};
template <>
struct CommandTag<cmd::CreatePerson> {
    static constexpr CommandType value = CommandType::CreatePerson;
};
template <>
struct CommandTag<cmd::GrantTrait> {
    static constexpr CommandType value = CommandType::GrantTrait;
};
template <>
struct CommandTag<cmd::RevokeTrait> {
    static constexpr CommandType value = CommandType::RevokeTrait;
};
template <>
struct CommandTag<cmd::Die> {
    static constexpr CommandType value = CommandType::Die;
};
template <>
struct CommandTag<cmd::Marry> {
    static constexpr CommandType value = CommandType::Marry;
};
template <>
struct CommandTag<cmd::EndMarriage> {
    static constexpr CommandType value = CommandType::EndMarriage;
};
template <>
struct CommandTag<cmd::PlanDeath> {
    static constexpr CommandType value = CommandType::PlanDeath;
};
template <>
struct CommandTag<cmd::RecordChronicle> {
    static constexpr CommandType value = CommandType::RecordChronicle;
};
template <>
struct CommandTag<cmd::AdjustOpinion> {
    static constexpr CommandType value = CommandType::AdjustOpinion;
};
template <>
struct CommandTag<cmd::AdjustUnity> {
    static constexpr CommandType value = CommandType::AdjustUnity;
};
template <>
struct CommandTag<cmd::AdjustPrestige> {
    static constexpr CommandType value = CommandType::AdjustPrestige;
};
template <>
struct CommandTag<cmd::AdjustHonorWithReason> {
    static constexpr CommandType value = CommandType::AdjustHonorWithReason;
};

template <>
struct CommandTag<cmd::AssignProvinceAdministrator> {
    static constexpr CommandType value = CommandType::AssignProvinceAdministrator;
};

template <>
struct CommandTag<cmd::SetTaxPolicy> {
    static constexpr CommandType value = CommandType::SetTaxPolicy;
};
template <>
struct CommandTag<cmd::SetRecruitPolicy> {
    static constexpr CommandType value = CommandType::SetRecruitPolicy;
};
template <>
struct CommandTag<cmd::ConscriptRecruits> {
    static constexpr CommandType value = CommandType::ConscriptRecruits;
};
template <>
struct CommandTag<cmd::StartConstruction> {
    static constexpr CommandType value = CommandType::StartConstruction;
};
template <>
struct CommandTag<cmd::CancelConstruction> {
    static constexpr CommandType value = CommandType::CancelConstruction;
};
template <>
struct CommandTag<cmd::TakeLoan> {
    static constexpr CommandType value = CommandType::TakeLoan;
};
template <>
struct CommandTag<cmd::RepayLoan> {
    static constexpr CommandType value = CommandType::RepayLoan;
};

template <class Variant>
struct TagTable;
template <class... Alternatives>
struct TagTable<std::variant<Alternatives...>> {
    static constexpr std::array<CommandType, sizeof...(Alternatives)> value = {
        CommandTag<Alternatives>::value...};
};

constexpr auto kTagByAlternative = TagTable<CommandPayload>::value;

constexpr bool tags_are_valid_and_unique() {
    for (std::size_t i = 0; i < kTagByAlternative.size(); ++i) {
        if (static_cast<std::uint8_t>(kTagByAlternative[i]) == 0) {
            return false;
        }
        for (std::size_t j = i + 1; j < kTagByAlternative.size(); ++j) {
            if (kTagByAlternative[i] == kTagByAlternative[j]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(tags_are_valid_and_unique(), "every command needs its own non-zero tag");

}  // namespace

CommandType type_of(const Command& command) noexcept {
    return kTagByAlternative[command.payload.index()];  // the payload is never valueless
}

std::string_view to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::SetProvinceOwner:
            return "SetProvinceOwner";
        case CommandType::CreateCharacter:
            return "CreateCharacter";
        case CommandType::KillCharacter:
            return "KillCharacter";
        case CommandType::SetDynastyHead:
            return "SetDynastyHead";
        case CommandType::AdjustHonor:
            return "AdjustHonor";
        case CommandType::CreatePerson:
            return "CreatePerson";
        case CommandType::GrantTrait:
            return "GrantTrait";
        case CommandType::RevokeTrait:
            return "RevokeTrait";
        case CommandType::Die:
            return "Die";
        case CommandType::Marry:
            return "Marry";
        case CommandType::EndMarriage:
            return "EndMarriage";
        case CommandType::PlanDeath:
            return "PlanDeath";
        case CommandType::RecordChronicle:
            return "RecordChronicle";
        case CommandType::AdjustOpinion:
            return "AdjustOpinion";
        case CommandType::AssignProvinceAdministrator:
            return "AssignProvinceAdministrator";
        case CommandType::SetTaxPolicy:
            return "SetTaxPolicy";
        case CommandType::SetRecruitPolicy:
            return "SetRecruitPolicy";
        case CommandType::ConscriptRecruits:
            return "ConscriptRecruits";
        case CommandType::StartConstruction:
            return "StartConstruction";
        case CommandType::CancelConstruction:
            return "CancelConstruction";
        case CommandType::TakeLoan:
            return "TakeLoan";
        case CommandType::RepayLoan:
            return "RepayLoan";
        case CommandType::AdjustUnity:
            return "AdjustUnity";
        case CommandType::AdjustPrestige:
            return "AdjustPrestige";
        case CommandType::AdjustHonorWithReason:
            return "AdjustHonorWithReason";
    }
    return "?";
}

std::string_view to_string(Rejection reason) noexcept {
    switch (reason) {
        case Rejection::None:
            return "accepted";
        case Rejection::UnknownEntity:
            return "unknown entity";
        case Rejection::NotAuthorized:
            return "not authorized";
        case Rejection::InvalidArgument:
            return "invalid argument";
        case Rejection::InvalidState:
            return "invalid state";
    }
    return "?";
}

Result<void, CommandError> validate(const World& world,
                                    const Command& command,
                                    const TraitCatalog* traits,
                                    const economy::EconomyRules* economy) {
    if (command.actor.valid() && !world.exists(command.actor)) {
        return reject(Rejection::UnknownEntity, "unknown actor");
    }
    return std::visit(Validator{world, command.actor, traits, economy}, command.payload);
}

void apply(World& world,
           const Command& command,
           const TraitCatalog* traits,
           const economy::EconomyRules* economy) {
    std::visit(Applier{world, command.actor, traits, economy}, command.payload);
}

ApplyStats process_commands(World& world,
                            std::span<const Command> commands,
                            CommandSource source,
                            CommandLog& log,
                            std::vector<DepartedHead>* departed_heads,
                            const TraitCatalog* traits,
                            const economy::EconomyRules* economy) {
    ApplyStats stats;
    for (const Command& command : commands) {
        const Result<void, CommandError> verdict = validate(world, command, traits, economy);
        LoggedCommand entry;
        entry.date = world.today();
        entry.source = source;
        entry.command = command;
        if (verdict.has_value()) {
            if (departed_heads != nullptr) {
                if (const auto* death = std::get_if<cmd::Die>(&command.payload)) {
                    const DynastyId dynasty = world.characters().dynasty[death->character.index()];
                    if (dynasty.valid() && world.dynasties().head[dynasty.index()] == death->character) {
                        const auto position =
                            std::ranges::lower_bound(*departed_heads, dynasty, {}, &DepartedHead::dynasty);
                        if (position != departed_heads->end() && position->dynasty == dynasty) {
                            position->predecessor = death->character;
                        } else {
                            departed_heads->insert(position, {dynasty, death->character});
                        }
                    }
                }
            }
            apply(world, command, traits, economy);
            ++stats.applied;
        } else {
            entry.outcome = verdict.error().reason;
            ++stats.rejected;
        }
        log.append(entry);
    }
    return stats;
}

}  // namespace pat::sim
