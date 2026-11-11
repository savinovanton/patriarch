#include "pat/core/rng.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/recruits.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/mortality.hpp"
#include "pat/sim/opinion.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// E7-T05: model tests of the World (docs/epics/E07-characters-dynasty.md, GDD 01 section 13.3).
//
// Random sequences of commands are thrown at a small world in dynasty mode with the economy on. The
// generator uses the test's OWN Pcg32 (never the streams of the world), so a sequence is a pure
// function of its seed. After every step the test checks, with code that shares nothing with
// World::check_invariants():
//   * kinship: parents exist, have the right sex, are older, and the ancestor graph has no cycle;
//     marriages are symmetric;
//   * indexes: the living list, the member lists, the province lists, the dynasty heads and the
//     life-event queue (ScheduledQueue) agree with the columns;
//   * economy: every account balances (treasury = opening + open lines), money moves only through the
//     statement, the groups add up to the population cache;
//   * the simplified model (who lives, who is married to whom, heads, honor, owners, administrators)
//     predicts both the outcome of every command and the resulting state;
// and World::check_invariants() itself. Further: the hash is the same before and after a save/load,
// a run that reloads in the middle ends where the uninterrupted one does, a replay reproduces the
// run, and the result does not depend on how the days are split between Simulation objects.
//
// REPRODUCING A FAILURE. Every message starts with "seed N": add N to kRegressionSeeds below (both
// the command sequences and the simulation runs take them) and run
//   patriarch_tests -tc="model: regression seeds*"
//
// BUDGET (one core): the whole file within 20 s in Debug, 100 s under ASan and 3 s in RelWithDebInfo.
// World::check_invariants() after every step is about half of it. The sizes below are the knobs; the
// times measured on the development machine are in the pull request of E7-T05.

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;
namespace eco = pat::sim::economy;

