// E10-T03: save, migration and hash of the court tables and the influence (docs/dev/court-model.md
// sections 13 and 20.13, docs/dev/influence-model.md section 17): sections 64..71 and 10, hash parts
// 32..37, 39 and 40, schema 14, the schema-13 fixture, damaged files and a load that continues like the
// original run.

#include "pat/core/bytes.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/court/portfolio.hpp"
#include "pat/sim/court/tables.hpp"
#include "pat/sim/influence.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "state_sections_fixture.hpp"
#include "test_bytes.hpp"
#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::court;
using pat::core::Fixed32;
using pat::core::StringId;
using pat::test::edit_sections;
using pat::test::expect_rejected;
using pat::test::load_state;
using pat::test::section;
using pat::test::Sections;
using pat::test::sections_of;
using pat::test::state_bytes;

namespace {

constexpr std::array<std::uint16_t, 9> kCourtSections{10, 64, 65, 66, 67, 68, 69, 70, 71};
/// Hash parts of the court and the influence (38 is E7-T25's and never set here).
constexpr std::array<std::size_t, 8> kCourtParts{32, 33, 34, 35, 36, 37, 39, 40};

Date today() {
    return Date::from_ymd(1703, 5, 20);
}

World make_plain_world() {
    World world(today(), 383);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(world.create_dynasty(StringId(100 + i)) == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), DynastyId(i), Fixed32::from_int(100)) ==
                ProvinceId(i));
    }
    return world;
}

