// Economy content (E9-T18): the schemas of the economy kinds, the files in data/economy/ and the
// builder that turns loaded content into pat::sim::economy::EconomyRules.

#include "pat/content/economy_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/sim/economy/rules.hpp"

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
#include <vector>

using namespace pat;
using namespace pat::sim::economy;
using pat::content::EconomyContentResult;
using pat::core::Fixed32;
using pat::data::ContentSource;
using pat::data::DirectorySource;
using pat::data::LoadError;
using pat::data::MemorySource;

namespace fs = std::filesystem;

namespace {

/// The files of one content root: relative path -> text (LF line endings).
using Files = std::map<std::string, std::string>;

Files shipped_files() {
    const DirectorySource dir(std::string(PATRIARCH_DATA_DIR) + "/economy");
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

MemorySource to_source(const Files& files, const std::string& name = "economy") {
    MemorySource source(name);
    for (const auto& [path, text] : files) {
        source.add(path, text);
    }
    return source;
}

EconomyContentResult load(const Files& files) {
    const MemorySource source = to_source(files);
    return content::load_economy_content(source);
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

/// 1-based line and column of the first occurrence of `needle` (ASCII text before it on the line).
data::SourcePos locate(const std::string& text, const std::string& needle) {
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
    return data::SourcePos{line, static_cast<std::uint32_t>(at - line_start + 1)};
}

EconomyRules rules_of(const Files& files) {
    const EconomyContentResult result = load(files);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    return result.value().rules;
}

const LoadError* first_error(const EconomyContentResult& result) {
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    return &result.error().front();
}

Fixed32 fixed(double v) {
    return Fixed32::from_double(v);
}

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("patriarch_e9t18_" + tag + "_" + std::to_string(ticks));
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

// ---- the shipped files equal the built-in defaults ------------------------------------------------

TEST_CASE("economy content: data/economy loads and equals EconomyRules::defaults() field by field") {
    const DirectorySource dir(std::string(PATRIARCH_DATA_DIR) + "/economy");
    const EconomyContentResult result = content::load_economy_content(dir);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());

    const EconomyRules& rules = result.value().rules;
    const EconomyRules defaults = EconomyRules::defaults();
    CHECK(rules.population == defaults.population);
    CHECK(rules.production == defaults.production);
    CHECK(rules.consumption == defaults.consumption);
    CHECK(rules.tax == defaults.tax);
    CHECK(rules.control == defaults.control);
    CHECK(rules.mood == defaults.mood);
    CHECK(rules.unrest == defaults.unrest);
    CHECK(rules.recruit == defaults.recruit);
    CHECK(rules.loans == defaults.loans);
    CHECK(rules.buildings == defaults.buildings);
    CHECK(rules.crown_account_mode == defaults.crown_account_mode);
    CHECK(rules.build_foreign_allowed == defaults.build_foreign_allowed);
    CHECK(rules == defaults);

    // The defaults are valid, and so are the loaded rules.
    CHECK(validate(rules).has_value());
    CHECK(result.value().content_hash != 0);
}

TEST_CASE("economy content: the whole data/ directory loads the same rules as data/economy") {
    const DirectorySource all(std::string(PATRIARCH_DATA_DIR));
    const EconomyContentResult result = content::load_economy_content(all);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    CHECK(result.value().rules == EconomyRules::defaults());
}

TEST_CASE("economy content: building keys outlive the load result") {
    EconomyRules rules;
    {
        const DirectorySource dir(std::string(PATRIARCH_DATA_DIR) + "/economy");
        const EconomyContentResult result = content::load_economy_content(dir);
        REQUIRE(result.has_value());
        rules = result.value().rules;
    }
    REQUIRE(rules.buildings.count == 1);
    CHECK(rules.buildings.defs[0].key == "building.mill");
}

TEST_CASE("economy content: the mill is defined as in economy-model 11.1") {
    const EconomyRules rules = rules_of(shipped_files());
    REQUIRE(rules.buildings.count == 1);
    const BuildingDef& mill = rules.buildings.defs[0];
    CHECK(mill.slot == 1);
    CHECK(mill.cost_kop == 400'000);
    CHECK(mill.build_days == 120);
    CHECK(mill.upkeep_kop_month == 3'000);
    CHECK(mill.output_permille[0] == 1150);
    CHECK(mill.output_permille[1] == 1000);
}

// ---- determinism and hash ------------------------------------------------------------------------

TEST_CASE("economy content: loading twice gives identical rules and hash") {
    const Files files = shipped_files();
    const EconomyContentResult a = load(files);
    const EconomyContentResult b = load(files);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value().rules == b.value().rules);
    CHECK(a.value().content_hash == b.value().content_hash);
}

TEST_CASE("economy content: the hash ignores files, comments and formatting but not values") {
    const Files files = shipped_files();
    const EconomyContentResult base = load(files);
    REQUIRE(base.has_value());

    // Same objects: other file names, blank lines and comments, objects moved between files.
    Files reshuffled;
    for (const auto& [path, text] : files) {
        reshuffled.emplace("moved/" + path, "\n\n# a comment\n" + text + "\n\n# the end\n");
    }
    const EconomyContentResult same = load(reshuffled);
    REQUIRE(same.has_value());
    CHECK(same.value().content_hash == base.value().content_hash);
    CHECK(same.value().rules == base.value().rules);

    // One value changed: the hash and the rules change.
    Files changed = files;
    edit(changed, "rules.pd", "refund_permille = 800", "refund_permille = 801");
    const EconomyContentResult other = load(changed);
    REQUIRE(other.has_value());
    CHECK(other.value().content_hash != base.value().content_hash);
    CHECK(other.value().rules.buildings.refund_permille == 801);
}

TEST_CASE("economy content: slots, not ids or file order, decide the indices") {
    Files files = shipped_files();
    edit(files,
         "goods.pd",
         "good \"salt\" {\n    slot = 1\n    base_price_kop = 1000",
         "good \"salt\" {\n    slot = 2\n    base_price_kop = 1111");
    edit(files,
         "goods.pd",
         "good \"fur\" {\n    slot = 2\n    base_price_kop = 1000",
         "good \"fur\" {\n    slot = 1\n    base_price_kop = 2222");
    const EconomyRules rules = rules_of(files);
    CHECK(rules.production.base_price_kop[1] == 2222);
    CHECK(rules.production.base_price_kop[2] == 1111);
}

// ---- editing values in data changes the rules without recompiling --------------------------------

namespace {

struct Tweak {
    const char* name;
    const char* file;
    const char* from;
    const char* to;
    std::function<void(EconomyRules&)> apply;
};

std::vector<Tweak> tweaks() {
    return {
        {"refund",
         "rules.pd",
         "refund_permille = 800",
         "refund_permille = 650",
         [](EconomyRules& r) { r.buildings.refund_permille = 650; }},
        {"Q7 crown account",
         "rules.pd",
         "crown_account_mode = separate",
         "crown_account_mode = emperor_dynasty",
         [](EconomyRules& r) { r.crown_account_mode = CrownAccountMode::EmperorDynasty; }},
        {"Q4 foreign building",
         "rules.pd",
         "build_foreign_allowed = no",
         "build_foreign_allowed = yes",
         [](EconomyRules& r) { r.build_foreign_allowed = true; }},
        {"Q6 city share",
         "rules.pd",
         "city_crown_share_permille = 300",
         "city_crown_share_permille = 450",
         [](EconomyRules& r) { r.tax.city_crown_share_permille = 450; }},
        {"Q3 loan rate",
         "rules.pd",
         "rate_ppm_year = 100000",
         "rate_ppm_year = 120000",
         [](EconomyRules& r) { r.loans.rate_ppm_year = 120000; }},
        {"Q3 loan count",
         "rules.pd",
         "max_loans_per_dynasty = 3",
         "max_loans_per_dynasty = 5",
         [](EconomyRules& r) { r.loans.max_loans_per_dynasty = 5; }},
        {"default tax level",
         "rules.pd",
         "default_tax_level = tax_level:normal",
         "default_tax_level = tax_level:high",
         [](EconomyRules& r) { r.tax.default_level = 2; }},
        {"default recruit level",
         "rules.pd",
         "default_recruit_level = recruit_level:usual",
         "default_recruit_level = recruit_level:intensified",
         [](EconomyRules& r) { r.recruit.default_level = 2; }},
        {"capacity",
         "rules.pd",
         "capacity_per_development = 25000",
         "capacity_per_development = 30000",
         [](EconomyRules& r) { r.population.capacity_per_development = 30000; }},
        {"anarchy threshold",
         "rules.pd",
         "anarchy_threshold = 15",
         "anarchy_threshold = 10",
         [](EconomyRules& r) { r.control.anarchy_threshold = 10; }},
        {"riot chance",
         "rules.pd",
         "riot_chance_max_permille = 250",
         "riot_chance_max_permille = 300",
         [](EconomyRules& r) { r.unrest.riot_chance_max_permille = 300; }},
        {"grain price",
         "goods.pd",
         "base_price_kop = 1200",
         "base_price_kop = 1500",
         [](EconomyRules& r) { r.production.base_price_kop[0] = 1500; }},
        {"grain need",
         "goods.pd",
         "peasants = 35",
         "peasants = 33",
         [](EconomyRules& r) { r.consumption.need_per_head_milli[3][0] = 33; }},
        {"salt need",
         "goods.pd",
         "good \"salt\" {\n    slot = 1\n    base_price_kop = 1000",
         "good \"salt\" {\n    slot = 1\n    base_price_kop = 1000\n    need_per_head_milli = { clergy = 7 }",
         [](EconomyRules& r) { r.consumption.need_per_head_milli[1][1] = 7; }},
        {"birth rate",
         "groups.pd",
         "birth_ppm_year = 36000",
         "birth_ppm_year = 37000",
         [](EconomyRules& r) { r.population.birth_ppm_year[0] = 37000; }},
        {"mood (decimal)",
         "groups.pd",
         "death_ppm_year = 36000\n    head_tax_kop_year = 20\n    wealth_base = 100\n    base_mood = 0",
         "death_ppm_year = 36000\n    head_tax_kop_year = 20\n    wealth_base = 100\n    base_mood = -2.5",
         [](EconomyRules& r) { r.mood.base_mood[4] = fixed(-2.5); }},
        {"owner share",
         "groups.pd",
         "province_kind \"port\" { owner_share_permille = 1000 }",
         "province_kind \"port\" { owner_share_permille = 900 }",
         [](EconomyRules& r) { r.production.owner_share_permille[4] = 900; }},
        {"tax level",
         "policies.pd",
         "tax_permille = 1600",
         "tax_permille = 1700",
         [](EconomyRules& r) { r.tax.levels[3].tax_permille = 1700; }},
        {"tax pressure (decimal)",
         "policies.pd",
         "peasants = 14",
         "peasants = 14.5",
         [](EconomyRules& r) { r.tax.levels[3].mood_pressure[3] = fixed(14.5); }},
        {"building cost",
         "buildings.pd",
         "cost_kop = 400000",
         "cost_kop = 500000",
         [](EconomyRules& r) { r.buildings.defs[0].cost_kop = 500000; }},
        {"building output",
         "buildings.pd",
         "output_permille = { grain = 1150 }",
         "output_permille = { grain = 1200, timber = 900 }",
         [](EconomyRules& r) {
             r.buildings.defs[0].output_permille[0] = 1200;
             r.buildings.defs[0].output_permille[3] = 900;
         }},
    };
}

}  // namespace

TEST_CASE("economy content: editing one value in data changes exactly that rule") {
    for (const Tweak& tweak : tweaks()) {
        CAPTURE(tweak.name);
        Files files = shipped_files();
        edit(files, tweak.file, tweak.from, tweak.to);
        EconomyRules expected = EconomyRules::defaults();
        tweak.apply(expected);
        CHECK(rules_of(files) == expected);
    }
}

TEST_CASE("economy content: a file edited on disk is picked up without recompiling") {
    TempDir dir("edit");
    for (const auto& [path, text] : shipped_files()) {
        dir.write(path, text);
    }
    const DirectorySource source(dir.path());
    auto first = content::load_economy_content(source);
    REQUIRE(first.has_value());
    CHECK(first.value().rules == EconomyRules::defaults());

    Files edited = shipped_files();
    edit(edited, "rules.pd", "losses_permille = 50", "losses_permille = 80");
    dir.write("rules.pd", edited["rules.pd"]);
    auto second = content::load_economy_content(source);
    REQUIRE(second.has_value());
    CHECK(second.value().rules.production.losses_permille == 80);
    CHECK(second.value().content_hash != first.value().content_hash);
}

// ---- a second root (a mod) replaces objects whole ---------------------------------------------------

TEST_CASE("economy content: a mod root replaces only the objects it defines") {
    const Files base_files = shipped_files();
    const MemorySource base = to_source(base_files, "base");

    SUBCASE("a tax level") {
        MemorySource mod("mod");
        mod.add("tax.pd",
                "format_version = 1\n"
                "tax_level \"high\" {\n    index = 2\n    tax_permille = 1400\n"
                "    mood_pressure = { townsfolk = 6, peasants = 7, cossacks = 8 }\n}\n");
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_economy_content(roots);
        INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
        REQUIRE(result.has_value());
        EconomyRules expected = EconomyRules::defaults();
        expected.tax.levels[2].tax_permille = 1400;
        CHECK(result.value().rules == expected);

        const auto plain = content::load_economy_content(base);
        REQUIRE(plain.has_value());
        CHECK(result.value().content_hash != plain.value().content_hash);
    }

    SUBCASE("the whole rules object, taking over the open owner questions") {
        // Rules are replaced as a whole: the mod repeats the object with its own parameters.
        Files edited = base_files;
        edit(edited, "rules.pd", "crown_account_mode = separate", "crown_account_mode = emperor_dynasty");
        edit(edited, "rules.pd", "build_foreign_allowed = no", "build_foreign_allowed = yes");
        MemorySource mod("mod");
        mod.add("rules.pd", edited["rules.pd"]);
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_economy_content(roots);
        INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
        REQUIRE(result.has_value());
        EconomyRules expected = EconomyRules::defaults();
        expected.crown_account_mode = CrownAccountMode::EmperorDynasty;
        expected.build_foreign_allowed = true;
        CHECK(result.value().rules == expected);
    }

    SUBCASE("a new building and a mod error point into the mod file") {
        MemorySource mod("mods/windmills");
        mod.add("b.pd",
                "format_version = 1\n"
                "building \"windmill\" {\n    slot = 2\n    name_key = \"building.windmill\"\n"
                "    cost_kop = 600000\n    build_days = 150\n    upkeep_kop_month = 4000\n"
                "    output_permille = { grain = 1300 }\n}\n");
        const ContentSource* roots[] = {&base, &mod};
        const auto result = content::load_economy_content(roots);
        INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
        REQUIRE(result.has_value());
        const EconomyRules& rules = result.value().rules;
        REQUIRE(rules.buildings.count == 2);
        CHECK(rules.buildings.defs[0].slot == 1);  // ordered by slot
        CHECK(rules.buildings.defs[1].slot == 2);
        CHECK(rules.buildings.defs[1].key == "building.windmill");
        CHECK(rules.buildings.defs[1].output_permille[0] == 1300);

        MemorySource bad("mods/bad");
        bad.add("b.pd",
                "format_version = 1\n"
                "building \"windmill\" {\n    slot = 1\n    name_key = \"building.windmill\"\n"
                "    cost_kop = 600000\n    build_days = 150\n    upkeep_kop_month = 4000\n}\n");
        const ContentSource* bad_roots[] = {&base, &bad};
        const auto clash = content::load_economy_content(bad_roots);
        const LoadError* error = first_error(clash);
        CHECK(error->root == 1);
        CHECK(error->root_name == "mods/bad");
        CHECK(error->file == "b.pd");
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.pos.line == 3);
    }
}

// ---- bad data is an error with a position, never a crash ---------------------------------------------

TEST_CASE("economy content: a value outside its corridor is PD104 at the value") {
    Files files = shipped_files();
    edit(files, "rules.pd", "refund_permille = 800", "refund_permille = 1500");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->file == "rules.pd");
    CHECK(error->diagnostic.code == data::ErrorCode::ValueOutOfRange);
    data::SourcePos expected = locate(files["rules.pd"], "refund_permille = 1500");
    expected.column += static_cast<std::uint32_t>(std::string("refund_permille = ").size());
    CHECK(error->diagnostic.pos == expected);
    CHECK(error->format().find("rules.pd:" + std::to_string(expected.line) + ":" +
                               std::to_string(expected.column)) != std::string::npos);
}

TEST_CASE("economy content: the schema corridors agree with validate() at their edges") {
    // The schemas repeat the corridors of validate() (the data module cannot include sim); the
    // edges are pinned here with the constants of the sim.
    struct Edge {
        const char* file;
        const char* from;
        const char* key;
        std::int64_t max;
    };
    const Edge edges[] = {
        {"goods.pd", "base_price_kop = 1200", "base_price_kop", kMaxPostingKop},
        {"rules.pd", "base_limit_kop = 1000000", "base_limit_kop", kMaxTreasuryKop},
        {"rules.pd", "per_honor_kop = 10000", "per_honor_kop", kMaxPostingKop},
        {"rules.pd", "labour_max_permille = 1500", "labour_max_permille", kMaxMultiplierPermille},
        {"rules.pd", "rate_ppm_year = 100000", "rate_ppm_year", kMaxRatePpm},
        {"rules.pd", "refund_permille = 800", "refund_permille", kPermilleOne},
    };
    for (const Edge& edge : edges) {
        CAPTURE(edge.key);
        Files at_max = shipped_files();
        edit(at_max, edge.file, edge.from, std::string(edge.key) + " = " + std::to_string(edge.max));
        CHECK(load(at_max).has_value());

        Files above = shipped_files();
        edit(above, edge.file, edge.from, std::string(edge.key) + " = " + std::to_string(edge.max + 1));
        const auto result = load(above);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::ValueOutOfRange);
    }
}

