#include "pat/content/economy_content.hpp"
#include "pat/content/load_errors.hpp"
#include "pat/content/m1_economy_content.hpp"
#include "pat/content/m1_scenario.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace pat;
using namespace pat::sim;
using namespace pat::sim::economy;
using Files = std::map<std::string, std::string>;

Files shipped_files(bool include_scenario = true) {
    const data::DirectorySource directory{std::string(PATRIARCH_DATA_DIR)};
    const auto paths = directory.list_files();
    REQUIRE(paths.has_value());
    Files files;
    for (const std::string& path : paths.value()) {
        if (!include_scenario && path.starts_with("scenarios/m1/")) {
            continue;
        }
        auto read = directory.read_file(path);
        REQUIRE(read.has_value());
        std::string text = std::move(read).value();
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        files.emplace(path, std::move(text));
    }
    return files;
}

data::MemorySource source_of(const Files& files) {
    data::MemorySource source("M1 economy test");
    for (const auto& [path, text] : files) {
        source.add(path, text);
    }
    return source;
}

std::string fixture() {
    std::string text = R"PD(format_version = 1
trade_node "fixture_moscow" { slot = 9 name_key = "node.fixture_moscow" }
trade_node "fixture_north" { slot = 10 name_key = "node.fixture_north" }
trade_node "fixture_volga" { slot = 11 name_key = "node.fixture_volga" }
economy_start "fixture" {
    crown_treasury_kop = 1234500
    dynasty_treasuries = { golitsyn = 600000 menshikov = 700000 naryshkin = 800000 sheremetev = 900000 }
}
)PD";
    constexpr std::array<std::string_view, 3> nodes{"fixture_moscow", "fixture_north", "fixture_volga"};
    for (int id = 1; id <= 20; ++id) {
        text += std::format(R"PD(holding "fixture_p{0:04}" {{
    province = province:p{0:04}
    kind = province_kind:rural
    owner_kind = dynasty
    owner = dynasty:golitsyn
    trade_node = trade_node:{1}
    development = 3
    slots_total = 2
    population = {{ nobility = 10 clergy = 5 townsfolk = 50 peasants = 1200 cossacks = 0 natives = 0 }}
    capacity = {{ grain = 250 }}
    mood = {{ peasants = 5 }}
    wealth = {{ peasants = 50 }}
    control = 80
    corruption = 0.05
    unrest = 0
    grain_stock = 1000
    buildings = [ building:mill ]
}}
)PD",
                            id,
                            nodes[static_cast<std::size_t>(id - 1) % nodes.size()]);
    }
    return text;
}

std::string replace_first(std::string text, std::string_view before, std::string_view after) {
    const auto at = text.find(before);
    INFO(before);
    REQUIRE(at != std::string::npos);
    text.replace(at, before.size(), after);
    return text;
}

content::M1EconomyContentResult build_fixture(const std::string& text) {
    Files files = shipped_files(false);
    files.emplace("fixtures/m1_economy.pd", text);
    const auto source = source_of(files);
    const auto loaded = data::load_content(source, data::content_schemas());
    INFO(content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    const auto rules = content::build_economy_rules(loaded.registry);
    REQUIRE(rules.has_value());
    return content::build_m1_economy_content(loaded.registry, rules.value());
}

Simulation simulation_for(const content::M1Scenario& scenario) {
    REQUIRE(scenario.economy_rules != nullptr);
    SimulationConfig config;
    config.traits = scenario.traits;
    config.economy_rules = scenario.economy_rules;
    Simulation simulation(scenario.world, std::move(config));
    install_economy_systems(simulation, *scenario.economy_rules);
    return simulation;
}

content::M1Scenario scenario() {
    const auto map = mapdata::generate_map(content::m1_map_params());
    auto result = content::load_m1_scenario(PATRIARCH_DATA_DIR, map, 7);
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result.has_value());
    }
    return std::move(result).value();
}

void require_valid(const World& world, const EconomyRules& rules) {
    const auto structural = world.check_invariants();
    for (const auto& problem : structural) {
        INFO(problem);
    }
    REQUIRE(structural.empty());
    const auto semantic = check_rules_invariants(world, rules);
    for (const auto& problem : semantic) {
        INFO(problem);
    }
    REQUIRE(semantic.empty());
}

}  // namespace

