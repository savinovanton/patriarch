// Demography content (E7-T07): the schema of demography_rules, the file data/balance/demography.pd and
// the builder that turns loaded content into pat::sim::DemographyRules.

#include "pat/content/demography_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/demography_schemas.hpp"
#include "pat/sim/demography.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pat;
using pat::content::DemographyContentResult;
using pat::core::Fixed32;
using pat::data::ContentSource;
using pat::data::DirectorySource;
using pat::data::LoadError;
using pat::data::MemorySource;
using pat::sim::DemographyRules;
using pat::sim::kDefaultDemography;

namespace fs = std::filesystem;

// A field added to DemographyRules changes the size. When this fails: add the field to flatten() below,
// to the schema (src/data/demography_schemas.cpp), to the builder (src/content/demography_content.cpp),
// to data/balance/demography.pd and to the tweaks table in this file.
static_assert(sizeof(DemographyRules) == 440,
              "DemographyRules changed: update the schema, the builder and this test");

namespace {

/// The files of one content root: relative path -> text (LF line endings).
using Files = std::map<std::string, std::string>;

Files shipped_files() {
    const DirectorySource dir(std::string(PATRIARCH_DATA_DIR) + "/balance");
    const auto listed = dir.list_files();
    REQUIRE(listed.has_value());
    Files files;
    for (const std::string& path : listed.value()) {
        auto text = dir.read_file(path);
        REQUIRE(text.has_value());
        std::string body = std::move(text).value();
        body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
        files.emplace(path, std::move(body));
    }
    return files;
}

MemorySource to_source(const Files& files, const std::string& name = "balance") {
    MemorySource source(name);
    for (const auto& [path, text] : files) {
        source.add(path, text);
    }
    return source;
}

DemographyContentResult load(const Files& files) {
    const MemorySource source = to_source(files);
    return content::load_demography_content(source);
}

/// Replaces the single occurrence of `from` in `file`.
void edit(Files& files, const std::string& file, const std::string& from, const std::string& to) {
    {
        INFO("no such file: " << file);
        REQUIRE(files.count(file) == 1);
    }
    std::string& text = files[file];
    const std::size_t at = text.find(from);
    {
        INFO("text not found in " << file << ": " << from);
        REQUIRE(at != std::string::npos);
    }
    {
        INFO("text is not unique in " << file << ": " << from);
        REQUIRE(text.find(from, at + 1) == std::string::npos);
    }
    text.replace(at, from.size(), to);
}

constexpr const char* kFile = "demography.pd";

/// 1-based line and column of the first occurrence of `needle` (ASCII text before it on the line);
/// `skip` moves the column right, to point at a value after `key = `.
data::SourcePos locate(const std::string& text, const std::string& needle, std::size_t skip = 0) {
    const std::size_t at = text.find(needle);
    {
        INFO("text not found: " << needle);
        REQUIRE(at != std::string::npos);
    }
    std::uint32_t line = 1;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < at; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    return data::SourcePos{line, static_cast<std::uint32_t>(at - line_start + 1 + skip)};
}

DemographyRules rules_of(const Files& files) {
    const DemographyContentResult result = load(files);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    return result.value().rules;
}

const LoadError* first_error(const DemographyContentResult& result) {
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    return &result.error().front();
}

/// Every number of the rules, named, in a fixed order (both age tables entry by entry, with their
/// count): two rules are equal exactly when their lists are.
using Flat = std::vector<std::pair<std::string, std::int64_t>>;

void flatten_table(Flat& out, const std::string& name, const sim::AgeTable& table) {
    out.emplace_back(name + ".count", table.count);
    for (std::size_t i = 0; i < table.buckets.size(); ++i) {
        out.emplace_back(name + "[" + std::to_string(i) + "].from_age", table.buckets[i].from_age);
        out.emplace_back(name + "[" + std::to_string(i) + "].value", table.buckets[i].value);
    }
}

Flat flatten(const DemographyRules& r) {
    Flat f;
    flatten_table(f, "mortality", r.mortality);
    f.emplace_back("noble_child_age_limit", r.noble_child_age_limit);
    f.emplace_back("noble_child_multiplier_permille", r.noble_child_multiplier_permille);
    f.emplace_back("low_health_below", r.low_health_below);
    f.emplace_back("high_health_from", r.high_health_from);
    f.emplace_back("low_health_mortality_permille", r.low_health_mortality_permille);
    f.emplace_back("high_health_mortality_permille", r.high_health_mortality_permille);
    f.emplace_back("base_conception_permille", r.base_conception_permille);
    flatten_table(f, "age_factor", r.age_factor);
    f.emplace_back("low_health_fertility_permille", r.low_health_fertility_permille);
    f.emplace_back("high_health_fertility_permille", r.high_health_fertility_permille);
    f.emplace_back("max_children_per_woman", r.max_children_per_woman);
    f.emplace_back("pregnancy_months", r.pregnancy_months);
    f.emplace_back("crowding_from_living", r.crowding_from_living);
    f.emplace_back("crowding_to_living", r.crowding_to_living);
    f.emplace_back("crowding_min_permille", r.crowding_min_permille);
    f.emplace_back("miscarriage_permille", r.miscarriage_permille);
    f.emplace_back("boys_per_100_girls", r.boys_per_100_girls);
    f.emplace_back("childbirth_risk_base_permille", r.childbirth_risk_base_permille);
    f.emplace_back("childbirth_risk_high_health_delta_permille",
                   r.childbirth_risk_high_health_delta_permille);
    f.emplace_back("childbirth_risk_low_health_delta_permille", r.childbirth_risk_low_health_delta_permille);
    f.emplace_back("childbirth_risk_physician_delta_permille", r.childbirth_risk_physician_delta_permille);
    f.emplace_back("stillbirth_permille", r.stillbirth_permille);
    f.emplace_back("newborn_health", r.newborn_health);
    f.emplace_back("skill_inherit_permille", r.skill_inherit_permille);
    f.emplace_back("skill_spread", r.skill_spread);
    f.emplace_back("adolescent_from", r.adolescent_from);
    f.emplace_back("prime_from", r.prime_from);
    f.emplace_back("mature_from", r.mature_from);
    f.emplace_back("elder_from", r.elder_from);
    f.emplace_back("marriage_age_male", r.marriage_age_male);
    f.emplace_back("marriage_age_female", r.marriage_age_female);
    f.emplace_back("health_drift.child", r.health_drift[0]);
    f.emplace_back("health_drift.adolescent", r.health_drift[1]);
    f.emplace_back("health_drift.prime", r.health_drift[2]);
    f.emplace_back("health_drift.mature", r.health_drift[3]);
    f.emplace_back("health_drift.elder", r.health_drift[4]);
    f.emplace_back("health_noise", r.health_noise);
    f.emplace_back("low_health_drift", r.low_health_drift);
    f.emplace_back("stress_high_from", r.stress_high_from);
    f.emplace_back("stress_critical_from", r.stress_critical_from);
    f.emplace_back("stress_high_health_drift", r.stress_high_health_drift);
    f.emplace_back("stress_critical_health_drift", r.stress_critical_health_drift);
    f.emplace_back("stress_gain_low_health", r.stress_gain_low_health);
    f.emplace_back("stress_gain_head", r.stress_gain_head);
    f.emplace_back("stress_decay_permille", r.stress_decay_permille);
    return f;
}

/// Compares two rules field by field, naming the field that differs.
void check_same(const DemographyRules& a, const DemographyRules& b) {
    const Flat fa = flatten(a);
    const Flat fb = flatten(b);
    REQUIRE(fa.size() == fb.size());
    for (std::size_t i = 0; i < fa.size(); ++i) {
        CAPTURE(fa[i].first);
        CHECK(fa[i].second == fb[i].second);
    }
}

/// Names of the fields in which the two rules differ.
std::vector<std::string> differences(const DemographyRules& a, const DemographyRules& b) {
    const Flat fa = flatten(a);
    const Flat fb = flatten(b);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < fa.size() && i < fb.size(); ++i) {
        if (fa[i].second != fb[i].second) {
            names.push_back(fa[i].first);
        }
    }
    return names;
}

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("patriarch_e7t07_" + tag + "_" + std::to_string(ticks));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& text) const {
        const fs::path target = path_ / relative;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << text;
    }