TEST_CASE("economy content: a dangling reference is PD111 at the reference") {
    Files files = shipped_files();
    edit(files, "rules.pd", "default_tax_level = tax_level:normal", "default_tax_level = tax_level:nomral");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::UnresolvedReference);
    CHECK(error->file == "rules.pd");
    CHECK(error->diagnostic.message.find("normal") != std::string::npos);  // did you mean
    CHECK(error->diagnostic.pos.line == locate(files["rules.pd"], "tax_level:nomral").line);
}

TEST_CASE("economy content: a repeated slot is an error at the second slot") {
    SUBCASE("goods") {
        Files files = shipped_files();
        edit(files, "goods.pd", "good \"fur\" {\n    slot = 2", "good \"fur\" {\n    slot = 1");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->file == "goods.pd");
        CHECK(error->diagnostic.message.find("already used by good") != std::string::npos);
        // Objects are visited by id: "fur" comes before "salt", so the later one (salt) is rejected.
        CHECK(error->diagnostic.pos.line ==
              locate(files["goods.pd"], "good \"salt\" {\n    slot = 1").line + 1);
    }
    SUBCASE("buildings") {
        Files files = shipped_files();
        files["buildings.pd"] +=
            "building \"mill2\" {\n    slot = 1\n    name_key = \"building.mill2\"\n    cost_kop = 1\n"
            "    build_days = 1\n    upkeep_kop_month = 0\n}\n";
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("slot 1 is already used by building") != std::string::npos);
    }
    SUBCASE("policy levels") {
        Files files = shipped_files();
        edit(
            files, "policies.pd", "index = 2\n    tax_permille = 1300", "index = 3\n    tax_permille = 1300");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("index 3 is already used") != std::string::npos);
    }
}

