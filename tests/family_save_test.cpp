#include "pat/core/bytes.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

Date fixture_date() {
    return Date::from_ymd(1703, 5, 20);
}

std::filesystem::path fixture_path(const char* name) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / name;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

CharacterId person(World& world,
                   DynastyId house,
                   Sex sex = Sex::Male,
                   int year = 1670,
                   CharacterId father = {},
                   CharacterId mother = {}) {
    CharacterInit init;
    init.dynasty = house;
    init.sex = sex;
    init.birth_date = Date::from_ymd(year, 1, 1);
    init.father = father;
    init.mother = mother;
    const auto id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

family::Negotiation proposal(World& world, std::uint32_t house = 0) {
    family::Negotiation row;
    row.initiator = DynastyId(house);
    row.groom_house = DynastyId(house);
    row.bride_house = DynastyId((house + 1) % 6);
    row.groom = person(world, row.groom_house);
    row.bride = person(world, row.bride_house, Sex::Female);
    row.sent = fixture_date() - 2;
    row.answer_due = fixture_date() + 28;
    row.cost_kop = 12'345;
    row.dowry_kop = 67'890;
    row.dowry_source = row.bride_house;
    row.dispensation = 1;
    return row;
}

family::Marriage married_pair(World& world, std::uint32_t house = 0) {
    family::Marriage row;
    row.groom_house = DynastyId(house);
    row.bride_house = DynastyId((house + 1) % 6);
    row.groom = person(world, row.groom_house);
    row.bride = person(world, row.bride_house, Sex::Female);
    REQUIRE(world.marry(row.groom, row.bride));
    row.wedding = fixture_date() - 30;
    row.dowry_kop = 100'000;
    return row;
}

// Fixed state only: no marriage, succession, demography or economy systems are installed.
World make_family_save_world() {
    World world(fixture_date(), 298);
    REQUIRE(world.enable_dynasty_mode());
    std::array<CharacterId, 6> heads;
    for (std::uint32_t i = 0; i < heads.size(); ++i) {
        const DynastyId house = world.create_dynasty(StringId(100 + i));
        REQUIRE(house == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), house, Fixed32::from_int(1000)) == ProvinceId(i));
        heads[i] = person(world, house, Sex::Male, 1660);
        REQUIRE(world.set_dynasty_head(house, heads[i]));
    }

    for (std::uint32_t house : {0U, 2U}) {
        REQUIRE(world.create_family_negotiation(proposal(world, house)).has_value());
    }
    for (std::uint32_t house : {0U, 1U}) {
        family::Betrothal row;
        row.groom_house = DynastyId(house);
        row.bride_house = DynastyId(house + 1);
        row.groom = person(world, row.groom_house, Sex::Male, 1693);
        row.bride = person(world, row.bride_house, Sex::Female, 1693);
        row.contracted = fixture_date();
        row.wedding_not_before = Date::from_ymd(1710, 1, 1);
        row.wedding_deadline = row.wedding_not_before + 90;
        row.dowry_kop = 23'456 + house;
        row.dowry_source = row.bride_house;
        row.dispensation = 1;
        row.origin = family::BetrothalOrigin::Command;
        REQUIRE(world.create_family_betrothal(row).has_value());
    }

    REQUIRE(world.create_family_marriage(married_pair(world)).has_value());
    auto closed = married_pair(world, 1);
    const auto closed_id = world.create_family_marriage(closed);
    REQUIRE(closed_id.has_value());
    REQUIRE(world.end_marriage(closed.groom));
    closed.id = *closed_id;
    closed.state = family::MarriageState::Closed;
    closed.ended = fixture_date() - 1;
    closed.end_reason = family::MarriageEndReason::Annulment;
    closed.refunded_kop = 12'345;
    REQUIRE(world.update_family_marriage(*closed_id, closed));

    auto widow = married_pair(world, 0);
    const auto widow_id = world.create_family_marriage(widow);
    REQUIRE(widow_id.has_value());
    REQUIRE(world.die(widow.groom, CauseOfDeath::Illness));
    widow.id = *widow_id;
    widow.state = family::MarriageState::Widowed;
    widow.ended = fixture_date();
    widow.end_reason = family::MarriageEndReason::Widowhood;
    widow.claim_kop = 35'000;
    REQUIRE(world.update_family_marriage(*widow_id, widow));
    family::PersonStatus widow_status;
    widow_status.character = widow.bride;
    widow_status.widowed_on = fixture_date();
    REQUIRE(world.set_family_status(widow_status));
    family::Betrothal widow_betrothal;
    widow_betrothal.groom_house = DynastyId(2);
    widow_betrothal.bride_house = widow.bride_house;
    widow_betrothal.groom = person(world, widow_betrothal.groom_house);
    widow_betrothal.bride = widow.bride;
    widow_betrothal.contracted = fixture_date();
    widow_betrothal.wedding_not_before = fixture_date() + 30;
    widow_betrothal.wedding_deadline = fixture_date() + 120;
    widow_betrothal.dowry_kop = widow.claim_kop;
    widow_betrothal.dowry_source = widow.groom_house;
    widow_betrothal.claim_marriage = *widow_id;
    widow_betrothal.origin = family::BetrothalOrigin::Command;
    REQUIRE(world.create_family_betrothal(widow_betrothal).has_value());

    family::PersonStatus remembered;
    remembered.character = person(world, DynastyId(1), Sex::Female);
    remembered.flags = family::kPersonTonsured | family::kPersonDisowned;
    remembered.tonsured_on = fixture_date() - 20;
    remembered.disowned_on = fixture_date() - 10;
    REQUIRE(world.set_family_status(remembered));
    REQUIRE(world.die(remembered.character, CauseOfDeath::Illness));

    for (std::uint32_t house : {0U, 1U}) {
        family::SuccessionState row;
        row.dynasty = DynastyId(house);
        row.designated_heir = person(world, row.dynasty);
        row.guardian = person(world, row.dynasty, Sex::Male, 1650);
        REQUIRE(world.die(row.guardian, CauseOfDeath::OldAge));
        row.law_changed_on = fixture_date() - 7;
        REQUIRE(world.set_family_succession(row));
    }
    for (std::uint32_t house : {2U, 4U}) {
        const CharacterId father = person(world, DynastyId(house));
        const CharacterId mother = person(world, DynastyId(1), Sex::Female, 1665);
        const CharacterId child = person(world, DynastyId(house), Sex::Male, 1695, father, mother);
        REQUIRE(world.marry(father, mother));
        REQUIRE(world.die(father, CauseOfDeath::Illness));
        REQUIRE(world.set_dynasty_head(DynastyId(house), child));
        family::Regency row;
        row.dynasty = DynastyId(house);
        row.ward = child;
        row.regent = mother;
        row.kind = family::RegencyKind::Mother;
        row.started = fixture_date() - 400;
        row.checks_done = 1;
        row.last_risk_permille = static_cast<std::uint16_t>(house * 100);
        row.excluded = heads[house];
        if (house == 4) {
            row.regent = {};
            row.kind = family::RegencyKind::Imperial;
        }
        REQUIRE(world.set_family_regency(row));
    }
    for (std::uint32_t house : {3U, 5U}) {
        REQUIRE(world.die(heads[house], CauseOfDeath::OldAge));
        REQUIRE(world.set_dynasty_head(DynastyId(house), {}));
        family::Dispute row;
        row.dynasty = DynastyId(house);
        row.predecessor = heads[house];
        row.opened = fixture_date();
        row.resolve_by = fixture_date() + 90;
        row.reason = family::DisputeReason::NoAppointment;
        row.claimants_n = 3;
        for (std::uint32_t i = 0; i < row.claimants_n; ++i) {
            row.claimants[i] = {person(world, row.dynasty, Sex::Male, 1665 + static_cast<int>(i)),
                                60 - 20 * static_cast<std::int32_t>(i)};
        }
        REQUIRE(world.set_family_dispute(row));
        REQUIRE(world.die(row.claimants[0].character, CauseOfDeath::Illness));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t province : {0U, 1U}) {
        REQUIRE(world.set_province_administrator(ProvinceId(province), heads[province]));
        family::LandGrant row;
        row.province = ProvinceId(province);
        row.holder = heads[province];
        row.granted_on = fixture_date() - 3;
        REQUIRE(world.set_family_land_grant(row));
    }
    REQUIRE(world.set_family_godparent_cursor(3));

    const auto answered = proposal(world, 1);
    const auto id = world.create_family_negotiation(answered);
    REQUIRE(id.has_value());
    REQUIRE(world.close_family_negotiation(*id));
    family::NegotiationOutcome outcome;
    outcome.negotiation = *id;
    outcome.initiator = answered.initiator;
    outcome.responder = answered.bride_house;
    outcome.groom = answered.groom;
    outcome.bride = answered.bride;
    outcome.sent = answered.sent;
    outcome.answered = fixture_date();
    outcome.result = family::NegotiationResult::Refused;
    outcome.reason = 2;
    outcome.score = -20;
    outcome.threshold = 5;
    outcome.terms = {10, -30, 3, 4, -5, 6, 7, -15};
    REQUIRE(world.append_family_outcome(outcome).has_value());
    require_valid(world);
    return world;
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