CharacterId person(World& world, std::uint32_t house) {
    CharacterInit init;
    init.dynasty = DynastyId(house);
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

struct Fixture {
    World world;
    std::array<CharacterId, 4> people;
};

Office office(CharacterId holder, OfficeTypeId type, std::uint32_t locus) {
    Office row;
    row.type = type;
    row.locus = locus;
    row.holder = holder;
    row.since = today() - 100;
    return row;
}

Contender contender(CharacterId candidate, std::uint32_t house, std::int32_t base, PetitionId petition) {
    Contender row;
    row.candidate = candidate;
    row.dynasty = DynastyId(house);
    for (std::size_t i = 0; i < row.terms.size(); ++i) {
        row.terms[i] = base - static_cast<std::int32_t>(3 * i);
        row.score += row.terms[i];
    }
    row.petition = petition;
    return row;
}

/// Every table non-empty, in the middle of petitions, vacancies and servants: two offices (one a province
/// seat, one appointed by the system), a disgrace, two vacancies, two decisions, a petition with a gift,
/// three servants, two favor rows with a party, portfolios in the three states, two orders, a market
/// seed and the influence balances with their log.
Fixture make_court_world() {
    Fixture f{make_plain_world(), {}};
    World& w = f.world;
    for (std::uint32_t i = 0; i < f.people.size(); ++i) {
        f.people[i] = person(w, i % 3);
    }
    const auto [a, b, c, d] = f.people;

    Office first = office(a, 1, 0);
    first.appointer = DynastyId(0);
    first.patron = b;
    first.flags = kOfficeExceptional;
    REQUIRE(w.add_court_office(first));
    Office regional = office(b, 7, 2);
    regional.flags = kOfficeBySystem | kOfficeProvinceSeat;
    REQUIRE(w.add_court_office(regional));
    REQUIRE(w.set_court_disgrace({c, today() + 400, DisgraceReason::Investigation}));

    Vacancy open;
    open.type = 4;
    open.locus = 1;
    open.opened = today() - 10;
    open.decide_by = today() + 50;
    open.reason = VacancyReason::HolderDied;
    open.previous_holder = c;
    open.postponed = 2;
    const auto vacancy = w.add_court_vacancy(open);
    REQUIRE(vacancy);
    open.type = 5;
    open.reason = VacancyReason::NewSeat;
    open.previous_holder = CharacterId{};
    open.postponed = 0;
    REQUIRE(w.add_court_vacancy(open));

    Petition petition;
    petition.vacancy = *vacancy;
    petition.petitioner = DynastyId(0);
    petition.candidate = d;
    petition.submitted = today() - 3;
    petition.gift_kop = 125'000;
    petition.flags = kPetitionService | kPetitionParty;
    const auto petition_id = w.add_court_petition(petition);
    REQUIRE(petition_id);

    Decision decided;
    decided.vacancy = 900;
    decided.type = 3;
    decided.locus = 0;
    decided.decided = today() - 40;
    decided.result = DecisionResult::Appointed;
    decided.n = 2;
    decided.contenders[0] = contender(a, 0, 20, 0);
    decided.contenders[1] = contender(b, 1, -4, *petition_id);
    REQUIRE(w.append_court_decision(decided));
    Decision none;
    none.vacancy = 901;
    none.type = 6;
    none.decided = today() - 20;
    none.result = DecisionResult::NoCandidate;
    REQUIRE(w.append_court_decision(none));

    for (std::uint32_t i = 0; i < 3; ++i) {
        Servant servant;
        servant.dynasty = DynastyId(i % 2);
        servant.name = StringId(500 + i);
        servant.birth_year = static_cast<std::uint16_t>(1680 + i);
        servant.hired_month = static_cast<std::uint16_t>(1236 + i);
        servant.role = static_cast<ServantRole>(1 + i);
        servant.sex = static_cast<std::uint8_t>(i % 2);
        servant.skill = static_cast<std::uint8_t>(5 + i);
        servant.loyalty = static_cast<std::uint8_t>(40 + 20 * i);
        servant.wage_kop = static_cast<std::uint16_t>(250 + i);
        servant.arrears = static_cast<std::uint8_t>(i);
        servant.portrait = static_cast<std::uint8_t>(9 + i);
        REQUIRE(w.add_court_servant(servant));
    }
    REQUIRE(w.set_court_market(1240, 0xC0FFEE1234ULL));

    Favor favor;
    favor.dynasty = DynastyId(1);
    favor.favor_x100 = 6200;
    favor.services = 2;
    favor.party = Party::NewService;
    favor.party_support_on = today() - 5;
    favor.gift_month = 1240;
    favor.gift_points_x100 = 300;
    favor.pressure_band = 1;
    REQUIRE(w.set_court_favor(favor));
    favor.dynasty = DynastyId(2);
    favor.favor_x100 = 1500;
    favor.services = 0;
    favor.party = Party::Church;
    favor.party_support_on = kNoDate;
    favor.gift_month = -1;
    favor.gift_points_x100 = 0;
    favor.pressure_band = 3;
    REQUIRE(w.set_court_favor(favor));

    Portfolio active;
    active.type = 11;
    active.course = 2;
    active.course_since = today() - 60;
    active.controller = DynastyId(0);
    active.set_by = DynastyId(0);  // the target stays "none": an invalid DynastyId, not 0
    active.order_ready = {today() + 10, kNoDate, today() + 30, kNoDate};
    REQUIRE(w.set_court_portfolio(active));
    Portfolio inertia = active;
    inertia.type = 4;
    inertia.locus = 1;
    inertia.state = PortfolioState::Inertia;
    inertia.next_step = today() + 90;
    inertia.set_by = DynastyId{};
    inertia.flags = kPortfolioBySystem;
    REQUIRE(w.set_court_portfolio(inertia));
    Portfolio rollback = active;
    rollback.type = 12;
    rollback.course = 5;
    rollback.controller = DynastyId(1);
    rollback.target = DynastyId(0);
    rollback.state = PortfolioState::Rollback;
    rollback.next_step = today() + 15;
    rollback.flags = kPortfolioHostile;
    REQUIRE(w.set_court_portfolio(rollback));

    PortfolioOrder order;
    order.type = 11;
    order.order = 1;
    order.issuer = DynastyId(0);
    order.holder = a;
    order.date = today() - 2;
    order.target = 2;
    order.cost_kop = 1'500'000;
    order.cost_favor_x100 = 250;
    order.power_permille = 700;
    order.execution_permille = 900;
    order.noise_x100 = 35;
    REQUIRE(w.append_court_order(order));
    order.order = 3;
    order.holder = CharacterId{};
    order.date = today();
    order.cost_favor_x100 = 0;
    REQUIRE(w.append_court_order(order));

    REQUIRE(w.add_influence(DynastyId(0), 1200, InfluenceReason::Start));
    REQUIRE(w.add_influence(DynastyId(2), 30000, InfluenceReason::Scenario));
    REQUIRE(w.add_influence(DynastyId(0), -300, InfluenceReason::PetitionPush));
    REQUIRE(w.add_influence(DynastyId(0), 50, InfluenceReason::Monthly));  // not logged
    pat::test::require_valid(w);
    return f;
}

bool has_section(const std::vector<std::uint8_t>& bytes, std::uint16_t id) {
    for (const auto& row : sections_of(bytes).rows) {
        if (row.id == id) {
            return true;
        }
    }
    return false;
}

/// The extended parts that differ between two hashes.
std::set<std::size_t> changed_parts(const WorldHash& before, const WorldHash& after) {
    std::set<std::size_t> parts;
    for (std::size_t i = 0; i < kExtPartCount; ++i) {
        if (before.ext[i] != after.ext[i]) {
            parts.insert(i);
        }
    }
    return parts;
}

void require_same_court(const World& restored, const World& original) {
    CHECK(restored.court() == original.court());
    CHECK(restored.influence_state() == original.influence_state());
    CHECK(restored.compute_hash() == original.compute_hash());
    // The derived indexes are rebuilt (C-12 is part of require_valid).
    for (const CharacterId id : {CharacterId(0), CharacterId(1), CharacterId(2)}) {
        CHECK(restored.court_offices_of(id) == original.court_offices_of(id));
    }
    CHECK(restored.court_offices_in(ProvinceId(2)) == original.court_offices_in(ProvinceId(2)));
    CHECK(restored.court_servants_of(DynastyId(0), ServantRole::Steward) ==
          original.court_servants_of(DynastyId(0), ServantRole::Steward));
    pat::test::require_valid(restored);
}

}  // namespace

