// Saving, loading, hashing and migrating the economy (E9-T03, docs/dev/economy-model.md section 13,
// ADR-0006 D6/D7): the fixtures of the earlier schemas, the round trip and the continuation of a
// loaded simulation, the hash part of the economy (every column, every table, independent of the way
// the state was reached), the sections of the file and what the loader does with damaged ones, the
// invariants that run on a loaded economy, and replays.

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/system.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "economy_test_access.hpp"

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

using Access = AccountTableTestAccess;

std::filesystem::path fixture_path(const char* name) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / name;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& p : problems) {
        MESSAGE(p);
    }
    REQUIRE(problems.empty());
}

bool mentions(const std::vector<std::string>& problems, const char* text) {
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) {
        return p.find(text) != std::string::npos;
    });
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

pat::core::Result<World, pat::core::Error> load(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    auto result = World::load_state(in);
    if (result.has_value() && !in.at_end()) {
        return pat::core::Result<World, pat::core::Error>::err(pat::core::Error{99, "trailing bytes"});
    }
    return result;
}

constexpr std::size_t ext_index(ExtPart part) {
    return static_cast<std::size_t>(part);
}

constexpr std::size_t part_index(EconomyHashPart part) {
    return static_cast<std::size_t>(part);
}

/// Tests corrupt the economy on purpose, which the API does not allow: the tables are const.
EconomyState& state(World& world) {
    return const_cast<EconomyState&>(world.economy());
}

std::uint64_t economy_ext(const World& world) {
    return world.compute_hash().ext[ext_index(ExtPart::Economy)];
}

bool contains(std::string_view message, std::string_view fragment) {
    return message.find(fragment) != std::string_view::npos;
}

void expect_refused(std::span<const std::uint8_t> bytes, const char* fragment) {
    const auto result = load(bytes);
    REQUIRE_FALSE(result.has_value());
    CAPTURE(result.error().message);
    CHECK(contains(result.error().message, fragment));
}

// ---- the payload of a save file ------------------------------------------------------------------

/// The uncompressed payload of a PSV1 file (world state followed by the pending commands).
std::vector<std::uint8_t> payload_of(const std::vector<std::uint8_t>& file) {
    ByteReader header(file);
    (void)header.bytes(4);
    (void)header.u16();
    (void)header.u16();
    const std::uint64_t size = header.u64();
    (void)header.u64();
    const std::uint64_t compressed = header.u64();
    const auto frame = header.bytes(static_cast<std::size_t>(compressed));
    auto payload = pat::core::decompress(frame, static_cast<std::size_t>(size));
    REQUIRE(payload.has_value());
    return payload.value();
}

// ---- the sections of the extension block ---------------------------------------------------------

struct SectionSpan {
    std::uint16_t id = 0;
    std::uint16_t version = 0;
    std::size_t body = 0;  // offset of the body
    std::size_t length = 0;
};

struct Layout {
    std::size_t count_at = 0;  // offset of the u16 section count
    std::size_t end = 0;       // offset just after the last section
    std::vector<SectionSpan> sections;
};

/// Walks the core body of a state (it knows the layout of schema >= 2) and lists the sections.
Layout parse_layout(std::span<const std::uint8_t> bytes) {
    Layout layout;
    ByteReader in(bytes);
    (void)in.u32();  // schema
    (void)in.i32();  // today
    (void)in.i64();  // ticks
    (void)in.u64();  // master seed
    const std::uint32_t streams = in.u32();
    (void)in.bytes(std::size_t{streams} * 24);
    const std::uint32_t dynasties = in.u32();
    (void)in.bytes(std::size_t{dynasties} * 12);
    for (std::uint32_t d = 0; d < dynasties; ++d) {
        for (int list = 0; list < 2; ++list) {
            const std::uint32_t n = in.u32();
            (void)in.bytes(std::size_t{n} * 4);
        }
    }
    const std::uint32_t provinces = in.u32();
    (void)in.bytes(std::size_t{provinces} * 13);
    const std::uint32_t capacity = in.u32();
    (void)in.bytes(capacity);
    (void)in.bytes(std::size_t{capacity} * 4);
    const std::uint32_t free_count = in.u32();
    (void)in.bytes(std::size_t{free_count} * 4);
    (void)in.bytes(std::size_t{capacity} * 21);
    (void)in.u32();  // world flags
    layout.count_at = bytes.size() - in.remaining();
    const std::uint16_t count = in.u16();
    for (std::uint16_t s = 0; s < count; ++s) {
        SectionSpan span;
        span.id = in.u16();
        span.version = in.u16();
        span.length = in.u32();
        span.body = bytes.size() - in.remaining();
        (void)in.bytes(span.length);
        layout.sections.push_back(span);
    }
    REQUIRE(in.ok());
    layout.end = bytes.size() - in.remaining();
    return layout;
}

struct Sec {
    std::uint16_t id = 0;
    std::uint16_t version = 1;
    std::vector<std::uint8_t> body;
};

Sec& section(std::vector<Sec>& sections, std::uint16_t id) {
    const auto it = std::find_if(sections.begin(), sections.end(), [&](const Sec& s) { return s.id == id; });
    REQUIRE(it != sections.end());
    return *it;
}

/// Rewrites the section table of a state after `edit` changed the list (lengths are recomputed).
std::vector<std::uint8_t> edit_sections(const std::vector<std::uint8_t>& bytes,
                                        const std::function<void(std::vector<Sec>&)>& edit) {
    const Layout layout = parse_layout(bytes);
    std::vector<Sec> sections;
    for (const SectionSpan& s : layout.sections) {
        sections.push_back(
            Sec{s.id,
                s.version,
                std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(s.body),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(s.body + s.length))});
    }
    edit(sections);
    ByteWriter out;
    out.bytes(std::span<const std::uint8_t>(bytes.data(), layout.count_at));
    out.u16(static_cast<std::uint16_t>(sections.size()));
    for (const Sec& s : sections) {
        out.u16(s.id);
        out.u16(s.version);
        out.u32(static_cast<std::uint32_t>(s.body.size()));
        out.bytes(s.body);
    }
    out.bytes(std::span<const std::uint8_t>(bytes.data() + layout.end, bytes.size() - layout.end));
    return out.take();
}

std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFFU);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// Ids of the economy sections (src/sim/state_sections.hpp is internal; the numbers are the format).
constexpr std::uint16_t kSecMeta = 32;
constexpr std::uint16_t kSecDynasty = 33;
constexpr std::uint16_t kSecProvince = 34;
constexpr std::uint16_t kSecPopulation = 35;
constexpr std::uint16_t kSecAccounts = 36;
constexpr std::uint16_t kSecConstruction = 37;
constexpr std::uint16_t kSecCredit = 38;
constexpr std::uint16_t kSecIncidents = 39;
constexpr std::uint16_t kSecPolicy = 40;

std::vector<std::uint16_t> section_ids(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint16_t> ids;
    for (const SectionSpan& s : parse_layout(bytes).sections) {
        ids.push_back(s.id);
    }
    return ids;
}

// ---- worlds ---------------------------------------------------------------------------------------

constexpr std::uint32_t kDynasties = 4;
constexpr std::uint32_t kProvinces = 9;

