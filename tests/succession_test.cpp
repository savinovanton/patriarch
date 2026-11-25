#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/succession.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pat::sim;

constexpr Date kStart = Date::from_ymd(1703, 1, 1);
constexpr std::uint64_t kSeed = 31;

struct Family {
    World world{kStart, kSeed};
    DynastyId dynasty;
    SuccessionRules rules = SuccessionRules::defaults();

    Family() {
        REQUIRE(world.enable_dynasty_mode());
        dynasty = world.create_dynasty(pat::core::StringId(1));
    }

    CharacterId person(int age, Sex sex = Sex::Male, CharacterId father = {}, CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = dynasty;
        init.birth_date = Date::from_ymd(kStart.year() - age, 1, 1);
        init.sex = sex;
        init.father = father;
        init.mother = mother;
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    void retire(CharacterId id) { REQUIRE(world.die(id, CauseOfDeath::Illness)); }

    [[nodiscard]] CharacterId successor(CharacterId predecessor) const {
        return fallback_successor(world, dynasty, predecessor);
    }

    [[nodiscard]] SuccessionResult resolve(CharacterId predecessor = {}) const {
        return resolve_successor(world, rules, dynasty, predecessor);
    }

    void law(SuccessionLaw value) { world.set_dynasty_succession_law(dynasty, value); }

    void status(CharacterId id, std::uint8_t flags) {
        family::PersonStatus row;
        row.character = id;
        row.flags = flags;
        if ((flags & family::kPersonTonsured) != 0) {
            row.tonsured_on = world.today();
        }
        if ((flags & family::kPersonDisowned) != 0) {
            row.disowned_on = world.today();
        }
        REQUIRE(world.set_family_status(row));
    }

    void designate(CharacterId id) {
        family::SuccessionState row;
        row.dynasty = dynasty;
        row.designated_heir = id;
        REQUIRE(world.set_family_succession(row));
    }

    void skill_points(CharacterId id, int value) {
        world.set_character_skill(id, Skill::Stewardship, pat::core::Fixed32::from_int(value));
    }
};

std::vector<CharacterId> order_ids(const SuccessionResult& result) {
    std::vector<CharacterId> ids;
    for (const auto& row : result.order) {
        ids.push_back(row.character);
    }
    return ids;
}

SuccessionRules neutral_council_rules() {
    auto rules = SuccessionRules::defaults();
    rules.skills_weight_permille = 1000;
    rules.age_weight_permille = 0;
    rules.son_bonus = 0;
    rules.daughter_bonus = 0;
    rules.brother_bonus = 0;
    rules.nephew_bonus = 0;
    rules.opinion_weight_permille = 0;
    rules.loyalty_weight_permille = 0;
    rules.piety_weight_permille = 0;
    rules.legitimized_penalty = 0;
    rules.female_penalty = 0;
    return rules;
}

Command die(CharacterId id) {
    return Command{DynastyId{}, cmd::Die{id, CauseOfDeath::Illness}};
}

std::size_t successor_commands(const Simulation& sim) {
    std::size_t count = 0;
    for (const auto& entry : sim.command_log().entries()) {
        if (entry.accepted() && entry.source == CommandSource::Internal &&
            std::holds_alternative<cmd::SetDynastyHead>(entry.command.payload)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("fallback succession: sons, brothers and nephews precede an older unrelated adult") {
    Family f;
    const CharacterId mother = f.person(89, Sex::Female);
    const CharacterId head = f.person(64, Sex::Male, {}, mother);
    const CharacterId brother = f.person(55, Sex::Male, {}, mother);
    const CharacterId nephew = f.person(29, Sex::Male, brother);
    const CharacterId younger = f.person(30, Sex::Male, head);
    const CharacterId first_older = f.person(35, Sex::Male, head);
    const CharacterId second_older = f.person(35, Sex::Male, head);
    const CharacterId daughter = f.person(39, Sex::Female, head);
    const CharacterId elder = f.person(72);
    const CharacterId minor = f.person(15, Sex::Male, head);

    f.retire(head);
    CHECK(f.successor(head) == first_older);
    f.retire(first_older);
    CHECK(f.successor(head) == second_older);
    f.retire(second_older);
    CHECK(f.successor(head) == younger);
    f.retire(younger);
    CHECK(f.successor(head) == brother);
    f.retire(brother);
    CHECK(f.successor(head) == nephew);
    f.retire(nephew);
    CHECK(f.successor(head) == elder);
    f.retire(elder);
    CHECK(f.successor(head) == daughter);
    CHECK(f.world.is_alive(minor));
}

TEST_CASE("fallback succession: daughters and their adult children precede unrelated women") {
    Family f;
    const CharacterId head = f.person(65);
    const CharacterId elder = f.person(72, Sex::Female);
    const CharacterId daughter = f.person(41, Sex::Female, head);
    const CharacterId granddaughter = f.person(19, Sex::Female, {}, daughter);
    const CharacterId minor = f.person(15, Sex::Male, head);

    f.retire(head);
    CHECK(f.successor(head) == daughter);
    f.retire(daughter);
    CHECK(f.successor(head) == granddaughter);
    f.retire(granddaughter);
    CHECK(f.successor(head) == elder);
    f.retire(elder);
    CHECK_FALSE(f.successor(head).valid());
    CHECK(f.world.is_alive(minor));
    CHECK_FALSE(f.world.dynasty_extinct(f.dynasty));
}

TEST_CASE("succession: a head death appoints a survivor after the whole batch and replays the appointment") {
    Family f;
    const CharacterId head = f.person(67);
    const CharacterId first = f.person(38, Sex::Male, head);
    const CharacterId survivor = f.person(29, Sex::Male, head);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));

    Simulation sim(f.world);
    ReplayRecorder recorder(kSeed, 0x5155ULL, kStart, 1);
    REQUIRE(sim.submit(die(head)));
    REQUIRE(sim.submit(die(first)));
    (void)sim.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.world().dynasties().head[f.dynasty.index()] == survivor);
    REQUIRE(successor_commands(sim) == 1);
    CHECK(sim.world().is_archived(first));
    CHECK(sim.world().check_invariants().empty());

    const auto entries = sim.world().query_chronicle(
        f.dynasty, sim.world().today(), sim.world().today(), ChronicleKind::HeadChanged);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().b == survivor);