TEST_CASE("economy content: policy level indices must have no gaps") {
    Files files = shipped_files();
    edit(files, "policies.pd", "index = 3\n    tax_permille = 1600", "index = 4\n    tax_permille = 1600");
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->diagnostic.message.find("index 3 is missing") != std::string::npos);
}

TEST_CASE("economy content: unknown names of groups, kinds and goods are errors at the name") {
    SUBCASE("a group in a need map") {
        Files files = shipped_files();
        edit(files, "goods.pd", "peasants = 35", "peasents = 35");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("did you mean 'peasants'") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files["goods.pd"], "peasents"));
    }
    SUBCASE("a group object") {
        Files files = shipped_files();
        edit(files, "groups.pd", "population_group \"nobility\"", "population_group \"nobles\"");
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        const std::string text = content::format_errors(result.error());
        CHECK(text.find("unknown population group 'nobles'") != std::string::npos);
        CHECK(text.find("population_group 'nobility' is not defined") != std::string::npos);
    }
    SUBCASE("a province kind") {
        Files files = shipped_files();
        edit(files, "groups.pd", "province_kind \"port\"", "province_kind \"harbour\"");
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        const std::string text = content::format_errors(result.error());
        CHECK(text.find("unknown province kind 'harbour'") != std::string::npos);
        CHECK(text.find("province_kind 'port' is not defined") != std::string::npos);
    }
    SUBCASE("a good in a building") {
        Files files = shipped_files();
        edit(files, "buildings.pd", "grain = 1150", "gran = 1150");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->file == "buildings.pd");
        CHECK(error->diagnostic.message.find("did you mean 'grain'") != std::string::npos);
        CHECK(error->diagnostic.pos == locate(files["buildings.pd"], "gran"));
    }
}

