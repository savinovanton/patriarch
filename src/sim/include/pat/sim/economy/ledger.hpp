#pragma once

#include "pat/sim/economy/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Ledger codes of the economy (E9-T02, docs/dev/economy-model.md section 6).
//
// A ledger code names a line ("statya") of an account's statement. The numbers are STABLE FOREVER:
// they are part of the save format and of the golden hashes. A number that was issued is never
// removed and never changes its meaning; a new line is a new number. Adding a code changes the dense
// layout of the statement (kLedgerLineCount), which needs a state schema bump and a migration (new
// lines are zero).
//
// Sign convention of the lines: income is positive, expense is negative.

namespace pat::sim::economy {

enum class LedgerCode : std::uint16_t {
    // taxes by population group: 1 + Group
    TaxNobility = 1,
    TaxClergy = 2,
    TaxTownsfolk = 3,
    TaxPeasants = 4,
    TaxCossacks = 5,
    TaxNatives = 6,
    // other income
    HoldingIncome = 10,
    Duties = 11,
    SalariesIn = 12,
    GrantsIn = 13,
    Tribute = 14,
    FarmingIncome = 15,
    DowryIn = 16,
    // expenses
    Embezzlement = 100,
    BuildingUpkeep = 101,
    Construction = 102,
    CourtUpkeep = 103,
    SalariesOut = 104,
    ArmyUpkeep = 105,
    GiftsBribes = 106,
    ChurchDonations = 107,
    QuotaMoney = 108,
    Interest = 109,
    RaidLoss = 110,
    DowryOut = 111,
    MarriageEnvoy = 112,
    // transfers between accounts (always in pairs, see kLedgerPairs)
    LoanTaken = 200,
    LoanRepaid = 201,
    LoanIssued = 202,
    LoanCollected = 203,
    QuotaCollected = 204,
    InterestCollected = 205,
    // service line: scenario opening balances, tests, debugging
    Adjustment = 300,
};

/// Every issued code in strictly ascending order; the position in this array is the dense index of
/// the line in the statement.
inline constexpr std::array kLedgerCodes = {
    LedgerCode::TaxNobility,     LedgerCode::TaxClergy,
    LedgerCode::TaxTownsfolk,    LedgerCode::TaxPeasants,
    LedgerCode::TaxCossacks,     LedgerCode::TaxNatives,
    LedgerCode::HoldingIncome,   LedgerCode::Duties,
    LedgerCode::SalariesIn,      LedgerCode::GrantsIn,
    LedgerCode::Tribute,         LedgerCode::FarmingIncome,
    LedgerCode::DowryIn,         LedgerCode::Embezzlement,
    LedgerCode::BuildingUpkeep,  LedgerCode::Construction,
    LedgerCode::CourtUpkeep,     LedgerCode::SalariesOut,
    LedgerCode::ArmyUpkeep,      LedgerCode::GiftsBribes,
    LedgerCode::ChurchDonations, LedgerCode::QuotaMoney,
    LedgerCode::Interest,        LedgerCode::RaidLoss,
    LedgerCode::DowryOut,        LedgerCode::MarriageEnvoy,
    LedgerCode::LoanTaken,       LedgerCode::LoanRepaid,
    LedgerCode::LoanIssued,      LedgerCode::LoanCollected,
    LedgerCode::QuotaCollected,  LedgerCode::InterestCollected,
    LedgerCode::Adjustment,
};

/// Number of lines of one statement.
inline constexpr std::size_t kLedgerLineCount = kLedgerCodes.size();

namespace detail {

inline constexpr std::uint16_t kNoLine = 0xFFFF;

[[nodiscard]] constexpr std::size_t ledger_code_span() noexcept {
    return static_cast<std::size_t>(kLedgerCodes.back()) + 1;
}

[[nodiscard]] constexpr std::array<std::uint16_t, 301> make_ledger_index_table() noexcept {
    std::array<std::uint16_t, 301> table{};
    for (auto& entry : table) {
        entry = kNoLine;
    }
    for (std::size_t i = 0; i < kLedgerCodes.size(); ++i) {
        table[static_cast<std::size_t>(kLedgerCodes[i])] = static_cast<std::uint16_t>(i);
    }
    return table;
}

inline constexpr auto kLedgerIndexTable = make_ledger_index_table();

[[nodiscard]] constexpr bool ledger_codes_ascend() noexcept {
    for (std::size_t i = 1; i < kLedgerCodes.size(); ++i) {
        if (static_cast<std::uint16_t>(kLedgerCodes[i - 1]) >= static_cast<std::uint16_t>(kLedgerCodes[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

static_assert(detail::ledger_codes_ascend(), "kLedgerCodes must ascend: the dense index follows the code");
static_assert(detail::ledger_code_span() == detail::kLedgerIndexTable.size(),
              "the index table must cover exactly the issued codes");

/// Dense index of an issued code (its position in kLedgerCodes); nullopt for a number that was never
/// issued.
[[nodiscard]] constexpr std::optional<std::size_t> ledger_index(LedgerCode code) noexcept {
    const auto value = static_cast<std::size_t>(code);
    if (value >= detail::kLedgerIndexTable.size() || detail::kLedgerIndexTable[value] == detail::kNoLine) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(detail::kLedgerIndexTable[value]);
}

[[nodiscard]] constexpr bool is_issued(LedgerCode code) noexcept {
    return ledger_index(code).has_value();
}

/// The code of a given dense index (the index must be below kLedgerLineCount).
[[nodiscard]] constexpr LedgerCode ledger_code_at(std::size_t index) noexcept {
    return kLedgerCodes[index];
}

/// Tax line of a population group (code = 1 + group).
[[nodiscard]] constexpr LedgerCode tax_code(Group group) noexcept {
    return static_cast<LedgerCode>(1 + static_cast<std::uint16_t>(group));
}

/// A pair of codes for a transfer between two accounts: the sum of both lines over all accounts is
/// zero in every month (invariant E-M3, economy-model sections 6.2 and 12).
struct LedgerPair {
    LedgerCode out;  ///< the sender's line (negative)
    LedgerCode in;   ///< the receiver's line (positive)
};

inline constexpr std::array kLedgerPairs = {
    LedgerPair{LedgerCode::LoanIssued, LedgerCode::LoanTaken},        // issue: crown -> dynasty
    LedgerPair{LedgerCode::LoanRepaid, LedgerCode::LoanCollected},    // repayment: dynasty -> crown
    LedgerPair{LedgerCode::QuotaMoney, LedgerCode::QuotaCollected},   // money quota: dynasty -> crown
    LedgerPair{LedgerCode::Interest, LedgerCode::InterestCollected},  // interest: dynasty -> crown
    LedgerPair{LedgerCode::DowryOut, LedgerCode::DowryIn},            // dowry: bride's house -> groom's
};

[[nodiscard]] constexpr bool is_ledger_pair(LedgerCode out, LedgerCode in) noexcept {
    for (const LedgerPair& pair : kLedgerPairs) {
        if (pair.out == out && pair.in == in) {
            return true;
        }
    }
    return false;
}

/// True for the codes that belong to a transfer pair. They are posted only by World::transfer: a
/// single posting on one of them would break the pair's balance.
[[nodiscard]] constexpr bool is_transfer_code(LedgerCode code) noexcept {
    for (const LedgerPair& pair : kLedgerPairs) {
        if (pair.out == code || pair.in == code) {
            return true;
        }
    }
    return false;
}

}  // namespace pat::sim::economy