    auto parsed = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().commands.size() == 2);
    Simulation replayed(f.world);
    CHECK(play_replay(replayed, parsed.value()).ok);
    CHECK(replayed.world().compute_hash() == sim.world().compute_hash());
    CHECK(successor_commands(replayed) == 1);

    (void)sim.tick();
    CHECK(successor_commands(sim) == 1);
}

TEST_CASE("succession: a saved internal death appoints the successor and the last death marks extinction") {
    Family f;
    const CharacterId head = f.person(61);
    const CharacterId child = f.person(30, Sex::Male, head);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));
    CHECK_FALSE(f.world.dynasty_extinct(f.dynasty));
    CHECK_FALSE(f.world.dynasty_extinct(DynastyId{}));

    Simulation original(f.world);
    original.restore_state(f.world, {die(head)});
    auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
    REQUIRE(loaded.has_value());
    Simulation resumed(f.world);
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));

    (void)original.tick();
    (void)resumed.tick();
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());
    CHECK(resumed.world().dynasties().head[f.dynasty.index()] == child);
    CHECK(successor_commands(resumed) == 1);

    REQUIRE(resumed.submit(die(child)));
    (void)resumed.tick();
    CHECK(resumed.world().dynasty_extinct(f.dynasty));
    CHECK_FALSE(resumed.world().dynasties().head[f.dynasty.index()].valid());
    CHECK(successor_commands(resumed) == 1);
    const auto entries = resumed.world().query_chronicle(
        f.dynasty, kStart, resumed.world().today(), ChronicleKind::Extinction);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().a == child);
}