private:
    fs::path path_;
};

}  // namespace

// ---- the shipped file equals the built-in defaults -----------------------------------------------

TEST_CASE("demography content: data/balance loads and equals kDefaultDemography field by field") {
    const DirectorySource dir(std::string(PATRIARCH_DATA_DIR) + "/balance");
    const DemographyContentResult result = content::load_demography_content(dir);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());

    check_same(result.value().rules, kDefaultDemography);
    CHECK(differences(result.value().rules, kDefaultDemography).empty());
    // The defaults are valid, and so are the loaded rules.
    CHECK(sim::validate(kDefaultDemography).has_value());
    CHECK(sim::validate(result.value().rules).has_value());
    CHECK(result.value().content_hash != 0);
}

TEST_CASE("demography content: the whole data/ directory loads the same rules as data/balance") {
    const DirectorySource all(std::string(PATRIARCH_DATA_DIR));
    const DemographyContentResult result = content::load_demography_content(all);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    check_same(result.value().rules, kDefaultDemography);
}

TEST_CASE("demography content: the loaded rules drive the pure functions like the defaults") {
    const DemographyRules rules = rules_of(shipped_files());
    for (std::int32_t age = 0; age <= 110; ++age) {
        for (const bool noble : {false, true}) {
            for (const int hp : {10, 50, 90}) {
                const auto a = sim::mortality_permille(rules, age, Fixed32::from_int(hp), noble);
                const auto b = sim::mortality_permille(kDefaultDemography, age, Fixed32::from_int(hp), noble);
                CHECK(a.total == b.total);
            }
        }
        sim::ConceptionInput input;
        input.wife_age = age;
        CHECK(sim::conception_permille(rules, input).total ==
              sim::conception_permille(kDefaultDemography, input).total);
        CHECK(sim::life_stage(rules, age) == sim::life_stage(kDefaultDemography, age));
    }
    CHECK(sim::male_birth_permille(rules) == sim::male_birth_permille(kDefaultDemography));
}

// ---- determinism and hash ------------------------------------------------------------------------

TEST_CASE("demography content: loading twice gives identical rules and hash") {
    const Files files = shipped_files();
    const DemographyContentResult a = load(files);
    const DemographyContentResult b = load(files);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(differences(a.value().rules, b.value().rules).empty());
    CHECK(a.value().content_hash == b.value().content_hash);
}

