#include "pat/sim/marriage_check.hpp"

#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

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

namespace {

void append_person_denials(const World& world,
                           const MarriageRules& rules,
                           CharacterId person,
                           MarriageCheckOptions options,
                           MarriageDenials& out,
                           std::optional<std::int32_t> cached_age = std::nullopt) {
    if (!world.exists(person)) {
        return;
    }
    const auto add = [&](MarriageDenial reason) { out.push_back(reason); };
    const auto& people = world.characters();
    const auto& eligibility = rules.eligibility;
    const auto index = person.index();
    const bool male = people.sex[index] == Sex::Male;
    const int age = cached_age ? *cached_age : age_years(people.birth_date[index], world.today());
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
        const auto* negotiation = world.family_negotiation_of(person);
        if (negotiation == nullptr || negotiation->id != options.ignore_negotiation) {
            add(MarriageDenial::InNegotiation);
        }
    } else if (engagement == 2) {
        const auto* betrothal = options.ignore_betrothal == 0 ? nullptr : world.family_betrothal_of(person);
        if (betrothal == nullptr || betrothal->id != options.ignore_betrothal) {
            add(MarriageDenial::Betrothed);
        }
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
        const auto mourning_days = male ? eligibility.mourning_days_male : eligibility.mourning_days_female;
        if (status->widowed_on != kNoDate &&
            static_cast<std::int64_t>(world.today().days()) - status->widowed_on.days() < mourning_days) {
            add(MarriageDenial::Mourning);
        }
    }
}

MarriageDenials assemble_denials(const World& world,
                                 const MarriageRules& rules,
                                 CharacterId groom,
                                 CharacterId bride,
                                 MarriageCheckOptions options,
                                 const FaithPolicy* faith,
                                 const KinResult& kinship,
                                 MarriageDenials out) {
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

    if (groom_exists && bride_exists) {
        if (std::ranges::find(people.ex_spouses[groom.index()], bride) !=
                people.ex_spouses[groom.index()].end() ||
            std::ranges::find(people.ex_spouses[bride.index()], groom) !=
                people.ex_spouses[bride.index()].end()) {
            add(MarriageDenial::PreviouslyMarried);
        }
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
        // Without an explicit policy every house has the same default Orthodox faith.
        if (faith != nullptr && !faith->allows(world, groom, bride)) {
            add(MarriageDenial::FaithMismatch);
        }
    }
    if (out.size() > 1) {
        std::ranges::sort(out);
        const auto duplicates = std::ranges::unique(out);
        out.erase(duplicates.begin(), duplicates.end());
    }
    return out;
}

}  // namespace

MarriageDenials check_marriage(const World& world,
                               const MarriageRules& rules,
                               CharacterId groom,
                               CharacterId bride,
                               MarriageCheckOptions options,
                               const FaithPolicy* faith,
                               KinResult* kinship_out) {
    const auto kinship = kinship_for_marriage(world, groom, bride);
    if (kinship_out != nullptr) {
        *kinship_out = kinship;
    }
    MarriageDenials out;
    append_person_denials(world, rules, groom, options, out);
    append_person_denials(world, rules, bride, options, out);
    return assemble_denials(world, rules, groom, bride, options, faith, kinship, std::move(out));
}

struct MarriageCheckContext::Impl {
    const World& world;
    const MarriageRules& rules;
    CharacterId member;
    MarriageCheckOptions options;
    const FaithPolicy* faith;
    kin::MarriageQuery kinship;
    std::vector<std::int32_t> ages;
    MarriageDenials member_denials;
    bool member_male;

    Impl(const World& world_in,
         const MarriageRules& rules_in,
         CharacterId member_in,
         MarriageCheckOptions options_in,
         const FaithPolicy* faith_in)
        : world(world_in), rules(rules_in), member(member_in), options(options_in), faith(faith_in),
          kinship(world_in, member_in),
          ages(world_in.characters().ids.capacity(), std::numeric_limits<std::int32_t>::min()),
          member_male(world_in.exists(member_in) &&
                      world_in.characters().sex[member_in.index()] == Sex::Male) {
        append_person_denials(world, rules, member, options, member_denials, age_of(member));
    }

    [[nodiscard]] std::int32_t age_of(CharacterId person) {
        if (!world.exists(person)) {
            return 0;
        }
        auto& age = ages[person.index()];
        if (age == std::numeric_limits<std::int32_t>::min()) {
            age = age_years(world.characters().birth_date[person.index()], world.today());
        }
        return age;
    }
};

MarriageCheckContext::MarriageCheckContext(const World& world,
                                           const MarriageRules& rules,
                                           CharacterId member,
                                           MarriageCheckOptions options,
                                           const FaithPolicy* faith)
    : impl_(std::make_unique<Impl>(world, rules, member, options, faith)) {}