TEST_CASE("succession: unrelated and rejected deaths leave the head, an explicit appointment prevails") {
    Family f;
    const CharacterId head = f.person(65);
    const CharacterId relative = f.person(32, Sex::Male, head);
    const CharacterId appointment = f.person(55);
    const CharacterId other = f.person(50);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));

    Simulation sim(std::move(f.world));
    REQUIRE(sim.submit(Command{DynastyId{}, cmd::Die{head, CauseOfDeath::None}}));
    REQUIRE(sim.submit(die(other)));
    (void)sim.tick();
    CHECK(sim.world().dynasties().head[f.dynasty.index()] == head);
    CHECK(successor_commands(sim) == 0);

    REQUIRE(sim.submit(die(head)));
    REQUIRE(sim.submit(Command{DynastyId{}, cmd::SetDynastyHead{f.dynasty, appointment}}));
    (void)sim.tick();
    CHECK(sim.world().dynasties().head[f.dynasty.index()] == appointment);
    CHECK(sim.world().is_alive(relative));
    CHECK(successor_commands(sim) == 0);

    Family headless;
    const CharacterId member = headless.person(51);
    const CharacterId other_member = headless.person(24, Sex::Male, member);
    Simulation without_head(std::move(headless.world));
    REQUIRE(without_head.submit(die(member)));
    (void)without_head.tick();
    CHECK(without_head.world().is_alive(other_member));
    CHECK_FALSE(without_head.world().dynasties().head[headless.dynasty.index()].valid());
    CHECK(successor_commands(without_head) == 0);
}

TEST_CASE("succession laws: majorat admits the eldest son at seventeen and a minor needs a regent") {
    Family f;
    const auto head = f.person(60);
    const auto elder = f.person(17, Sex::Male, head);
    const auto younger = f.person(12, Sex::Male, head);
    f.retire(head);
    const auto result = f.resolve(head);
    CHECK(result.kind == SuccessionKind::Successor);
    CHECK(result.winner == elder);
    CHECK_FALSE(result.minor);
    CHECK_FALSE(result.female_head);
    CHECK(result.honor_delta == 0);
    CHECK(order_ids(result) == std::vector<CharacterId>{elder, younger});
    CHECK(result.order.front().reason == HeirReason::EldestSon);
    f.retire(elder);
    const auto regency = f.resolve(head);
    CHECK(regency.winner == younger);
    CHECK(regency.minor);
}

TEST_CASE("succession laws: representation crosses dead foreign and excluded branches") {
    Family f;
    const auto head = f.person(80);
    const auto elder = f.person(55, Sex::Male, head);
    const auto younger = f.person(40, Sex::Male, head);
    const auto grandchild = f.person(32, Sex::Male, elder);
    const auto great_grandchild = f.person(10, Sex::Male, grandchild);
    f.retire(head);
    SUBCASE("dead elder is represented before the living younger son") {
        f.retire(elder);
    }
    SUBCASE("a transferred elder still connects his own-house descendants") {
        const auto foreign = f.world.create_dynasty(pat::core::StringId(2));
        f.world.set_character_dynasty(elder, foreign);
    }
    SUBCASE("disowning the elder does not disown his descendants") {
        f.status(elder, family::kPersonDisowned);
    }
    SUBCASE("tonsure of the elder does not remove the branch") {
        f.status(elder, family::kPersonTonsured);
    }
    SUBCASE("illegitimacy of the elder does not exclude his lawful descendants") {
        f.world.set_character_legitimacy(elder, Legitimacy::Illegitimate);
    }
    auto result = f.resolve(head);
    CHECK(order_ids(result) == std::vector<CharacterId>{grandchild, great_grandchild, younger});
    CHECK(result.winner == grandchild);
    CHECK(result.order.front().reason == HeirReason::SonLine);
    f.retire(grandchild);
    result = f.resolve(head);
    CHECK(result.winner == great_grandchild);
    CHECK(result.minor);
}

TEST_CASE("succession laws: minorat reverses generations by birth with ascending id ties") {
    Family f;
    const auto head = f.person(80);
    const auto elder = f.person(55, Sex::Male, head);
    const auto younger = f.person(45, Sex::Male, head);
    const auto first_twin = f.person(20, Sex::Male, younger);
    const auto second_twin = f.person(20, Sex::Male, younger);
    const auto older_grandchild = f.person(25, Sex::Male, younger);
    f.retire(head);
    f.law(SuccessionLaw::Minorat);
    auto result = f.resolve(head);
    CHECK(result.winner == younger);
    CHECK(result.order.front().reason == HeirReason::YoungestSon);
    CHECK(order_ids(result) ==
          std::vector<CharacterId>{younger, first_twin, second_twin, older_grandchild, elder});
    f.retire(younger);
    result = f.resolve(head);
    CHECK(result.winner == first_twin);
    CHECK(result.order.front().reason == HeirReason::SonLine);
}