TEST_CASE("demography content: the hash ignores files, comments and formatting but not values") {
    const Files files = shipped_files();
    const DemographyContentResult base = load(files);
    REQUIRE(base.has_value());

    // Same object: another file name, blank lines and comments.
    Files reshuffled;
    for (const auto& [path, text] : files) {
        reshuffled.emplace("moved/" + path, "\n\n# a comment\n" + text + "\n\n# the end\n");
    }
    const DemographyContentResult same = load(reshuffled);
    REQUIRE(same.has_value());
    CHECK(same.value().content_hash == base.value().content_hash);
    CHECK(differences(same.value().rules, base.value().rules).empty());

    // One value changed: the hash and exactly that rule change.
    Files changed = files;
    edit(changed, kFile, "pregnancy_months = 9", "pregnancy_months = 10");
    const DemographyContentResult other = load(changed);
    REQUIRE(other.has_value());
    CHECK(other.value().content_hash != base.value().content_hash);
    CHECK(other.value().rules.pregnancy_months == 10);

    // The hash is the same for the same values written differently (spaces, one line).
    Files respaced = files;
    edit(respaced, kFile, "low_below = 30", "low_below   =   30");
    const DemographyContentResult spaced = load(respaced);
    REQUIRE(spaced.has_value());
    CHECK(spaced.value().content_hash == base.value().content_hash);
}

// ---- editing values in data changes exactly that rule --------------------------------------------

namespace {

struct Tweak {
    const char* name;
    const char* from;
    const char* to;
    /// Names (flatten()) of the fields that must differ from the defaults afterwards.
    std::vector<std::string> changed;
};

/// One tweak for every number of the file (scalars and age-table steps).
std::vector<Tweak> tweaks() {
    return {
        {"mortality first bucket", "permille = 220", "permille = 230", {"mortality[0].value"}},
        {"mortality bucket 1", "permille = 70 }", "permille = 75 }", {"mortality[1].value"}},
        {"mortality bucket 2",
         "from_age = 5, permille = 20",
         "from_age = 5, permille = 21",
         {"mortality[2].value"}},
        {"mortality bucket 3", "permille = 15 }", "permille = 16 }", {"mortality[3].value"}},
        {"mortality bucket 4", "permille = 22 }", "permille = 23 }", {"mortality[4].value"}},
        {"mortality bucket 5", "permille = 45 }", "permille = 46 }", {"mortality[5].value"}},
        {"mortality bucket 6", "permille = 110 }", "permille = 111 }", {"mortality[6].value"}},
        {"mortality bucket 7", "permille = 240 }", "permille = 241 }", {"mortality[7].value"}},
        {"mortality last bucket", "permille = 450 }", "permille = 451 }", {"mortality[8].value"}},
        {"mortality bucket age",
         "from_age = 70, permille",
         "from_age = 75, permille",
         {"mortality[7].from_age"}},
        {"noble child age",
         "noble_child_age_limit = 15",
         "noble_child_age_limit = 10",
         {"noble_child_age_limit"}},
        {"noble child multiplier",
         "noble_child_multiplier_permille = 800",
         "noble_child_multiplier_permille = 900",
         {"noble_child_multiplier_permille"}},
        {"low health mortality",
         "low_health_multiplier_permille = 2000",
         "low_health_multiplier_permille = 1800",
         {"low_health_mortality_permille"}},
        {"high health mortality",
         "high_health_multiplier_permille = 750",
         "high_health_multiplier_permille = 700",
         {"high_health_mortality_permille"}},
        {"low health", "low_below = 30", "low_below = 25", {"low_health_below"}},
        {"high health", "high_from = 80", "high_from = 85", {"high_health_from"}},
        {"base conception", "base_permille = 12", "base_permille = 15", {"base_conception_permille"}},
        {"age factor first",
         "from_age = 0, factor_permille = 0",
         "from_age = 0, factor_permille = 10",
         {"age_factor[0].value"}},
        {"age factor 15",
         "from_age = 15, factor_permille = 800",
         "from_age = 15, factor_permille = 850",
         {"age_factor[1].value"}},
        {"age factor 20", "factor_permille = 1000", "factor_permille = 1100", {"age_factor[2].value"}},
        {"age factor 30",
         "from_age = 30, factor_permille = 800",
         "from_age = 30, factor_permille = 750",
         {"age_factor[3].value"}},
        {"age factor 35", "factor_permille = 500 }", "factor_permille = 450 }", {"age_factor[4].value"}},
        {"age factor 40", "factor_permille = 200 }", "factor_permille = 250 }", {"age_factor[5].value"}},
        {"age factor last",
         "from_age = 45, factor_permille = 0",
         "from_age = 45, factor_permille = 5",
         {"age_factor[6].value"}},
        {"age factor age",
         "from_age = 40, factor_permille",
         "from_age = 41, factor_permille",
         {"age_factor[5].from_age"}},
        {"low health fertility",
         "low_health_factor_permille = 500",
         "low_health_factor_permille = 400",
         {"low_health_fertility_permille"}},
        {"high health fertility",
         "high_health_factor_permille = 1100",
         "high_health_factor_permille = 1200",
         {"high_health_fertility_permille"}},
        {"child cap",
         "max_children_per_woman = 14",
         "max_children_per_woman = 12",
         {"max_children_per_woman"}},
        {"pregnancy", "pregnancy_months = 9", "pregnancy_months = 8", {"pregnancy_months"}},
        {"crowding from",
         "crowding_from_living = 3000",
         "crowding_from_living = 2500",
         {"crowding_from_living"}},
        {"crowding to", "crowding_to_living = 5000", "crowding_to_living = 6000", {"crowding_to_living"}},
        {"crowding minimum",
         "crowding_min_permille = 50",
         "crowding_min_permille = 100",
         {"crowding_min_permille"}},
        {"miscarriage", "miscarriage_permille = 30", "miscarriage_permille = 40", {"miscarriage_permille"}},
        {"sex ratio", "boys_per_100_girls = 105", "boys_per_100_girls = 103", {"boys_per_100_girls"}},
        {"childbirth base",
         "risk_base_permille = 50",
         "risk_base_permille = 60",
         {"childbirth_risk_base_permille"}},
        {"childbirth high health",
         "risk_high_health_delta_permille = -10",
         "risk_high_health_delta_permille = -15",
         {"childbirth_risk_high_health_delta_permille"}},
        {"childbirth low health",
         "risk_low_health_delta_permille = 10",
         "risk_low_health_delta_permille = 20",
         {"childbirth_risk_low_health_delta_permille"}},
        {"childbirth physician",
         "risk_physician_delta_permille = -10",
         "risk_physician_delta_permille = -5",
         {"childbirth_risk_physician_delta_permille"}},
        {"stillbirth", "stillbirth_permille = 20", "stillbirth_permille = 25", {"stillbirth_permille"}},
        {"newborn health", "newborn_health = 70", "newborn_health = 75", {"newborn_health"}},
        {"skill inheritance",
         "skill_inherit_permille = 300",
         "skill_inherit_permille = 350",
         {"skill_inherit_permille"}},
        {"skill spread", "skill_spread = 3", "skill_spread = 4", {"skill_spread"}},
        {"adolescent", "adolescent_from = 7", "adolescent_from = 8", {"adolescent_from"}},
        {"prime", "prime_from = 16", "prime_from = 17", {"prime_from"}},
        {"mature", "mature_from = 51", "mature_from = 50", {"mature_from"}},
        {"elder", "elder_from = 66", "elder_from = 65", {"elder_from"}},
        {"marriage male", "marriage_age_male = 16", "marriage_age_male = 17", {"marriage_age_male"}},
        {"marriage female", "marriage_age_female = 15", "marriage_age_female = 14", {"marriage_age_female"}},
        {"health drift child", "child = 15 ", "child = 16 ", {"health_drift.child"}},
        {"health drift adolescent", "adolescent = 10 ", "adolescent = 11 ", {"health_drift.adolescent"}},
        {"health drift prime", "prime = 0 ", "prime = 1 ", {"health_drift.prime"}},
        {"health drift mature", "mature = -10 ", "mature = -11 ", {"health_drift.mature"}},
        {"health drift elder", "elder = -40 ", "elder = -41 ", {"health_drift.elder"}},
        {"health noise", "noise = 20 ", "noise = 25 ", {"health_noise"}},
        {"low health drift", "low_health = -10 ", "low_health = -12 ", {"low_health_drift"}},
        {"stress high", "high_from = 60 ", "high_from = 55 ", {"stress_high_from"}},
        {"stress critical", "critical_from = 90 ", "critical_from = 95 ", {"stress_critical_from"}},
        {"stress high drift",
         "high_health_drift = -20",
         "high_health_drift = -25",
         {"stress_high_health_drift"}},
        {"stress critical drift",
         "critical_health_drift = -60",
         "critical_health_drift = -65",
         {"stress_critical_health_drift"}},
        {"stress gain illness", "gain_low_health = 100", "gain_low_health = 110", {"stress_gain_low_health"}},
        {"stress gain head", "gain_head = 30", "gain_head = 40", {"stress_gain_head"}},
        {"stress decay", "decay_permille = 100", "decay_permille = 150", {"stress_decay_permille"}},
    };
}

}  // namespace

