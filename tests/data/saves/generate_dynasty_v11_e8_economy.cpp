// Generate once with main a1c3e69, before E8-T03 changes the world schema or the ledger layout.
// Reproduction and the captured hashes: generate_dynasty_v11_e8_economy.md.

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/world.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

static_assert(World::kStateSchema == 11, "This generator must use the pre-E8-T03 code");
static_assert(kSaveFormatVersion == 2);
static_assert(kLedgerLineCount == 31, "Do not regenerate using the extended E8 ledger layout");

namespace {

bool checked(bool ok, const char* operation) {
    if (!ok) {
        std::fprintf(stderr, "failed: %s\n", operation);
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: generate_dynasty_v11_e8_economy <output.psv>\n");
        return 2;
    }

    World world(Date::from_ymd(1703, 1, 1), 298);
    if (!checked(world.enable_dynasty_mode(), "enable dynasty mode")) {
        return 3;
    }
    for (std::uint32_t i = 0; i < 3; ++i) {
        const DynastyId house = world.create_dynasty(StringId(100 + i));
        const ProvinceId province =
            world.create_province(StringId(200 + i), house, Fixed32::from_int(1000 + 100 * i));
        CharacterInit init;
        init.dynasty = house;
        init.birth_date = Date::from_ymd(1660 + static_cast<int>(i), 5, 10);
        init.name = StringId(300 + i);
        const CharacterId head = world.create_character(init);
        if (!checked(province.valid() && head.valid() && world.set_dynasty_head(house, head),
                     "create province and dynasty head")) {
            return 3;
        }
    }
    if (!checked(world.enable_economy(), "enable economy")) {
        return 3;
    }
    const AccountRef crown = AccountRef::crown();
    const AccountRef borrower = AccountRef::of(DynastyId(0));
    const AccountRef second = AccountRef::of(DynastyId(1));
    const AccountRef third = AccountRef::of(DynastyId(2));
    if (!checked(world.set_initial_treasury(crown, 2'000'000) &&
                     world.set_initial_treasury(borrower, 100'000) &&
                     world.set_initial_treasury(second, 50'000) && world.set_initial_treasury(third, 75'000),
                 "opening balances")) {
        return 3;
    }
    const EconomyRules rules = EconomyRules::defaults();
    const auto loan = world.take_loan(DynastyId(0), 50'000, rules.loans);
    if (!checked(loan.has_value(), "take crown loan")) {
        return 3;
    }
    if (!checked(world.post_money(borrower, LedgerCode::TaxPeasants, 12'345) == 12'345 &&
                     world.post_money(borrower, LedgerCode::BuildingUpkeep, -2'345) == -2'345 &&
                     world.post_money(second, LedgerCode::FarmingIncome, 7'777) == 7'777 &&
                     world.post_money(second, LedgerCode::ArmyUpkeep, -777) == -777 &&
                     world.transfer(
                         borrower, crown, LedgerCode::QuotaMoney, LedgerCode::QuotaCollected, 3'210) == 3'210,
                 "January postings")) {
        return 3;
    }
    const Date february = Date::from_ymd(1703, 2, 1);
    while (world.today() < february) {
        (void)world.advance_day();
    }
    if (!checked(world.close_ledger_month(february), "close January") ||
        !checked(world.post_money(borrower, LedgerCode::CourtUpkeep, -1'999) == -1'999 &&
                     world.post_money(borrower, LedgerCode::TaxTownsfolk, 2'333) == 2'333 &&
                     world.repay_loan(*loan, 5'000, rules.loans).has_value(),
                 "February postings and partial repayment")) {
        return 3;
    }
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        std::fprintf(stderr, "%s\n", problem.c_str());
    }
    if (!problems.empty()) {
        return 4;
    }

    const WorldHash original = world.compute_hash();
    SaveSummary summary = summary_from_world(world);
    summary.scenario = "schema11.e8.economy.fixture";
    summary.player_dynasty = 0;
    summary.treasury = world.treasury(borrower);
    summary.name = "Schema 11 before E8-T03";
    const auto bytes = serialize_save(world, {}, summary);
    auto loaded = deserialize_save(bytes);
    if (!checked(loaded.has_value(), "read fixture with original schema-11 reader") ||
        !checked(loaded.value().world.compute_hash() == original, "original full world hash") ||
        !checked(loaded.value().world.economy() == world.economy(), "original economy equality")) {
        return 4;
    }
    const auto written = write_file_atomic(std::filesystem::path(argv[1]), bytes);
    if (!checked(written.has_value(), "write fixture")) {
        return 5;
    }
    std::printf("source=a1c3e69 schema=%u container=%u ledger_lines=%zu bytes=%zu\n",
                World::kStateSchema,
                static_cast<unsigned>(kSaveFormatVersion),
                kLedgerLineCount,
                bytes.size());
    std::printf("total=%" PRIu64 " calendar=%" PRIu64 " rng=%" PRIu64 " dynasties=%" PRIu64
                " provinces=%" PRIu64 " characters=%" PRIu64 "\n",
                original.total,
                original.calendar,
                original.rng,
                original.dynasties,
                original.provinces,
                original.characters);
    for (std::size_t i = 0; i < original.ext.size(); ++i) {
        if (original.ext[i] != 0) {
            std::printf("ext[%zu]=%" PRIu64 "\n", i, original.ext[i]);
        }
    }
    std::printf("borrower_treasury=%" PRId64 " crown_treasury=%" PRId64
                " closed_months=%u loans=%zu loan_id=%u\n",
                world.treasury(borrower),
                world.treasury(crown),
                static_cast<unsigned>(world.economy().meta.closed_months),
                world.economy().loans.size(),
                *loan);
    return 0;
}
