#pragma once

#include "pat/sim/date.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/entity_ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Why a loan operation cannot be applied. These checks never change the world.
enum class LoanStatus : std::uint8_t {
    Ok,
    NoEconomy,
    UnknownDynasty,
    UnknownLoan,
    InvalidAmount,
    LimitExceeded,
    TooManyLoans,
    Bankrupt,
    InsufficientFunds,
    InvalidState,
};

/// The outstanding principal of all the dynasty's loans; zero without economy or for an unknown house.
[[nodiscard]] Kopecks dynasty_debt(const World& world, DynastyId dynasty) noexcept;

/// F13: max(0, base + per_honor * max(0, honor) - outstanding principal).
/// The optional additive breakdown also includes the clamp at zero.
[[nodiscard]] Kopecks loan_limit(const World& world,
                                 const LoanRules& rules,
                                 DynastyId dynasty,
                                 Breakdown* breakdown = nullptr) noexcept;

/// Row of a stable loan id, or nullopt when the loan has been fully repaid or never existed.
[[nodiscard]] std::optional<std::size_t> find_loan(const World& world, std::uint32_t loan_id) noexcept;

[[nodiscard]] LoanStatus
check_take_loan(const World& world, const LoanRules& rules, DynastyId borrower, Kopecks amount_kop) noexcept;
[[nodiscard]] LoanStatus
check_repay_loan(const World& world, std::uint32_t loan_id, Kopecks amount_kop) noexcept;

struct LoanInterest {
    Kopecks amount_kop = 0;
    std::int32_t carry = 0;
};

/// Exact monthly F12: (principal * annual_rate_ppm + carry) / 12,000,000, retaining the remainder.
/// Dividing the annual numerator only after adding the carry avoids losing kopecks over long runs.
/// Valid inputs are principal in 0..kMaxTreasuryKop, rate in 0..1,000,000 and carry in
/// 0..kMaxInterestCarry. Invalid inputs return zero, without overflowing an intermediate.
[[nodiscard]] LoanInterest
compute_loan_interest(Kopecks principal, std::int32_t rate_ppm_year, std::int32_t carry) noexcept;

struct LoanSettlementReport {
    Kopecks interest_kop = 0;
    Kopecks refused_kop = 0;
    std::size_t newly_overdue = 0;
    std::size_t newly_bankrupt = 0;
};

}  // namespace pat::sim::economy