TEST_CASE("demography content: editing one value in data changes exactly that rule") {
    for (const Tweak& tweak : tweaks()) {
        CAPTURE(tweak.name);
        Files files = shipped_files();
        edit(files, kFile, tweak.from, tweak.to);
        const DemographyRules rules = rules_of(files);
        CHECK(differences(rules, kDefaultDemography) == tweak.changed);
    }
}

TEST_CASE("demography content: every rule is covered by a tweak") {
    // The tweaks table must name every number of the rules (the age of a step is only tweaked for some
    // steps; the first step always starts at 0). A field that the builder forgets to read stays zero and
    // is noticed by the equality test with the defaults; a field read from the wrong place is noticed by
    // its tweak.
    std::vector<std::string> covered;
    for (const Tweak& tweak : tweaks()) {
        for (const std::string& name : tweak.changed) {
            covered.push_back(name);
        }
    }
    std::vector<std::string> required;
    for (const auto& [name, value] : flatten(kDefaultDemography)) {
        (void)value;
        if (name.find('[') == std::string::npos) {
            if (!name.ends_with(".count")) {
                required.push_back(name);
            }
        }
    }
    for (std::int32_t i = 0; i < kDefaultDemography.mortality.count; ++i) {
        required.push_back("mortality[" + std::to_string(i) + "].value");
    }
    for (std::int32_t i = 0; i < kDefaultDemography.age_factor.count; ++i) {
        required.push_back("age_factor[" + std::to_string(i) + "].value");
    }
    for (const std::string& name : required) {
        CAPTURE(name);
        CHECK(std::count(covered.begin(), covered.end(), name) == 1);
    }
}

TEST_CASE("demography content: a file edited on disk is picked up without recompiling") {
    TempDir dir("edit");
    for (const auto& [path, text] : shipped_files()) {
        dir.write(path, text);
    }
    const DirectorySource source(dir.path());
    auto first = content::load_demography_content(source);
    REQUIRE(first.has_value());
    check_same(first.value().rules, kDefaultDemography);

    Files edited = shipped_files();
    edit(edited, kFile, "boys_per_100_girls = 105", "boys_per_100_girls = 110");
    dir.write(kFile, edited[kFile]);
    auto second = content::load_demography_content(source);
    REQUIRE(second.has_value());
    CHECK(second.value().rules.boys_per_100_girls == 110);
    CHECK(second.value().content_hash != first.value().content_hash);
}