auto load_state(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    auto loaded = World::load_state(in);
    if (loaded.has_value()) {
        REQUIRE(in.at_end());
    }
    return loaded;
}

struct Section {
    std::uint16_t id = 0;
    std::uint16_t version = 0;
    std::vector<std::uint8_t> body;
};

struct Sections {
    std::size_t count_at = 0;
    std::size_t end = 0;
    std::vector<Section> rows;
};

Sections sections_of(std::span<const std::uint8_t> bytes) {
    Sections sections;
    ByteReader in(bytes);
    (void)in.u32();
    (void)in.i32();
    (void)in.i64();
    (void)in.u64();
    const auto streams = in.u32();
    (void)in.bytes(std::size_t{streams} * 24);
    const auto dynasties = in.u32();
    (void)in.bytes(std::size_t{dynasties} * 12);
    for (std::uint32_t i = 0; i < dynasties; ++i) {
        for (int list = 0; list < 2; ++list) {
            const auto n = in.u32();
            (void)in.bytes(std::size_t{n} * 4);
        }
    }
    const auto provinces = in.u32();
    (void)in.bytes(std::size_t{provinces} * 13);
    const auto capacity = in.u32();
    (void)in.bytes(capacity);
    (void)in.bytes(std::size_t{capacity} * 4);
    const auto free = in.u32();
    (void)in.bytes(std::size_t{free} * 4);
    (void)in.bytes(std::size_t{capacity} * 21);
    (void)in.u32();
    sections.count_at = in.position();
    const auto count = in.u16();
    for (std::uint16_t i = 0; i < count; ++i) {
        Section row;
        row.id = in.u16();
        row.version = in.u16();
        const auto length = in.u32();
        const auto body = in.bytes(length);
        row.body.assign(body.begin(), body.end());
        sections.rows.push_back(std::move(row));
    }
    REQUIRE(in.ok());
    sections.end = in.position();
    return sections;
}