TEST_CASE("succession laws: daughters brothers nephews and nearest kin have distinct reasons") {
    Family f;
    f.rules.female_succession = FemaleSuccession::Always;
    const auto father = f.person(95);
    const auto uncle = f.person(85);
    const auto head = f.person(65, Sex::Male, father);
    const auto brother = f.person(55, Sex::Male, father);
    const auto sister = f.person(60, Sex::Female, father);
    const auto daughter = f.person(40, Sex::Female, head);
    const auto grandson = f.person(19, Sex::Male, {}, daughter);
    const auto brother_son = f.person(30, Sex::Male, brother);
    const auto sister_son = f.person(35, Sex::Male, {}, sister);
    const auto unrelated = f.person(90);
    static_cast<void>(uncle);
    f.retire(head);
    const auto result = f.resolve(head);
    const auto ids = order_ids(result);
    const auto rank = [&](CharacterId id) { return std::ranges::find(ids, id) - ids.begin(); };
    const auto reason = [&](CharacterId id) {
        return result.order[static_cast<std::size_t>(rank(id))].reason;
    };
    CHECK(result.winner == daughter);
    CHECK(result.female_head);
    CHECK(result.honor_delta == -10);
    CHECK(rank(daughter) < rank(grandson));
    CHECK(rank(grandson) < rank(brother));
    CHECK(rank(brother) < rank(brother_son));
    CHECK(rank(brother_son) < rank(sister_son));
    CHECK(rank(sister_son) < rank(father));
    CHECK(rank(father) < rank(sister));
    CHECK(rank(sister) < rank(unrelated));
    CHECK(reason(daughter) == HeirReason::Daughter);
    CHECK(reason(grandson) == HeirReason::DaughterLine);
    CHECK(reason(brother) == HeirReason::Brother);
    CHECK(reason(brother_son) == HeirReason::Nephew);
    CHECK(reason(sister_son) == HeirReason::Nephew);
    CHECK(reason(father) == HeirReason::Kin);
    CHECK(reason(sister) == HeirReason::Kin);
    CHECK(reason(unrelated) == HeirReason::Kin);
}

TEST_CASE("succession laws: eligibility and female policies use eligible adult men only") {
    Family f;
    const auto head = f.person(60);
    const auto daughter = f.person(30, Sex::Female, head);
    const auto man = f.person(40);
    const auto boy = f.person(10);
    f.retire(head);
    CHECK(f.resolve(head).winner == man);
    f.status(man, family::kPersonDisowned);
    auto result = f.resolve(head);
    CHECK(result.winner == daughter);
    CHECK(result.female_head);
    CHECK(order_ids(result) == std::vector<CharacterId>{daughter, boy});

    f.rules.female_succession = FemaleSuccession::Never;
    CHECK(f.resolve(head).winner == boy);
    f.law(SuccessionLaw::Appointment);
    f.designate(daughter);
    result = f.resolve(head);
    REQUIRE(result.dispute.has_value());
    CHECK(result.dispute->reason == family::DisputeReason::NoAppointment);

    f.rules.female_succession = FemaleSuccession::Always;
    REQUIRE(f.world.clear_family_status(man));
    result = f.resolve(head);
    CHECK(result.winner == daughter);
    CHECK(result.order.front().reason == HeirReason::Appointed);

    f.rules.female_succession = FemaleSuccession::IfNoAdultMale;
    CHECK(f.resolve(head).winner == daughter);  // valid appointment bypasses this sex preference
    f.law(SuccessionLaw::Majorat);
    CHECK(f.resolve(head).winner == man);  // a designation alone does not change the law
    f.world.set_character_legitimacy(man, Legitimacy::Illegitimate);
    CHECK(f.resolve(head).winner == daughter);
    f.rules.illegitimate_inherit = true;
    CHECK(f.resolve(head).winner == man);
    f.world.set_character_legitimacy(man, Legitimacy::Legitimized);
    f.rules.illegitimate_inherit = false;
    CHECK(f.resolve(head).winner == man);
}