/// A dynasty-mode world with a thick economy: every table has rows, every column differs from the
/// default somewhere, the statement has `closed` closed months and a month is open. The date is the
/// 17th of January, i.e. the middle of a month.
World rich_world(bool one_treasury = false, int closed = 3) {
    World world(Date::from_ymd(1703, 1, 17), 77);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        (void)world.create_dynasty(StringId(10 + d));
    }
    std::vector<CharacterId> people;
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        for (int k = 0; k < 3; ++k) {
            CharacterInit init;
            init.dynasty = DynastyId(d);
            init.birth_date = Date::from_ymd(1670 - k, 3, 1 + static_cast<int>(d));
            init.sex = k % 2 == 0 ? Sex::Male : Sex::Female;
            const CharacterId id = world.create_character(init);
            REQUIRE(id.valid());
            people.push_back(id);
        }
    }
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        (void)world.create_province(StringId(100 + p),
                                    p % 5 == 4 ? DynastyId() : DynastyId(p % kDynasties),
                                    Fixed32::from_int(static_cast<int>(5 + 3 * p)));
    }
    REQUIRE(world.enable_economy(one_treasury ? DynastyId(1) : DynastyId()));
    world.set_economy_rules_hash(0x1234567890ABCDEFULL);
    REQUIRE(world.set_province_owner_kind(ProvinceId(4), OwnerKind::Crown));

    const EconomyColumnsMut c = world.economy_columns_mut();
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        const int i = static_cast<int>(p);
        c.kind[p] = static_cast<std::uint8_t>(p % kProvinceKindCount);
        c.tax_policy[p] = static_cast<std::uint8_t>(p % 4);
        c.recruit_policy[p] = static_cast<std::uint8_t>(p % 3);
        c.slots_total[p] = static_cast<std::uint8_t>(2 + p % 5);
        c.trade_node[p] = static_cast<std::uint16_t>(7 * p);
        c.control[p] = Fixed32::from_int(100 - 3 * i);
        c.corruption[p] = Fixed32::from_ratio(i, 20);
        c.unrest[p] = Fixed32::from_int(2 * i);
        c.labour_base[p] = 1000 * (p + 1);
        c.grain_stock[p] = Fixed32::from_int(50 + i);
        c.recruit_pool[p] = 20 * p;
        c.income_last_month[p] = 12'345 * static_cast<Kopecks>(p);
        c.last_slot_date[p] = Date::from_ymd(1703, 1, 1) + i;
        for (std::size_t good = 0; good < kMaxGoods; ++good) {
            c.capacity[p * kMaxGoods + good] = Fixed32::from_int(i + static_cast<int>(good));
        }
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const std::size_t row = p * kGroupCount + g;
            (void)world.set_group_size(ProvinceId(p),
                                       static_cast<Group>(g),
                                       g == 5 ? 0 : 400 * static_cast<std::uint32_t>(g + 1) + p);
            c.mood[row] = Fixed32::from_int(static_cast<int>(row % 21) - 10);
            c.wealth[row] = Fixed32::from_int(80 + static_cast<int>(row % 50));
            c.growth_modifier[row] = static_cast<std::int32_t>(row * 1000) - 20'000;
            c.growth_carry[row] = static_cast<std::int32_t>(row * 37) - 900;
        }
        for (std::size_t s = 0; s + 1 < c.slots_total[p]; ++s) {
            REQUIRE(world.set_slot_building(ProvinceId(p), s, static_cast<std::uint8_t>(1 + (p + s) % 5)));
        }
        if (p % 2 == 0) {
            REQUIRE(world.set_province_administrator(ProvinceId(p), people[p % people.size()]));
        }
    }
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        c.manpower_reserve[d] = 100 * d;
        c.bankrupt[d] = static_cast<std::uint8_t>(d % 2);
    }

    const AccountRef crown = AccountRef::crown();
    const auto month = [&](int k) {
        for (std::uint32_t d = 0; d < kDynasties; ++d) {
            const AccountRef who = AccountRef::of(DynastyId(d));
            const Kopecks scale = static_cast<Kopecks>(d + 1) * (k + 1);
            (void)world.post_money(who, LedgerCode::TaxPeasants, 1000 * scale);
            (void)world.post_money(who, LedgerCode::Embezzlement, -100 * scale);
            (void)world.post_money(who, LedgerCode::HoldingIncome, 333 * scale);
            (void)world.post_money(who, LedgerCode::BuildingUpkeep, -250 * scale);
            (void)world.transfer(crown, who, LedgerCode::LoanIssued, LedgerCode::LoanTaken, 5000 * scale);
            (void)world.transfer(who, crown, LedgerCode::QuotaMoney, LedgerCode::QuotaCollected, 70 * scale);
        }
        (void)world.post_money(crown, LedgerCode::Tribute, 90'000 * (k + 1));
    };
    for (int k = 0; k < closed; ++k) {
        month(k);
        REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2 + k, 1)));
    }
    month(closed);
    (void)world.post_money(AccountRef::of(DynastyId(3)), LedgerCode::Adjustment, -500'000);  // in debt

    for (int i = 0; i < 6; ++i) {
        REQUIRE(world.log_incident(static_cast<IncidentKind>(1 + i % 8),
                                   ProvinceId(static_cast<std::uint32_t>(i)),
                                   DynastyId(static_cast<std::uint32_t>(i) % kDynasties),
                                   i * 10 - 20));
    }

    EconomyState& e = state(world);
    {  // two buildings under construction, on free slots, ordered by (finish date, seq)
        ConstructionQueue& q = e.construction;
        q.province = {ProvinceId(5), ProvinceId(3)};
        q.slot = {1, 4};
        q.building = {3, 5};
        q.payer = {DynastyId(1), DynastyId(3)};
        q.start_date = {Date::from_ymd(1703, 1, 2), Date::from_ymd(1703, 1, 9)};
        q.finish_date = {Date::from_ymd(1703, 6, 1), Date::from_ymd(1703, 7, 1)};
        q.paid_kop = {400'000, 650'000};
        q.seq = {0, 1};
        e.meta.next_construction_seq = 2;
    }
    {
        LoanTable& l = e.loans;
        l.loan_id = {0, 4};
        l.borrower = {DynastyId(1), DynastyId(3)};
        l.principal = {200'000, 80'000};
        l.rate_ppm_year = {80'000, 100'000};
        l.issued = {Date::from_ymd(1703, 1, 1), Date::from_ymd(1703, 1, 5)};
        l.due = {Date::from_ymd(1704, 1, 1), Date::from_ymd(1704, 2, 1)};
        l.interest_carry = {12'345, 999'999};
        l.overdue = {0, 1};
        l.collateral = {ProvinceId(), ProvinceId(2)};
        e.meta.next_loan_id = 5;
    }
    {
        QuotaTable& q = e.quotas;
        q.quota_id = {0, 3};
        q.dynasty = {DynastyId(0), DynastyId(2)};
        q.kind = {static_cast<std::uint8_t>(QuotaKind::Money),
                  static_cast<std::uint8_t>(QuotaKind::Recruits)};
        q.amount = {150'000, 40};
        q.period_start = {Date::from_ymd(1703, 1, 1), Date::from_ymd(1703, 1, 1)};
        q.deadline = {Date::from_ymd(1703, 3, 1), Date::from_ymd(1703, 9, 1)};
        e.meta.next_quota_id = 4;
        QuotaOutcomeRing& o = e.outcomes;
        o.seq = {3, 5, 9};
        o.dynasty = {DynastyId(0), DynastyId(1), DynastyId(2)};
        o.kind = {0, 1, 0};
        o.due = {1000, 50, 7000};
        o.delivered = {1000, 20, 0};
        o.settled_on = {Date::from_ymd(1702, 12, 1), Date::from_ymd(1702, 12, 1), Date::from_ymd(1703, 1, 1)};
        o.flags = {0, kQuotaOutcomeShort, kQuotaOutcomeShort | kQuotaOutcomePenalty};
        e.meta.next_outcome_seq = 10;
    }
    require_valid(world);
    return world;
}

}  // namespace

// =================================================================================================
// fixtures of the earlier schemas
// =================================================================================================

namespace {

struct FixtureCase {
    const char* file;
    std::uint32_t schema;      // the schema the file was written with
    std::uint64_t total_hash;  // WorldHash::total as the code of that schema computed it
    std::size_t characters;    // living
    std::size_t provinces;
};

constexpr FixtureCase kFixtures[] = {
    // made by the code of E1 (state schema 1): simcli --seed 42 --days 1461 --save ...
    {"e1_demo_v1.psv", 1, 0xe9822290b7ce3cc3ULL, 116, 80},
    // made by the code of E7-T02 (schema 2), E7-T03 (schema 3), E7-T04 (schema 4): stressgen
    {"dynasty_v2.psv", 2, 0x3aa123d9ea55bb03ULL, 240, 24},
    {"dynasty_v3.psv", 3, 0xd11a46b2f4fc2641ULL, 240, 24},
    {"dynasty_v4_archive.psv", 4, 0x0ab1c575c79b7467ULL, 240, 24},
    // made by the code of main right before E9-T03 (schema 4, E9-T02 tables in the World but not saved):
    //   stressgen --profile year1 --seed 11 --characters 300 --dynasties 12 --provinces 60 --archived 60
    //             --armies 0 --schemes 0 --contracts 0 --events 0 --output dynasty_v4_provinces.psv
    {"dynasty_v4_provinces.psv", 4, 0xb10331b93ac1a93fULL, 300, 60},
    //   simcli --seed 42 --days 400 --save e1_demo_v4.psv   (a demo world in the middle of a year)
    {"e1_demo_v4.psv", 4, 0x11402051e903e28dULL, 79, 80},
};

}  // namespace

TEST_CASE("fixtures: every earlier schema loads, keeps its hash and has no economy") {
    for (const FixtureCase& fixture : kFixtures) {
        CAPTURE(fixture.file);
        const auto file = read_file(fixture_path(fixture.file));
        REQUIRE(file.has_value());
        const std::vector<std::uint8_t> old_payload = payload_of(file.value());
        ByteReader schema_word(old_payload);
        CHECK(schema_word.u32() == fixture.schema);

        auto loaded = deserialize_save(file.value());
        REQUIRE(loaded.has_value());
        World& world = loaded.value().world;
        require_valid(world);
        const WorldHash hash = world.compute_hash();
        CHECK(hash.total == fixture.total_hash);
        CHECK(hash.ext[ext_index(ExtPart::Economy)] == 0);
        CHECK(world.living().size() == fixture.characters);
        CHECK(world.provinces().size() == fixture.provinces);
        CHECK_FALSE(world.economy_present());
        CHECK(world.economy() == EconomyState{});
        for (const std::uint64_t part : world.economy_hash_parts()) {
            CHECK(part == 0);
        }

        // The state saves and loads again with the same hash, and a second save is identical.
        const std::vector<std::uint8_t> bytes = state_bytes(world);
        ByteReader now_schema(bytes);
        CHECK(now_schema.u32() == World::kStateSchema);
        for (const std::uint16_t id : section_ids(bytes)) {
            CHECK(id < kSecMeta);  // an empty economy writes no economy section
        }
        auto again = load(bytes);
        REQUIRE(again.has_value());
        CHECK(again.value().compute_hash() == hash);
        CHECK(state_bytes(again.value()) == bytes);

        if (fixture.schema >= 2) {
            // The layout did not change for a world without economy: the same bytes except the
            // schema word (and the pending command count behind the world in the file).
            REQUIRE(bytes.size() < old_payload.size());
            CHECK(std::equal(bytes.begin() + 4, bytes.end(), old_payload.begin() + 4));
        }
    }
}