Section& section(Sections& sections, std::uint16_t id) {
    const auto found = std::find_if(
        sections.rows.begin(), sections.rows.end(), [&](const Section& row) { return row.id == id; });
    REQUIRE(found != sections.rows.end());
    return *found;
}

std::vector<std::uint8_t> edit_sections(const std::vector<std::uint8_t>& bytes,
                                        const std::function<void(Sections&)>& edit) {
    auto sections = sections_of(bytes);
    edit(sections);
    ByteWriter out;
    out.bytes(std::span<const std::uint8_t>(bytes).first(sections.count_at));
    out.u16(static_cast<std::uint16_t>(sections.rows.size()));
    for (const auto& row : sections.rows) {
        out.u16(row.id);
        out.u16(row.version);
        out.u32(static_cast<std::uint32_t>(row.body.size()));
        out.bytes(row.body);
    }
    out.bytes(std::span<const std::uint8_t>(bytes).subspan(sections.end));
    return out.take();
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    REQUIRE(at + 4 <= bytes.size());
    ByteReader in(std::span<const std::uint8_t>(bytes).subspan(at, 4));
    return in.u32();
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    REQUIRE(at + 4 <= bytes.size());
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
    }
}

void expect_rejected(const std::vector<std::uint8_t>& bytes) {
    const auto result = load_state(bytes);
    REQUIRE_FALSE(result.has_value());
}

