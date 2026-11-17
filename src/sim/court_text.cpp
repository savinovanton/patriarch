#include "pat/sim/court_text.hpp"

#include <array>
#include <cstddef>

namespace pat::sim {

namespace {

struct DenialName {
    CourtDenial code;
    std::string_view key;
};

constexpr std::array kDenials{
    DenialName{CourtDenial::NotAlive, "court.denial.not_alive"},
    DenialName{CourtDenial::TooYoung, "court.denial.too_young"},
    DenialName{CourtDenial::SexNotAllowed, "court.denial.sex_not_allowed"},
    DenialName{CourtDenial::RankTooLow, "court.denial.rank_too_low"},
    DenialName{CourtDenial::SkillTooLow, "court.denial.skill_too_low"},
    DenialName{CourtDenial::ForbiddenTrait, "court.denial.forbidden_trait"},
    DenialName{CourtDenial::Tonsured, "court.denial.tonsured"},
    DenialName{CourtDenial::Disowned, "court.denial.disowned"},
    DenialName{CourtDenial::Disgraced, "court.denial.disgraced"},
    DenialName{CourtDenial::FaithMismatch, "court.denial.faith_mismatch"},
    DenialName{CourtDenial::AlreadyHoldsOffice, "court.denial.already_holds_office"},
    DenialName{CourtDenial::NotInHouse, "court.denial.not_in_house"},
    DenialName{CourtDenial::ChildHead, "court.denial.child_head"},
    DenialName{CourtDenial::NoSuchOfficeType, "court.denial.no_such_office_type"},
    DenialName{CourtDenial::NoSuchSeat, "court.denial.no_such_seat"},
    DenialName{CourtDenial::SeatOccupied, "court.denial.seat_occupied"},
    DenialName{CourtDenial::SeatNotVacant, "court.denial.seat_not_vacant"},
    DenialName{CourtDenial::NotAppointer, "court.denial.not_appointer"},
    DenialName{CourtDenial::WrongAppointerKind, "court.denial.wrong_appointer_kind"},
    DenialName{CourtDenial::DateWindowClosed, "court.denial.date_window_closed"},
    DenialName{CourtDenial::NoEmperor, "court.denial.no_emperor"},
    DenialName{CourtDenial::PetitionLimit, "court.denial.petition_limit"},
    DenialName{CourtDenial::PetitionDuplicate, "court.denial.petition_duplicate"},
    DenialName{CourtDenial::GiftInvalid, "court.denial.gift_invalid"},
    DenialName{CourtDenial::CannotAfford, "court.denial.cannot_afford"},
    DenialName{CourtDenial::NoServiceCredit, "court.denial.no_service_credit"},
    DenialName{CourtDenial::NoParty, "court.denial.no_party"},
    DenialName{CourtDenial::PartySupportCooldown, "court.denial.party_support_cooldown"},
    DenialName{CourtDenial::PetitionNotFound, "court.denial.petition_not_found"},
    DenialName{CourtDenial::NotPetitioner, "court.denial.not_petitioner"},
    DenialName{CourtDenial::PetitionCapWorld, "court.denial.petition_cap_world"},
    DenialName{CourtDenial::NoSuchRole, "court.denial.no_such_role"},
    DenialName{CourtDenial::RoleOccupied, "court.denial.role_occupied"},
    DenialName{CourtDenial::MarketIndexInvalid, "court.denial.market_index_invalid"},
    DenialName{CourtDenial::ServantNotFound, "court.denial.servant_not_found"},
    DenialName{CourtDenial::NotYourServant, "court.denial.not_your_servant"},
    DenialName{CourtDenial::WorldServantCap, "court.denial.world_servant_cap"},
    DenialName{CourtDenial::GiftNonPositive, "court.denial.gift_non_positive"},
    DenialName{CourtDenial::NoEconomy, "court.denial.no_economy"},
    DenialName{CourtDenial::NotInDynastyMode, "court.denial.not_in_dynasty_mode"},
    DenialName{CourtDenial::InternalOnly, "court.denial.internal_only"},
    DenialName{CourtDenial::NoCourtRules, "court.denial.no_court_rules"},
};

struct TermName {
    std::uint16_t code;
    std::string_view key;
};

// One row per Breakdown code of section 9; the string is the code itself.
constexpr std::array kTerms{
    TermName{14000, "court.term.t14000"}, TermName{14001, "court.term.t14001"},
    TermName{14002, "court.term.t14002"}, TermName{14003, "court.term.t14003"},
    TermName{14004, "court.term.t14004"}, TermName{14005, "court.term.t14005"},
    TermName{14006, "court.term.t14006"}, TermName{14007, "court.term.t14007"},
    TermName{14008, "court.term.t14008"}, TermName{14009, "court.term.t14009"},
    TermName{14010, "court.term.t14010"}, TermName{14011, "court.term.t14011"},
    TermName{14100, "court.term.t14100"}, TermName{14101, "court.term.t14101"},
    TermName{14102, "court.term.t14102"}, TermName{14200, "court.term.t14200"},
    TermName{14201, "court.term.t14201"}, TermName{14202, "court.term.t14202"},
    TermName{14203, "court.term.t14203"}, TermName{14204, "court.term.t14204"},
    TermName{14205, "court.term.t14205"}, TermName{14206, "court.term.t14206"},
    TermName{14207, "court.term.t14207"}, TermName{14300, "court.term.t14300"},
    TermName{14301, "court.term.t14301"}, TermName{14302, "court.term.t14302"},
    TermName{14400, "court.term.t14400"}, TermName{14401, "court.term.t14401"},
    TermName{14402, "court.term.t14402"}, TermName{14403, "court.term.t14403"},
    TermName{14404, "court.term.t14404"}, TermName{14500, "court.term.t14500"},
    TermName{14501, "court.term.t14501"}, TermName{14502, "court.term.t14502"},
    TermName{14600, "court.term.t14600"}, TermName{14601, "court.term.t14601"},
    TermName{14602, "court.term.t14602"},
};

constexpr std::array<std::string_view, 7> kVacancyReasons{"court.reason.vacancy.new_seat",
                                                          "court.reason.vacancy.holder_died",
                                                          "court.reason.vacancy.resigned",
                                                          "court.reason.vacancy.dismissed",
                                                          "court.reason.vacancy.tonsured",
                                                          "court.reason.vacancy.ineligible",
                                                          "court.reason.vacancy.reform"};
constexpr std::array<std::string_view, 5> kResignReasons{"court.reason.resign.voluntary",
                                                         "court.reason.resign.holder_died",
                                                         "court.reason.resign.tonsured",
                                                         "court.reason.resign.ineligible",
                                                         "court.reason.resign.seat_closed"};
constexpr std::array<std::string_view, 4> kDismissReasons{"court.reason.dismiss.emperor_will",
                                                          "court.reason.dismiss.disgrace",
                                                          "court.reason.dismiss.purge",
                                                          "court.reason.dismiss.event"};
constexpr std::array<std::string_view, 3> kWithdrawReasons{"court.reason.withdraw.voluntary",
                                                           "court.reason.withdraw.candidate_gone",
                                                           "court.reason.withdraw.vacancy_closed"};
constexpr std::array<std::string_view, 5> kFireReasons{"court.reason.fire.dismissed",
                                                       "court.reason.fire.quit",
                                                       "court.reason.fire.house_extinct",
                                                       "court.reason.fire.betrayed",
                                                       "court.reason.fire.retired"};

constexpr std::array<std::string_view, 10> kChronicle{"chronicle.court.assigned",
                                                      "chronicle.court.resigned",
                                                      "chronicle.court.dismissed",
                                                      "chronicle.court.disgraced",
                                                      "chronicle.court.disgrace_ended",
                                                      "chronicle.court.vacancy_opened",
                                                      "chronicle.court.decision_postponed",
                                                      "chronicle.court.petition_rejected",
                                                      "chronicle.court.servant_hired",
                                                      "chronicle.court.servant_left"};

template <std::size_t N>
std::string_view pick(const std::array<std::string_view, N>& keys, std::uint8_t reason) noexcept {
    return reason >= 1 && reason <= N ? keys[reason - 1U] : std::string_view{};
}

}  // namespace

std::string_view court_denial_key(CourtDenial denial) noexcept {
    for (const DenialName& entry : kDenials) {
        if (entry.code == denial) {
            return entry.key;
        }
    }
    return {};
}

std::string_view court_term_key(std::uint16_t code) noexcept {
    for (const TermName& entry : kTerms) {
        if (entry.code == code) {
            return entry.key;
        }
    }
    return {};
}

std::string_view court_reason_key(CourtReasonKind kind, std::uint8_t reason) noexcept {
    switch (kind) {
        case CourtReasonKind::Vacancy:
            return pick(kVacancyReasons, reason);
        case CourtReasonKind::Resign:
            return pick(kResignReasons, reason);
        case CourtReasonKind::Dismiss:
            return pick(kDismissReasons, reason);
        case CourtReasonKind::Withdraw:
            return pick(kWithdrawReasons, reason);
        case CourtReasonKind::Fire:
            return pick(kFireReasons, reason);
    }
    return {};
}

std::string_view court_chronicle_key(std::uint16_t param) noexcept {
    if (param < 3100 || param >= 4100) {
        return {};
    }
    const auto kind = static_cast<std::size_t>((param - 3000) / 100);  // 1...10
    return kind <= kChronicle.size() ? kChronicle[kind - 1] : std::string_view{};
}

}  // namespace pat::sim