TEST_CASE("succession laws: an ineligible appointment reports a proposal even with one claimant") {
    Family f;
    const auto head = f.person(60);
    const auto heir = f.person(12, Sex::Male, head);
    const auto excluded = f.person(30);
    f.retire(head);
    f.status(excluded, family::kPersonDisowned);
    f.designate(excluded);
    f.law(SuccessionLaw::Appointment);
    const auto result = f.resolve(head);
    CHECK(result.kind == SuccessionKind::Dispute);
    CHECK_FALSE(result.winner.valid());
    REQUIRE(result.dispute.has_value());
    CHECK(result.dispute->reason == family::DisputeReason::NoAppointment);
    REQUIRE(result.dispute->claimants.size() == 1);
    CHECK(result.dispute->claimants.front().character == heir);
    CHECK(result.lawful_heir == heir);
    CHECK_FALSE(f.world.family_dispute(f.dynasty));
    CHECK_FALSE(f.world.family_regency(f.dynasty));
    f.designate(heir);
    const auto appointed = f.resolve(head);
    CHECK(appointed.winner == heir);
    CHECK(appointed.minor);
    CHECK(appointed.order.front().reason == HeirReason::Appointed);
}

TEST_CASE("succession laws: appointment chooses the designated member and keeps majorat display order") {
    Family f;
    const auto head = f.person(65);
    const auto elder = f.person(40, Sex::Male, head);
    const auto appointed = f.person(30, Sex::Male, head);
    f.retire(head);
    f.law(SuccessionLaw::Appointment);
    f.designate(appointed);
    const auto result = f.resolve(head);
    CHECK(result.winner == appointed);
    CHECK(result.lawful_heir == elder);
    CHECK(order_ids(result) == std::vector<CharacterId>{elder, appointed});
    CHECK(result.order[1].reason == HeirReason::Appointed);
}

TEST_CASE("succession laws: council score preserves fractional skills and zero opinions in the mean") {
    using pat::core::Fixed32;
    Family f;
    const auto head = f.person(70);
    const auto son = f.person(40, Sex::Male, head);
    const auto admirer = f.person(30, Sex::Female);
    const auto critic = f.person(45);
    const auto neutral = f.person(20);
    const auto child = f.person(10);
    f.retire(head);
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        f.world.set_character_skill(son, static_cast<Skill>(i), Fixed32::from_ratio(35, 2));
    }
    f.world.set_character_loyalty_head(son, Fixed32::from_int(-25));
    f.world.set_character_piety(son, Fixed32::from_int(75));
    f.world.set_character_legitimacy(son, Legitimacy::Legitimized);
    REQUIRE(f.world.add_opinion(admirer, son, Fixed32::from_int(80), kOtherOpinionReason));
    REQUIRE(f.world.add_opinion(critic, son, Fixed32::from_int(-20), kOtherOpinionReason));
    REQUIRE(f.world.add_opinion(child, son, Fixed32::from_int(100), kOtherOpinionReason));
    static_cast<void>(neutral);
    economy::Breakdown explain;
    const auto score = council_score(f.world, f.rules, f.dynasty, head, son, &explain);
    CHECK(score.terms == std::array<std::int32_t, kCouncilScoreTerms>{35, 12, 30, 10, -5, 7, -10});
    CHECK(score.total == 79);
    CHECK(score.opinion_mean == 20);
    CHECK(score.support == 8);
    CHECK(explain.total() == 79 * std::int64_t{Fixed32::one_raw});
    CHECK(explain.is_consistent());
    CHECK(explain.root().unit == economy::Unit::Points);
    CHECK(score == council_score(f.world, f.rules, f.dynasty, head, son));
    const auto result = f.resolve(head);
    const auto row = std::ranges::find(result.order, son, &SuccessionCandidate::character);
    REQUIRE(row != result.order.end());
    CHECK(row->council == score);
    for (std::size_t i = 0; i < explain.size(); ++i) {
        const auto code = explain.node(static_cast<economy::NodeIndex>(i)).code;
        CHECK_FALSE(succession_term_key(code).empty());
    }
    CHECK(succession_term_key(65535).empty());
}

TEST_CASE("succession laws: direct kinship council bonuses and female status penalty are distinct") {
    Family f;
    const auto father = f.person(95);
    const auto head = f.person(70, Sex::Male, father);
    const auto brother = f.person(60, Sex::Male, father);
    const auto nephew = f.person(35, Sex::Male, brother);
    const auto daughter = f.person(40, Sex::Female, head);
    f.world.set_character_legitimacy(daughter, Legitimacy::Legitimized);
    f.retire(head);
    CHECK(council_score(f.world, f.rules, f.dynasty, head, brother).terms[2] == 20);
    CHECK(council_score(f.world, f.rules, f.dynasty, head, nephew).terms[2] == 10);
    const auto score = council_score(f.world, f.rules, f.dynasty, head, daughter);
    CHECK(score.terms[2] == 15);
    CHECK(score.terms[6] == -25);
}

