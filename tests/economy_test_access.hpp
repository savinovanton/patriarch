#pragma once

#include "pat/sim/economy/tables.hpp"

#include <array>
#include <vector>

namespace pat::sim::economy {

/// Tests damage the private columns of the account table on purpose to prove that check_invariants()
/// and the loader notice (declared a friend in tables.hpp). One definition for all test files.
struct AccountTableTestAccess {
    static std::vector<Kopecks>& treasury(AccountTable& t) { return t.treasury_; }
    static std::vector<Kopecks>& opening(AccountTable& t) { return t.opening_; }
    static std::vector<Kopecks>& open_lines(AccountTable& t) { return t.open_lines_; }
    static std::vector<Kopecks>& ring_opening(AccountTable& t) { return t.ring_opening_; }
    static std::vector<Kopecks>& ring_lines(AccountTable& t) { return t.ring_lines_; }
    static std::array<Date, kLedgerRingMonths>& ring_month_start(AccountTable& t) {
        return t.ring_month_start_;
    }
};

}  // namespace pat::sim::economy