TEST_CASE("court save: a world without a court writes nothing and hashes as before") {
    World world = make_plain_world();
    (void)person(world, 0);
    const auto bytes = state_bytes(world);
    for (const auto id : kCourtSections) {
        CHECK_FALSE(has_section(bytes, id));
    }
    const WorldHash hash = world.compute_hash();
    for (const std::size_t part : kCourtParts) {
        CHECK(hash.ext[part] == 0);
    }
    // The file of the previous schema is the same bytes with the older schema word.
    auto older = bytes;
    pat::test::store_le<std::uint32_t>(older, 0, 13);
    auto loaded = load_state(older);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == hash);
    CHECK(loaded.value().court() == State{});
    CHECK(loaded.value().influence_state() == InfluenceState{});
    CHECK(state_bytes(loaded.value()) == bytes);  // saved again: schema 14, nothing else changes
    CHECK(World::kStateSchema == 14);
}

TEST_CASE("court save: every table round-trips with its hash and derived indexes") {
    const Fixture f = make_court_world();
    const auto bytes = state_bytes(f.world);
    for (const auto id : kCourtSections) {
        CAPTURE(id);
        CHECK(has_section(bytes, id));
    }
    auto loaded = load_state(bytes);
    REQUIRE(loaded.has_value());
    require_same_court(loaded.value(), f.world);
    CHECK(state_bytes(loaded.value()) == bytes);

    const WorldHash hash = loaded.value().compute_hash();
    for (const std::size_t part : kCourtParts) {
        CAPTURE(part);
        CHECK(hash.ext[part] != 0);
    }
    CHECK(hash.ext[38] == 0);
    CHECK(ext_part_name(32) == "court_meta");
    CHECK(ext_part_name(39) == "court_portfolio");
    CHECK(ext_part_name(40) == "influence");
    CHECK(static_cast<std::size_t>(ExtPart::CourtFavor) == 37);

    // The portfolio without a target keeps an invalid DynastyId (dynasty 0 exists).
    const Portfolio* active = loaded.value().find_court_portfolio(11, 0);
    REQUIRE(active != nullptr);
    CHECK_FALSE(active->target.valid());
    CHECK(loaded.value().find_court_portfolio(12, 0)->target == DynastyId(0));

    // The save of a saved world is stable, through the container as well.
    const auto file = serialize_save(f.world);
    auto from_file = deserialize_save(file);
    REQUIRE(from_file.has_value());
    CHECK(from_file.value().summary.state_schema == 14);
    require_same_court(from_file.value().world, f.world);
    CHECK(serialize_save(from_file.value().world) == file);
}