TEST_CASE("succession laws: council requires a clear margin and breaks ties by birth then id") {
    Family f;
    f.rules = neutral_council_rules();
    f.law(SuccessionLaw::FamilyCouncil);
    const auto head = f.person(65);
    const auto elder = f.person(40, Sex::Male, head);
    const auto younger = f.person(30, Sex::Male, head);
    const auto twin = f.person(30, Sex::Male, head);
    f.retire(head);
    f.skill_points(younger, 15);
    auto result = f.resolve(head);
    CHECK(result.winner == younger);
    CHECK(result.order.front().reason == HeirReason::CouncilChoice);
    CHECK(result.lawful_heir == elder);
    f.skill_points(younger, 14);
    result = f.resolve(head);
    CHECK(result.kind == SuccessionKind::Dispute);
    REQUIRE(result.dispute.has_value());
    CHECK(result.dispute->reason == family::DisputeReason::CloseScores);
    f.skill_points(younger, 0);
    result = f.resolve(head);
    CHECK(order_ids(result) == std::vector<CharacterId>{elder, younger, twin});
    f.retire(elder);
    f.retire(twin);
    CHECK(f.resolve(head).winner == younger);  // no runner-up means no close-score dispute
}

TEST_CASE("succession laws: unpopular heirs use a strict support boundary and the next adult rival") {
    Family f;
    f.rules = neutral_council_rules();
    const auto head = f.person(70);
    const auto heir = f.person(40, Sex::Male, head);
    const auto represented_child = f.person(10, Sex::Male, heir);
    const auto rival = f.person(30, Sex::Male, head);
    f.retire(head);
    f.world.set_character_loyalty_head(heir, pat::core::Fixed32::from_int(-40));
    CHECK(f.resolve(head).winner == heir);
    f.world.set_character_loyalty_head(heir, pat::core::Fixed32::from_int(-42));
    const auto result = f.resolve(head);
    CHECK(result.kind == SuccessionKind::Dispute);
    REQUIRE(result.dispute.has_value());
    CHECK(result.dispute->reason == family::DisputeReason::UnpopularHeir);
    CHECK(order_ids(result) == std::vector<CharacterId>{heir, represented_child, rival});
    f.rules.unpopular_dispute_enabled = false;
    CHECK(f.resolve(head).winner == heir);
    f.rules.unpopular_dispute_enabled = true;
    f.retire(rival);
    CHECK(f.resolve(head).winner == heir);  // a minor alone is not an adult rival
}

TEST_CASE("succession laws: dispute candidates are capped and retain a distant lawful heir") {
    Family f;
    f.rules = neutral_council_rules();
    f.law(SuccessionLaw::Appointment);
    const auto head = f.person(75);
    const auto lawful = f.person(50, Sex::Male, head);
    std::array<CharacterId, 5> rivals;
    for (std::size_t i = 0; i < rivals.size(); ++i) {
        rivals[i] = f.person(45 - static_cast<int>(i), Sex::Male, head);
        // Five skills let the total exceed the per-skill maximum of twenty.
        for (std::size_t s = 0; s < kSkillCount; ++s) {
            f.world.set_character_skill(
                rivals[i], static_cast<Skill>(s), pat::core::Fixed32::from_int(20 - static_cast<int>(i)));
        }
    }
    f.retire(head);
    const auto result = f.resolve(head);
    REQUIRE(result.dispute.has_value());
    const auto& claimants = result.dispute->claimants;
    REQUIRE(claimants.size() == family::kMaxDisputeClaimants);
    CHECK(claimants[0].character == rivals[0]);
    CHECK(claimants[1].character == rivals[1]);
    CHECK(claimants[2].character == rivals[2]);
    CHECK(claimants[3].character == lawful);
    CHECK(claimants[3].lawful_bonus == 10);
    for (const auto& claimant : claimants) {
        economy::Breakdown explain;
        explain_claimant_score(claimant, explain);
        CHECK(explain.is_consistent());
        CHECK(explain.total() == claimant.score * std::int64_t{pat::core::Fixed32::one_raw});
        CHECK(claimant.score == claimant.council_score + claimant.lawful_bonus + claimant.appointment_bonus);
    }
}