TEST_CASE("demography content: a changed value changes the probabilities the functions return") {
    Files files = shipped_files();
    edit(files, kFile, "noble_child_multiplier_permille = 800", "noble_child_multiplier_permille = 500");
    const DemographyRules rules = rules_of(files);
    const auto changed = sim::mortality_permille(rules, 0, Fixed32::from_int(50), true).total;
    const auto original = sim::mortality_permille(kDefaultDemography, 0, Fixed32::from_int(50), true).total;
    CHECK(changed < original);
    CHECK(changed == core::Fixed64::from_int(110));  // 220 x 0.5
}

// ---- a second root (a mod) replaces the object whole ---------------------------------------------

TEST_CASE("demography content: a mod root overrides the object") {
    const Files base_files = shipped_files();
    const MemorySource base = to_source(base_files, "base");

    SUBCASE("one value changed in a repeated object") {
        Files edited = base_files;
        edit(edited, kFile, "max_children_per_woman = 14", "max_children_per_woman = 10");
        MemorySource mod("mod");
        mod.add("demography.pd", edited[kFile]);
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_demography_content(roots);
        INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
        REQUIRE(result.has_value());
        CHECK(differences(result.value().rules, kDefaultDemography) ==
              std::vector<std::string>{"max_children_per_woman"});

        const auto plain = content::load_demography_content(base);
        REQUIRE(plain.has_value());
        CHECK(result.value().content_hash != plain.value().content_hash);
    }

    SUBCASE("the object in another file of the mod root still wins") {
        Files edited = base_files;
        edit(edited, kFile, "adolescent_from = 7", "adolescent_from = 8");
        MemorySource mod("mod");
        mod.add("balance/anything.pd", edited[kFile]);
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_demography_content(roots);
        REQUIRE(result.has_value());
        CHECK(differences(result.value().rules, kDefaultDemography) ==
              std::vector<std::string>{"adolescent_from"});
    }

    SUBCASE("a mod object that is not valid points into the mod file") {
        Files edited = base_files;
        edit(edited, kFile, "elder_from = 66", "elder_from = 40");  // not above mature_from
        MemorySource bad("mods/bad");
        bad.add("b.pd", edited[kFile]);
        const ContentSource* roots[] = {&base, &bad};
        const auto result = content::load_demography_content(roots);
        const LoadError* error = first_error(result);
        CHECK(error->root == 1);
        CHECK(error->root_name == "mods/bad");
        CHECK(error->file == "b.pd");
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.pos == locate(edited[kFile], "life_stages {"));
    }

    SUBCASE("a mod that leaves out a block is a schema error in the mod file") {
        MemorySource bad("mods/partial");
        bad.add("b.pd",
                "format_version = 1\ndemography_rules \"main\" {\n    health { low_below = 30, high_from = "
                "80 }\n}\n");
        const ContentSource* roots[] = {&base, &bad};
        const auto result = content::load_demography_content(roots);
        const LoadError* error = first_error(result);
        CHECK(error->root == 1);
        CHECK(error->diagnostic.code == data::ErrorCode::MissingField);
        CHECK(error->diagnostic.pos.line == 2);
    }

    SUBCASE("a second object with another id in the mod is rejected, not merged") {
        Files edited = base_files;
        std::string& text = edited[kFile];
        text.replace(text.find("\"main\""), 6, "\"other\"");
        MemorySource mod("mod");
        mod.add("other.pd", text);
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_demography_content(roots);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("only one demography_rules") != std::string::npos);
        CHECK(error->root == 1);
        CHECK(error->file == "other.pd");
    }
}

// ---- schema errors are positioned and helpful -----------------------------------------------------

TEST_CASE("demography content: a value outside its corridor is PD104 at the value") {
    Files files = shipped_files();
    edit(files, kFile, "pregnancy_months = 9", "pregnancy_months = 99");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->file == kFile);
    CHECK(error->diagnostic.code == data::ErrorCode::ValueOutOfRange);
    const data::SourcePos expected =
        locate(files[kFile], "pregnancy_months = 99", std::string("pregnancy_months = ").size());
    CHECK(error->diagnostic.pos == expected);
    CHECK(error->format().find("demography.pd:" + std::to_string(expected.line) + ":" +
                               std::to_string(expected.column)) != std::string::npos);
}

TEST_CASE("demography content: a value inside an age table step is positioned at the value") {
    Files files = shipped_files();
    edit(files, kFile, "from_age = 80, permille = 450", "from_age = 80, permille = 4500");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ValueOutOfRange);
    CHECK(error->diagnostic.pos ==
          locate(files[kFile], "permille = 4500", std::string("permille = ").size()));
}

TEST_CASE("demography content: a typo in a field name is PD101 with a hint") {
    Files files = shipped_files();
    edit(files, kFile, "noble_child_multiplier_permille", "noble_child_multipler_permille");
    const auto result = load(files);
    REQUIRE_FALSE(result.has_value());
    // The misspelled field is also missing under its right name (PD102, reported at the block, so it
    // sorts first); the typo itself is PD101 at the key.
    const LoadError* typo = nullptr;
    bool missing = false;
    for (const LoadError& error : result.error()) {
        if (error.diagnostic.code == data::ErrorCode::UnknownField) {
            typo = &error;
        }
        missing = missing || error.diagnostic.code == data::ErrorCode::MissingField;
    }
    CHECK(missing);
    REQUIRE(typo != nullptr);
    CHECK(typo->diagnostic.message.find("did you mean 'noble_child_multiplier_permille'") !=
          std::string::npos);
    CHECK(typo->diagnostic.pos == locate(files[kFile], "noble_child_multipler_permille"));
}

