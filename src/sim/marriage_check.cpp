#include "pat/sim/marriage_check.hpp"

#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <array>

namespace pat::sim {

std::string_view FaithPolicy::faith_of(DynastyId house) const noexcept {
    if (house.valid() && house.index() < faith_by_house_.size() && !faith_by_house_[house.index()].empty()) {
        return faith_by_house_[house.index()];
    }
    return "orthodox";
}

bool FaithPolicy::allows(const World& world, CharacterId a, CharacterId b) const {
    if (!world.exists(a) || !world.exists(b)) {
        return false;
    }
    const auto& people = world.characters();
    return faith_of(people.dynasty[a.index()]) == faith_of(people.dynasty[b.index()]);
}

KinResult kinship_for_marriage(const World& world, CharacterId a, CharacterId b) {
    return {kin::blood_degree(world, a, b),
            kin::affinity_degree(world, a, b),
            kin::spiritually_related(world, a, b)};
}

MarriageDenials check_marriage(const World& world,
                               const MarriageRules& rules,
                               CharacterId groom,
                               CharacterId bride,
                               MarriageCheckOptions options,
                               const FaithPolicy* faith) {
    MarriageDenials out;
    const auto add = [&](MarriageDenial reason) { out.push_back(reason); };
    const auto& people = world.characters();
    const auto& eligibility = rules.eligibility;
    const bool groom_exists = world.exists(groom);
    const bool bride_exists = world.exists(bride);
    if (!world.is_alive(groom) || !world.is_alive(bride)) {
        add(MarriageDenial::NotAlive);
    }

    if (groom_exists && bride_exists) {
        if (groom == bride) {
            add(MarriageDenial::SamePerson);
        }
        if (eligibility.require_opposite_sex && people.sex[groom.index()] == people.sex[bride.index()]) {
            add(MarriageDenial::SameSex);
        }
        if (eligibility.forbid_same_house && people.dynasty[groom.index()].valid() &&
            people.dynasty[groom.index()] == people.dynasty[bride.index()]) {
            add(MarriageDenial::SameHouse);
        }
    }

    if (options.actor.valid() && !(groom_exists && people.dynasty[groom.index()] == options.actor) &&
        !(bride_exists && people.dynasty[bride.index()] == options.actor)) {
        add(MarriageDenial::NotYourMember);
    }

    for (const auto person : {groom, bride}) {
        if (!world.exists(person)) {
            continue;
        }
        const auto index = person.index();
        const bool male = people.sex[index] == Sex::Male;
        const int age = age_years(people.birth_date[index], world.today());
        if (options.as_betrothal) {
            if (age < eligibility.min_betrothal_age) {
                add(MarriageDenial::TooYoungToBetroth);
            }
        } else if (age < (male ? eligibility.min_marriage_age_male : eligibility.min_marriage_age_female)) {
            add(male ? MarriageDenial::GroomTooYoung : MarriageDenial::BrideTooYoung);
        }
        if (people.spouse[index].valid()) {
            add(MarriageDenial::AlreadyMarried);
        }
        const auto engagement = world.engagement_of(person);
        if (engagement == 1) {
            add(MarriageDenial::InNegotiation);
        } else if (engagement == 2) {
            add(MarriageDenial::Betrothed);
        }
        if (people.marriages[index] >= kMaxMarriages) {
            add(MarriageDenial::MarriageLimit);
        }
        if (!male && world.is_pregnant(person)) {
            add(MarriageDenial::Pregnant);
        }
        if (const auto* status = world.family_status(person)) {
            if ((status->flags & family::kPersonTonsured) != 0) {
                add(MarriageDenial::Tonsured);
            }
            if ((status->flags & family::kPersonDisowned) != 0 &&
                (!options.actor.valid() || people.dynasty[index] == options.actor)) {
                add(MarriageDenial::Disowned);
            }
            const auto mourning_days =
                male ? eligibility.mourning_days_male : eligibility.mourning_days_female;
            if (status->widowed_on != kNoDate &&
                static_cast<std::int64_t>(world.today().days()) - status->widowed_on.days() < mourning_days) {
                add(MarriageDenial::Mourning);
            }
        }
    }

    if (groom_exists && bride_exists) {
        if (std::ranges::find(people.ex_spouses[groom.index()], bride) !=
                people.ex_spouses[groom.index()].end() ||
            std::ranges::find(people.ex_spouses[bride.index()], groom) !=
                people.ex_spouses[bride.index()].end()) {
            add(MarriageDenial::PreviouslyMarried);
        }
        const auto kinship = kinship_for_marriage(world, groom, bride);
        if (kinship.blood_degree != 0 && kinship.blood_degree <= eligibility.blood_forbidden_degree) {
            if (kinship.blood_degree <= eligibility.blood_absolute_degree) {
                add(MarriageDenial::BloodKinship);
            } else if (!options.dispensation) {
                add(MarriageDenial::BloodKinshipDispensable);
            }
        }
        if (kinship.affinity_degree != 0 &&
            kinship.affinity_degree <= eligibility.affinity_forbidden_degree) {
            add(MarriageDenial::Affinity);
        }
        if (kinship.spiritual) {
            add(MarriageDenial::SpiritualKinship);
        }
        const FaithPolicy same_faith;
        if (!(faith != nullptr ? *faith : same_faith).allows(world, groom, bride)) {
            add(MarriageDenial::FaithMismatch);
        }
    }
    std::ranges::sort(out);
    const auto duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

std::optional<MarriageDenial> candidate_denial(const MarriageDenials& denials) {
    constexpr std::array priority = {MarriageDenial::Betrothed,
                                     MarriageDenial::InNegotiation,
                                     MarriageDenial::AlreadyMarried,
                                     MarriageDenial::Mourning,
                                     MarriageDenial::Tonsured};
    for (const auto reason : priority) {
        if (std::ranges::find(denials, reason) != denials.end()) {
            return reason;
        }
    }
    return denials.empty() ? std::nullopt : std::optional(*std::ranges::min_element(denials));
}

std::string_view marriage_denial_key(MarriageDenial reason) noexcept {
    switch (reason) {
#define PAT_MARRIAGE_DENIAL_KEY(name)                                                                        \
    case MarriageDenial::name:                                                                               \
        return "marriage.denial." #name;
        PAT_MARRIAGE_DENIAL_KEY(NotAlive)
        PAT_MARRIAGE_DENIAL_KEY(SamePerson)
        PAT_MARRIAGE_DENIAL_KEY(SameSex)
        PAT_MARRIAGE_DENIAL_KEY(SameHouse)
        PAT_MARRIAGE_DENIAL_KEY(GroomTooYoung)
        PAT_MARRIAGE_DENIAL_KEY(BrideTooYoung)
        PAT_MARRIAGE_DENIAL_KEY(TooYoungToBetroth)
        PAT_MARRIAGE_DENIAL_KEY(AlreadyMarried)
        PAT_MARRIAGE_DENIAL_KEY(Betrothed)
        PAT_MARRIAGE_DENIAL_KEY(InNegotiation)
        PAT_MARRIAGE_DENIAL_KEY(Tonsured)
        PAT_MARRIAGE_DENIAL_KEY(Disowned)
        PAT_MARRIAGE_DENIAL_KEY(MarriageLimit)
        PAT_MARRIAGE_DENIAL_KEY(PreviouslyMarried)
        PAT_MARRIAGE_DENIAL_KEY(Mourning)
        PAT_MARRIAGE_DENIAL_KEY(Pregnant)
        PAT_MARRIAGE_DENIAL_KEY(BloodKinship)
        PAT_MARRIAGE_DENIAL_KEY(BloodKinshipDispensable)
        PAT_MARRIAGE_DENIAL_KEY(Affinity)
        PAT_MARRIAGE_DENIAL_KEY(SpiritualKinship)
        PAT_MARRIAGE_DENIAL_KEY(FaithMismatch)
        PAT_MARRIAGE_DENIAL_KEY(ChurchConsentRequired)
        PAT_MARRIAGE_DENIAL_KEY(NotYourMember)
        PAT_MARRIAGE_DENIAL_KEY(HouseBusy)
        PAT_MARRIAGE_DENIAL_KEY(InsufficientFunds)
        PAT_MARRIAGE_DENIAL_KEY(DuplicateNegotiation)
        PAT_MARRIAGE_DENIAL_KEY(RecentlyRefused)
        PAT_MARRIAGE_DENIAL_KEY(NegotiationNotFound)
        PAT_MARRIAGE_DENIAL_KEY(NotInitiator)
        PAT_MARRIAGE_DENIAL_KEY(NoActingHead)
        PAT_MARRIAGE_DENIAL_KEY(TableFull)
        PAT_MARRIAGE_DENIAL_KEY(BetrothalNotFound)
        PAT_MARRIAGE_DENIAL_KEY(NotBetrothalParty)
        PAT_MARRIAGE_DENIAL_KEY(NotMarried)
        PAT_MARRIAGE_DENIAL_KEY(CannotTonsureHead)
        PAT_MARRIAGE_DENIAL_KEY(NotYourSpouse)
#undef PAT_MARRIAGE_DENIAL_KEY
    }
    return {};
}

}  // namespace pat::sim
