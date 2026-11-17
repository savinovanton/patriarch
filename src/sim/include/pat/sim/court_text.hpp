#pragma once

#include <cstdint>
#include <string_view>

// Court presentation metadata (E10-T04, docs/dev/court-model.md sections 7.3, 9, 10.3): stable codes
// and the localisation keys of data/loc/ru/court.pd. The functions return an empty view for a code
// they do not know. The codes are written into command results, the chronicle and saves and never
// change meaning.

namespace pat::sim {

/// Reasons a court command or a check refuses (section 7.3); 0 is "no reason".
enum class CourtDenial : std::uint16_t {
    None = 0,
    NotAlive = 1,
    TooYoung = 2,
    SexNotAllowed = 3,
    RankTooLow = 4,
    SkillTooLow = 5,
    ForbiddenTrait = 6,
    Tonsured = 7,
    Disowned = 8,
    Disgraced = 9,
    FaithMismatch = 10,
    AlreadyHoldsOffice = 11,
    NotInHouse = 12,
    ChildHead = 13,
    NoSuchOfficeType = 20,
    NoSuchSeat = 21,
    SeatOccupied = 22,
    SeatNotVacant = 23,
    NotAppointer = 24,
    WrongAppointerKind = 25,
    DateWindowClosed = 26,
    NoEmperor = 27,
    PetitionLimit = 40,
    PetitionDuplicate = 41,
    GiftInvalid = 42,
    CannotAfford = 43,
    NoServiceCredit = 44,
    NoParty = 45,
    PartySupportCooldown = 46,
    PetitionNotFound = 47,
    NotPetitioner = 48,
    PetitionCapWorld = 49,
    NoSuchRole = 60,
    RoleOccupied = 61,
    MarketIndexInvalid = 62,
    ServantNotFound = 63,
    NotYourServant = 64,
    WorldServantCap = 65,
    GiftNonPositive = 80,
    NoEconomy = 81,
    NotInDynastyMode = 90,
    InternalOnly = 91,
    NoCourtRules = 92,
};

/// `court.denial.<name>` of a denial code.
[[nodiscard]] std::string_view court_denial_key(CourtDenial denial) noexcept;

/// `court.term.<code>` of a Breakdown code of the court families 14000...14600 (section 9).
[[nodiscard]] std::string_view court_term_key(std::uint16_t code) noexcept;

/// Command reason lists of section 7.1 and the vacancy reasons of section 4.4 (`court.reason.*`).
enum class CourtReasonKind : std::uint8_t { Vacancy, Resign, Dismiss, Withdraw, Fire };
/// `court.reason.<kind>.<name>`; reasons count from 1.
[[nodiscard]] std::string_view court_reason_key(CourtReasonKind kind, std::uint8_t reason) noexcept;

/// `chronicle.court.<name>` for ChronicleKind::Note param `3000 + 100 * kind + office slot`.
[[nodiscard]] std::string_view court_chronicle_key(std::uint16_t param) noexcept;

}  // namespace pat::sim