namespace {

constexpr std::uint32_t kSequences = 10'000;  // acceptance of E7-T05: 10 000+ sequences
constexpr std::uint32_t kStepsPerSequence = 12;
constexpr std::uint64_t kEconomyEvery = 5;     // every fifth sequence has the economy switched on
constexpr std::int32_t kReloadPermille = 250;  // share of the sequences that save and load midway
constexpr std::uint64_t kTemplateWorlds = 32;
constexpr std::uint32_t kSimulationSeeds = 16;
constexpr int kSimulationDays = 150;  // five month closings, 28/29/30/31-day months
constexpr std::uint64_t kBaseSeed = 0x0E7'0005'0000ULL;
constexpr std::uint64_t kGeneratorStream = 0x6D6F64656CULL;  // "model"
constexpr int kMaxReportedFailures = 5;

/// Seeds that once failed (or are worth keeping). Add the seed of a failure message here.
constexpr std::array<std::uint64_t, 3> kRegressionSeeds{1, 170, 0x0E7'0005'0000ULL + 4242};

constexpr std::uint32_t kDynasties = 4;
constexpr std::uint32_t kProvinces = 6;
constexpr std::uint32_t kFounders = 10;

Date start_date() {
    return Date::from_ymd(1703, 1, 1);
}

const eco::EconomyRules& rules() {
    static const eco::EconomyRules value = eco::EconomyRules::defaults();
    return value;
}

// ---- the scenario -------------------------------------------------------------------------------

/// A small world: four houses, six provinces (one of the crown, one ownerless, one city), ten adults.
/// Without the economy the provinces only have owners (the E1 state).
World build_world(std::uint64_t seed, bool with_economy = true) {
    World world(start_date(), seed);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        const DynastyId owner = p + 1 < kProvinces ? DynastyId(p % kDynasties) : DynastyId();
        (void)world.create_province(StringId(100 + p), owner, Fixed32::from_int(1));
    }
    Pcg32 rng(seed, 7);
    for (std::uint32_t c = 0; c < kFounders; ++c) {
        CharacterInit init;
        init.dynasty = DynastyId(c % kDynasties);
        init.birth_date = Date::from_ymd(1650 + rng.range(0, 30), 1 + rng.range(0, 11), 1 + rng.range(0, 27));
        init.sex = (c / kDynasties) % 2 == 0 ? Sex::Male : Sex::Female;
        REQUIRE(world.create_character(init).valid());
    }
    if (!with_economy) {
        return world;
    }
    REQUIRE(world.enable_economy());
    REQUIRE(
        world.set_economy_policy_levels(4, 3));  // the levels of the rules: the range of the policy commands
    eco::EconomyColumnsMut columns = world.economy_columns_mut();
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        const auto peasants = static_cast<std::uint32_t>(rng.range(4'000, 30'000));
        const auto townsfolk = static_cast<std::uint32_t>(rng.range(0, 3'000));
        (void)world.set_group_size(ProvinceId(p), eco::Group::Peasants, peasants);
        (void)world.set_group_size(ProvinceId(p), eco::Group::Townsfolk, townsfolk);
        columns.tax_policy[p] = static_cast<std::uint8_t>(rng.bounded(4));
        columns.labour_base[p] = peasants + townsfolk;
        columns.capacity[p * eco::kMaxGoods + eco::kGrainGood] = Fixed32::from_int(rng.range(100, 2'000));
        columns.control[p] = Fixed32::from_int(rng.range(5, 100));                // some start in turmoil
        columns.recruit_pool[p] = static_cast<std::uint32_t>(rng.range(0, 800));  // people to conscript (T13)
    }
    columns.kind[1] = static_cast<std::uint8_t>(eco::ProvinceKind::City);
    REQUIRE(world.set_province_owner_kind(ProvinceId(4), eco::OwnerKind::Crown));
    (void)world.post_money(eco::AccountRef::crown(), eco::LedgerCode::Adjustment, 5'000'000);
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        (void)world.post_money(
            eco::AccountRef::of(DynastyId(d)), eco::LedgerCode::Adjustment, rng.range(0, 900'000));
    }
    return world;
}

/// A copy of one of kTemplateWorlds prepared worlds: building one costs more than a whole sequence of
/// commands, and the sequences do not draw from the streams of the world.
World template_world(std::uint64_t seed, bool with_economy) {
    static const std::vector<World> worlds = [] {
        std::vector<World> out;
        for (std::uint64_t i = 0; i < 2 * kTemplateWorlds; ++i) {
            out.push_back(build_world(i / 2, i % 2 == 1));
        }
        return out;
    }();
    return worlds[static_cast<std::size_t>((seed % kTemplateWorlds) * 2 + (with_economy ? 1 : 0))];
}

Simulation make_simulation(World world) {
    Simulation sim(std::move(world));
    sim.add_system(make_mortality_system(kDefaultDemography));
    sim.add_system(make_opinion_system());
    eco::install_economy_systems(sim, rules());
    return sim;
}

// ---- the generator ------------------------------------------------------------------------------

CharacterId random_person(Pcg32& rng, const World& world) {
    const auto living = world.living();
    const auto capacity = static_cast<std::uint32_t>(world.characters().ids.capacity());
    if (living.empty() || rng.chance_permille(120)) {
        return CharacterId(rng.bounded(capacity + 2));  // may be dead or may not exist at all
    }
    return living[rng.bounded(static_cast<std::uint32_t>(living.size()))];
}

DynastyId random_dynasty(Pcg32& rng, const World& world) {
    const auto count = static_cast<std::uint32_t>(world.dynasties().size());
    return DynastyId(rng.chance_permille(60) ? count + rng.bounded(2) : rng.bounded(count));
}

ProvinceId random_province(Pcg32& rng, const World& world) {
    const auto count = static_cast<std::uint32_t>(world.provinces().size());
    return ProvinceId(rng.chance_permille(60) ? count + rng.bounded(2) : rng.bounded(count));
}

/// The actor of a command: mostly the system, sometimes a house (which is then often refused).
DynastyId random_actor(Pcg32& rng, const World& world, std::int32_t house_permille) {
    return rng.chance_permille(house_permille) ? random_dynasty(rng, world) : DynastyId();
}

/// One random command, valid or not. Reads the world only to pick ids that mean something.
Command random_command(Pcg32& rng, const World& world) {
    const Date today = world.today();
    const std::uint32_t roll = rng.bounded(100);
    if (roll < 22) {
        CharacterInit init;
        init.dynasty = random_dynasty(rng, world);
        init.birth_date = rng.chance_permille(40) ? today + rng.range(1, 400) : today - rng.range(0, 5'000);
        init.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
        if (rng.chance_permille(500)) {
            init.father = random_person(rng, world);
        }
        if (rng.chance_permille(500)) {
            init.mother = random_person(rng, world);
        }
        return Command{random_actor(rng, world, 50), cmd::CreatePerson{init}};
    }
    if (roll < 40) {
        return Command{random_actor(rng, world, 40),
                       cmd::Marry{random_person(rng, world), random_person(rng, world)}};
    }
    if (roll < 48) {
        return Command{random_actor(rng, world, 40),
                       cmd::EndMarriage{random_person(rng, world), static_cast<MarriageEnd>(rng.bounded(3))}};
    }
    if (roll < 60) {
        return Command{random_actor(rng, world, 40),
                       cmd::Die{random_person(rng, world), static_cast<CauseOfDeath>(rng.bounded(11))}};
    }
    if (roll < 68) {
        const CharacterId head = rng.chance_permille(150) ? CharacterId() : random_person(rng, world);
        return Command{random_actor(rng, world, 400), cmd::SetDynastyHead{random_dynasty(rng, world), head}};
    }
    if (roll < 76) {
        const DynastyId actor = random_actor(rng, world, 40);
        const DynastyId dynasty = random_dynasty(rng, world);
        const std::int32_t delta = rng.range(-4'000, 4'000);
        switch (rng.bounded(6)) {
            case 0:
                return Command{actor, cmd::AdjustHonorWithReason{dynasty, delta, kStandingReasonUnspecified}};
            case 1:
                return Command{
                    actor,
                    cmd::AdjustUnity{dynasty, Fixed32::from_int(delta / 100), kStandingReasonUnspecified}};
            case 2:
                return Command{
                    actor,
                    cmd::AdjustPrestige{dynasty, Fixed32::from_int(delta / 100), kStandingReasonUnspecified}};
            default:
                return Command{actor, cmd::AdjustHonor{dynasty, delta}};
        }
    }
    if (roll < 86) {
        const ProvinceId province = random_province(rng, world);
        const DynastyId new_owner = rng.chance_permille(150) ? DynastyId() : random_dynasty(rng, world);
        DynastyId actor = random_actor(rng, world, 300);
        if (world.exists(province) && rng.chance_permille(300)) {
            actor = world.provinces().owner[province.index()];
        }
        return Command{actor, cmd::SetProvinceOwner{province, new_owner}};
    }
    if (roll < 90) {
        // a policy of a dynasty: mostly in the range of the rules (4 and 3 levels), sometimes beyond it
        const auto level =
            static_cast<std::uint8_t>(rng.chance_permille(150) ? rng.bounded(8) : rng.bounded(4));
        const DynastyId dynasty = random_dynasty(rng, world);
        DynastyId actor = random_actor(rng, world, 250);
        if (rng.chance_permille(400)) {
            actor = dynasty;
        }
        if (rng.chance_permille(500)) {
            return Command{actor, cmd::SetTaxPolicy{dynasty, level}};
        }
        return Command{actor, cmd::SetRecruitPolicy{dynasty, level}};
    }
    if (roll < 94) {
        const ProvinceId province = random_province(rng, world);
        const CharacterId who = rng.chance_permille(150) ? CharacterId() : random_person(rng, world);
        DynastyId actor = random_actor(rng, world, 200);
        if (world.exists(province) && rng.chance_permille(400)) {
            actor = world.provinces().owner[province.index()];
        }
        return Command{actor, cmd::AssignProvinceAdministrator{province, who}};
    }
    if (roll < 96) {
        // conscription (E9-T13): mostly of a pool that exists, sometimes of one more than there is
        const ProvinceId province = random_province(rng, world);
        DynastyId actor = random_actor(rng, world, 200);
        std::uint32_t count = rng.bounded(3) == 0 ? 0 : 1 + rng.bounded(1'000);
        if (world.exists(province) && world.economy_present()) {
            const std::size_t p = province.index();
            if (rng.chance_permille(400)) {
                actor = world.provinces().owner[p];
            }
            const std::uint32_t pool = world.economy().provinces.recruit_pool[p];
            if (rng.chance_permille(700)) {
                count = 1 + rng.bounded(pool + 1);  // up to one more than the pool
            }
        }
        return Command{actor, cmd::ConscriptRecruits{province, count}};
    }
    if (roll < 97) {
        const Date planned = rng.chance_permille(200) ? kNoDate : today + rng.range(-30, 120);
        return Command{random_actor(rng, world, 40), cmd::PlanDeath{random_person(rng, world), planned}};
    }
    if (roll < 99) {
        return Command{random_actor(rng, world, 40),
                       cmd::AdjustOpinion{random_person(rng, world),
                                          random_person(rng, world),
                                          Fixed32::from_int(rng.range(-20, 20)),
                                          kOtherOpinionReason}};
    }
    return Command{DynastyId(), cmd::KillCharacter{random_person(rng, world)}};  // never in dynasty mode
}

// ---- independent checks of the structure ---------------------------------------------------------

std::string kinship_problem(const World& world) {
    const CharacterTable& t = world.characters();
    const std::size_t rows = t.ids.capacity();
    // Depth-first walk over (father, mother) with three colours: 1 = on the path, 2 = finished.
    std::vector<std::uint8_t> colour(rows, 0);
    std::vector<std::pair<std::size_t, int>> stack;
    for (std::size_t root = 0; root < rows; ++root) {
        if (colour[root] != 0 || !world.exists(CharacterId(static_cast<std::uint32_t>(root)))) {
            continue;
        }
        stack.emplace_back(root, 0);
        colour[root] = 1;
        while (!stack.empty()) {
            const std::size_t at = stack.back().first;
            const int edge = stack.back().second++;
            if (edge >= 2) {
                colour[at] = 2;
                stack.pop_back();
                continue;
            }
            const CharacterId parent = edge == 0 ? t.father[at] : t.mother[at];
            if (!parent.valid()) {
                continue;
            }
            if (!world.exists(parent)) {
                return std::format("person {} has a parent {} that does not exist", at, parent.value());
            }
            const std::size_t p = parent.index();
            if (t.sex[p] != (edge == 0 ? Sex::Male : Sex::Female)) {
                return std::format("person {} has a parent {} of the wrong sex", at, p);
            }
            if (t.birth_date[p] >= t.birth_date[at]) {
                return std::format("person {} is not younger than the parent {}", at, p);
            }
            if (colour[p] == 1) {
                return std::format("the ancestors of person {} form a cycle through {}", at, p);
            }
            if (colour[p] == 0) {
                colour[p] = 1;
                stack.emplace_back(p, 0);
            }
        }
    }
    for (const CharacterId id : world.living()) {
        const CharacterId spouse = t.spouse[id.index()];
        if (!spouse.valid()) {
            continue;
        }
        if (spouse == id || !world.is_alive(spouse) || t.spouse[spouse.index()] != id) {
            return std::format("the marriage of person {} is not mutual", id.value());
        }
        if (t.marriages[id.index()] == 0 || t.marriages[id.index()] > kMaxMarriages) {
            return std::format("person {} is married with an impossible marriage count", id.value());
        }
    }
    return {};
}

std::string index_problem(const World& world) {
    const CharacterTable& t = world.characters();
    const std::size_t rows = t.ids.capacity();
    // the living list: strictly ascending and exactly the living rows
    std::vector<CharacterId> alive;
    for (std::size_t i = 0; i < rows; ++i) {
        const CharacterId id(static_cast<std::uint32_t>(i));
        if (world.is_alive(id)) {
            alive.push_back(id);
        }
    }
    const auto living = world.living();
    if (!std::equal(living.begin(), living.end(), alive.begin(), alive.end())) {
        return "the living list differs from the living rows";
    }
    // the member lists and the heads
    for (std::size_t d = 0; d < world.dynasties().size(); ++d) {
        const DynastyId dynasty(static_cast<std::uint32_t>(d));
        std::vector<CharacterId> expected;
        for (const CharacterId id : alive) {
            if (t.dynasty[id.index()] == dynasty) {
                expected.push_back(id);
            }
        }
        const auto listed = world.members_of(dynasty);
        std::vector<CharacterId> members(listed.begin(), listed.end());
        std::sort(members.begin(), members.end());
        if (members != expected) {
            return std::format("the member list of dynasty {} differs from the dynasty column", d);
        }
        if (world.dynasty_extinct(dynasty) != expected.empty()) {
            return std::format("dynasty {} is wrongly reported (not) extinct", d);
        }
        const CharacterId head = world.dynasties().head[d];
        if (head.valid() && (!world.is_alive(head) || t.dynasty[head.index()] != dynasty)) {
            return std::format("the head of dynasty {} is not a living member", d);
        }
        std::vector<ProvinceId> owned;
        for (std::size_t p = 0; p < world.provinces().size(); ++p) {
            if (world.provinces().owner[p] == dynasty) {
                owned.emplace_back(static_cast<std::uint32_t>(p));
            }
        }
        const auto held = world.provinces_of(dynasty);
        std::vector<ProvinceId> provinces(held.begin(), held.end());
        std::sort(provinces.begin(), provinces.end());
        if (provinces != owned) {
            return std::format("the province list of dynasty {} differs from the owner column", d);
        }
    }
    // the life-event queue: canonical order, and every living person has a birthday (and the planned
    // death, if any) in it; stale entries of the dead are allowed and are dropped when they come due
    const auto entries = world.life_queue().snapshot();
    std::vector<std::uint8_t> birthday(rows, 0);
    std::vector<std::uint8_t> death(rows, 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (i > 0 && (entries[i - 1].due > entry.due ||
                      (entries[i - 1].due == entry.due && entries[i - 1].sequence >= entry.sequence))) {
            return "the life-event queue is not ordered by (due, sequence)";
        }
        if (entry.sequence >= world.life_queue().next_sequence()) {
            return "a life event has a sequence number from the future";
        }
        const std::size_t who = entry.item.character.index();
        if (!entry.item.character.valid() || who >= rows) {
            return "a life event names nobody";
        }
        if (entry.item.kind == LifeEventKind::Birthday) {
            birthday[who] = 1;
        } else if (entry.due == t.planned_death[who]) {
            death[who] = 1;
        }
    }
    for (const CharacterId id : alive) {
        if (birthday[id.index()] == 0) {
            return std::format("the living person {} has no birthday in the queue", id.value());
        }
        if (t.planned_death[id.index()] != kNoDate && death[id.index()] == 0) {
            return std::format("the planned death of person {} is not in the queue", id.value());
        }
    }
    return {};
}

/// Sums over all accounts: the money that exists and the lines that explain it.
struct Books {
    eco::Kopecks treasury = 0;
    eco::Kopecks open_lines = 0;
    eco::Kopecks newest_closed_lines = 0;
    Date open_month;
};

Books books_of(const World& world) {
    Books out;
    const eco::AccountTable& accounts = world.economy().accounts;
    for (std::size_t a = 0; a < accounts.size(); ++a) {
        out.treasury += accounts.treasury(a);
        for (const eco::Kopecks line : accounts.open_lines(a)) {
            out.open_lines += line;
        }
        for (const eco::Kopecks line : accounts.ring_lines(a, 0)) {
            out.newest_closed_lines += line;
        }
    }
    out.open_month = world.economy().meta.open_month_start;
    return out;
}

std::string economy_problem(const World& world) {
    if (!world.economy_present()) {
        return "the economy disappeared";
    }
    const eco::EconomyState& e = world.economy();
    if (e.accounts.size() != 1 + world.dynasties().size()) {
        return "the accounts do not match the dynasties";
    }
    for (std::size_t a = 0; a < e.accounts.size(); ++a) {
        eco::Kopecks lines = 0;
        for (const eco::Kopecks line : e.accounts.open_lines(a)) {
            lines += line;
        }
        if (e.accounts.treasury(a) != e.accounts.opening(a) + lines) {
            return std::format("account {}: treasury {} != opening {} + lines {}",
                               a,
                               e.accounts.treasury(a),
                               e.accounts.opening(a),
                               lines);
        }
    }
    for (std::size_t p = 0; p < world.provinces().size(); ++p) {
        std::uint64_t persons = 0;
        for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
            persons += e.groups.size[p * eco::kGroupCount + g];
        }
        const ProvinceId province(static_cast<std::uint32_t>(p));
        if (persons != world.population_persons(province) || persons > eco::kMaxProvincePersons ||
            world.provinces().population[p] !=
                Fixed32::from_ratio(static_cast<std::int64_t>(persons), 1000)) {
            return std::format("province {}: the groups ({} people) and the population disagree", p, persons);
        }
        const CharacterId administrator = e.provinces.administrator[p];
        if (administrator.valid() && !world.is_alive(administrator)) {
            return std::format("province {} is administered by the dead person {}", p, administrator.value());
        }
        const bool by_dynasty =
            e.provinces.owner_kind[p] == static_cast<std::uint8_t>(eco::OwnerKind::Dynasty);
        if (by_dynasty != world.provinces().owner[p].valid()) {
            return std::format("province {}: the owner kind and the owner disagree", p);
        }
    }
    return {};
}

/// Money appears and disappears only through the statement: the change of all balances over a step
/// equals the change of all lines (the lines of a month that was closed in between are in the ring).
std::string money_flow_problem(const Books& before, const Books& after) {
    const eco::Kopecks lines = before.open_month == after.open_month
                                   ? after.open_lines - before.open_lines
                                   : after.newest_closed_lines - before.open_lines + after.open_lines;
    if (after.treasury - before.treasury != lines) {
        return std::format(
            "the balances moved by {} but the statement by {}", after.treasury - before.treasury, lines);
    }
    return {};
}

std::string structure_problem(const World& world) {
    const std::vector<std::string> invariants = world.check_invariants();
    if (!invariants.empty()) {
        return "check_invariants: " + invariants.front();
    }
    if (std::string problem = kinship_problem(world); !problem.empty()) {
        return problem;
    }
    if (std::string problem = index_problem(world); !problem.empty()) {
        return problem;
    }
    return world.economy_present() ? economy_problem(world) : std::string();
}

// ---- the simplified model ------------------------------------------------------------------------

struct ModelPerson {
    bool alive = false;
    DynastyId dynasty;
    Sex sex = Sex::Male;
    Date birth;
    CharacterId spouse;
    std::uint8_t marriages = 0;
    std::vector<CharacterId> former;
};

/// What the test believes the world looks like, kept with the plainest possible code.
class Model {
public:
    explicit Model(const World& world) : today_(world.today()) {
        const CharacterTable& t = world.characters();
        for (std::size_t i = 0; i < t.ids.capacity(); ++i) {
            ModelPerson person;
            person.alive = world.is_alive(CharacterId(static_cast<std::uint32_t>(i)));
            person.dynasty = t.dynasty[i];
            person.sex = t.sex[i];
            person.birth = t.birth_date[i];
            people_.push_back(person);
        }
        head_ = world.dynasties().head;
        honor_ = world.dynasties().honor;
        owner_ = world.provinces().owner;
        economy_ = world.economy_present();
        administrator_.assign(owner_.size(), CharacterId());
        tax_policy_.assign(head_.size(), eco::kPolicyDefault);
        recruit_policy_.assign(head_.size(), eco::kPolicyDefault);
        pool_.assign(owner_.size(), 0);
        size_.assign(owner_.size(), {});
        reserve_.assign(head_.size(), 0);
        if (economy_) {
            pool_ = world.economy().provinces.recruit_pool;
            reserve_ = world.economy().dynasties.manpower_reserve;
            for (std::size_t p = 0; p < owner_.size(); ++p) {
                for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
                    size_[p][g] = world.economy().groups.size[p * eco::kGroupCount + g];
                }
            }
            administrator_ = world.economy().provinces.administrator;
            tax_policy_ = world.economy().dynasties.tax_policy;
            recruit_policy_ = world.economy().dynasties.recruit_policy;
        }
    }

    /// Predicts whether the command is accepted and, if so, applies it to the model.
    bool apply(const Command& command) {
        return std::visit([&](const auto& payload) { return apply(command.actor, payload); },
                          command.payload);
    }

    /// A direct change of house (World::set_character_dynasty; there is no command for it yet).
    void change_house(CharacterId who, DynastyId dynasty) {
        ModelPerson& person = people_[who.index()];
        if (person.dynasty.valid() && head_[person.dynasty.index()] == who) {
            head_[person.dynasty.index()] = CharacterId();
        }
        person.dynasty = dynasty;
    }

    [[nodiscard]] std::string difference(const World& world) const {
        const CharacterTable& t = world.characters();
        if (t.ids.capacity() != people_.size()) {
            return std::format(
                "the world has {} person rows, the model {}", t.ids.capacity(), people_.size());
        }
        for (std::size_t i = 0; i < people_.size(); ++i) {
            const ModelPerson& person = people_[i];
            const CharacterId id(static_cast<std::uint32_t>(i));
            if (world.is_alive(id) != person.alive) {
                return std::format(
                    "person {}: alive in the world {}, in the model {}", i, !person.alive, person.alive);
            }
            if (!person.alive) {
                continue;
            }
            if (t.dynasty[i] != person.dynasty || t.spouse[i] != person.spouse ||
                t.marriages[i] != person.marriages) {
                return std::format("person {}: house, spouse or marriage count differ from the model", i);
            }
        }
        if (world.dynasties().head != head_) {
            return "the heads differ from the model";
        }
        if (world.dynasties().honor != honor_) {
            return "the honor differs from the model";
        }
        if (world.provinces().owner != owner_) {
            return "the province owners differ from the model";
        }
        if (world.economy_present() != economy_) {
            return "the economy appeared or disappeared";
        }
        if (economy_ && world.economy().provinces.administrator != administrator_) {
            return "the administrators differ from the model";
        }
        if (economy_ && world.economy().dynasties.tax_policy != tax_policy_) {
            return "the tax policies of the dynasties differ from the model";
        }
        if (economy_ && world.economy().dynasties.recruit_policy != recruit_policy_) {
            return "the recruit policies of the dynasties differ from the model";
        }
        if (economy_) {
            if (world.economy().provinces.recruit_pool != pool_) {
                return "the recruit pools differ from the model";
            }
            if (world.economy().dynasties.manpower_reserve != reserve_) {
                return "the manpower reserves differ from the model";
            }
            for (std::size_t p = 0; p < size_.size(); ++p) {
                std::uint64_t persons = 0;
                for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
                    if (world.economy().groups.size[p * eco::kGroupCount + g] != size_[p][g]) {
                        return std::format(
                            "the people of group {} of province {} differ from the model", g, p);
                    }
                    persons += size_[p][g];
                }
                if (world.population_persons(ProvinceId(static_cast<std::uint32_t>(p))) != persons) {
                    return std::format("the population of province {} is not the sum of its groups", p);
                }
            }
        }
        return {};
    }

private:
    [[nodiscard]] bool alive(CharacterId id) const {
        return id.valid() && id.index() < people_.size() && people_[id.index()].alive;
    }
    [[nodiscard]] bool known(DynastyId id) const { return id.valid() && id.index() < head_.size(); }
    [[nodiscard]] bool known(ProvinceId id) const { return id.valid() && id.index() < owner_.size(); }

    void part(CharacterId a) {
        const CharacterId b = people_[a.index()].spouse;
        people_[a.index()].spouse = CharacterId();
        people_[b.index()].spouse = CharacterId();
        people_[a.index()].former.push_back(b);
        people_[b.index()].former.push_back(a);
    }

    bool apply(DynastyId actor, const cmd::SetProvinceOwner& c) {
        if (!known(c.province) || (c.new_owner.valid() && !known(c.new_owner)) ||
            (!c.new_owner.valid() && actor.valid())) {
            return false;
        }
        DynastyId& current = owner_[c.province.index()];
        if ((actor.valid() && actor != current) || current == c.new_owner) {
            return false;
        }
        current = c.new_owner;
        // an administrator stays only if he is of the house of the new owner (E9 #242)
        CharacterId& administrator = administrator_[c.province.index()];
        if (administrator.valid() &&
            (!c.new_owner.valid() || people_[administrator.index()].dynasty != c.new_owner)) {
            administrator = CharacterId();
        }
        return true;
    }
    bool apply(DynastyId /*actor*/, const cmd::CreateCharacter& /*c*/) { return false; }  // not generated
    bool apply(DynastyId /*actor*/, const cmd::KillCharacter& /*c*/) { return false; }    // dynasty mode
    bool apply(DynastyId actor, const cmd::SetDynastyHead& c) {
        if (!known(c.dynasty) || (actor.valid() && actor != c.dynasty)) {
            return false;
        }
        if (c.head.valid() && (!alive(c.head) || people_[c.head.index()].dynasty != c.dynasty)) {
            return false;
        }
        head_[c.dynasty.index()] = c.head;
        return true;
    }
    bool apply(DynastyId actor, const cmd::AdjustHonor& c) {
        if (actor.valid() || !known(c.dynasty)) {
            return false;
        }
        const std::int64_t value = static_cast<std::int64_t>(honor_[c.dynasty.index()]) + c.delta;
        honor_[c.dynasty.index()] =
            static_cast<std::int32_t>(std::clamp<std::int64_t>(value, kMinHonor, kMaxHonor));
        return true;
    }
    bool apply(DynastyId actor, const cmd::AdjustHonorWithReason& c) {
        return apply(actor, cmd::AdjustHonor{c.dynasty, c.delta});
    }
    // unity and prestige are not modelled: only the outcome of the command is predicted
    bool apply(DynastyId actor, const cmd::AdjustUnity& c) { return !actor.valid() && known(c.dynasty); }
    bool apply(DynastyId actor, const cmd::AdjustPrestige& c) { return !actor.valid() && known(c.dynasty); }
    bool apply(DynastyId actor, const cmd::CreatePerson& c) {
        const CharacterInit& init = c.init;
        if (actor.valid() || !known(init.dynasty) || init.birth_date > today_) {
            return false;
        }
        const std::array<std::pair<CharacterId, Sex>, 2> parents{
            {{init.father, Sex::Male}, {init.mother, Sex::Female}}};
        for (const auto& [parent, sex] : parents) {
            if (!parent.valid()) {
                continue;
            }
            if (!alive(parent) || people_[parent.index()].sex != sex ||
                people_[parent.index()].birth >= init.birth_date) {
                return false;
            }
        }
        ModelPerson person;
        person.alive = true;
        person.dynasty = init.dynasty;
        person.sex = init.sex;
        person.birth = init.birth_date;
        people_.push_back(person);  // dynasty mode never reuses an id: the new person is the next row
        return true;
    }
    bool apply(DynastyId actor, const cmd::Die& c) {
        const auto cause = static_cast<std::uint8_t>(c.cause);
        if (actor.valid() || !alive(c.character) || cause == 0 || cause >= kCauseOfDeathCount) {
            return false;
        }
        ModelPerson& person = people_[c.character.index()];
        if (person.spouse.valid()) {
            part(c.character);
        }
        person.alive = false;
        if (person.dynasty.valid() && head_[person.dynasty.index()] == c.character) {
            head_[person.dynasty.index()] = CharacterId();
        }
        for (CharacterId& administrator : administrator_) {
            if (administrator == c.character) {
                administrator = CharacterId();
            }
        }
        return true;
    }
    bool apply(DynastyId actor, const cmd::Marry& c) {
        if (actor.valid() || !alive(c.a) || !alive(c.b) || c.a == c.b) {
            return false;
        }
        ModelPerson& a = people_[c.a.index()];
        ModelPerson& b = people_[c.b.index()];
        if (a.spouse.valid() || b.spouse.valid() || a.marriages >= kMaxMarriages ||
            b.marriages >= kMaxMarriages ||
            std::find(a.former.begin(), a.former.end(), c.b) != a.former.end()) {
            return false;
        }
        a.spouse = c.b;
        b.spouse = c.a;
        ++a.marriages;
        ++b.marriages;
        return true;
    }
    bool apply(DynastyId actor, const cmd::EndMarriage& c) {
        if (actor.valid() || !alive(c.a) || static_cast<std::uint8_t>(c.reason) >= kMarriageEndCount ||
            !people_[c.a.index()].spouse.valid()) {
            return false;
        }
        part(c.a);
        return true;
    }
    bool apply(DynastyId actor, const cmd::PlanDeath& c) {
        return !actor.valid() && alive(c.character) &&
               planned_death_is_valid(people_[c.character.index()].birth, c.date);
    }
    bool apply(DynastyId /*actor*/, const cmd::RecordChronicle& /*c*/) { return false; }  // not generated
    // This model runs without a trait catalogue, so trait commands are rejected.
    bool apply(DynastyId /*actor*/, const cmd::GrantTrait& /*c*/) { return false; }
    bool apply(DynastyId /*actor*/, const cmd::RevokeTrait& /*c*/) { return false; }
    bool apply(DynastyId actor, const cmd::AdjustOpinion& c) {
        return !actor.valid() && alive(c.from) && alive(c.to) && c.from != c.to;
    }
    /// The model knows the rules of the template worlds: 4 tax levels and 3 recruit levels.
    bool set_policy(std::vector<std::uint8_t>& column,
                    std::uint8_t levels,
                    DynastyId actor,
                    DynastyId dynasty,
                    std::uint8_t level) {
        if (!economy_ || !known(dynasty) || (actor.valid() && actor != dynasty) || level >= levels) {
            return false;
        }
        column[dynasty.index()] = level;
        return true;
    }
    bool apply(DynastyId actor, const cmd::SetTaxPolicy& c) {
        return set_policy(tax_policy_, 4, actor, c.dynasty, c.level);
    }
    bool apply(DynastyId actor, const cmd::SetRecruitPolicy& c) {
        return set_policy(recruit_policy_, 3, actor, c.dynasty, c.level);
    }
    /// Conscription (E9-T13): a province held by a dynasty, owned by the actor or the system, 1 or more
    /// people, no more than the pool, the people of the recruit groups and the room in the reserve.
    bool apply(DynastyId actor, const cmd::ConscriptRecruits& c) {
        if (!economy_ || !known(c.province)) {
            return false;
        }
        const std::size_t p = c.province.index();
        const DynastyId owner = owner_[p];
        if ((actor.valid() && actor != owner) || !owner.valid() || c.count == 0 || c.count > pool_[p] ||
            c.count > eco::recruit_population(size_[p]) ||
            c.count > std::numeric_limits<std::uint32_t>::max() - reserve_[owner.index()]) {
            return false;
        }
        const auto take = eco::split_conscription(size_[p], c.count);
        for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
            size_[p][g] -= take[g];
        }
        pool_[p] -= c.count;
        reserve_[owner.index()] += c.count;
        return true;
    }
    bool apply(DynastyId actor, const cmd::AssignProvinceAdministrator& c) {
        if (!economy_ || !known(c.province) || (c.administrator.valid() && !alive(c.administrator))) {
            return false;
        }
        const std::size_t p = c.province.index();
        if (actor.valid()) {
            if (owner_[p] != actor ||
                (c.administrator.valid() && people_[c.administrator.index()].dynasty != actor)) {
                return false;
            }
        }
        if (administrator_[p] == c.administrator) {
            return false;
        }
        administrator_[p] = c.administrator;
        return true;
    }

    Date today_;
    bool economy_ = false;
    std::vector<ModelPerson> people_;
    std::vector<CharacterId> head_;
    std::vector<std::int32_t> honor_;
    std::vector<DynastyId> owner_;
    std::vector<CharacterId> administrator_;
    std::vector<std::uint8_t> tax_policy_;
    std::vector<std::uint8_t> recruit_policy_;
    std::vector<std::uint32_t> pool_;                                // recruit pool per province (T13)
    std::vector<std::array<std::uint32_t, eco::kGroupCount>> size_;  // people per group per province
    std::vector<std::uint32_t> reserve_;                             // manpower reserve per dynasty
};

// ---- runs ---------------------------------------------------------------------------------------

/// Reports a problem with everything needed to reproduce it; true when there was one.
bool report(const std::string& problem, std::uint64_t seed, std::uint32_t step, std::string_view what) {
    if (problem.empty()) {
        return false;
    }
    FAIL_CHECK("seed " << seed << " step " << step << " (" << what << "): " << problem);
    return true;
}

/// How many commands of each kind (index of the payload) were generated and how many were applied.
struct Coverage {
    std::array<std::uint32_t, std::variant_size_v<CommandPayload>> generated{};
    std::array<std::uint32_t, std::variant_size_v<CommandPayload>> applied{};
};

/// One sequence of commands against the model; some reload at a random step. False on a failure.
bool run_command_sequence(std::uint64_t seed, Coverage* coverage = nullptr) {
    Pcg32 rng(seed, kGeneratorStream);
    // One sequence in kEconomyEvery runs with the economy: its invariants cost several times the rest.
    World world = template_world(seed, seed % kEconomyEvery == 0);
    Model model(world);
    std::optional<World> uninterrupted;  // the same run without the reload
    const std::uint32_t reload_at =
        rng.chance_permille(kReloadPermille) ? rng.bounded(kStepsPerSequence) : kStepsPerSequence;
    CommandLog log;
    if (report(structure_problem(world), seed, 0, "start")) {
        return false;
    }
    for (std::uint32_t step = 0; step < kStepsPerSequence; ++step) {
        if (step == reload_at) {
            auto loaded = deserialize_save(serialize_save(world));
            if (!loaded) {
                return !report(loaded.error().message, seed, step, "load");
            }
            if (loaded.value().world.compute_hash() != world.compute_hash()) {
                return !report("the hash changed over save and load", seed, step, "load");
            }
            uninterrupted.emplace(std::move(world));
            world = std::move(loaded.value().world);
        }
        std::string_view what = "change of house";
        if (rng.chance_permille(60) && !world.living().empty()) {
            // "change of house": a World operation, there is no command for it yet
            const CharacterId who =
                world.living()[rng.bounded(static_cast<std::uint32_t>(world.living().size()))];
            const DynastyId house(rng.bounded(kDynasties));
            const Books before = books_of(world);
            world.set_character_dynasty(who, house);
            if (uninterrupted) {
                uninterrupted->set_character_dynasty(who, house);
            }
            model.change_house(who, house);
            if (report(money_flow_problem(before, books_of(world)), seed, step, what)) {
                return false;
            }
        } else {
            const Command command = random_command(rng, world);
            what = to_string(type_of(command));
            const Books before = books_of(world);
            log.clear();
            const bool applied =
                process_commands(world, {&command, 1}, CommandSource::External, log).applied == 1;
            if (uninterrupted) {
                (void)process_commands(*uninterrupted, {&command, 1}, CommandSource::External, log);
            }
            const bool predicted = model.apply(command);
            if (coverage != nullptr) {
                ++coverage->generated[command.payload.index()];
                coverage->applied[command.payload.index()] += applied ? 1U : 0U;
            }
            if (applied != predicted) {
                return !report(std::format("the world {} the command, the model {} it",
                                           applied ? "applied" : "refused",
                                           predicted ? "applies" : "refuses"),
                               seed,
                               step,
                               what);
            }
            if (report(money_flow_problem(before, books_of(world)), seed, step, what)) {
                return false;
            }
        }
        if (report(structure_problem(world), seed, step, what) ||
            report(model.difference(world), seed, step, what)) {
            return false;
        }
    }
    if (uninterrupted && uninterrupted->compute_hash() != world.compute_hash()) {
        return !report(
            "the run with a reload ended in another state than the run without", seed, reload_at, "end");
    }
    return true;
}

using Script = std::vector<std::vector<Command>>;  // the commands submitted before each tick

struct Recorded {
    Script script;
    std::vector<WorldHash> hashes;  // after each tick
    Replay replay;
};

/// The reference run: generates the script as it goes and checks every invariant after every day.
std::optional<Recorded> record_run(std::uint64_t seed) {
    Pcg32 rng(seed, kGeneratorStream + 1);
    Simulation sim = make_simulation(build_world(seed));
    ReplayRecorder recorder(seed, 0x0E70005, start_date(), 30);
    Recorded out;
    for (int day = 0; day < kSimulationDays; ++day) {
        const auto step = static_cast<std::uint32_t>(day);
        std::vector<Command> commands;
        for (std::uint32_t n = rng.bounded(3); n > 0; --n) {
            commands.push_back(random_command(rng, sim.world()));
            REQUIRE(sim.submit(commands.back()));
        }
        const Books before = books_of(sim.world());
        (void)sim.tick();
        recorder.after_tick(sim);
        if (report(structure_problem(sim.world()), seed, step, "tick") ||
            report(money_flow_problem(before, books_of(sim.world())), seed, step, "tick")) {
            return std::nullopt;
        }
        out.script.push_back(std::move(commands));
        out.hashes.push_back(sim.world().compute_hash());
    }
    out.replay = recorder.finish(sim);
    return out;
}

enum class Interruption { SaveAndLoad, NewSimulation };

/// Plays the script again, interrupted at random days, and compares the hash after every day.
bool replay_interrupted(std::uint64_t seed, const Recorded& recorded, Interruption how) {
    Pcg32 rng(seed, kGeneratorStream + 2 + static_cast<std::uint64_t>(how));
    Simulation sim = make_simulation(build_world(seed));
    const std::string_view what = how == Interruption::SaveAndLoad ? "save and load" : "new simulation";
    for (std::size_t day = 0; day < recorded.script.size(); ++day) {
        const auto step = static_cast<std::uint32_t>(day);
        for (const Command& command : recorded.script[day]) {
            REQUIRE(sim.submit(command));
        }
        (void)sim.tick();
        if (sim.world().compute_hash() != recorded.hashes[day]) {
            return !report("diverged from the uninterrupted run: " +
                               first_differing_part(recorded.hashes[day], sim.world().compute_hash()),
                           seed,
                           step,
                           what);
        }
        if (!rng.chance_permille(how == Interruption::SaveAndLoad ? 30 : 80)) {
            continue;
        }
        const std::span<const Command> pending = sim.pending_internal();
        if (how == Interruption::SaveAndLoad) {
            auto loaded = deserialize_save(serialize_save(sim.world(), pending));
            if (!loaded) {
                return !report(loaded.error().message, seed, step, what);
            }
            if (loaded.value().world.compute_hash() != recorded.hashes[day]) {
                return !report("the hash changed over save and load", seed, step, what);
            }
            sim.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
        } else {
            // the days are split between Simulation objects: systems must keep nothing outside the World
            World copy = sim.world();
            std::vector<Command> queued(pending.begin(), pending.end());
            Simulation next = make_simulation(build_world(seed));
            next.restore_state(std::move(copy), std::move(queued));
            sim = std::move(next);
        }
    }
    return true;
}

bool run_simulation(std::uint64_t seed) {
    const std::optional<Recorded> recorded = record_run(seed);
    if (!recorded) {
        return false;
    }
    if (!replay_interrupted(seed, *recorded, Interruption::SaveAndLoad) ||
        !replay_interrupted(seed, *recorded, Interruption::NewSimulation)) {
        return false;
    }
    auto replay = deserialize_replay(serialize_replay(recorded->replay));
    if (!replay) {
        return !report(replay.error().message, seed, 0, "replay file");
    }
    Simulation fresh = make_simulation(build_world(seed));
    const ReplayResult played = play_replay(fresh, replay.value());
    if (!played.ok) {
        const std::string where = played.divergence ? std::format("tick {}, part {}",
                                                                  played.divergence->tick,
                                                                  played.divergence->first_differing_part)
                                                    : played.error;
        return !report("the replay does not reproduce the run: " + where, seed, 0, "replay");
    }
    return true;
}

}  // namespace

TEST_CASE("model: 10 000 random command sequences keep every invariant and agree with the model") {
    int failures = 0;
    Coverage coverage;
    for (std::uint32_t i = 0; i < kSequences && failures < kMaxReportedFailures; ++i) {
        failures += run_command_sequence(kBaseSeed + i, &coverage) ? 0 : 1;
    }
    CHECK(failures == 0);
    // The generator must not be vacuous: every kind it emits is both applied and refused sometimes.
    std::uint32_t generated = 0;
    std::uint32_t applied = 0;
    for (std::size_t kind = 0; kind < coverage.generated.size(); ++kind) {
        generated += coverage.generated[kind];
        applied += coverage.applied[kind];
        if (coverage.generated[kind] == 0) {
            continue;  // CreateCharacter and RecordChronicle are not generated
        }
        CAPTURE(kind);
        CHECK(coverage.applied[kind] < coverage.generated[kind]);
        if (kind != CommandPayload(cmd::KillCharacter{})
                        .index()) {  // KillCharacter is always refused in dynasty mode
            CHECK(coverage.applied[kind] > 0);
        }
    }
    MESSAGE("commands generated: " << generated << ", applied: " << applied);
}

TEST_CASE("model: simulated days with commands, reloads, hand-overs and a replay end in the same state") {
    int failures = 0;
    for (std::uint32_t i = 0; i < kSimulationSeeds && failures < kMaxReportedFailures; ++i) {
        failures += run_simulation(kBaseSeed + i) ? 0 : 1;
    }
    CHECK(failures == 0);
}

TEST_CASE("model: regression seeds") {
    for (const std::uint64_t seed : kRegressionSeeds) {
        CHECK(run_command_sequence(seed));
        CHECK(run_simulation(seed));
    }
}

TEST_CASE("model: the generator is a pure function of the seed") {
    const World world = build_world(11);
    Pcg32 a(11, kGeneratorStream);
    Pcg32 b(11, kGeneratorStream);
    for (int i = 0; i < 200; ++i) {
        CHECK(random_command(a, world) == random_command(b, world));
    }
    CHECK(build_world(11).compute_hash() == world.compute_hash());
    CHECK(build_world(12).compute_hash() != world.compute_hash());
}

TEST_CASE("regression #241: control at its target must not depend on the side it came from") {
    const eco::ControlRules& control = rules().control;
    const auto settle = [&](eco::AdministratorProfile profile, int start) {
        eco::AdministrationInput in;
        in.profile = profile;
        in.control = Fixed32::from_int(start);
        for (int month = 0; month < 600; ++month) {
            const eco::Administration next = eco::compute_administration(control, in);
            in.control = next.control;
            in.corruption = next.corruption;
        }
        return in;
    };
    // the worst administrator: the target is 70 - 30 = 40, exactly the low threshold
    const eco::AdministratorProfile worst{0, 50};
    CHECK(eco::control_state(control, settle(worst, 60).control) == eco::ControlState::Normal);
    CHECK(eco::control_state(control, settle(worst, 20).control) == eco::ControlState::Normal);
    // a fresh province (control 100, corruption 0) must end at the corruption of the rules, in per-mille
    const eco::AdministrationInput fresh = settle(eco::AdministratorProfile{50, 50}, 100);
    CHECK(eco::corruption_permille(fresh.corruption) == control.corruption_base_permille);
}

// ---- regressions of defects found by the audit of the economy ----------------------------------

TEST_CASE("regression #242: a province that changes hands does not keep the administrator of the old house") {
    World world = build_world(3);
    const ProvinceId province(0);
    const DynastyId old_owner = world.provinces().owner[province.index()];
    const CharacterId steward = world.members_of(old_owner).front();
    REQUIRE(world.set_province_administrator(province, steward));
    world.set_province_owner(province, DynastyId((old_owner.value() + 1) % kDynasties));
    CHECK_FALSE(world.economy().provinces.administrator[province.index()].valid());
}