TEST_CASE("court hash: each table moves only its own part, its counter and the total") {
    Fixture f{make_plain_world(), {}};
    World& w = f.world;
    const CharacterId a = person(w, 0);
    const CharacterId b = person(w, 1);
    const std::size_t meta = 32;
    const auto step = [&](const std::function<void()>& change, std::set<std::size_t> expected) {
        const WorldHash before = w.compute_hash();
        change();
        const WorldHash after = w.compute_hash();
        CHECK(changed_parts(before, after) == expected);
        CHECK_FALSE(after.total == before.total);
        pat::test::require_valid(w);
    };
    step([&] { REQUIRE(w.add_court_office(office(a, 1, 0))); }, {meta, 33});
    step([&] { REQUIRE(w.set_court_disgrace({b, today() + 30, DisgraceReason::Sovereign})); }, {33});
    Vacancy open;
    open.type = 2;
    open.opened = today();
    open.decide_by = today() + 30;
    step([&] { REQUIRE(w.add_court_vacancy(open)); }, {meta, 34});
    Decision decided;
    decided.type = 2;
    decided.decided = today();
    decided.result = DecisionResult::Postponed;
    step([&] { REQUIRE(w.append_court_decision(decided)); }, {meta, 34});
    Petition petition;
    petition.vacancy = 1;
    petition.petitioner = DynastyId(1);
    petition.candidate = b;
    petition.submitted = today();
    step([&] { REQUIRE(w.add_court_petition(petition)); }, {meta, 35});
    Servant servant;
    servant.dynasty = DynastyId(2);
    step([&] { REQUIRE(w.add_court_servant(servant)); }, {meta, 36});
    Favor favor;
    favor.dynasty = DynastyId(0);
    favor.party = Party::OldNobility;
    step([&] { REQUIRE(w.set_court_favor(favor)); }, {37});
    step([&] { REQUIRE(w.set_court_market(1240, 7)); }, {meta});
    Portfolio portfolio;
    portfolio.type = 9;
    portfolio.controller = DynastyId(0);
    step([&] { REQUIRE(w.set_court_portfolio(portfolio)); }, {39});
    PortfolioOrder order;
    order.type = 9;
    order.issuer = DynastyId(0);
    order.date = today();
    step([&] { REQUIRE(w.append_court_order(order)); }, {meta, 39});
    step([&] { REQUIRE(w.add_influence(DynastyId(1), 100, InfluenceReason::Monthly)); }, {40});
    step([&] { REQUIRE(w.add_influence(DynastyId(1), 100, InfluenceReason::Event)); }, {40});
}

TEST_CASE("court save: the schema-13 fixture still loads with the same hash and an empty court") {
    const auto path = std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / "dynasty_v13_court.psv";
    const auto file = read_file(path);
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const World& world = loaded.value().world;
    CHECK(loaded.value().summary.state_schema == 13);
    const WorldHash hash = world.compute_hash();
    CHECK(hash.total == 9575088371203622812ULL);  // main 7a359af, schema 13 (generate_dynasty_v13_court.md)
    CHECK(hash.ext[24] == 12839949525724368523ULL);  // the event rows of schema 13 are kept
    CHECK(world.court() == State{});
    CHECK(world.influence_state() == InfluenceState{});
    for (const std::size_t part : kCourtParts) {
        CHECK(hash.ext[part] == 0);
    }
    pat::test::require_valid(world);
    // Saved again it is a schema-14 file with the same state; its round-trip is stable byte for byte.
    const auto saved = serialize_save(world);
    const auto again = deserialize_save(saved);
    REQUIRE(again.has_value());
    CHECK(again.value().summary.state_schema == 14);
    CHECK(again.value().world.compute_hash() == hash);
    CHECK(serialize_save(again.value().world) == saved);
}