family::State& family_state(World& world) {
    return const_cast<family::State&>(world.family());
}

bool write_fixture_requested() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    const auto error = _dupenv_s(&value, &length, "PATRIARCH_WRITE_E8_FIXTURE");
    const bool requested = error == 0 && value != nullptr && length == 2 && value[0] == '1';
    std::free(value);
    return requested;
#else
    const char* value = std::getenv("PATRIARCH_WRITE_E8_FIXTURE");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
#endif
}

}  // namespace

TEST_CASE("family save: every table and archived reference round-trips in the current schema") {
    const World world = make_family_save_world();
    const auto bytes = serialize_save(world);
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().summary.state_schema == World::kStateSchema);
    CHECK(loaded.value().world.family() == world.family());
    CHECK(loaded.value().world.economy() == world.economy());
    CHECK(loaded.value().world.compute_hash() == world.compute_hash());
    require_valid(loaded.value().world);
    CHECK(serialize_save(loaded.value().world) == bytes);
    CHECK(state_bytes(loaded.value().world) == state_bytes(world));
    const auto& restored = loaded.value().world;
    const auto& negotiation = restored.family().negotiations.front();
    const auto& betrothal = restored.family().betrothals.front();
    const auto& marriage = restored.family().marriages.front();
    CHECK(restored.engagement_of(negotiation.groom) == 1);
    CHECK(restored.engagement_of(betrothal.bride) == 2);
    REQUIRE(restored.family_marriage_of(marriage.groom) != nullptr);
    CHECK(restored.family_marriage_of(marriage.groom)->id == marriage.id);
    const auto& dispute = restored.family().disputes.front();
    CHECK(restored.is_archived(dispute.claimants[0].character));
    CHECK(restored.family_caretaker(dispute.dynasty) == dispute.claimants[1].character);
}

TEST_CASE("family save: committed schema-12 fixture round-trips") {
    const World expected = make_family_save_world();
    const auto path = fixture_path("dynasty_v12_e8.psv");
    if (write_fixture_requested()) {
        const auto bytes = serialize_save(expected);
        REQUIRE(write_file_atomic(path, bytes).has_value());
        MESSAGE("wrote " << path.string() << ", bytes=" << bytes.size()
                         << ", total=" << expected.compute_hash().total);
    }
    const auto file = read_file(path);
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().summary.state_schema == 12);
    CHECK(loaded.value().world.family() == expected.family());
    CHECK(loaded.value().world.compute_hash() == expected.compute_hash());
    CHECK(state_bytes(loaded.value().world) == state_bytes(expected));
    CHECK(serialize_save(loaded.value().world) == file.value());
    require_valid(loaded.value().world);
}