TEST_CASE("demography content: a typo in a step field and a missing field are reported with positions") {
    SUBCASE("a step field") {
        Files files = shipped_files();
        edit(files, kFile, "from_age = 45, permille = 45", "from_age = 45, pemille = 45");
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        bool hinted = false;
        for (const LoadError& error : result.error()) {
            hinted = hinted || error.diagnostic.message.find("did you mean 'permille'") != std::string::npos;
        }
        CHECK(hinted);
    }
    SUBCASE("a missing field") {
        Files files = shipped_files();
        edit(files, kFile, "        mature_from = 51\n", "");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::MissingField);
        CHECK(error->diagnostic.message.find("mature_from") != std::string::npos);
        CHECK(error->diagnostic.pos.line == locate(files[kFile], "life_stages {").line);
    }
    SUBCASE("a wrong type") {
        Files files = shipped_files();
        edit(files, kFile, "prime_from = 16", "prime_from = \"sixteen\"");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::WrongType);
        CHECK(error->diagnostic.pos == locate(files[kFile], "\"sixteen\""));
    }
}

TEST_CASE("demography content: the schema corridors agree with validate() at their edges") {
    // The schema repeats the corridors of validate() (the data module cannot include sim). The edges are
    // pinned here with the constants of the sim. Rule violations that validate() reports as Multiplier
    // and Conception errors cannot get past the schema: they are PD104 at the value.
    struct Edge {
        const char* from;
        const char* key;
        std::int64_t max;
    };
    const std::int64_t multiplier_max = DemographyRules::kMaxMultiplierPermille;
    const Edge edges[] = {
        {"noble_child_multiplier_permille = 800", "noble_child_multiplier_permille", multiplier_max},
        {"low_health_multiplier_permille = 2000", "low_health_multiplier_permille", multiplier_max},
        {"high_health_multiplier_permille = 750", "high_health_multiplier_permille", multiplier_max},
        {"low_health_factor_permille = 500", "low_health_factor_permille", multiplier_max},
        {"high_health_factor_permille = 1100", "high_health_factor_permille", multiplier_max},
        {"from_age = 20, factor_permille = 1000", "from_age = 20, factor_permille", multiplier_max},
        {"noble_child_age_limit = 15", "noble_child_age_limit", 200},
        {"base_permille = 12", "base_permille", 1000},
        {"crowding_to_living = 5000", "crowding_to_living", 1048576},
        {"crowding_min_permille = 50", "crowding_min_permille", 1000},
        {"miscarriage_permille = 30", "miscarriage_permille", 1000},
        {"stillbirth_permille = 20", "stillbirth_permille", 1000},
        {"newborn_health = 70", "newborn_health", 100},
        {"skill_inherit_permille = 300", "skill_inherit_permille", multiplier_max},
        {"skill_spread = 3", "skill_spread", 20},
        {"pregnancy_months = 9", "pregnancy_months", 24},
        {"boys_per_100_girls = 105", "boys_per_100_girls", 10000},
        {"from_age = 15, permille = 15", "from_age = 15, permille", 1000},
        {"high_from = 80", "high_from", 100},
        {"child = 15 ", "child", 10000},
        {"elder = -40 ", "elder", 10000},
        {"noise = 20 ", "noise", 10000},
        {"critical_from = 90 ", "critical_from", 100},
        {"gain_head = 30", "gain_head", 10000},
        {"decay_permille = 100", "decay_permille", 1000},
    };
    for (const Edge& edge : edges) {
        CAPTURE(edge.key);
        Files at_max = shipped_files();
        edit(at_max, kFile, edge.from, std::string(edge.key) + " = " + std::to_string(edge.max));
        {
            const auto result = load(at_max);
            INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
            CHECK(result.has_value());
        }

        Files above = shipped_files();
        edit(above, kFile, edge.from, std::string(edge.key) + " = " + std::to_string(edge.max + 1));
        const auto result = load(above);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::ValueOutOfRange);
    }

    // The lower edges.
    struct Low {
        const char* from;
        const char* to;
    };
    const Low lows[] = {{"max_children_per_woman = 14", "max_children_per_woman = 0"},
                        {"pregnancy_months = 9", "pregnancy_months = 0"},
                        {"boys_per_100_girls = 105", "boys_per_100_girls = 0"},
                        {"permille = 220", "permille = -1"},
                        {"adolescent_from = 7", "adolescent_from = 0"},
                        {"low_below = 30", "low_below = -1"},
                        {"crowding_from_living = 3000", "crowding_from_living = -1"},
                        {"crowding_to_living = 5000", "crowding_to_living = 0"}};
    for (const Low& low : lows) {
        CAPTURE(low.to);
        Files files = shipped_files();
        edit(files, kFile, low.from, low.to);
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::ValueOutOfRange);
    }
}

// ---- rules that span fields: validate() errors, placed in the file ---------------------------------

TEST_CASE("demography content: mortality steps that do not increase are reported at the step") {
    Files files = shipped_files();
    edit(files, kFile, "from_age = 5, permille = 20", "from_age = 1, permille = 20");  // 1 again
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->file == kFile);
    CHECK(error->diagnostic.message.find("inconsistent") != std::string::npos);
    CHECK(error->diagnostic.message.find("mortality") != std::string::npos);
    CHECK(error->diagnostic.pos ==
          locate(files[kFile], "from_age = 1, permille = 20", std::string("from_age = ").size()));
}

TEST_CASE("demography content: a mortality table that has a gap at the start is reported at the first step") {
    Files files = shipped_files();
    edit(files, kFile, "from_age = 0, permille = 220", "from_age = 1, permille = 220");
    edit(files, kFile, "from_age = 1, permille = 70", "from_age = 2, permille = 70");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->diagnostic.message.find("not 0") != std::string::npos);
    CHECK(error->diagnostic.pos ==
          locate(files[kFile], "from_age = 1, permille = 220", std::string("from_age = ").size()));
}