TEST_CASE("succession laws: dispute margin includes its boundary and designation adds claimant points") {
    Family f;
    f.rules = neutral_council_rules();
    const auto head = f.person(70);
    const auto lawful = f.person(45, Sex::Male, head);
    const auto top = f.person(40, Sex::Male, head);
    const auto boundary = f.person(35, Sex::Male, head);
    const auto outside = f.person(30, Sex::Male, head);
    f.retire(head);
    f.skill_points(top, 20);
    f.skill_points(boundary, 0);
    f.skill_points(outside, 0);
    f.rules.dispute_margin = 10;
    f.skill_points(boundary, 10);
    f.skill_points(outside, 9);
    f.designate(boundary);
    f.world.set_character_loyalty_head(lawful, pat::core::Fixed32::from_int(-100));
    const auto result = f.resolve(head);
    REQUIRE(result.dispute.has_value());
    const auto& rows = result.dispute->claimants;
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].character == boundary);
    CHECK(rows[0].appointment_bonus == 20);
    CHECK(rows[0].score == 30);
    CHECK(rows[1].character == top);
    CHECK(rows[2].character == lawful);
    CHECK(std::ranges::none_of(rows, [&](const auto& row) { return row.character == outside; }));
}

TEST_CASE("succession laws: partition ranks like majorat and queries preserve the World and RNG") {
    Family f;
    const auto head = f.person(65);
    const auto elder = f.person(35, Sex::Male, head);
    static_cast<void>(f.person(30, Sex::Male, head));
    f.retire(head);
    const auto majorat = f.resolve(head);
    f.law(SuccessionLaw::Partition);
    const auto hash = f.world.compute_hash();
    const LawSuccessionPolicy policy(f.rules);
    const SuccessionPolicy& interface = policy;
    const auto partition = interface.resolve(f.world, f.dynasty, head);
    CHECK(partition == majorat);
    CHECK(partition.winner == elder);
    CHECK(f.world.compute_hash() == hash);
    CHECK(interface.resolve(f.world, f.dynasty, head) == partition);
    CHECK(f.world.compute_hash() == hash);
    CHECK(f.world.family().land_grants.empty());
}

TEST_CASE("succession laws: a vacant house uses its last dead head or council scores without history") {
    Family f;
    const auto head = f.person(60);
    const auto son = f.person(20, Sex::Male, head);
    const auto elder = f.person(70);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));
    f.retire(head);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, {}));
    const auto anchored = f.resolve();
    CHECK(anchored.predecessor == head);
    CHECK(anchored.winner == son);
    CHECK(anchored.order.back().character == elder);

    Family headless;
    headless.rules = neutral_council_rules();
    const auto older = headless.person(40);
    const auto first_twin = headless.person(25);
    const auto second_twin = headless.person(25);
    headless.skill_points(first_twin, 10);
    headless.skill_points(second_twin, 10);
    const auto unanchored = headless.resolve();
    CHECK_FALSE(unanchored.predecessor.valid());
    CHECK(unanchored.winner == first_twin);
    CHECK(order_ids(unanchored) == std::vector<CharacterId>{first_twin, second_twin, older});
    CHECK(unanchored.order.front().reason == HeirReason::Kin);
}

TEST_CASE("succession laws: no eligible candidate and invalid query inputs give no successor") {
    Family f;
    const auto head = f.person(60);
    const auto member = f.person(30, Sex::Male, head);
    f.retire(head);
    f.status(member, family::kPersonDisowned);
    const auto result = f.resolve(head);
    CHECK(result.kind == SuccessionKind::NoCandidate);
    CHECK(result.order.empty());
    CHECK_FALSE(result.winner.valid());
    CHECK_FALSE(result.dispute.has_value());
    CHECK(resolve_successor(f.world, f.rules, DynastyId(999), head).order.empty());
    CHECK(f.resolve(CharacterId(999)).order.empty());
    CHECK(council_score(f.world, f.rules, f.dynasty, head, head).total == 0);
    World legacy(kStart, kSeed);
    CHECK(resolve_successor(legacy, f.rules, {}, {}).order.empty());
}