TEST_CASE("economy content: rules that span fields are checked with the block's position") {
    Files files = shipped_files();
    edit(files, "rules.pd", "riot_from = 50", "riot_from = 20");  // below calm_below = 25
    const auto result = load(files);
    const LoadError* error = first_error(result);
    CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
    CHECK(error->file == "rules.pd");
    CHECK(error->diagnostic.message.find("inconsistent") != std::string::npos);
    CHECK(error->diagnostic.pos == locate(files["rules.pd"], "unrest {"));
}

TEST_CASE("economy content: missing or repeated rules objects and too many buildings") {
    SUBCASE("no economy_rules") {
        Files files = shipped_files();
        files.erase("rules.pd");
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        CHECK(content::format_errors(result.error()).find("no economy_rules object") != std::string::npos);
    }
    SUBCASE("two economy_rules") {
        Files files = shipped_files();
        files["rules2.pd"] = files["rules.pd"];
        files["rules2.pd"].replace(files["rules2.pd"].find("\"main\""), 6, "\"other\"");
        const auto result = load(files);
        const LoadError* error = first_error(result);
        CHECK(error->diagnostic.code == data::ErrorCode::ContentRule);
        CHECK(error->diagnostic.message.find("only one economy_rules") != std::string::npos);
    }
    SUBCASE("no tax levels") {
        Files files = shipped_files();
        files.erase("policies.pd");
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(content::format_errors(result.error()).empty());
    }
    SUBCASE("more than 16 buildings") {
        Files files = shipped_files();
        std::string text = "format_version = 1\n";
        for (int i = 2; i <= 17 + 1; ++i) {
            text +=
                "building \"b" + std::to_string(i) + "\" {\n    slot = " + std::to_string(i) +
                "\n    name_key = \"b\"\n    cost_kop = 1\n    build_days = 1\n    upkeep_kop_month = 0\n}\n";
        }
        files["more.pd"] = text;
        const auto result = load(files);
        REQUIRE_FALSE(result.has_value());
        CHECK(content::format_errors(result.error()).find("at most 16 buildings") != std::string::npos);
    }
}