TEST_CASE("court save: a file of the previous schema cannot hold court or influence sections") {
    const auto bytes = state_bytes(make_court_world().world);
    for (const std::uint16_t id : kCourtSections) {
        CAPTURE(id);
        // Keep only this one of the new sections, then claim schema 13.
        auto one = edit_sections(bytes, [&](Sections& s) {
            std::erase_if(s.rows, [&](const pat::test::Section& row) {
                return row.id != id && std::find(kCourtSections.begin(), kCourtSections.end(), row.id) !=
                                           kCourtSections.end();
            });
        });
        pat::test::store_le<std::uint32_t>(one, 0, 13);
        expect_rejected(one);
    }
    auto newer = bytes;
    pat::test::store_le<std::uint32_t>(newer, 0, World::kStateSchema + 1);
    expect_rejected(newer);
}

TEST_CASE("court sections: versions, lengths, counts and row order are checked before use") {
    const auto bytes = state_bytes(make_court_world().world);
    // Size of the first row of the first table of a section (64 has no table).
    const auto first_row_bytes = [](std::uint16_t id) -> std::size_t {
        switch (id) {
            case 10:
                return 8;
            case 65:
                return 26;
            case 66:
                return 23;
            case 67:
                return 33;
            case 68:
                return 24;
            case 69:
                return 23;
            case 70:
            case 71:
                return 44;
            default:
                return 0;
        }
    };
    for (const std::uint16_t id : kCourtSections) {
        CAPTURE(id);
        expect_rejected(edit_sections(bytes, [&](Sections& s) { section(s, id).version = 2; }));
        for (int cut = 0; cut < 3; ++cut) {
            expect_rejected(edit_sections(bytes, [&](Sections& s) {
                auto& body = section(s, id).body;
                body.resize(cut == 0 ? 0 : cut == 1 ? body.size() / 2 : body.size() - 1);
            }));
        }
        expect_rejected(edit_sections(bytes, [&](Sections& s) { section(s, id).body.push_back(0); }));
        if (id == 64) {
            continue;
        }
        expect_rejected(edit_sections(bytes, [&](Sections& s) {
            pat::test::store_le<std::uint32_t>(
                section(s, id).body, 0, std::numeric_limits<std::uint32_t>::max());
        }));
        // A row repeated at the front breaks the strict order.
        expect_rejected(edit_sections(bytes, [&](Sections& s) {
            auto& body = section(s, id).body;
            const auto n = pat::test::load_le<std::uint32_t>(body, 0);
            REQUIRE(n >= 1);
            const std::size_t size = first_row_bytes(id);
            const std::vector<std::uint8_t> first(body.begin() + 4,
                                                  body.begin() + 4 + static_cast<std::ptrdiff_t>(size));
            body.insert(body.begin() + 4, first.begin(), first.end());
            pat::test::store_le<std::uint32_t>(body, 0, n + 1);
        }));
    }
    expect_rejected(edit_sections(bytes, [](Sections& s) { s.rows.push_back(section(s, 65)); }));
}