TEST_CASE("migration: a schema-4 file declares no economy and a schema-5 reader gives it an empty one") {
    // The bytes of a world that never had the economy are valid in schema 4 and in schema 5.
    for (const char* name : {"dynasty_v4_archive.psv", "dynasty_v4_provinces.psv", "e1_demo_v4.psv"}) {
        CAPTURE(name);
        auto file = read_file(fixture_path(name));
        REQUIRE(file.has_value());
        auto loaded = deserialize_save(file.value());
        REQUIRE(loaded.has_value());
        std::vector<std::uint8_t> bytes = state_bytes(loaded.value().world);
        bytes[0] = 4;  // as if written by the previous version
        auto old = load(bytes);
        REQUIRE(old.has_value());
        CHECK(old.value().compute_hash() == loaded.value().world.compute_hash());
        CHECK_FALSE(old.value().economy_present());
        bytes[0] = 5;
        REQUIRE(load(bytes).has_value());
        bytes[0] = static_cast<std::uint8_t>(World::kStateSchema + 1);  // from the future
        expect_refused(bytes, "newer version");
    }
}

TEST_CASE("migration: a world loaded from an old schema can switch the economy on and save it") {
    auto file = read_file(fixture_path("dynasty_v4_provinces.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;
    const WorldHash before = world.compute_hash();
    REQUIRE(world.enable_economy());
    require_valid(world);
    const WorldHash after = world.compute_hash();
    CHECK(after.ext[ext_index(ExtPart::Economy)] != 0);
    CHECK(after.total != before.total);
    CHECK(after.characters == before.characters);
    CHECK(after.dynasties == before.dynasties);
    (void)world.post_money(AccountRef::of(DynastyId(2)), LedgerCode::Adjustment, 123'456);
    (void)world.set_group_size(ProvinceId(3), Group::Townsfolk, 777);

    const std::vector<std::uint8_t> bytes = state_bytes(world);
    auto again = load(bytes);
    REQUIRE(again.has_value());
    CHECK(again.value().economy() == world.economy());
    CHECK(again.value().compute_hash() == world.compute_hash());
    CHECK(state_bytes(again.value()) == bytes);
    require_valid(again.value());
    // The economy keeps growing with the world after the load.
    (void)again.value().create_dynasty(StringId(999));
    (void)again.value().create_province(StringId(998), DynastyId(0), Fixed32::from_int(4));
    require_valid(again.value());
    CHECK(again.value().economy().accounts.size() == world.economy().accounts.size() + 1);
}

TEST_CASE("migration 9 -> 10: loan carries keep their fractional kopecks and migrate only once") {
    World old = rich_world();
    // Section 38 keeps its byte layout in schema 10. These are valid schema-9 bytes: its carries
    // counted millionths of a kopeck, including the largest value the old reader admitted.
    std::vector<std::uint8_t> fixture = state_bytes(old);
    put32(fixture, 0, 9);
    auto migrated = load(fixture);
    REQUIRE(migrated.has_value());
    CHECK(migrated.value().economy().loans.interest_carry[0] == 148'140);
    CHECK(migrated.value().economy().loans.interest_carry[1] == 11'999'988);

    World expected = old;
    for (std::int32_t& carry : state(expected).loans.interest_carry) {
        carry *= 12;
    }
    CHECK(migrated.value().economy() == expected.economy());
    CHECK(migrated.value().compute_hash() == expected.compute_hash());
    require_valid(migrated.value());

    const std::vector<std::uint8_t> current = state_bytes(migrated.value());
    CHECK(get32(current, 0) == World::kStateSchema);
    CHECK(current == state_bytes(expected));
    auto again = load(current);
    REQUIRE(again.has_value());
    CHECK(again.value().compute_hash() == expected.compute_hash());
    CHECK(state_bytes(again.value()) == current);
}

TEST_CASE("save: loan interest carry bounds follow the declared schema") {
    World world = rich_world();
    state(world).loans.interest_carry[0] = kMaxInterestCarry;
    auto current = load(state_bytes(world));
    REQUIRE(current.has_value());
    CHECK(current.value().economy().loans.interest_carry[0] == kMaxInterestCarry);

    // A value admitted by the new unit is still corrupt when a file declares the old unit.
    for (const std::int32_t invalid : {std::int32_t{-1}, std::int32_t{1'000'000}, kMaxInterestCarry}) {
        state(world).loans.interest_carry[0] = invalid;
        std::vector<std::uint8_t> old = state_bytes(world);
        put32(old, 0, 9);
        expect_refused(old, "old loan interest carry");
    }
    state(world).loans.interest_carry[0] = kMaxInterestCarry + 1;
    expect_refused(state_bytes(world), "interest carry");
}

// =================================================================================================
// the round trip
// =================================================================================================

TEST_CASE("save: a thick economy round-trips, with the same hash, state and bytes") {
    for (const bool one_treasury : {false, true}) {
        CAPTURE(one_treasury);
        const World world = rich_world(one_treasury);
        const WorldHash hash = world.compute_hash();
        CHECK(hash.ext[ext_index(ExtPart::Economy)] != 0);
        const std::vector<std::uint8_t> bytes = state_bytes(world);
        CHECK(section_ids(bytes).size() >= 8);
        const auto ids = section_ids(bytes);
        for (const std::uint16_t economy_id : {kSecMeta,
                                               kSecDynasty,
                                               kSecProvince,
                                               kSecPopulation,
                                               kSecAccounts,
                                               kSecConstruction,
                                               kSecCredit,
                                               kSecIncidents}) {
            CHECK(std::count(ids.begin(), ids.end(), economy_id) == 1);
        }
        CHECK(std::is_sorted(ids.begin(), ids.end()));

        auto loaded = load(bytes);
        REQUIRE(loaded.has_value());
        const World& copy = loaded.value();
        require_valid(copy);
        CHECK(copy.economy() == world.economy());  // every table, the rules hash and the ring included
        CHECK(copy.economy().meta.rules_hash == 0x1234567890ABCDEFULL);
        CHECK(copy.compute_hash() == hash);
        CHECK(copy.economy_hash_parts() == world.economy_hash_parts());
        CHECK(copy.provinces() == world.provinces());
        CHECK(state_bytes(copy) == bytes);
        CHECK(copy.resolve_account(AccountRef::crown()) == world.resolve_account(AccountRef::crown()));
        CHECK(copy.treasury(AccountRef::of(DynastyId(3))) == world.treasury(AccountRef::of(DynastyId(3))));
    }
}

TEST_CASE("save: a minimal economy (no provinces, no dynasties, a crown only) round-trips") {
    World world(Date::from_ymd(1703, 1, 1), 5);
    REQUIRE(world.enable_economy());
    (void)world.post_money(AccountRef::crown(), LedgerCode::Adjustment, 42);
    const WorldHash hash = world.compute_hash();
    CHECK(hash.ext[ext_index(ExtPart::Economy)] != 0);  // "on, nearly empty" is not "off"
    const std::vector<std::uint8_t> bytes = state_bytes(world);
    const auto ids = section_ids(bytes);
    CHECK(ids == std::vector<std::uint16_t>{kSecMeta, kSecAccounts, kSecPolicy});
    auto loaded = load(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().economy() == world.economy());
    CHECK(loaded.value().compute_hash() == hash);
    require_valid(loaded.value());
    // and switched on it stays on: a new province gets its rows
    (void)loaded.value().create_dynasty(StringId(1));
    (void)loaded.value().create_province(StringId(2), DynastyId(0), Fixed32::from_int(3));
    require_valid(loaded.value());
}

TEST_CASE("save: a world without economy writes no economy section and has no economy hash") {
    World world(Date::from_ymd(1703, 1, 1), 5);
    (void)world.create_dynasty(StringId(1));
    (void)world.create_province(StringId(2), DynastyId(0), Fixed32::from_int(3));
    const WorldHash hash = world.compute_hash();
    CHECK(hash.ext[ext_index(ExtPart::Economy)] == 0);
    const std::vector<std::uint8_t> bytes = state_bytes(world);
    CHECK(section_ids(bytes).empty());
    ByteReader schema(bytes);
    CHECK(schema.u32() == World::kStateSchema);

    // The hash of the plain world does not know the economy; with it on, only `total` and ext move.
    World with = world;
    REQUIRE(with.enable_economy());
    const WorldHash on = with.compute_hash();
    CHECK(on.total != hash.total);
    CHECK(on.calendar == hash.calendar);
    CHECK(on.rng == hash.rng);
    CHECK(on.dynasties == hash.dynasties);
    CHECK(on.characters == hash.characters);
    CHECK(on.provinces == hash.provinces);  // whole thousands survive the trip through persons
    for (std::size_t part = 0; part < kExtPartCount; ++part) {
        if (part != ext_index(ExtPart::Economy)) {
            CHECK(on.ext[part] == hash.ext[part]);
        }
    }
    CHECK(ext_part_name(ext_index(ExtPart::Economy)) == "economy");
    CHECK(std::string_view(economy_hash_part_name(part_index(EconomyHashPart::Accounts))) == "accounts");
}

TEST_CASE("save: the economy hash is the hash of its nine sub-hashes, and rules_hash is not part of it") {
    World world = rich_world();
    const auto parts = world.economy_hash_parts();
    pat::core::HashStream h;
    for (const std::uint64_t part : parts) {
        CHECK(part != 0);
        h.u64(part);
    }
    CHECK(economy_ext(world) == h.value());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            CHECK(parts[i] != parts[j]);
        }
    }

    const WorldHash before = world.compute_hash();
    const std::vector<std::uint8_t> bytes_before = state_bytes(world);
    world.set_economy_rules_hash(0xAAAA);
    CHECK(world.compute_hash() == before);      // not state
    CHECK(state_bytes(world) != bytes_before);  // but it is saved
    auto loaded = load(state_bytes(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().economy().meta.rules_hash == 0xAAAA);
}

TEST_CASE("E8 ledger: schema-11 economy retains its exact legacy hash and gains zero lines") {
    auto file = read_file(fixture_path("dynasty_v11_e8_economy.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const World& world = loaded.value().world;
    // Recorded by main a1c3e69, before the E8 codes shifted the dense indices (see fixture .md).
    CHECK(world.compute_hash().total == 15479572076255755579ULL);
    CHECK(economy_ext(world) == 12620759142815849368ULL);
    const auto& accounts = world.economy().accounts;
    for (std::size_t row = 0; row < accounts.size(); ++row) {
        for (const auto code : {LedgerCode::DowryIn, LedgerCode::MarriageEnvoy}) {
            CHECK(accounts.open_line(row, code) == 0);
            for (std::size_t month = 0; month < kLedgerRingMonths; ++month) {
                CHECK(accounts.ring_lines(row, month)[*ledger_index(code)] == 0);
            }
        }
    }
    require_valid(world);
    auto reloaded = load(state_bytes(world));
    REQUIRE(reloaded.has_value());
    CHECK(reloaded.value().compute_hash() == world.compute_hash());
    CHECK(reloaded.value().economy() == world.economy());
}

TEST_CASE("E8 ledger: nonzero hash extensions distinguish account, period and code") {
    const World base = rich_world();
    const auto accounts_hash = [](const World& world) {
        return world.economy_hash_parts()[part_index(EconomyHashPart::Accounts)];
    };
    const auto variant = [&](std::size_t account, int month, LedgerCode code) {
        World copy = base;
        auto& accounts = state(copy).accounts;
        const auto line = *ledger_index(code);
        if (month < 0) {
            Access::open_lines(accounts)[account * kLedgerLineCount + line] = 7;
        } else {
            Access::ring_lines(
                accounts)[(account * kLedgerRingMonths + static_cast<std::size_t>(month)) * kLedgerLineCount +
                          line] = 7;
        }
        return accounts_hash(copy);
    };
    // Keep all legacy bytes fixed and vary only the context of one nonzero extension cell.
    const auto open = variant(1, -1, LedgerCode::DowryIn);
    CHECK(open != accounts_hash(base));
    CHECK(open != variant(2, -1, LedgerCode::DowryIn));
    CHECK(open != variant(1, -1, LedgerCode::MarriageEnvoy));
    CHECK(open != variant(1, 0, LedgerCode::DowryIn));
    CHECK(variant(1, 0, LedgerCode::DowryIn) != variant(1, 1, LedgerCode::DowryIn));
}

TEST_CASE("E8 ledger: nonzero extensions survive ring wrap, reload and further postings") {
    World world(Date::from_ymd(1703, 1, 1), 298);
    const auto bride = world.create_dynasty(StringId(1));
    const auto groom = world.create_dynasty(StringId(2));
    REQUIRE(world.enable_economy());
    const auto sender = AccountRef::of(bride);
    const auto receiver = AccountRef::of(groom);
    for (int month = 0; month < static_cast<int>(kLedgerRingMonths) + 3; ++month) {
        const Kopecks amount = 100 + month;
        REQUIRE(world.post_money(sender, LedgerCode::MarriageEnvoy, -amount) == -amount);
        REQUIRE(world.transfer(sender, receiver, LedgerCode::DowryOut, LedgerCode::DowryIn, amount) ==
                amount);
        const int next = month + 1;
        REQUIRE(world.close_ledger_month(Date::from_ymd(1703 + next / 12, 1 + next % 12, 1)));
    }
    REQUIRE(world.economy().meta.closed_months == kLedgerRingMonths);
    REQUIRE(world.post_money(sender, LedgerCode::MarriageEnvoy, -17) == -17);
    REQUIRE(world.transfer(sender, receiver, LedgerCode::DowryOut, LedgerCode::DowryIn, 19) == 19);
    require_valid(world);
    auto loaded = load(state_bytes(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().economy_hash_parts() == world.economy_hash_parts());
    CHECK(loaded.value().economy() == world.economy());
    const auto next_month = Date::from_ymd(1704, 5, 1);
    for (World* current : {&world, &loaded.value()}) {
        REQUIRE(current->post_money(sender, LedgerCode::MarriageEnvoy, -23) == -23);
        REQUIRE(current->transfer(sender, receiver, LedgerCode::DowryOut, LedgerCode::DowryIn, 29) == 29);
        REQUIRE(current->close_ledger_month(next_month));
        require_valid(*current);
    }
    CHECK(loaded.value().compute_hash() == world.compute_hash());
}

// =================================================================================================
// the hash: every column, every table; no dependence on the way the state was reached
// =================================================================================================

namespace {

template <class T>
void inc(T& v) {
    v = static_cast<T>(v + 1);
}
void inc(Fixed32& v) {
    v = Fixed32::from_raw(v.raw() + 1);
}
void inc(Date& v) {
    v = v + 1;
}
void inc(DynastyId& v) {
    v = DynastyId(v.value() + 1);
}
void inc(ProvinceId& v) {
    v = ProvinceId(v.value() + 1);
}
void inc(CharacterId& v) {
    v = CharacterId(v.value() + 1);
}

using Change = std::pair<const char*, std::function<void(EconomyState&)>>;

// Changes one cell of a column of the rich world; the name shows up in failures.
#define PAT_COLUMN(expr)                                                                                     \
    Change {                                                                                                 \
        #expr, [](EconomyState& e) { inc(e.expr); }                                                          \
    }

/// Every change must move the sub-hash of its table, the hash part of the economy and `total`.
void expect_changes_part(EconomyHashPart part, const std::vector<Change>& changes) {
    const World base = rich_world();
    const WorldHash base_hash = base.compute_hash();
    const auto base_parts = base.economy_hash_parts();
    for (const Change& entry : changes) {
        CAPTURE(entry.first);
        World copy = base;
        entry.second(state(copy));
        REQUIRE_FALSE(copy.economy() == base.economy());
        CHECK(copy.economy_hash_parts()[part_index(part)] != base_parts[part_index(part)]);
        const WorldHash h = copy.compute_hash();
        CHECK(h.ext[ext_index(ExtPart::Economy)] != base_hash.ext[ext_index(ExtPart::Economy)]);
        CHECK(h.total != base_hash.total);
        // and the other fixed parts are those of the base: only the economy moved
        CHECK(h.calendar == base_hash.calendar);
        CHECK(h.dynasties == base_hash.dynasties);
        CHECK(h.characters == base_hash.characters);
    }
}

}  // namespace

TEST_CASE("hash: every scalar of the economy header counts") {
    expect_changes_part(EconomyHashPart::Meta,
                        {
                            PAT_COLUMN(meta.crown_dynasty),
                            PAT_COLUMN(meta.open_month_start),
                            PAT_COLUMN(meta.closed_months),
                            PAT_COLUMN(meta.next_loan_id),
                            PAT_COLUMN(meta.next_quota_id),
                            PAT_COLUMN(meta.next_outcome_seq),
                            PAT_COLUMN(meta.next_incident_seq),
                            PAT_COLUMN(meta.next_construction_seq),
                        });
}

TEST_CASE("hash: every column of the dynasties counts") {
    expect_changes_part(EconomyHashPart::Dynasties,
                        {
                            PAT_COLUMN(dynasties.manpower_reserve[2]),
                            PAT_COLUMN(dynasties.bankrupt[1]),
                        });
}

TEST_CASE("hash: every column of the provinces counts, the building grid included") {
    expect_changes_part(EconomyHashPart::Provinces,
                        {
                            PAT_COLUMN(provinces.kind[2]),
                            PAT_COLUMN(provinces.owner_kind[3]),
                            PAT_COLUMN(provinces.tax_policy[1]),
                            PAT_COLUMN(provinces.recruit_policy[1]),
                            PAT_COLUMN(provinces.slots_total[5]),
                            PAT_COLUMN(provinces.trade_node[4]),
                            PAT_COLUMN(provinces.administrator[2]),
                            PAT_COLUMN(provinces.control[3]),
                            PAT_COLUMN(provinces.corruption[3]),
                            PAT_COLUMN(provinces.unrest[3]),
                            PAT_COLUMN(provinces.labour_base[6]),
                            PAT_COLUMN(provinces.grain_stock[6]),
                            PAT_COLUMN(provinces.recruit_pool[7]),
                            PAT_COLUMN(provinces.income_last_month[7]),
                            PAT_COLUMN(provinces.last_slot_date[8]),
                            PAT_COLUMN(provinces.capacity[0]),
                            PAT_COLUMN(provinces.capacity[kMaxGoods * 4 + 9]),
                            PAT_COLUMN(provinces.capacity[kMaxGoods * 8 + 5]),
                            PAT_COLUMN(provinces.slot_building[0]),
                            PAT_COLUMN(provinces.slot_building[kMaxSlots * 3 + 2]),
                            PAT_COLUMN(provinces.slot_building[kMaxSlots * 8 + kMaxSlots - 1]),
                        });
}

TEST_CASE("hash: every column of the population groups counts") {
    expect_changes_part(EconomyHashPart::Groups,
                        {
                            PAT_COLUMN(groups.size[0]),
                            PAT_COLUMN(groups.size[kGroupCount * 8 + 5]),
                            PAT_COLUMN(groups.mood[7]),
                            PAT_COLUMN(groups.wealth[7]),
                            PAT_COLUMN(groups.growth_modifier[7]),
                            PAT_COLUMN(groups.growth_carry[kGroupCount * 8 + 5]),
                        });
}

TEST_CASE("hash: every column of the construction queue counts") {
    expect_changes_part(EconomyHashPart::Construction,
                        {
                            PAT_COLUMN(construction.province[1]),
                            PAT_COLUMN(construction.slot[0]),
                            PAT_COLUMN(construction.building[1]),
                            PAT_COLUMN(construction.payer[0]),
                            PAT_COLUMN(construction.start_date[1]),
                            PAT_COLUMN(construction.finish_date[0]),
                            PAT_COLUMN(construction.paid_kop[1]),
                            PAT_COLUMN(construction.seq[1]),
                            Change{"one entry more",
                                   [](EconomyState& e) {
                                       ConstructionQueue& q = e.construction;
                                       q.province.push_back(q.province.back());
                                       q.slot.push_back(q.slot.back());
                                       q.building.push_back(q.building.back());
                                       q.payer.push_back(q.payer.back());
                                       q.start_date.push_back(q.start_date.back());
                                       q.finish_date.push_back(q.finish_date.back());
                                       q.paid_kop.push_back(q.paid_kop.back());
                                       q.seq.push_back(q.seq.back());
                                   }},
                        });
}

TEST_CASE("hash: every column of the loans counts") {
    expect_changes_part(EconomyHashPart::Loans,
                        {
                            PAT_COLUMN(loans.loan_id[1]),
                            PAT_COLUMN(loans.borrower[0]),
                            PAT_COLUMN(loans.principal[1]),
                            PAT_COLUMN(loans.rate_ppm_year[0]),
                            PAT_COLUMN(loans.issued[1]),
                            PAT_COLUMN(loans.due[0]),
                            PAT_COLUMN(loans.interest_carry[0]),
                            PAT_COLUMN(loans.overdue[0]),
                            PAT_COLUMN(loans.collateral[1]),
                        });
}

TEST_CASE("hash: every column of the quotas and of their outcomes counts") {
    expect_changes_part(EconomyHashPart::Quotas,
                        {
                            PAT_COLUMN(quotas.quota_id[1]),
                            PAT_COLUMN(quotas.dynasty[0]),
                            PAT_COLUMN(quotas.kind[0]),
                            PAT_COLUMN(quotas.amount[1]),
                            PAT_COLUMN(quotas.period_start[0]),
                            PAT_COLUMN(quotas.deadline[1]),
                            PAT_COLUMN(outcomes.seq[2]),
                            PAT_COLUMN(outcomes.dynasty[1]),
                            PAT_COLUMN(outcomes.kind[0]),
                            PAT_COLUMN(outcomes.due[1]),
                            PAT_COLUMN(outcomes.delivered[2]),
                            PAT_COLUMN(outcomes.settled_on[0]),
                            PAT_COLUMN(outcomes.flags[1]),
                        });
}

TEST_CASE("hash: every column of the incident log counts") {
    expect_changes_part(EconomyHashPart::Incidents,
                        {
                            PAT_COLUMN(incidents.seq[3]),
                            PAT_COLUMN(incidents.date[2]),
                            PAT_COLUMN(incidents.province[1]),
                            PAT_COLUMN(incidents.dynasty[4]),
                            PAT_COLUMN(incidents.kind[5]),
                            PAT_COLUMN(incidents.param[0]),
                        });
}

TEST_CASE("hash: balances and every line of the statement count, the unused months of the ring do not") {
    constexpr std::size_t kClosed = 3;  // closed months of the rich world
    std::vector<Change> changes = {
        {"treasury", [](EconomyState& e) { inc(Access::treasury(e.accounts)[1]); }},
        {"opening balance", [](EconomyState& e) { inc(Access::opening(e.accounts)[2]); }},
        {"crown treasury", [](EconomyState& e) { inc(Access::treasury(e.accounts)[0]); }},
        {"last row", [](EconomyState& e) { inc(Access::treasury(e.accounts)[kDynasties]); }},
        {"first month start", [](EconomyState& e) { inc(Access::ring_month_start(e.accounts)[0]); }},
        {"oldest month start",
         [](EconomyState& e) { inc(Access::ring_month_start(e.accounts)[kClosed - 1]); }},
    };
    for (const LedgerCode code : kLedgerCodes) {
        const std::size_t line = *ledger_index(code);
        changes.push_back({"an open line", [line](EconomyState& e) {
                               inc(Access::open_lines(e.accounts)[(1 + 1) * kLedgerLineCount + line]);
                           }});
        changes.push_back(
            {"a line of the newest closed month", [line](EconomyState& e) {
                 inc(Access::ring_lines(e.accounts)[((2 * kLedgerRingMonths) + 0) * kLedgerLineCount + line]);
             }});
        changes.push_back(
            {"a line of the oldest closed month", [line](EconomyState& e) {
                 inc(Access::ring_lines(
                     e.accounts)[((3 * kLedgerRingMonths) + (kClosed - 1)) * kLedgerLineCount + line]);
             }});
    }
    for (std::size_t month = 0; month < kClosed; ++month) {
        changes.push_back({"an opening balance of the ring", [month](EconomyState& e) {
                               inc(Access::ring_opening(e.accounts)[kLedgerRingMonths * 1 + month]);
                           }});
    }
    expect_changes_part(EconomyHashPart::Accounts, changes);

    // Unused months of the ring are zero by invariant (the loader would refuse them) and are not hashed.
    const World base = rich_world();
    World copy = base;
    inc(Access::ring_opening(state(copy).accounts)[kLedgerRingMonths * 1 + kClosed + 2]);
    CHECK(copy.economy_hash_parts() == base.economy_hash_parts());
    CHECK_FALSE(copy.check_invariants().empty());
}

TEST_CASE("hash: the same state reached in another order has the same hash") {
    const std::array<std::pair<LedgerCode, Kopecks>, 5> postings = {{
        {LedgerCode::TaxPeasants, 1200},
        {LedgerCode::HoldingIncome, 340},
        {LedgerCode::TaxPeasants, -200},
        {LedgerCode::Embezzlement, -90},
        {LedgerCode::HoldingIncome, 60},
    }};
    // (a) postings in two orders, one split in two
    {
        World a = rich_world();
        World b = a;
        for (const auto& [code, amount] : postings) {
            (void)a.post_money(AccountRef::of(DynastyId(2)), code, amount);
        }
        for (auto it = postings.rbegin(); it != postings.rend(); ++it) {
            (void)b.post_money(AccountRef::of(DynastyId(2)), it->first, it->second);
        }
        CHECK(a.economy() == b.economy());
        CHECK(a.compute_hash() == b.compute_hash());
        CHECK(state_bytes(a) == state_bytes(b));
        (void)b.post_money(AccountRef::of(DynastyId(2)), LedgerCode::Duties, 1);
        CHECK(a.compute_hash() != b.compute_hash());
    }
    // (b) the economy switched on before or after the dynasties and provinces were created
    {
        const auto content = [](World& world) {
            for (std::uint32_t d = 0; d < 3; ++d) {
                (void)world.create_dynasty(StringId(d));
            }
            for (std::uint32_t p = 0; p < 5; ++p) {
                (void)world.create_province(
                    StringId(50 + p), DynastyId(p % 3), Fixed32::from_int(static_cast<int>(2 * p + 1)));
            }
        };
        World early(Date::from_ymd(1703, 1, 1), 9);
        REQUIRE(early.enable_economy());
        content(early);
        World late(Date::from_ymd(1703, 1, 1), 9);
        content(late);
        REQUIRE(late.enable_economy());
        CHECK(early.economy() == late.economy());
        CHECK(early.compute_hash() == late.compute_hash());
        CHECK(state_bytes(early) == state_bytes(late));
        require_valid(early);
    }
    // (c) the ring keeps the logical order: closing the same months from a loaded copy matches
    {
        World a = rich_world(false, 0);
        auto b = load(state_bytes(a));
        REQUIRE(b.has_value());
        for (int month = 0; month < 14; ++month) {  // more than the ring holds
            for (World* world : {&a, &b.value()}) {
                (void)world->post_money(AccountRef::of(DynastyId(1)), LedgerCode::TaxPeasants, 100 + month);
                REQUIRE(world->close_ledger_month(
                    Date::from_ymd(1703 + (month + 1) / 12, (month + 1) % 12 + 1, 1)));
            }
            CHECK(a.compute_hash() == b.value().compute_hash());
        }
        CHECK(static_cast<std::size_t>(a.economy().meta.closed_months) == kLedgerRingMonths);
        CHECK(a.economy() == b.value().economy());
        auto c = load(state_bytes(a));
        REQUIRE(c.has_value());
        CHECK(c.value().compute_hash() == a.compute_hash());
        CHECK(state_bytes(c.value()) == state_bytes(a));
    }
}

// =================================================================================================
// the sections of the file and what the loader does with damaged ones
// =================================================================================================

namespace {

// Offsets inside the bodies of the sections (see the layout at the top of world_economy_io.cpp).
constexpr std::size_t kMetaPresentAt = 0;
constexpr std::size_t kMetaCrownAt = 1 + 8;
constexpr std::size_t kMetaClosedAt = 1 + 8 + 4 + 4;
constexpr std::size_t kLoanRow = 37;
constexpr std::size_t kQuotaRow = 25;
constexpr std::size_t kProvinceRow = 5 + 2 + 4 + 12 + 4 + 4 + 4 + 8 + 4 + 40 + 6;
constexpr std::size_t kGroupRow = 20;

std::vector<std::uint8_t> rich_bytes() {
    return state_bytes(rich_world());
}

using Damage = std::function<void(std::vector<Sec>&)>;

std::vector<std::uint8_t>& body_of(std::vector<Sec>& secs, std::uint16_t id) {
    return section(secs, id).body;
}

void remove_section(std::vector<Sec>& secs, std::uint16_t id) {
    secs.erase(std::remove_if(secs.begin(), secs.end(), [&](const Sec& s) { return s.id == id; }),
               secs.end());
}

std::size_t economy_start_of(const std::vector<std::uint8_t>& bytes) {
    for (const SectionSpan& s : parse_layout(bytes).sections) {
        if (s.id == kSecMeta) {
            return s.body - 8;  // the header of the first economy section
        }
    }
    FAIL("no economy in the file");
    return 0;
}

}  // namespace

TEST_CASE("sections: a damaged table of the economy is refused with a reason, never a crash") {
    const std::vector<std::uint8_t> good = rich_bytes();
    REQUIRE(load(good).has_value());

    const auto refuse = [&](const char* what, const Damage& damage, const char* fragment) {
        INFO(what);
        expect_refused(edit_sections(good, damage), fragment);
    };

    // ---- the header and the order of sections
    refuse("no header", [](auto& secs) { remove_section(secs, kSecMeta); }, "without the economy header");
    refuse("present is 2", [](auto& secs) { body_of(secs, kSecMeta)[kMetaPresentAt] = 2; }, "does not say");
    refuse("present is 0", [](auto& secs) { body_of(secs, kSecMeta)[kMetaPresentAt] = 0; }, "does not say");
    refuse(
        "13 closed months", [](auto& secs) { body_of(secs, kSecMeta)[kMetaClosedAt] = 13; }, "closed months");
    refuse(
        "255 closed months",
        [](auto& secs) { body_of(secs, kSecMeta)[kMetaClosedAt] = 255; },
        "closed months");
    refuse(
        "two sections swapped",
        [](auto& secs) { std::swap(secs[secs.size() - 1], secs[secs.size() - 2]); },
        "out of order");
    refuse("a section twice", [](auto& secs) { secs.push_back(secs.back()); }, "out of order");
    refuse("version 2", [](auto& secs) { secs.back().version = 2; }, "unsupported");
    refuse("an unknown section", [](auto& secs) { secs.push_back(Sec{41, 1, {1, 2, 3, 4}}); }, "unsupported");
    refuse(
        "unused bytes in a section",
        [](auto& secs) { body_of(secs, kSecDynasty).push_back(0); },
        "malformed");
    refuse(
        "a short section", [](auto& secs) { body_of(secs, kSecIncidents).pop_back(); }, "impossible length");
    refuse("an empty section", [](auto& secs) { body_of(secs, kSecProvince).clear(); }, "do not match");

    // ---- counts that disagree with the dynasties and provinces
    for (const std::uint16_t id : {kSecDynasty, kSecProvince, kSecPopulation}) {
        refuse(
            "rows + 1",
            [id](auto& secs) { put32(body_of(secs, id), 0, get32(body_of(secs, id), 0) + 1); },
            "do not match");
        refuse(
            "rows - 1",
            [id](auto& secs) { put32(body_of(secs, id), 0, get32(body_of(secs, id), 0) - 1); },
            "do not match");
        refuse(
            "rows is huge", [id](auto& secs) { put32(body_of(secs, id), 0, 0xFFFFFFFFU); }, "do not match");
    }
    // a missing table stays empty: the invariant E-S1 notices
    refuse(
        "provinces missing", [](auto& secs) { remove_section(secs, kSecProvince); }, "violates invariants");
    refuse(
        "population missing",
        [](auto& secs) { remove_section(secs, kSecPopulation); },
        "violates invariants");
    refuse("dynasties missing", [](auto& secs) { remove_section(secs, kSecDynasty); }, "violates invariants");
    refuse("accounts missing", [](auto& secs) { remove_section(secs, kSecAccounts); }, "violates invariants");

    // ---- the statement
    refuse(
        "too many lines",
        [](auto& secs) { put16(body_of(secs, kSecAccounts), 0, kLedgerLineCount + 1); },
        "number of lines");
    refuse(
        "0xFFFF lines", [](auto& secs) { put16(body_of(secs, kSecAccounts), 0, 0xFFFF); }, "number of lines");
    refuse(
        "an unissued code",
        [](auto& secs) { put16(body_of(secs, kSecAccounts), 2 + 2 * 3, 7); },
        "ledger code");
    refuse(
        "codes out of order",
        [](auto& secs) {
            std::vector<std::uint8_t>& b = body_of(secs, kSecAccounts);
            std::swap(b[2], b[4]);
            std::swap(b[3], b[5]);
        },
        "ledger code");
    refuse(
        "a code twice",
        [](auto& secs) {
            std::vector<std::uint8_t>& b = body_of(secs, kSecAccounts);
            b[4] = b[2];
            b[5] = b[3];
        },
        "ledger code");
    refuse(
        "accounts + 1",
        [](auto& secs) {
            std::vector<std::uint8_t>& b = body_of(secs, kSecAccounts);
            const std::size_t at = 2 + 2 * kLedgerLineCount;
            put32(b, at, get32(b, at) + 1);
        },
        "do not match");
    refuse(
        "no accounts",
        [](auto& secs) { put32(body_of(secs, kSecAccounts), 2 + 2 * kLedgerLineCount, 0); },
        "do not match");
    refuse(
        "the statement is cut",
        [](auto& secs) {
            std::vector<std::uint8_t>& b = body_of(secs, kSecAccounts);
            b.resize(b.size() - 9);
        },
        "do not match");

    // ---- the lists
    refuse(
        "the construction queue claims more than it has",
        [](auto& secs) { put32(body_of(secs, kSecConstruction), 0, 3); },
        "impossible length");
    refuse(
        "the construction queue is huge",
        [](auto& secs) { put32(body_of(secs, kSecConstruction), 0, 0x7FFFFFFFU); },
        "impossible length");
    refuse(
        "loans claim more than they have",
        [](auto& secs) { put32(body_of(secs, kSecCredit), 0, 1000); },
        "impossible length");
    refuse(
        "quotas claim more",
        [](auto& secs) { put32(body_of(secs, kSecCredit), 4 + 2 * kLoanRow, 100'000); },
        "impossible length");
    refuse(
        "17 outcomes",
        [](auto& secs) { put32(body_of(secs, kSecCredit), 4 + 2 * kLoanRow + 4 + 2 * kQuotaRow, 17); },
        "impossible length");
    refuse(
        "257 incidents",
        [](auto& secs) { put32(body_of(secs, kSecIncidents), 0, 257); },
        "impossible length");
    refuse(
        "incidents claim more",
        [](auto& secs) { put32(body_of(secs, kSecIncidents), 0, 7); },
        "impossible length");

    // ---- an economy has no place in a file of an older schema
    for (const std::uint8_t schema : {std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4}}) {
        std::vector<std::uint8_t> old = good;
        old[0] = schema;
        expect_refused(old, "unsupported");
    }
    std::vector<std::uint8_t> ancient = good;
    ancient[0] = 1;  // a schema without an extension block reads the economy bytes as the commands behind
    CHECK_FALSE(load(ancient).has_value());
}

TEST_CASE("sections: a lie about the length of a section is an error") {
    const std::vector<std::uint8_t> good = rich_bytes();
    const Layout layout = parse_layout(good);
    for (const SectionSpan& span : layout.sections) {
        for (const std::int64_t delta : {std::int64_t{-1}, std::int64_t{1}, std::int64_t{1000}}) {
            std::vector<std::uint8_t> bytes = good;
            put32(bytes,
                  span.body - 4,
                  static_cast<std::uint32_t>(static_cast<std::int64_t>(span.length) + delta));
            CAPTURE(span.id);
            CAPTURE(delta);
            CHECK_FALSE(load(bytes).has_value());
        }
    }
}

TEST_CASE("sections: values and references inside well-formed sections are checked by the invariants") {
    const std::vector<std::uint8_t> good = rich_bytes();
    const Layout layout = parse_layout(good);
    const auto patch = [&](std::uint16_t id, std::size_t at, std::uint32_t value, std::size_t width = 4) {
        std::vector<std::uint8_t> bytes = good;
        const auto it = std::find_if(
            layout.sections.begin(), layout.sections.end(), [&](const SectionSpan& s) { return s.id == id; });
        REQUIRE(it != layout.sections.end());
        for (std::size_t i = 0; i < width; ++i) {
            bytes[it->body + at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
        }
        return bytes;
    };
    constexpr const char* kBad = "violates invariants";
    // province row 2 (the rows follow the u32 count)
    const std::size_t row2 = 4 + 2 * kProvinceRow;
    expect_refused(patch(kSecProvince, row2 + 0, 200, 1), kBad);        // unknown kind
    expect_refused(patch(kSecProvince, row2 + 1, 9, 1), kBad);          // unknown owner kind
    expect_refused(patch(kSecProvince, row2 + 4, 7, 1), kBad);          // 7 slots
    expect_refused(patch(kSecProvince, row2 + 7, 0xFFFFFF00U), kBad);   // an administrator that is not there
    expect_refused(patch(kSecProvince, row2 + 7, 5000), kBad);          // ... or that does not exist
    expect_refused(patch(kSecProvince, row2 + 11, 0x7FFFFFFFU), kBad);  // control out of range
    expect_refused(patch(kSecProvince, row2 + 19, 0x80000000U), kBad);  // negative unrest
    expect_refused(patch(kSecProvince, row2 + 7 + 4 + 12 + 4 + 4 + 4 + 8, 0x7FFFFFFFU),
                   kBad);  // processed in the far future
    // a group (row 7)
    const std::size_t group = 4 + 7 * kGroupRow;
    expect_refused(patch(kSecPopulation, group + 0, 5), kBad);             // size against the cache
    expect_refused(patch(kSecPopulation, group + 4, 0x7FFFFFFFU), kBad);   // mood
    expect_refused(patch(kSecPopulation, group + 8, 0x7FFFFFFFU), kBad);   // wealth
    expect_refused(patch(kSecPopulation, group + 12, 0x7FFFFFFFU), kBad);  // growth modifier
    expect_refused(patch(kSecPopulation, group + 16, 0x7FFFFFFFU), kBad);  // growth carry
    // the header: an unknown crown dynasty
    expect_refused(patch(kSecMeta, kMetaCrownAt, 77), kBad);
    // credit: the first loan (unknown borrower, overdue flag 5)
    expect_refused(patch(kSecCredit, 4 + 4, 99), kBad);
    expect_refused(patch(kSecCredit, 4 + 4 + 4 + 8 + 4 + 4 + 4 + 4, 5, 1), kBad);
}

TEST_CASE("invariants: a world that breaks them is refused by the loader, whatever the table") {
    struct Case {
        const char* name;
        std::function<void(World&)> damage;
        const char* problem;  // what check_invariants() says
        bool refused;         // the saved file is refused (not when the writer drops the damaged part)

        Case(const char* name_,
             std::function<void(World&)> damage_,
             const char* problem_,
             bool refused_ = true)
            : name(name_), damage(std::move(damage_)), problem(problem_), refused(refused_) {}
    };
    const std::vector<Case> cases = {
        {"treasury", [](World& w) { inc(Access::treasury(state(w).accounts)[1]); }, "treasury"},
        {"opening balance", [](World& w) { inc(Access::opening(state(w).accounts)[2]); }, "treasury"},
        {"an open line", [](World& w) { inc(Access::open_lines(state(w).accounts)[0]); }, "treasury"},
        {"a closed month does not chain",
         [](World& w) { inc(Access::ring_opening(state(w).accounts)[kLedgerRingMonths + 1]); },
         "closed month"},
        {"a transfer pair does not cancel",
         [](World& w) {
             const std::size_t line = *ledger_index(LedgerCode::LoanTaken);
             inc(Access::open_lines(state(w).accounts)[2 * kLedgerLineCount + line]);
             inc(Access::treasury(state(w).accounts)[2]);
         },
         "does not cancel out"},
        {"data in an unused month",
         [](World& w) { inc(Access::ring_opening(state(w).accounts)[kLedgerRingMonths * 2 + 9]); },
         "unused ring month",
         false},
        {"a month dated out of order",
         [](World& w) { Access::ring_month_start(state(w).accounts)[0] = Date::from_ymd(1703, 9, 1); },
         "out of order"},
        {"the population cache", [](World& w) { inc(state(w).groups.size[3]); }, "population cache"},
        {"unrest", [](World& w) { state(w).provinces.unrest[1] = Fixed32::from_int(101); }, "unrest"},
        {"corruption",
         [](World& w) { state(w).provinces.corruption[1] = Fixed32::from_int(2); },
         "corruption"},
        {"owner kind", [](World& w) { state(w).provinces.owner_kind[0] = 0; }, "owner kind"},
        {"administrator",
         [](World& w) { state(w).provinces.administrator[0] = CharacterId(9999); },
         "administrator"},
        {"a building beyond the slots",
         [](World& w) { state(w).provinces.slots_total[3] = 1; },
         "building in slot"},
        {"construction on a built slot",
         [](World& w) { state(w).provinces.slot_building[5 * kMaxSlots + 1] = 1; },
         "occupied slot"},
        {"construction in the wrong order",
         [](World& w) {
             std::swap(state(w).construction.finish_date[0], state(w).construction.finish_date[1]);
         },
         "out of (finish date, seq) order"},
        {"loan order", [](World& w) { state(w).loans.loan_id[1] = 0; }, "ascending order of ids"},
        {"loan principal", [](World& w) { state(w).loans.principal[0] = -5; }, "principal"},
        {"quota amount", [](World& w) { state(w).quotas.amount[0] = 0; }, "asks for nothing"},
        {"quota period",
         [](World& w) { state(w).quotas.deadline[1] = state(w).quotas.period_start[1]; },
         "ends before"},
        {"outcome flags", [](World& w) { state(w).outcomes.flags[0] = 0x80; }, "malformed"},
        {"incident counter", [](World& w) { state(w).meta.next_incident_seq = 2; }, "incident"},
        {"incident kind", [](World& w) { state(w).incidents.kind[0] = 0; }, "malformed"},
        {"processed in the future",
         [](World& w) { state(w).provinces.last_slot_date[2] = Date::from_ymd(1710, 1, 1); },
         "impossible date"},
        {"the crown dynasty does not exist",
         [](World& w) { state(w).meta.crown_dynasty = DynastyId(50); },
         "crown dynasty"},
        {"fewer closed months than data",
         [](World& w) { state(w).meta.closed_months = 2; },
         "unused ring month",
         false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        World w = rich_world();
        c.damage(w);
        const auto problems = w.check_invariants();
        CHECK(mentions(problems, c.problem));
        if (c.refused) {
            expect_refused(state_bytes(w), "violates invariants");
        }
    }
}

// =================================================================================================
// random damage
// =================================================================================================

TEST_CASE("save: random damage to a saved economy is handled safely") {
    const std::vector<std::uint8_t> bytes = rich_bytes();
    REQUIRE(load(bytes).has_value());
    const Layout layout = parse_layout(bytes);
    const std::size_t economy_start = economy_start_of(bytes);

    Pcg32 rng(808, 3);
    std::size_t survived = 0;
    for (int round = 0; round < 600; ++round) {
        std::vector<std::uint8_t> damaged = bytes;
        const int flips = 1 + static_cast<int>(rng.bounded(4));
        for (int k = 0; k < flips; ++k) {
            const std::size_t lo = round % 5 == 0 ? 0 : economy_start;  // mostly inside the economy
            const std::size_t where = lo + rng.bounded(static_cast<std::uint32_t>(damaged.size() - lo));
            damaged[where] = static_cast<std::uint8_t>(rng.bounded(256));
        }
        if (round % 11 == 0) {  // four bytes forced to the extremes, to hit the counts
            const std::size_t where =
                economy_start + rng.bounded(static_cast<std::uint32_t>(damaged.size() - economy_start - 4));
            for (std::size_t i = 0; i < 4; ++i) {
                damaged[where + i] = rng.chance_permille(500) ? 0xFF : 0x00;
            }
        }
        if (round % 7 == 0) {
            damaged.resize(damaged.size() - 1 - rng.bounded(60));
        }
        const auto result = load(damaged);
        if (result.has_value()) {  // an accepted file is a valid world that saves and loads again
            ++survived;
            CHECK(result.value().check_invariants().empty());
            auto again = load(state_bytes(result.value()));
            REQUIRE(again.has_value());
            CHECK(again.value().compute_hash() == result.value().compute_hash());
        }
    }
    CHECK(survived < 600);
    CHECK(survived > 0);  // values inside their ranges are legitimate: only the bounds are checked

    // every prefix of the state is an error
    for (std::size_t n = 0; n < bytes.size(); n += 7) {
        CHECK_FALSE(load(std::span<const std::uint8_t>(bytes.data(), n)).has_value());
    }
    // the first bytes of every economy section (counts, flags) set to extreme values
    for (const SectionSpan& s : layout.sections) {
        for (std::size_t i = 0; i < std::min<std::size_t>(s.length, 12); ++i) {
            for (const std::uint8_t value : {std::uint8_t{0}, std::uint8_t{0xFF}, std::uint8_t{0x80}}) {
                std::vector<std::uint8_t> damaged = bytes;
                damaged[s.body + i] = value;
                const auto result = load(damaged);
                if (result.has_value()) {
                    CHECK(result.value().check_invariants().empty());
                }
            }
        }
    }
}

TEST_CASE("save: random damage to the economy of an old fixture with the economy switched on") {
    auto file = read_file(fixture_path("dynasty_v4_provinces.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;
    REQUIRE(world.enable_economy());
    const std::vector<std::uint8_t> bytes = state_bytes(world);
    const std::size_t economy_start = economy_start_of(bytes);
    Pcg32 rng(31, 4);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> damaged = bytes;
        for (int k = 0; k < 3; ++k) {
            damaged[economy_start + rng.bounded(static_cast<std::uint32_t>(damaged.size() - economy_start))] =
                static_cast<std::uint8_t>(rng.bounded(256));
        }
        const auto result = load(damaged);
        if (result.has_value()) {
            CHECK(result.value().check_invariants().empty());
        }
    }
}

// =================================================================================================
// a simulation with an economy: continuation after a load, replays
// =================================================================================================

namespace {

/// Stands in for the systems of E9-T06+: every day it posts money, moves some between accounts, changes
/// columns, wraps the incident ring and closes the month on the first day. Only the World API and the
/// named random stream, like the real systems.
class EconomyChurn final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "test.economy"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        Pcg32& rng = world.rng().stream("test.economy");
        const Date today = context.date;
        if (today.day() == 1) {
            (void)world.close_ledger_month(today);
        }
        const auto dynasties = static_cast<std::uint32_t>(world.dynasties().size());
        const auto provinces = static_cast<std::uint32_t>(world.provinces().size());
        static constexpr std::array<LedgerCode, 8> kCodes = {LedgerCode::TaxPeasants,
                                                             LedgerCode::TaxTownsfolk,
                                                             LedgerCode::HoldingIncome,
                                                             LedgerCode::Embezzlement,
                                                             LedgerCode::BuildingUpkeep,
                                                             LedgerCode::Construction,
                                                             LedgerCode::RaidLoss,
                                                             LedgerCode::Adjustment};
        for (int k = 0; k < 3; ++k) {
            const AccountRef who =
                rng.bounded(5) == 0 ? AccountRef::crown() : AccountRef::of(DynastyId(rng.bounded(dynasties)));
            const LedgerCode code = kCodes[rng.bounded(static_cast<std::uint32_t>(kCodes.size()))];
            (void)world.post_money(who, code, static_cast<Kopecks>(rng.bounded(40'000)) - 15'000);
        }
        if (rng.chance_permille(200)) {
            (void)world.transfer(AccountRef::crown(),
                                 AccountRef::of(DynastyId(rng.bounded(dynasties))),
                                 LedgerCode::LoanIssued,
                                 LedgerCode::LoanTaken,
                                 1 + static_cast<Kopecks>(rng.bounded(9000)));
        }
        const ProvinceId province(rng.bounded(provinces));
        const std::size_t i = province.index();
        const EconomyColumnsMut c = world.economy_columns_mut();
        const std::int32_t unrest = c.unrest[i].raw() + rng.range(-300, 300) * (Fixed32::one_raw / 100);
        c.unrest[i] = Fixed32::from_raw(std::clamp<std::int32_t>(unrest, 0, 100 * Fixed32::one_raw));
        c.last_slot_date[i] = today;
        c.income_last_month[i] = rng.bounded(100'000);
        (void)world.set_group_size(province, Group::Peasants, 2000 + rng.bounded(30'000));
        if (rng.chance_permille(600)) {
            (void)world.log_incident(static_cast<IncidentKind>(1 + rng.bounded(8)),
                                     province,
                                     DynastyId(rng.bounded(dynasties)),
                                     rng.range(-50, 50));
        }
    }
};

Simulation economy_simulation(bool one_treasury = false) {
    Simulation sim(rich_world(one_treasury, 0));
    sim.add_system(std::make_unique<EconomyChurn>());
    return sim;
}

}  // namespace

TEST_CASE("simulation: the economy changes the hash every day and the churn keeps the invariants") {
    Simulation sim = economy_simulation();
    std::uint64_t previous = economy_ext(sim.world());
    int changed = 0;
    for (int day = 0; day < 120; ++day) {
        (void)sim.tick();
        const std::uint64_t now = economy_ext(sim.world());
        changed += now != previous ? 1 : 0;
        previous = now;
        if (day % 30 == 0) {
            require_valid(sim.world());
        }
    }
    CHECK(changed == 120);
    require_valid(sim.world());
    CHECK(sim.world().economy().meta.closed_months >= 3);  // the 1st of February, March, April, May
}

TEST_CASE("simulation: a save in the middle of a month resumes identically, to the last byte") {
    for (const bool one_treasury : {false, true}) {
        for (const int days_before_save : {45, 15, 1, 0, 100}) {  // Mar 3, Feb 1 (a month start), ...
            CAPTURE(one_treasury);
            CAPTURE(days_before_save);
            Simulation original = economy_simulation(one_treasury);
            original.run_days(days_before_save);

            const auto file = serialize_save(original.world(), original.pending_internal());
            Simulation resumed = economy_simulation(one_treasury);
            auto loaded = deserialize_save(file);
            REQUIRE(loaded.has_value());
            resumed.restore_state(std::move(loaded.value().world),
                                  std::move(loaded.value().pending_internal));
            CHECK(resumed.world().compute_hash() == original.world().compute_hash());
            CHECK(resumed.world().economy() == original.world().economy());

            // 500 days: the incident ring wraps and so does the statement ring (more than 12 month ends)
            for (int step = 0; step < 5; ++step) {
                original.run_days(100);
                resumed.run_days(100);
                CHECK(resumed.world().compute_hash() == original.world().compute_hash());
            }
            CHECK(resumed.world().economy() == original.world().economy());
            CHECK(state_bytes(resumed.world()) == state_bytes(original.world()));
            CHECK(original.world().economy().incidents.size() == kIncidentLogSize);
            CHECK(static_cast<std::size_t>(original.world().economy().meta.closed_months) ==
                  kLedgerRingMonths);
            require_valid(original.world());
            require_valid(resumed.world());
        }
    }
}

TEST_CASE("simulation: saving again and again never changes the run") {
    Simulation straight = economy_simulation();
    Simulation hopping = economy_simulation();
    straight.run_days(210);
    for (int hop = 0; hop < 7; ++hop) {
        hopping.run_days(30);
        const auto file = serialize_save(hopping.world(), hopping.pending_internal());
        auto loaded = deserialize_save(file);
        REQUIRE(loaded.has_value());
        hopping.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    }
    CHECK(hopping.world().compute_hash() == straight.world().compute_hash());
    CHECK(hopping.world().economy() == straight.world().economy());
}

TEST_CASE("replay: an economy run is reproduced, a divergence in the economy is found") {
    Simulation recorded = economy_simulation();
    ReplayRecorder recorder(77, 0xEC0ULL, recorded.world().today(), 30);
    for (int day = 0; day < 200; ++day) {
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    CHECK(replay.checkpoints.size() >= 6);
    // The checkpoint records `total` and the five fixed parts; `total` takes the economy in.
    CHECK(recorded.world().compute_hash().ext[ext_index(ExtPart::Economy)] != 0);

    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());
    {
        Simulation again = economy_simulation();
        const ReplayResult result = play_replay(again, parsed.value());
        CHECK(result.ok);
        CHECK_FALSE(result.divergence.has_value());
        CHECK(again.world().compute_hash() == recorded.world().compute_hash());
        CHECK(again.world().economy() == recorded.world().economy());
    }
    {  // the same scenario with one number of the economy changed: only `total` can tell
        Simulation other = economy_simulation();
        inc(state(const_cast<World&>(other.world())).provinces.recruit_pool[3]);
        const ReplayResult result = play_replay(other, parsed.value());
        CHECK_FALSE(result.ok);
        REQUIRE(result.divergence.has_value());
        CHECK(result.divergence->tick > 0);
        CHECK(result.divergence->first_differing_part.find("economy") != std::string::npos);
    }
}

TEST_CASE("replay: a world without economy still ignores the extended part for the recorded fields") {
    // WorldHash::same_recorded_parts compares total and the five fixed parts only.
    World plain(Date::from_ymd(1703, 1, 1), 5);
    (void)plain.create_dynasty(StringId(1));
    World with = plain;
    REQUIRE(with.enable_economy());
    const WorldHash a = plain.compute_hash();
    const WorldHash b = with.compute_hash();
    CHECK_FALSE(a.same_recorded_parts(b));  // total differs: the economy is in it
    WorldHash c = a;
    c.ext[ext_index(ExtPart::Economy)] = 123;  // a part that is not recorded
    CHECK(a.same_recorded_parts(c));
    CHECK_FALSE(a == c);
}