TEST_CASE("economy content: broken text never crashes") {
    const Files files = shipped_files();

    SUBCASE("garbage") {
        Files bad = files;
        bad["rules.pd"] = "format_version = 1\neconomy_rules \"main\" { ??? }}}";
        const auto result = load(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(content::format_errors(result.error()).empty());
    }
    SUBCASE("a missing root directory") {
        const DirectorySource nowhere(std::string(PATRIARCH_DATA_DIR) + "/no_such_directory");
        const auto result = content::load_economy_content(nowhere);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().front().diagnostic.code == data::ErrorCode::UnreadableSource);
    }
    SUBCASE("every file cut at many places") {
        for (const auto& [path, text] : files) {
            for (std::size_t cut = 0; cut < text.size(); cut += 83) {
                Files bad = files;
                bad[path] = text.substr(0, cut);
                const auto result = load(bad);  // either rules or errors, but no crash
                if (!result.has_value()) {
                    CHECK_FALSE(result.error().empty());
                }
            }
        }
    }
    SUBCASE("every number replaced by an absurd one") {
        const std::string rules_text = files.at("rules.pd");
        for (std::size_t at = 0; at < rules_text.size(); ++at) {
            if (rules_text[at] < '0' || rules_text[at] > '9' ||
                (at > 0 && rules_text[at - 1] >= '0' && rules_text[at - 1] <= '9')) {
                continue;
            }
            std::size_t end = at;
            while (end < rules_text.size() && rules_text[end] >= '0' && rules_text[end] <= '9') {
                ++end;
            }
            for (const char* absurd : {"0", "-1", "99999999999999999999", "2147483648"}) {
                Files bad = files;
                bad["rules.pd"] = rules_text.substr(0, at) + absurd + rules_text.substr(end);
                const auto result = load(bad);
                if (result.has_value()) {
                    CHECK(validate(result.value().rules).has_value());
                } else {
                    CHECK_FALSE(result.error().empty());
                }
            }
        }
    }
}

TEST_CASE("economy content: the economy kinds are registered and documented in the schema set") {
    const data::SchemaSet schemas = data::content_schemas();
    for (const char* kind : {"economy_rules",
                             "good",
                             "population_group",
                             "province_kind",
                             "tax_level",
                             "recruit_level",
                             "building"}) {
        CAPTURE(kind);
        const data::ObjectSchema* schema = schemas.find(kind);
        REQUIRE(schema != nullptr);
        CHECK_FALSE(schema->description().empty());
        for (const data::FieldSpec& field : schema->fields()) {
            CAPTURE(field.name);
            CHECK_FALSE(field.description.empty());
        }
    }
}
