#include "pat/sim/economy/loans.hpp"

#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>

namespace pat::sim::economy {

namespace {

std::optional<Date> loan_due_date(Date issued, std::int32_t term_months) noexcept {
    if (!is_sane_date(issued) || term_months < 1 || term_months > 600) {
        return std::nullopt;
    }
    const auto date = issued.ymd();
    const int month_index = date.year * 12 + date.month - 1 + term_months;
    int year = month_index / 12;
    int month = month_index % 12;
    if (month < 0) {
        --year;
        month += 12;
    }
    const Date due = Date::from_ymd(year, month + 1, 1);
    return is_sane_date(due) ? std::optional<Date>(due) : std::nullopt;
}

bool exact_posting_fits(const AccountTable& accounts,
                        std::size_t row,
                        LedgerCode code,
                        Kopecks amount) noexcept {
    if (amount < -kMaxPostingKop || amount > kMaxPostingKop ||
        apply_posting(accounts.treasury(row), amount).applied != amount) {
        return false;
    }
    const auto line = checked_add(accounts.open_line(row, code), amount);
    return line && *line >= -kMaxLedgerLineKop && *line <= kMaxLedgerLineKop;
}

bool exact_transfer_fits(const World& world,
                         AccountRef from,
                         AccountRef to,
                         LedgerCode out,
                         LedgerCode in,
                         Kopecks amount) noexcept {
    const auto from_row = world.resolve_account(from);
    const auto to_row = world.resolve_account(to);
    return from_row && to_row && *from_row != *to_row &&
           exact_posting_fits(world.economy().accounts, *from_row, out, -amount) &&
           exact_posting_fits(world.economy().accounts, *to_row, in, amount);
}

bool refresh_bankruptcy(World& world, const LoanRules& rules, DynastyId dynasty) {
    const Kopecks debt = dynasty_debt(world, dynasty);
    auto flags = world.economy_columns_mut().bankrupt;
    const std::size_t row = dynasty.index();
    if (flags[row] != 0) {
        if (debt == 0 || debt < rules.bankruptcy_exit_kop) {
            flags[row] = 0;
        }
        return false;
    }
    if (debt > 0 && debt >= rules.bankruptcy_threshold_kop) {
        flags[row] = 1;
        world.add_honor(dynasty, -rules.bankruptcy_penalty_honor, kStandingReasonBankruptcy);
        (void)world.log_incident(IncidentKind::Bankruptcy, ProvinceId(), dynasty, 0);
        return true;
    }
    return false;
}

}  // namespace

Kopecks dynasty_debt(const World& world, DynastyId dynasty) noexcept {
    if (!world.economy_present() || !world.exists(dynasty)) {
        return 0;
    }
    const LoanTable& loans = world.economy().loans;
    Kopecks debt = 0;
    for (std::size_t row = 0; row < loans.size(); ++row) {
        if (loans.borrower[row] == dynasty) {
            debt = saturating_add(debt, loans.principal[row]);
        }
    }
    return debt;
}

Kopecks
loan_limit(const World& world, const LoanRules& rules, DynastyId dynasty, Breakdown* breakdown) noexcept {
    Kopecks base = 0;
    Kopecks honor = 0;
    Kopecks debt = 0;
    if (world.economy_present() && world.exists(dynasty)) {
        base = rules.base_limit_kop;
        honor = saturating_mul(rules.per_honor_kop, std::max(0, world.dynasties().honor[dynasty.index()]));
        debt = dynasty_debt(world, dynasty);
    }
    const Kopecks before_clamp = saturating_sub(saturating_add(base, honor), debt);
    const Kopecks limit = std::max<Kopecks>(0, before_clamp);
    if (breakdown != nullptr) {
        breakdown->begin(term::kLoanTotal, Unit::Kopecks, limit);
        (void)breakdown->add_term(kRootNode, term::kLoanBase, base);
        (void)breakdown->add_term(kRootNode, term::kLoanHonor, honor);
        (void)breakdown->add_term(kRootNode, term::kLoanDebt, -debt);
        if (before_clamp != limit) {
            (void)breakdown->add_step(kRootNode, term::kLoanClamp, before_clamp, limit);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the loan limit terms must add up to the available limit");
    }
    return limit;
}

std::optional<std::size_t> find_loan(const World& world, std::uint32_t loan_id) noexcept {
    if (!world.economy_present()) {
        return std::nullopt;
    }
    const auto& ids = world.economy().loans.loan_id;
    const auto position = std::lower_bound(ids.begin(), ids.end(), loan_id);
    return position != ids.end() && *position == loan_id
               ? std::optional<std::size_t>(static_cast<std::size_t>(position - ids.begin()))
               : std::nullopt;
}

LoanStatus
check_take_loan(const World& world, const LoanRules& rules, DynastyId borrower, Kopecks amount_kop) noexcept {
    if (!world.economy_present()) {
        return LoanStatus::NoEconomy;
    }
    if (!world.exists(borrower)) {
        return LoanStatus::UnknownDynasty;
    }
    if (amount_kop <= 0 || amount_kop > kMaxPostingKop) {
        return LoanStatus::InvalidAmount;
    }
    const auto& economy = world.economy();
    if (economy.dynasties.bankrupt[borrower.index()] != 0) {
        return LoanStatus::Bankrupt;
    }
    if (amount_kop > loan_limit(world, rules, borrower)) {
        return LoanStatus::LimitExceeded;
    }
    const auto count = std::count(economy.loans.borrower.begin(), economy.loans.borrower.end(), borrower);
    if (count >= rules.max_loans_per_dynasty) {
        return LoanStatus::TooManyLoans;
    }
    if (rules.rate_ppm_year < 0 || rules.rate_ppm_year > 1'000'000 ||
        economy.meta.next_loan_id == std::numeric_limits<std::uint32_t>::max() ||
        !loan_due_date(world.today(), rules.term_months) ||
        !exact_transfer_fits(world,
                             AccountRef::crown(),
                             AccountRef::of(borrower),
                             LedgerCode::LoanIssued,
                             LedgerCode::LoanTaken,
                             amount_kop)) {
        return LoanStatus::InvalidState;
    }
    return LoanStatus::Ok;
}

LoanStatus check_repay_loan(const World& world, std::uint32_t loan_id, Kopecks amount_kop) noexcept {
    if (!world.economy_present()) {
        return LoanStatus::NoEconomy;
    }
    const auto row = find_loan(world, loan_id);
    if (!row) {
        return LoanStatus::UnknownLoan;
    }
    const LoanTable& loans = world.economy().loans;
    if (amount_kop <= 0 || amount_kop > kMaxPostingKop || amount_kop > loans.principal[*row]) {
        return LoanStatus::InvalidAmount;
    }
    const AccountRef borrower = AccountRef::of(loans.borrower[*row]);
    if (world.treasury(borrower) < amount_kop) {
        return LoanStatus::InsufficientFunds;
    }
    if (!exact_transfer_fits(world,
                             borrower,
                             AccountRef::crown(),
                             LedgerCode::LoanRepaid,
                             LedgerCode::LoanCollected,
                             amount_kop)) {
        return LoanStatus::InvalidState;
    }
    return LoanStatus::Ok;
}

LoanInterest
compute_loan_interest(Kopecks principal, std::int32_t rate_ppm_year, std::int32_t carry) noexcept {
    if (principal < 0 || principal > kMaxTreasuryKop || rate_ppm_year < 0 || rate_ppm_year > 1'000'000 ||
        carry < 0 || carry > kMaxInterestCarry) {
        return {};
    }
    constexpr Kopecks kDenominator = kInterestCarryDenominator;
    // The wide quotient handles principal * rate up to 10^21. Its remainder needs only the
    // principal modulo the denominator, so the remaining multiplication fits in int64.
    const Kopecks quotient = checked_mul_div(principal, rate_ppm_year, kDenominator).value_or(0);
    const Kopecks remainder = (principal % kDenominator) * rate_ppm_year % kDenominator + carry;
    return {quotient + remainder / kDenominator, static_cast<std::int32_t>(remainder % kDenominator)};
}

}  // namespace pat::sim::economy

namespace pat::sim {

std::optional<std::uint32_t>
World::take_loan(DynastyId borrower, economy::Kopecks amount_kop, const economy::LoanRules& rules) {
    if (economy::check_take_loan(*this, rules, borrower, amount_kop) != economy::LoanStatus::Ok) {
        return std::nullopt;
    }
    economy::LoanTable& loans = economy_.loans;
    const std::uint32_t id = economy_.meta.next_loan_id++;
    loans.loan_id.push_back(id);
    loans.borrower.push_back(borrower);
    loans.principal.push_back(amount_kop);
    loans.rate_ppm_year.push_back(rules.rate_ppm_year);
    loans.issued.push_back(today());
    loans.due.push_back(*economy::loan_due_date(today(), rules.term_months));
    loans.interest_carry.push_back(0);
    loans.overdue.push_back(0);
    loans.collateral.emplace_back();
    const economy::Kopecks moved = transfer(economy::AccountRef::crown(),
                                            economy::AccountRef::of(borrower),
                                            economy::LedgerCode::LoanIssued,
                                            economy::LedgerCode::LoanTaken,
                                            amount_kop);
    PAT_ASSERT(moved == amount_kop, "a validated loan must transfer its full principal");
    (void)moved;
    (void)economy::refresh_bankruptcy(*this, rules, borrower);
    return id;
}

std::optional<economy::Kopecks>
World::repay_loan(std::uint32_t loan_id, economy::Kopecks amount_kop, const economy::LoanRules& rules) {
    if (economy::check_repay_loan(*this, loan_id, amount_kop) != economy::LoanStatus::Ok) {
        return std::nullopt;
    }
    const std::size_t row = *economy::find_loan(*this, loan_id);
    economy::LoanTable& loans = economy_.loans;
    const DynastyId borrower = loans.borrower[row];
    const economy::Kopecks moved = transfer(economy::AccountRef::of(borrower),
                                            economy::AccountRef::crown(),
                                            economy::LedgerCode::LoanRepaid,
                                            economy::LedgerCode::LoanCollected,
                                            amount_kop);
    PAT_ASSERT(moved == amount_kop, "a validated repayment must transfer the full amount");
    loans.principal[row] -= moved;
    if (loans.principal[row] == 0) {
        const auto erase_row = [row](auto& column) {
            column.erase(column.begin() + static_cast<std::ptrdiff_t>(row));
        };
        erase_row(loans.loan_id);
        erase_row(loans.borrower);
        erase_row(loans.principal);
        erase_row(loans.rate_ppm_year);
        erase_row(loans.issued);
        erase_row(loans.due);
        erase_row(loans.interest_carry);
        erase_row(loans.overdue);
        erase_row(loans.collateral);
    }
    (void)economy::refresh_bankruptcy(*this, rules, borrower);
    return moved;
}

economy::LoanSettlementReport World::settle_loans(const economy::LoanRules& rules, Date month_start) {
    economy::LoanSettlementReport report;
    if (!economy_present() || !is_sane_date(month_start) || month_start.day() != 1 ||
        month_start <= economy_.meta.open_month_start) {
        return report;
    }
    economy::LoanTable& loans = economy_.loans;
    for (std::size_t row = 0; row < loans.size(); ++row) {
        if (loans.issued[row] >= month_start || loans.principal[row] == 0) {
            continue;
        }
        const DynastyId borrower = loans.borrower[row];
        const auto interest = economy::compute_loan_interest(
            loans.principal[row], loans.rate_ppm_year[row], loans.interest_carry[row]);
        loans.interest_carry[row] = interest.carry;
        economy::Kopecks unpaid = interest.amount_kop;
        while (unpaid > 0) {
            const economy::Kopecks chunk = std::min(unpaid, economy::kMaxPostingKop);
            const economy::Kopecks paid = transfer(economy::AccountRef::of(borrower),
                                                   economy::AccountRef::crown(),
                                                   economy::LedgerCode::Interest,
                                                   economy::LedgerCode::InterestCollected,
                                                   chunk);
            report.interest_kop = economy::saturating_add(report.interest_kop, paid);
            unpaid -= paid;
            if (paid != chunk) {
                break;
            }
        }
        report.refused_kop = economy::saturating_add(report.refused_kop, unpaid);
        if (month_start >= loans.due[row] && loans.overdue[row] == 0) {
            loans.overdue[row] = 1;
            add_honor(borrower, -rules.overdue_penalty_honor, kStandingReasonLoanOverdue);
            (void)log_incident(economy::IncidentKind::LoanOverdue,
                               ProvinceId(),
                               borrower,
                               static_cast<std::int32_t>(std::min<std::uint32_t>(
                                   loans.loan_id[row], std::numeric_limits<std::int32_t>::max())));
            ++report.newly_overdue;
        }
    }
    for (std::size_t row = 0; row < dynasties_.size(); ++row) {
        report.newly_bankrupt +=
            economy::refresh_bankruptcy(*this, rules, DynastyId(static_cast<std::uint32_t>(row))) ? 1U : 0U;
    }
    return report;
}

}  // namespace pat::sim