TEST_CASE("family save: schema-11 economy fixture keeps its original full world hash") {
    const auto file = read_file(fixture_path("dynasty_v11_e8_economy.psv"));
    REQUIRE(file.has_value());
    REQUIRE(file.value().size() == 726);
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const World& world = loaded.value().world;
    WorldHash expected{};
    expected.total = 15479572076255755579ULL;
    expected.calendar = 14369850264643241161ULL;
    expected.rng = 4172692673313245204ULL;
    expected.dynasties = 12330653458028426818ULL;
    expected.provinces = 17062011115320500544ULL;
    expected.characters = 12579569978138345613ULL;
    expected.ext[0] = 12478008331234465636ULL;
    expected.ext[1] = 6662615029219399614ULL;
    expected.ext[5] = 2318443219762437030ULL;
    expected.ext[16] = 12620759142815849368ULL;
    CHECK(world.compute_hash() == expected);
    CHECK(world.family() == family::State{});
    CHECK(world.today() == Date::from_ymd(1703, 2, 1));
    CHECK(world.treasury(economy::AccountRef::of(DynastyId(0))) == 152'124);
    CHECK(world.treasury(economy::AccountRef::crown()) == 1'958'210);
    CHECK(world.economy().meta.closed_months == 1);
    REQUIRE(world.economy().loans.size() == 1);
    CHECK(world.economy().loans.loan_id[0] == 0);
    CHECK(world.economy().loans.principal[0] == 45'000);
    const auto borrower = world.resolve_account(economy::AccountRef::of(DynastyId(0)));
    REQUIRE(borrower.has_value());
    const auto tax = economy::ledger_index(economy::LedgerCode::TaxPeasants);
    const auto loan = economy::ledger_index(economy::LedgerCode::LoanTaken);
    REQUIRE(tax.has_value());
    REQUIRE(loan.has_value());
    CHECK(world.economy().accounts.ring_lines(*borrower, 0)[*tax] == 12'345);
    CHECK(world.economy().accounts.ring_lines(*borrower, 0)[*loan] == 50'000);
    const auto current = serialize_save(world, loaded.value().pending_internal);
    auto again = deserialize_save(current);
    REQUIRE(again.has_value());
    CHECK(again.value().world.compute_hash() == expected);
    CHECK(again.value().world.economy() == world.economy());
    CHECK(serialize_save(again.value().world, again.value().pending_internal) == current);
    require_valid(again.value().world);
}

TEST_CASE("family save: advanced counters survive after negotiations and betrothals are closed") {
    World world = make_family_save_world();
    const auto negotiations = world.family().negotiations;
    const auto betrothals = world.family().betrothals;
    for (const auto& row : negotiations) {
        REQUIRE(world.close_family_negotiation(row.id));
    }
    for (const auto& row : betrothals) {
        REQUIRE(world.close_family_betrothal(row.id));
    }
    const auto before = world.family();
    auto loaded = deserialize_save(serialize_save(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.family() == before);
    CHECK(loaded.value().world.compute_hash() == world.compute_hash());
    REQUIRE(loaded.value().world.create_family_negotiation(proposal(loaded.value().world)).has_value());
    CHECK(loaded.value().world.family().negotiations.front().id == before.next_negotiation_id);
    CHECK(loaded.value().world.family().next_negotiation_id == before.next_negotiation_id + 1);

    World counters_only(fixture_date(), 298);
    REQUIRE(counters_only.enable_dynasty_mode());
    auto& state = family_state(counters_only);
    state.next_negotiation_id = 7;
    state.next_betrothal_id = 8;
    state.next_marriage_id = 9;
    state.next_outcome_seq = std::numeric_limits<std::uint32_t>::max();
    require_valid(counters_only);
    auto copy = deserialize_save(serialize_save(counters_only));
    REQUIRE(copy.has_value());
    CHECK(copy.value().world.family() == counters_only.family());
    CHECK(copy.value().world.compute_hash() == counters_only.compute_hash());
    CHECK(copy.value().world.compute_hash().ext[8] != 0);
}

TEST_CASE("family save: defaults are absent while a lone cursor persists in its own part") {
    World world(fixture_date(), 298);
    REQUIRE(world.enable_dynasty_mode());
    const auto empty = sections_of(state_bytes(world));
    CHECK(std::none_of(empty.rows.begin(), empty.rows.end(), [](const Section& row) {
        return row.id >= 16 && row.id <= 23;
    }));
    for (std::size_t part = 8; part <= 14; ++part) {
        CHECK(world.compute_hash().ext[part] == 0);
    }
    const auto house = world.create_dynasty(StringId(1));
    (void)person(world, house);
    REQUIRE(world.set_family_godparent_cursor(1));
    const auto bytes = state_bytes(world);
    auto loaded = load_state(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().family().godparent_cursor == 1);
    CHECK(loaded.value().compute_hash() == world.compute_hash());
    for (std::size_t part = 8; part <= 14; ++part) {
        if (part == 12) {
            CHECK(world.compute_hash().ext[part] != 0);
        } else {
            CHECK(world.compute_hash().ext[part] == 0);
        }
    }
}

TEST_CASE("family hash: each table affects only its assigned part and the total") {
    const World source = make_family_save_world();
    const std::array<std::function<void(family::State&)>, 7> edits{
        [](family::State& s) { ++s.negotiations.front().cost_kop; },
        [](family::State& s) { s.betrothals.front().wedding_deadline += 1; },
        [](family::State& s) { ++s.marriages[1].refunded_kop; },
        [](family::State& s) {
            s.person_statuses.front().widowed_on = s.person_statuses.front().widowed_on - 1;
        },
        [](family::State& s) { ++s.godparent_cursor; },
        [](family::State& s) { ++s.regencies.front().last_risk_permille; },
        [](family::State& s) { ++s.disputes.front().claimants[0].score; },
    };
    const WorldHash before = source.compute_hash();
    for (std::size_t i = 0; i < edits.size(); ++i) {
        World changed = source;
        edits[i](family_state(changed));
        require_valid(changed);
        const WorldHash after = changed.compute_hash();
        const auto part = 8 + i;
        CAPTURE(part);
        CHECK(after.total != before.total);
        CHECK(after.calendar == before.calendar);
        CHECK(after.rng == before.rng);
        CHECK(after.dynasties == before.dynasties);
        CHECK(after.provinces == before.provinces);
        CHECK(after.characters == before.characters);
        for (std::size_t ext = 0; ext < before.ext.size(); ++ext) {
            if (ext == part) {
                CHECK(after.ext[ext] != before.ext[ext]);
            } else {
                CHECK(after.ext[ext] == before.ext[ext]);
            }
        }
    }
}

TEST_CASE("family sections: lengths versions counts and row order are checked before use") {
    const auto bytes = state_bytes(make_family_save_world());
    constexpr std::array<std::size_t, 8> row_sizes{0, 55, 50, 54, 17, 16, 24, 50};
    for (std::uint16_t id = 16; id <= 23; ++id) {
        CAPTURE(id);
        expect_rejected(edit_sections(bytes, [&](Sections& sections) { section(sections, id).version = 2; }));
        for (int cut = 0; cut < 3; ++cut) {
            expect_rejected(edit_sections(bytes, [&](Sections& sections) {
                auto& body = section(sections, id).body;
                body.resize(cut == 0 ? 0 : cut == 1 ? body.size() / 2 : body.size() - 1);
            }));
        }
        expect_rejected(
            edit_sections(bytes, [&](Sections& sections) { section(sections, id).body.push_back(0); }));
        if (id == 16) {
            for (std::size_t counter = 0; counter < 4; ++counter) {
                expect_rejected(edit_sections(
                    bytes, [&](Sections& sections) { put32(section(sections, 16).body, counter * 4, 0); }));
            }
            continue;
        }
        const std::size_t count_at = id == 21 ? 4 : 0;
        expect_rejected(edit_sections(bytes, [&](Sections& sections) {
            put32(section(sections, id).body, count_at, std::numeric_limits<std::uint32_t>::max());
        }));
        expect_rejected(edit_sections(bytes, [&](Sections& sections) {
            auto& body = section(sections, id).body;
            const auto n = get32(body, count_at);
            REQUIRE(n > 0);
            const auto row_size = row_sizes[id - 16];
            const auto start = count_at + 4;
            const std::vector<std::uint8_t> first(body.begin() + static_cast<std::ptrdiff_t>(start),
                                                  body.begin() +
                                                      static_cast<std::ptrdiff_t>(start + row_size));
            body.insert(body.begin() + static_cast<std::ptrdiff_t>(start), first.begin(), first.end());
            put32(body, count_at, n + 1);
        }));
    }
}

TEST_CASE("family sections: bounded malformed enum reference date and counter corpus is refused") {
    const auto bytes = state_bytes(make_family_save_world());
    const std::array<std::function<void(Sections&)>, 19> damage{
        [](Sections& s) { section(s, 17).body[4 + 54] = 255; },
        [](Sections& s) { section(s, 18).body[4 + 49] = 255; },
        [](Sections& s) { section(s, 19).body[4 + 32] = 255; },
        [](Sections& s) { section(s, 19).body[4 + 37] = 255; },
        [](Sections& s) { section(s, 20).body[4 + 4] = 128; },
        [](Sections& s) { section(s, 22).body[4 + 12] = 255; },
        [](Sections& s) { section(s, 23).body[4 + 8] = 255; },
        [](Sections& s) { section(s, 23).body[4 + 17] = 5; },
        [](Sections& s) { put32(section(s, 17).body, 4 + 8, 999'999); },
        [](Sections& s) { put32(section(s, 18).body, 4 + 24, 0); },
        [](Sections& s) { put32(section(s, 16).body, 0, 1); },
        [](Sections& s) { put32(section(s, 16).body, 4, 1); },
        [](Sections& s) { put32(section(s, 16).body, 8, 1); },
        [](Sections& s) {
            auto& body = section(s, 17).body;
            const auto outcomes_at = 4 + 55 * static_cast<std::size_t>(get32(body, 0));
            put32(body, outcomes_at, 65);
        },
        [](Sections& s) {
            auto& body = section(s, 17).body;
            const auto outcome_at = 8 + 55 * static_cast<std::size_t>(get32(body, 0));
            body[outcome_at + 32] = 255;
        },
        [](Sections& s) {
            auto& body = section(s, 17).body;
            const auto outcome_at = 8 + 55 * static_cast<std::size_t>(get32(body, 0));
            put32(body, outcome_at + 35, 999);
        },
        [](Sections& s) { put32(section(s, 21).body, 0, 999'999); },
        [](Sections& s) { put32(section(s, 22).body, 4 + 4, 999'999); },
        [](Sections& s) { put32(section(s, 23).body, 4 + 18, 999'999); },
    };
    for (std::size_t i = 0; i < damage.size(); ++i) {
        CAPTURE(i);
        expect_rejected(edit_sections(bytes, damage[i]));
    }
    expect_rejected(
        edit_sections(bytes, [](Sections& sections) { sections.rows.push_back(section(sections, 16)); }));
    auto earlier_schema = bytes;
    put32(earlier_schema, 0, 11);
    expect_rejected(earlier_schema);
}

TEST_CASE("family save: queued death and empty-title disputes resume without gameplay systems") {
    World world = make_family_save_world();
    const auto victim = world.family().negotiations.front().groom;
    const std::vector<Command> pending{{DynastyId(), cmd::Die{victim, CauseOfDeath::Illness}}};
    Simulation continuous(world);
    continuous.restore_state(world, pending);
    auto loaded = deserialize_save(serialize_save(world, pending));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().pending_internal == pending);
    Simulation resumed(std::move(loaded.value().world));
    resumed.restore_state(resumed.world(), loaded.value().pending_internal);
    for (int day = 0; day < 3; ++day) {
        (void)continuous.tick();
        (void)resumed.tick();
        CHECK(continuous.world().family() == resumed.world().family());
        CHECK(continuous.world().compute_hash() == resumed.world().compute_hash());
        CHECK(state_bytes(continuous.world()) == state_bytes(resumed.world()));
        for (const auto& dispute : resumed.world().family().disputes) {
            CHECK_FALSE(resumed.world().dynasties().head[dispute.dynasty.index()].valid());
        }
        require_valid(resumed.world());
    }
    CHECK(resumed.world().is_archived(victim));
    CHECK(resumed.world().family_negotiation_of(victim) == nullptr);
}

TEST_CASE("family replay: existing death commands reproduce family cleanup and saved explanations") {
    constexpr std::uint64_t scenario = 0xE8'0003ULL;
    Simulation recorded(make_family_save_world());
    const auto victim = recorded.world().family().negotiations.front().groom;
    ReplayRecorder recorder(298, scenario, recorded.world().today(), 1);
    for (int day = 0; day < 3; ++day) {
        if (day == 1) {
            REQUIRE(recorded.submit(Command{DynastyId(), cmd::Die{victim, CauseOfDeath::Illness}}));
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    auto replay = deserialize_replay(serialize_replay(recorder.finish(recorded)));
    REQUIRE(replay.has_value());
    REQUIRE(replay.value().commands.size() == 1);
    Simulation again(make_family_save_world());
    const auto result = play_replay(again, replay.value());
    CHECK(result.ok);
    CHECK(again.world().family() == recorded.world().family());
    CHECK(again.world().compute_hash() == recorded.world().compute_hash());
    CHECK(state_bytes(again.world()) == state_bytes(recorded.world()));
}