TEST_CASE("court sections: enumerations, references, bounds and counters a file may not carry are refused") {
    const Fixture f = make_court_world();
    const auto bytes = state_bytes(f.world);
    const auto& court = f.world.court();
    // Offsets inside the bodies (after the u32 row count of the first table).
    const std::size_t disgraces = 4 + court.offices.size() * 26;  // the count of the second table of 65
    const std::size_t decisions = 4 + court.vacancies.size() * 23;
    const std::size_t log = 4 + f.world.influence_state().rows.size() * 8;
    const auto u32_at = [](std::uint16_t id, std::size_t at, std::uint32_t value) {
        return [=](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, id).body, at, value); };
    };
    const auto u8_at = [](std::uint16_t id, std::size_t at, std::uint8_t value) {
        return [=](Sections& s) { section(s, id).body.at(at) = value; };
    };
    const std::vector<std::function<void(Sections&)>> damage{
        // 64: counters not above the ids in use, a market month below -1
        u32_at(64, 0, 2),
        u32_at(64, 4, 1),
        u32_at(64, 8, 0),
        u32_at(64, 12, 3),
        u32_at(64, 16, 2),
        u32_at(64, 20, 1),
        u32_at(64, 24, static_cast<std::uint32_t>(-2)),
        // 65: an unknown holder, a type out of 1..63, unknown flags, a province seat outside the map,
        // a disgrace with an unknown reason or already over
        u32_at(65, 4 + 9, 999),
        u8_at(65, 4 + 4, 0),
        u8_at(65, 4 + 4, 64),
        u8_at(65, 4 + 25, 0x08),
        u32_at(65, 4 + 26 + 5, 77),
        u8_at(65, disgraces + 4 + 8, 0),
        u8_at(65, disgraces + 4 + 8, 4),
        u32_at(65, disgraces + 4 + 4, static_cast<std::uint32_t>(today().days())),
        // 66: an unknown reason, decide_by before opened, a vacancy on a held seat, a decision with too
        // many contenders, an unknown result or terms that do not explain the score
        u8_at(66, 4 + 17, 0),
        u8_at(66, 4 + 17, 8),
        u32_at(66, 4 + 13, static_cast<std::uint32_t>((today() - 400).days())),
        [](Sections& s) {
            auto& body = section(s, 66).body;
            body.at(4 + 4) = 1;  // the seat (1, 0) is held
            pat::test::store_le<std::uint32_t>(body, 4 + 5, 0);
        },
        u8_at(66, decisions + 4 + 18, 5),
        u8_at(66, decisions + 4 + 17, 0),
        u32_at(66, decisions + 4 + 19 + 8, 12345),
        // 67: an unknown vacancy, a candidate of another dynasty, a negative gift, unknown flags
        u32_at(67, 4 + 4, 77),
        u32_at(67, 4 + 8, 1),
        [](Sections& s) { pat::test::store_le<std::int64_t>(section(s, 67).body, 4 + 20, -1); },
        u8_at(67, 4 + 28, 0x04),
        // 68: an unknown dynasty or role, skill and loyalty above their bounds
        u32_at(68, 4 + 4, 9),
        u8_at(68, 4 + 16, 0),
        u8_at(68, 4 + 16, 5),
        u8_at(68, 4 + 18, 21),
        u8_at(68, 4 + 19, 101),
        // 69: an unknown dynasty, favor above 10,000, an unknown party, a pressure band above 3, a row
        // equal to the defaults
        u32_at(69, 4, 9),
        u32_at(69, 4 + 4, 10001),
        u8_at(69, 4 + 9, 4),
        u8_at(69, 4 + 22, 4),
        [](Sections& s) {
            auto& body = section(s, 69).body;
            pat::test::store_le<std::uint32_t>(body, 4, 0);  // dynasty 0 (row order still ascends)
            pat::test::store_le<std::int32_t>(body, 4 + 4, kDefaultFavorX100);
            body.at(4 + 8) = 0;
            body.at(4 + 9) = 0;
            pat::test::store_le<std::int32_t>(body, 4 + 10, kNoDate.days());
            pat::test::store_le<std::int32_t>(body, 4 + 14, -1);
            pat::test::store_le<std::int32_t>(body, 4 + 18, 0);
            body.at(4 + 22) = 0;
        },
        // 70: an unknown state, a course out of 0..7, an unknown target or controller, an active row with
        // a next step, unknown flags
        u8_at(70, 4 + 22, 0),
        u8_at(70, 4 + 22, 4),
        u8_at(70, 4 + 5, 8),
        u32_at(70, 4 + 18, 5),
        u32_at(70, 4 + 10, 9),
        u32_at(70, 4 + 44 + 23, static_cast<std::uint32_t>((today() + 5).days())),
        u8_at(70, 4 + 43, 0x04),
        // 71: an order number above 3, execution above 1000, a negative cost, an unknown issuer
        u8_at(71, 4 + 9, 4),
        [](Sections& s) { pat::test::store_le<std::uint16_t>(section(s, 71).body, 4 + 40, 1001); },
        [](Sections& s) { pat::test::store_le<std::int64_t>(section(s, 71).body, 4 + 26, -5); },
        u32_at(71, 4 + 10, 7),
        // 10: a zero or too large balance, an unknown dynasty, an unknown or monthly reason, a pad that
        // is not 0, a zero delta, dates that go backwards, a log longer than 64
        u32_at(10, 4 + 4, 0),
        u32_at(10, 4 + 4, kMaxInfluenceX100 + 1),
        u32_at(10, 4, 3),
        [&](Sections& s) { pat::test::store_le<std::uint16_t>(section(s, 10).body, log + 4 + 12, 77); },
        [&](Sections& s) { pat::test::store_le<std::uint16_t>(section(s, 10).body, log + 4 + 12, 1); },
        [&](Sections& s) { pat::test::store_le<std::uint16_t>(section(s, 10).body, log + 4 + 14, 1); },
        [&](Sections& s) { pat::test::store_le<std::int32_t>(section(s, 10).body, log + 4 + 8, 0); },
        [&](Sections& s) {
            pat::test::store_le<std::int32_t>(section(s, 10).body, log + 4 + 16, (today() - 1).days());
            pat::test::store_le<std::int32_t>(section(s, 10).body, log + 4, today().days());
        },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 10).body, log, 65); },
    };
    for (std::size_t i = 0; i < damage.size(); ++i) {
        CAPTURE(i);
        expect_rejected(edit_sections(bytes, damage[i]));
    }
}