TEST_CASE("M1 economy content: roots without scenario data keep the legacy definition mode") {
    const auto source = source_of(shipped_files(false));
    const auto loaded = data::load_content(source, data::content_schemas());
    INFO(content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    const auto rules = content::build_economy_rules(loaded.registry);
    REQUIRE(rules.has_value());
    const auto empty = content::build_m1_economy_content(loaded.registry, rules.value());
    REQUIRE(empty.has_value());
    CHECK(empty.value().holdings.empty());
    CHECK(empty.value().trade_nodes.empty());
    CHECK(empty.value().dynasty_treasuries.empty());

    Files partial = shipped_files(false);
    partial.emplace("fixtures/m1_economy.pd", R"PD(format_version = 1
trade_node "only_node" { slot = 1 name_key = "node.only" }
)PD");
    const auto partial_source = source_of(partial);
    const auto parsed = data::load_content(partial_source, data::content_schemas());
    REQUIRE(parsed.ok());
    CHECK_FALSE(content::build_m1_economy_content(parsed.registry, rules.value()).has_value());
}

TEST_CASE("M1 economy content: stable node slots and province keys are independent of registry ids") {
    const auto built = build_fixture(fixture());
    if (!built) {
        INFO(content::format_errors(built.error()));
        REQUIRE(built.has_value());
    }
    REQUIRE(built.value().holdings.size() == 20);
    REQUIRE(built.value().trade_nodes.size() == 3);
    CHECK(built.value().trade_nodes[0].slot == 9);
    CHECK(built.value().trade_nodes[1].slot == 10);
    CHECK(built.value().trade_nodes[2].slot == 11);
    const auto& first = built.value().holdings.front();
    CHECK(first.province == "p0001");
    CHECK(first.trade_node == 9);
    CHECK(first.population[static_cast<std::size_t>(Group::Peasants)] == 1200);
    CHECK(first.capacity[0] == core::Fixed32::from_int(250));
    CHECK(first.buildings == std::vector<std::uint8_t>{1});
    CHECK(built.value().crown_treasury_kop == 1234500);
}

TEST_CASE("M1 economy content: invalid aggregate state and unsupported map keys are source located") {
    struct InvalidEdit {
        const char* name;
        const char* before;
        const char* after;
    };
    constexpr InvalidEdit edits[] = {
        {"duplicate node slot", "slot = 10", "slot = 9"},
        {"unknown population group", "nobility = 10", "unknown_group = 10"},
        {"unknown capacity good", "grain = 250", "unknown_good = 250"},
        {"population total", "nobility = 10 clergy = 5", "nobility = 32000000 clergy = 32000000"},
        {"owner kind", "owner_kind = dynasty", "owner_kind = crown"},
        {"building slot count", "slots_total = 2", "slots_total = 0"},
        {"duplicate building",
         "buildings = [ building:mill ]",
         "buildings = [ building:mill building:mill ]"},
        {"unknown treasury dynasty", "golitsyn = 600000", "unknown_dynasty = 600000"},
    };
    for (const auto& edit : edits) {
        SUBCASE(edit.name) {
            const auto invalid = build_fixture(replace_first(fixture(), edit.before, edit.after));
            REQUIRE_FALSE(invalid.has_value());
            CHECK(
                std::any_of(invalid.error().begin(), invalid.error().end(), [](const data::LoadError& error) {
                    return error.file == "fixtures/m1_economy.pd" && error.diagnostic.pos.line > 0 &&
                           error.diagnostic.pos.column > 0;
                }));
        }
    }
    SUBCASE("duplicate province coverage") {
        const auto invalid = build_fixture(replace_first(fixture(), "province:p0020", "province:p0019"));
        REQUIRE_FALSE(invalid.has_value());
        CHECK_FALSE(invalid.error().empty());
    }
    SUBCASE("building kind") {
        const auto invalid = build_fixture(replace_first(fixture(), "building:mill", "building:manufactory"));
        REQUIRE_FALSE(invalid.has_value());
    }
    SUBCASE("building development") {
        std::string text = replace_first(fixture(), "building:mill", "building:manufactory");
        text = replace_first(std::move(text), "kind = province_kind:rural", "kind = province_kind:city");
        text = replace_first(std::move(text), "development = 3", "development = 1");
        CHECK_FALSE(build_fixture(text).has_value());
    }
}

TEST_CASE("M1 economy content: invalid references and numeric corridors fail schema loading") {
    for (const auto& [before, after] : {std::pair{"trade_node:fixture_moscow", "trade_node:missing"},
                                        std::pair{"owner = dynasty:golitsyn", "owner = dynasty:missing"},
                                        std::pair{"slots_total = 2", "slots_total = 7"}}) {
        Files files = shipped_files(false);
        files.emplace("fixtures/m1_economy.pd", replace_first(fixture(), before, after));
        const auto source = source_of(files);
        const auto invalid = data::load_content(source, data::content_schemas());
        CHECK_FALSE(invalid.ok());
        CHECK(std::any_of(invalid.errors.begin(), invalid.errors.end(), [](const data::LoadError& error) {
            return error.file == "fixtures/m1_economy.pd" && error.diagnostic.pos.line > 0 &&
                   error.diagnostic.pos.column > 0;
        }));
    }
}

TEST_CASE("M1 economy scenario: shipped holdings initialize populated provinces and clean opening balances") {
    const auto start = scenario();
    REQUIRE(start.economy_rules != nullptr);
    const auto& world = start.world;
    require_valid(world, *start.economy_rules);
    const auto first = start.map_to_world.at(1);
    REQUIRE(world.exists(first));
    const auto index = first.index();
    const auto& economy = world.economy();
    CHECK(world.population_persons(first) == 12800);
    CHECK(world.provinces().population[index] == core::Fixed32::from_ratio(12800, 1000));
    CHECK(world.provinces().development[index] == 3);
    CHECK(economy.provinces.slots_total[index] == 3);
    CHECK(economy.provinces.trade_node[index] == 1);
    CHECK(economy.provinces.slot_building[index * kMaxSlots] == 1);
    CHECK(economy.provinces.control[index] == core::Fixed32::from_int(80));
    REQUIRE(world.provinces().owner[index].valid());
    const auto* owner = start.registry.find("dynasty", "golitsyn");
    REQUIRE(owner != nullptr);
    CHECK(world.dynasties().name[world.provinces().owner[index].index()] == owner->id);
    std::array<bool, 4> seen_nodes{};
    for (const ProvinceId province : start.map_to_world) {
        if (!province.valid()) {
            continue;
        }
        REQUIRE(world.population_persons(province) > 0);
        CHECK(economy.provinces.tax_policy[province.index()] == start.economy_rules->tax.default_level);
        CHECK(economy.provinces.recruit_policy[province.index()] ==
              start.economy_rules->recruit.default_level);
        const auto node = economy.provinces.trade_node[province.index()];
        REQUIRE(node >= 1);
        REQUIRE(node <= 3);
        seen_nodes[node] = true;
    }
    CHECK(seen_nodes[1]);
    CHECK(seen_nodes[2]);
    CHECK(seen_nodes[3]);
    bool funded = false;
    CHECK(economy.meta.closed_months == 0);
    for (std::size_t account = 0; account < economy.accounts.size(); ++account) {
        funded = funded || economy.accounts.treasury(account) > 0;
        CHECK(economy.accounts.treasury(account) == economy.accounts.opening(account));
        CHECK(std::all_of(economy.accounts.open_lines(account).begin(),
                          economy.accounts.open_lines(account).end(),
                          [](Kopecks value) { return value == 0; }));
    }
    CHECK(funded);
}

TEST_CASE(
    "M1 economy scenario: configured systems change the economy and survive save continuation and replay") {
    const auto start = scenario();
    Simulation straight = simulation_for(start);
    const auto original_economy = straight.world().economy();
    const DynastyId borrower = straight.world().provinces().owner[start.map_to_world.at(1).index()];
    REQUIRE(borrower.valid());
    ReplayRecorder recorder(7, start.replay_id, straight.world().today(), 15);
    std::vector<std::uint8_t> saved;
    for (int day = 0; day < 62; ++day) {
        if (day == 5) {
            REQUIRE(straight.submit(Command{borrower, cmd::SetTaxPolicy{borrower, 0}}));
        }
        (void)straight.tick();
        recorder.after_tick(straight);
        if (day == 16) {
            saved = serialize_save(straight.world(), straight.pending_internal());
        }
    }
    CHECK(straight.world().economy() != original_economy);
    CHECK(straight.world().economy().meta.closed_months == 2);
    REQUIRE_FALSE(straight.command_log().entries().empty());
    CHECK(straight.command_log().entries().front().outcome == Rejection::None);
    require_valid(straight.world(), *start.economy_rules);
    auto restored = deserialize_save(saved);
    REQUIRE(restored.has_value());
    Simulation resumed = simulation_for(start);
    resumed.restore_state(std::move(restored.value().world), std::move(restored.value().pending_internal));
    resumed.run_days(62 - 17);
    CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
    CHECK(resumed.world().economy() == straight.world().economy());
    const auto replay = recorder.finish(straight);
    REQUIRE(replay.commands.size() == 1);
    Simulation played = simulation_for(start);
    const auto result = play_replay(played, replay);
    INFO(result.error);
    CHECK(result.ok);
    CHECK(played.world().compute_hash() == straight.world().compute_hash());
}

TEST_CASE("M1 economy scenario: initial treasury seeding leaves no ledger income and cannot rewrite live "
          "accounts") {
    World world(Date::from_ymd(1703, 1, 1), 7);
    const DynastyId dynasty = world.create_dynasty({});
    REQUIRE(world.enable_economy());
    const AccountRef account = AccountRef::of(dynasty);
    REQUIRE(world.set_initial_treasury(account, 123456));
    const auto row = world.resolve_account(account);
    REQUIRE(row.has_value());
    CHECK(world.economy().accounts.opening(*row) == 123456);
    CHECK(world.treasury(account) == 123456);
    CHECK(world.economy().meta.closed_months == 0);
    const auto seeded = world.compute_hash();
    CHECK_FALSE(world.set_initial_treasury(account, 1));
    CHECK_FALSE(world.set_initial_treasury(AccountRef::crown(), kMaxTreasuryKop + 1));
    CHECK(world.compute_hash() == seeded);
    (void)world.advance_day();
    const auto advanced = world.compute_hash();
    CHECK_FALSE(world.set_initial_treasury(AccountRef::crown(), 99));
    CHECK(world.compute_hash() == advanced);
    CHECK(world.check_invariants().empty());
}