TEST_CASE("demography content: an empty or too long mortality table") {
    SUBCASE("empty: reported at the field") {
        Files files = shipped_files();
        const std::size_t open = files[kFile].find("buckets = [");
        const std::size_t close = files[kFile].find(']', open);
        REQUIRE(open != std::string::npos);
        REQUIRE(close != std::string::npos);
        files[kFile].replace(open, close - open + 1, "buckets = []");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("bucket count 0") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files[kFile], "buckets = []"));
    }
    SUBCASE("17 steps: reported at the 17th") {
        Files files = shipped_files();
        std::string more = "{ from_age = 80, permille = 450 }";
        for (int age = 81; age <= 88; ++age) {
            more += "\n            { from_age = " + std::to_string(age) + ", permille = 450 }";
        }
        edit(files, kFile, "{ from_age = 80, permille = 450 }", more);
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("bucket count 17") != std::string::npos);
        CHECK(error->diagnostic.pos ==
              locate(files[kFile], "from_age = 88", std::string("from_age = ").size()));
    }
    SUBCASE("16 steps are fine") {
        Files files = shipped_files();
        std::string more = "{ from_age = 80, permille = 450 }";
        for (int age = 81; age <= 87; ++age) {
            more += "\n            { from_age = " + std::to_string(age) + ", permille = 450 }";
        }
        edit(files, kFile, "{ from_age = 80, permille = 450 }", more);
        const DemographyRules rules = rules_of(files);
        CHECK(rules.mortality.count == 16);
        CHECK(rules.mortality.lookup(87) == 450);
    }
}

TEST_CASE("demography content: age-factor steps that do not increase are reported at the step") {
    SUBCASE("not increasing") {
        Files files = shipped_files();
        edit(files, kFile, "from_age = 35, factor_permille = 500", "from_age = 30, factor_permille = 500");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("age_factor") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files[kFile],
                                              "from_age = 30, factor_permille = 500",
                                              std::string("from_age = ").size()));
    }
    SUBCASE("does not start at 0") {
        Files files = shipped_files();
        edit(files, kFile, "from_age = 0, factor_permille = 0", "from_age = 3, factor_permille = 0");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("age_factor") != std::string::npos);
        CHECK(error->diagnostic.pos ==
              locate(files[kFile], "from_age = 3, factor_permille = 0", std::string("from_age = ").size()));
    }
}

TEST_CASE("demography content: health thresholds that are not low < high are reported at the health block") {
    Files files = shipped_files();
    edit(files, kFile, "low_below = 30", "low_below = 80");  // equal to high_from
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->diagnostic.message.find("health thresholds") != std::string::npos);
    CHECK(error->diagnostic.pos == locate(files[kFile], "health {"));
}

TEST_CASE("demography content: childbirth risk that leaves 0...1000 is reported at the birth block") {
    SUBCASE("below zero") {
        Files files = shipped_files();
        edit(files, kFile, "risk_base_permille = 50", "risk_base_permille = 5");  // 5 - 10 - 10 < 0
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("childbirth risk") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files[kFile], "birth {"));
    }
    SUBCASE("above 1000") {
        Files files = shipped_files();
        edit(files, kFile, "risk_base_permille = 50", "risk_base_permille = 995");  // + 10 low health
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.pos == locate(files[kFile], "birth {"));
    }
}

TEST_CASE("demography content: stress thresholds that do not increase are reported at the stress block") {
    Files files = shipped_files();
    edit(files, kFile, "critical_from = 90", "critical_from = 60");  // equal to high_from
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->diagnostic.message.find("stress thresholds") != std::string::npos);
    CHECK(error->diagnostic.pos == locate(files[kFile], "stress {"));
}

TEST_CASE("demography content: life stages that do not increase or marriage ages outside them") {
    SUBCASE("elder not above mature") {
        Files files = shipped_files();
        edit(files, kFile, "elder_from = 66", "elder_from = 51");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("stage ages") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files[kFile], "life_stages {"));
    }
    SUBCASE("prime before adolescent") {
        Files files = shipped_files();
        edit(files, kFile, "prime_from = 16", "prime_from = 7");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.pos == locate(files[kFile], "life_stages {"));
    }
    SUBCASE("marriage age below the adolescent stage") {
        Files files = shipped_files();
        edit(files, kFile, "marriage_age_female = 15", "marriage_age_female = 6");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("marriage_age_female") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files[kFile], "life_stages {"));
    }
    SUBCASE("marriage age in the mature stage") {
        Files files = shipped_files();
        edit(files, kFile, "marriage_age_male = 16", "marriage_age_male = 51");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("marriage_age_male") != std::string::npos);
    }
}

TEST_CASE("demography content: all problems of a file come back in one list, sorted by position") {
    Files files = shipped_files();
    edit(files, kFile, "pregnancy_months = 9", "pregnancy_months = 99");
    edit(files, kFile, "boys_per_100_girls = 105", "boys_per_100_girls = 0");
    edit(files, kFile, "adolescent_from = 7", "adolescent_fro = 7");
    const auto result = load(files);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().size() >= 3);
    for (std::size_t i = 1; i < result.error().size(); ++i) {
        CHECK(result.error()[i - 1].diagnostic.pos <= result.error()[i].diagnostic.pos);
    }
}

// ---- missing or repeated objects, broken text -----------------------------------------------------