TEST_CASE("court sections: no prefix and no flipped byte of the court sections crashes the loader") {
    const Fixture f = make_court_world();
    const auto bytes = state_bytes(f.world);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const auto result = load_state(std::span<const std::uint8_t>(bytes).first(length));
        CHECK_FALSE(result.has_value());
    }
    const Sections sections = sections_of(bytes);
    std::size_t checked = 0;
    for (const auto& row : sections.rows) {
        if (std::find(kCourtSections.begin(), kCourtSections.end(), row.id) == kCourtSections.end()) {
            continue;
        }
        for (std::size_t at = 0; at < row.body.size(); ++at) {
            for (const std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
                const auto damaged =
                    edit_sections(bytes, [&](Sections& s) { section(s, row.id).body[at] ^= mask; });
                auto result = load_state(damaged);
                if (result.has_value()) {
                    CHECK(result.value().check_invariants().empty());  // accepted only if still a valid world
                }
                ++checked;
            }
        }
    }
    CHECK(checked > 2000);
}

TEST_CASE(
    "court save: a load in the middle of petitions, vacancies and servants continues like the original") {
    const Fixture f = make_court_world();
    const std::vector<Command> none;
    Simulation continuous(f.world);
    continuous.restore_state(f.world, none);
    auto loaded = deserialize_save(serialize_save(f.world, none));
    REQUIRE(loaded.has_value());
    Simulation resumed(std::move(loaded.value().world));
    resumed.restore_state(resumed.world(), loaded.value().pending_internal);
    for (int day = 0; day < 40; ++day) {
        (void)continuous.tick();
        (void)resumed.tick();
        REQUIRE(continuous.world().compute_hash() == resumed.world().compute_hash());
    }
    CHECK(state_bytes(continuous.world()) == state_bytes(resumed.world()));
    CHECK(resumed.world().court() == continuous.world().court());
    CHECK(resumed.world().influence_state() == continuous.world().influence_state());
    pat::test::require_valid(resumed.world());
}