MarriageCheckContext::~MarriageCheckContext() = default;

std::int32_t MarriageCheckContext::age_of(CharacterId person) {
    return impl_->age_of(person);
}

MarriageDenials MarriageCheckContext::check(CharacterId other, KinResult* kinship_out) {
    auto& context = *impl_;
    const auto kinship = context.kinship.between(other);
    if (kinship_out != nullptr) {
        *kinship_out = kinship;
    }
    MarriageDenials out = context.member_denials;
    append_person_denials(context.world, context.rules, other, context.options, out, context.age_of(other));
    return assemble_denials(context.world,
                            context.rules,
                            context.member_male ? context.member : other,
                            context.member_male ? other : context.member,
                            context.options,
                            context.faith,
                            kinship,
                            std::move(out));
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
#define PAT_MARRIAGE_DENIAL_KEY(name, suffix)                                                                \
    case MarriageDenial::name:                                                                               \
        return "marriage.denial." #suffix;
        PAT_MARRIAGE_DENIAL_KEY(NotAlive, not_alive)
        PAT_MARRIAGE_DENIAL_KEY(SamePerson, same_person)
        PAT_MARRIAGE_DENIAL_KEY(SameSex, same_sex)
        PAT_MARRIAGE_DENIAL_KEY(SameHouse, same_house)
        PAT_MARRIAGE_DENIAL_KEY(GroomTooYoung, groom_too_young)
        PAT_MARRIAGE_DENIAL_KEY(BrideTooYoung, bride_too_young)
        PAT_MARRIAGE_DENIAL_KEY(TooYoungToBetroth, too_young_to_betroth)
        PAT_MARRIAGE_DENIAL_KEY(AlreadyMarried, already_married)
        PAT_MARRIAGE_DENIAL_KEY(Betrothed, betrothed)
        PAT_MARRIAGE_DENIAL_KEY(InNegotiation, in_negotiation)
        PAT_MARRIAGE_DENIAL_KEY(Tonsured, tonsured)
        PAT_MARRIAGE_DENIAL_KEY(Disowned, disowned)
        PAT_MARRIAGE_DENIAL_KEY(MarriageLimit, marriage_limit)
        PAT_MARRIAGE_DENIAL_KEY(PreviouslyMarried, previously_married)
        PAT_MARRIAGE_DENIAL_KEY(Mourning, mourning)
        PAT_MARRIAGE_DENIAL_KEY(Pregnant, pregnant)
        PAT_MARRIAGE_DENIAL_KEY(BloodKinship, blood_kinship)
        PAT_MARRIAGE_DENIAL_KEY(BloodKinshipDispensable, blood_kinship_dispensable)
        PAT_MARRIAGE_DENIAL_KEY(Affinity, affinity)
        PAT_MARRIAGE_DENIAL_KEY(SpiritualKinship, spiritual_kinship)
        PAT_MARRIAGE_DENIAL_KEY(FaithMismatch, faith_mismatch)
        PAT_MARRIAGE_DENIAL_KEY(ChurchConsentRequired, church_consent_required)
        PAT_MARRIAGE_DENIAL_KEY(NotYourMember, not_your_member)
        PAT_MARRIAGE_DENIAL_KEY(HouseBusy, house_busy)
        PAT_MARRIAGE_DENIAL_KEY(InsufficientFunds, insufficient_funds)
        PAT_MARRIAGE_DENIAL_KEY(DuplicateNegotiation, duplicate_negotiation)
        PAT_MARRIAGE_DENIAL_KEY(RecentlyRefused, recently_refused)
        PAT_MARRIAGE_DENIAL_KEY(NegotiationNotFound, negotiation_not_found)
        PAT_MARRIAGE_DENIAL_KEY(NotInitiator, not_initiator)
        PAT_MARRIAGE_DENIAL_KEY(NoActingHead, no_acting_head)
        PAT_MARRIAGE_DENIAL_KEY(TableFull, table_full)
        PAT_MARRIAGE_DENIAL_KEY(BetrothalNotFound, betrothal_not_found)
        PAT_MARRIAGE_DENIAL_KEY(NotBetrothalParty, not_betrothal_party)
        PAT_MARRIAGE_DENIAL_KEY(NotMarried, not_married)
        PAT_MARRIAGE_DENIAL_KEY(CannotTonsureHead, cannot_tonsure_head)
        PAT_MARRIAGE_DENIAL_KEY(NotYourSpouse, not_your_spouse)
        PAT_MARRIAGE_DENIAL_KEY(HouseChanged, house_changed)
#undef PAT_MARRIAGE_DENIAL_KEY
    }
    return {};
}

}  // namespace pat::sim