TEST_CASE("demography content: missing or repeated demography_rules objects") {
    SUBCASE("none") {
        Files files = shipped_files();
        files.erase(kFile);
        files["other.pd"] = "format_version = 1\n";
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        CHECK(content::format_errors(result.error()).find("no demography_rules object") != std::string::npos);
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::ContentRule);
    }
    SUBCASE("no files at all") {
        const MemorySource nothing("empty");
        const auto result = content::load_demography_content(nothing);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }
    SUBCASE("two in one root") {
        Files files = shipped_files();
        files["second.pd"] = files[kFile];
        files["second.pd"].replace(files["second.pd"].find("\"main\""), 6, "\"other\"");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("only one demography_rules") != std::string::npos);
    }
    SUBCASE("an object without an id") {
        Files files = shipped_files();
        edit(files, kFile, "demography_rules \"main\" {", "demography_rules {");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::BadLabel);
    }
}

TEST_CASE("demography content: broken text never crashes") {
    const Files files = shipped_files();

    SUBCASE("garbage") {
        Files bad = files;
        bad[kFile] = "format_version = 1\ndemography_rules \"main\" { ??? }}}";
        const auto result = load(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(content::format_errors(result.error()).empty());
    }
    SUBCASE("a missing root directory") {
        const DirectorySource nowhere(std::string(PATRIARCH_DATA_DIR) + "/no_such_directory");
        const auto result = content::load_demography_content(nowhere);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::UnreadableSource);
    }
    SUBCASE("a list where a block is expected, and the reverse") {
        Files bad = files;
        edit(bad, kFile, "health {", "health = [");
        edit(bad, kFile, "low_below = 30", "30");
        edit(bad, kFile, "high_from = 80\n    }", "]");
        const auto result = load(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }
    SUBCASE("a table that is a number, a step that is a number") {
        Files bad = files;
        const std::size_t open = bad[kFile].find("buckets = [");
        const std::size_t close = bad[kFile].find(']', open);
        bad[kFile].replace(open, close - open + 1, "buckets = 5");
        const auto result = load(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::WrongType);

        Files bad2 = files;
        edit(bad2, kFile, "{ from_age = 0, factor_permille = 0 }", "7");
        const auto result2 = load(bad2);
        REQUIRE_FALSE(result2.has_value());
        CHECK(result2.error().front().diagnostic.code == data::ErrorCode::WrongType);
    }
    SUBCASE("the file cut at many places") {
        const std::string& text = files.at(kFile);
        for (std::size_t cut = 0; cut < text.size(); cut += 37) {
            Files bad = files;
            bad[kFile] = text.substr(0, cut);
            const auto result = load(bad);  // either rules or errors, but no crash
            if (result.has_value()) {
                CHECK(sim::validate(result.value().rules).has_value());
            } else {
                CHECK_FALSE(result.error().empty());
            }
        }
    }
    SUBCASE("every number replaced by an absurd one") {
        const std::string& text = files.at(kFile);
        for (std::size_t at = 0; at < text.size(); ++at) {
            if (text[at] < '0' || text[at] > '9' || (at > 0 && text[at - 1] >= '0' && text[at - 1] <= '9')) {
                continue;
            }
            // Numbers in comments are not data.
            const std::size_t line_start =
                text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1;
            const std::size_t hash = text.find('#', line_start);
            if (hash != std::string::npos && hash < at) {
                continue;
            }
            std::size_t end = at;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }
            for (const char* absurd : {"0", "-1", "99999999999999999999", "2147483648", "1000000"}) {
                Files bad = files;
                bad[kFile] = text.substr(0, at) + absurd + text.substr(end);
                const auto result = load(bad);
                if (result.has_value()) {
                    CHECK(sim::validate(result.value().rules).has_value());
                } else {
                    CHECK_FALSE(result.error().empty());
                }
            }
        }
    }
}

// ---- the schema is registered and documented ------------------------------------------------------

namespace {

void check_documented(const data::ObjectSchema& schema) {
    CAPTURE(schema.kind());
    CHECK_FALSE(schema.description().empty());
    for (const data::FieldSpec& field : schema.fields()) {
        CAPTURE(field.name);
        CHECK_FALSE(field.description.empty());
        const data::Type* type = &field.type;
        while ((type->kind == data::TypeKind::List || type->kind == data::TypeKind::Map) && type->element) {
            type = type->element.get();
        }
        if (type->kind == data::TypeKind::Block && type->schema) {
            check_documented(*type->schema);
        }
    }
}

}  // namespace

TEST_CASE("demography content: the kind is registered and every field has a description") {
    const data::SchemaSet schemas = data::content_schemas();
    const data::ObjectSchema* schema = schemas.find("demography_rules");
    REQUIRE(schema != nullptr);
    check_documented(*schema);
    CHECK(std::string(data::kDemographyRulesKind) == "demography_rules");
}

TEST_CASE("demography content: the schema offers a field for every number of the rules") {
    // Count the numeric leaves of the schema: 5 blocks, the two tables count as one field each (their
    // steps are separate numbers) and every other leaf is one scalar of DemographyRules.
    const data::SchemaSet schemas = data::content_schemas();
    const data::ObjectSchema* schema = schemas.find("demography_rules");
    REQUIRE(schema != nullptr);
    std::size_t scalars = 0;
    std::size_t tables = 0;
    for (const data::FieldSpec& block : schema->fields()) {
        REQUIRE(block.type.kind == data::TypeKind::Block);
        for (const data::FieldSpec& field : block.type.schema->fields()) {
            (field.type.kind == data::TypeKind::List ? tables : scalars) += 1;
        }
    }
    CHECK(tables == 2);
    CHECK(scalars == 44);  // the int32 scalars of DemographyRules (440 bytes = 2 tables x 132 + 44 x 4)
}
