#include "pat/data/content_source.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lint.hpp"

// contentlint (E4-T08, docs/dev/contentlint.md): the library API with in-memory corpora, the command
// line front end on the corpora in tests/data/contentlint and on the real data/, garbage input.

namespace {

using pat::contentlint::Finding;
using pat::contentlint::Format;
using pat::contentlint::Options;
using pat::contentlint::Report;
using pat::contentlint::Severity;
using pat::data::MemorySource;

struct CliRun {
    int code = -1;
    std::string out;
    std::string err;
};

CliRun run(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::ostringstream out;
    std::ostringstream err;
    CliRun result;
    result.code = pat::contentlint::run_cli(views, out, err);
    result.out = out.str();
    result.err = err.str();
    return result;
}

std::string corpus(const std::string& name) {
    return std::string(PATRIARCH_TEST_DATA_DIR) + "/contentlint/" + name;
}

bool has_code(const Report& report, std::string_view code) {
    return std::ranges::any_of(report.findings, [&](const Finding& f) { return f.code == code; });
}

std::size_t count_code(const Report& report, std::string_view code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(report.findings, [&](const Finding& f) { return f.code == code; }));
}

Report lint_memory(const MemorySource& root, const Options& options, const MemorySource* scan = nullptr) {
    const pat::data::ContentSource* roots[] = {&root};
    if (scan != nullptr) {
        const pat::data::ContentSource* scans[] = {scan};
        return pat::contentlint::lint(roots, scans, options);
    }
    return pat::contentlint::lint(roots, {}, options);
}

/// A string literal with embedded NUL bytes, taken whole.
template <std::size_t N>
std::string raw(const char (&text)[N]) {
    return std::string(text, N - 1);
}

Options plain_options() {
    Options options;
    options.run_rules = false;
    options.check_unused = false;
    return options;
}

}  // namespace

TEST_CASE("contentlint: the good corpus is clean even with --strict") {
    const CliRun r = run({"--root", corpus("good"), "--no-rules", "--strict"});
    CHECK(r.code == 0);
    CHECK(r.out.find("0 error(s), 0 warning(s)") != std::string::npos);
    CHECK(r.out.find("coverage: en") != std::string::npos);
    CHECK(r.err.empty());
}

TEST_CASE("contentlint: the real data/ is clean with --strict") {
    // ui.* keys are not referenced by the UI yet (docs/dev/contentlint.md); everything else is checked,
    // including cross-object rules for the economy, demography and M1 definitions.
    const CliRun r = run({"--root", PATRIARCH_DATA_DIR, "--strict", "--ignore-unused", "ui."});
    INFO(r.out);
    CHECK(r.code == 0);
}

TEST_CASE("contentlint: the real data/ reports unreferenced ui keys as warnings, errors under --strict") {
    const CliRun lenient = run({"--root", PATRIARCH_DATA_DIR});
    CHECK(lenient.code == 0);
    CHECK(lenient.out.find("warning[CL004]") != std::string::npos);
    const CliRun strict = run({"--root", PATRIARCH_DATA_DIR, "--strict"});
    CHECK(strict.code == 1);
    CHECK(strict.out.find("error[CL004]") != std::string::npos);
}

TEST_CASE("contentlint: broken schema file reports every problem with position") {
    const CliRun r = run({"--root", corpus("bad/schema"), "--no-rules"});
    CHECK(r.code == 1);
    INFO(r.out);
    CHECK(r.out.find("traits.pd:11:5: error[PD101]") != std::string::npos);  // skils
    CHECK(r.out.find("did you mean 'skills'") != std::string::npos);
    CHECK(r.out.find("traits.pd:9:12: error[PD104]") != std::string::npos);   // slot = 0
    CHECK(r.out.find("traits.pd:15:16: error[PD103]") != std::string::npos);  // name_key = 5
    CHECK(r.out.find("traits.pd:21:1: error[PD102]") != std::string::npos);   // missing name_key
    CHECK(r.out.find("traits.pd:27:11: error[PD014]") != std::string::npos);  // unterminated object
    CHECK(r.out.find("/bad/schema/traits.pd:") != std::string::npos);
}

TEST_CASE("contentlint: unresolved reference is PD111 with a hint") {
    const CliRun r = run({"--root", corpus("bad/reference"), "--no-rules"});
    CHECK(r.code == 1);
    CHECK(r.out.find("traits.pd:17:16: error[PD111]") != std::string::npos);
    CHECK(r.out.find("did you mean 'honest'") != std::string::npos);
}

TEST_CASE("contentlint: schema-valid trait errors are reported by the rule validator") {
    const CliRun r = run({"--root", corpus("bad/rules")});
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(r.out.find("traits.pd:8:5: error[PD115]: a trait cannot be its own opposite") != std::string::npos);
}

TEST_CASE("contentlint: duplicate localisation key is LC003 at the second definition") {
    const CliRun r = run({"--root", corpus("bad/loc_duplicate"), "--no-rules"});
    CHECK(r.code == 1);
    CHECK(r.out.find("loc/ru/ui.pd:5:1: error[LC003]") != std::string::npos);
}

TEST_CASE("contentlint: bad plural forms and message syntax are reported") {
    const CliRun r = run({"--root", corpus("bad/loc_plural"), "--no-rules"});
    CHECK(r.code == 1);
    INFO(r.out);
    CHECK(r.out.find("error[LC015]") != std::string::npos);  // two forms for Russian
    CHECK(r.out.find("error[LC014]") != std::string::npos);  // missing ')'
    CHECK(r.out.find("error[LC010]") != std::string::npos);  // stray '}'
}

TEST_CASE("contentlint: --format github writes workflow annotations") {
    const CliRun r = run({"--root", corpus("bad/reference"), "--no-rules", "--format", "github"});
    CHECK(r.code == 1);
    CHECK(r.out.find("::error file=") != std::string::npos);
    CHECK(r.out.find(",line=17,col=16,title=PD111::") != std::string::npos);
    CHECK(r.out.find("contentlint: ") != std::string::npos);

    Finding f;
    f.severity = Severity::Warning;
    f.file = "data/a,b:c.pd";
    f.line = 3;
    f.column = 4;
    f.code = "CL001";
    f.message = "100% bad\nsecond line";
    CHECK(f.format_github() ==
          "::warning file=data/a%2Cb%3Ac.pd,line=3,col=4,title=CL001::100%25 bad%0Asecond line");
    CHECK(f.format_text() == "data/a,b:c.pd:3:4: warning[CL001]: 100% bad\nsecond line");
    f.file.clear();
    f.line = 0;
    CHECK(f.format_text() == "warning[CL001]: 100% bad\nsecond line");
    CHECK(f.format_github().starts_with("::warning title=CL001::"));
}

TEST_CASE("contentlint: output is deterministic and sorted") {
    const CliRun a = run({"--root", corpus("bad/schema"), "--no-rules"});
    const CliRun b = run({"--root", corpus("bad/schema"), "--no-rules"});
    CHECK(a.out == b.out);

    MemorySource root("m");
    root.add("z.pd", R"(format_version = 1
trait "z" {
    name_key = 1
    desc_key = "trait.z.desc"
    slot = 1
    category = personality
}
)");
    root.add("a.pd", R"(format_version = 1
trait "a" {
    name_key = 1
    desc_key = "trait.a.desc"
    slot = 2
    category = personality
}
)");
    const Report report = lint_memory(root, plain_options());
    REQUIRE(report.findings.size() == 2);
    CHECK(report.findings[0].file == "m/a.pd");
    CHECK(report.findings[1].file == "m/z.pd");
}

TEST_CASE("contentlint: usage and I/O problems exit with 2") {
    CHECK(run({"--bogus"}).code == 2);
    CHECK(run({"stray"}).code == 2);
    CHECK(run({"--root"}).code == 2);
    CHECK(run({"--format", "xml"}).code == 2);
    CHECK(run({"--lang", "Ru"}).code == 2);
    CHECK(run({"--fallback", "en,"}).code == 2);
    CHECK(run({"--strict=yes"}).code == 2);
    CHECK(run({"--root", corpus("does/not/exist")}).code == 2);
    CHECK(run({"--root", corpus("good"), "--used-keys", corpus("no-such-list.txt")}).code == 2);
    CHECK(run({"--root", corpus("good"), "--scan", corpus("no-such-dir")}).code == 2);
    const CliRun bad = run({"--bogus"});
    CHECK(bad.err.find("unknown option '--bogus'") != std::string::npos);
    CHECK(bad.err.find("usage: contentlint") != std::string::npos);

    const CliRun help = run({"--help"});
    CHECK(help.code == 0);
    CHECK(help.out.find("exit codes") != std::string::npos);
}

TEST_CASE("contentlint: option values may be written with '='") {
    const CliRun r =
        run({"--root=" + corpus("good"), "--no-rules", "--strict", "--format=text", "--fallback=en,ru"});
    CHECK(r.code == 0);
}

TEST_CASE("contentlint: garbage input is reported, never a crash") {
    MemorySource root("garbage");
    root.add("empty.pd", "");
    root.add("bom_only.pd", "\xEF\xBB\xBF");
    root.add("truncated.pd", "format_version = 1\ntrait \"a\" {\n    name_key = \"tra");
    root.add("nul.pd", raw("format_version = 1\ntrait \"a\" { name_key = \"x\0y\" slot = 1 }\n"));
    root.add("bytes.pd", raw("\xFF\xFE\x00\x01\x80\x80 trait { = = }}} \xC3"));
    root.add("deep.pd",
             "format_version = 1\ntrait \"a\" " + std::string(20000, '{') + std::string(20000, '}'));
    root.add(
        "big_number.pd",
        "format_version = 1\ntrait \"a\" { name_key = \"k\" slot = 99999999999999999999999999999999 }\n");
    root.add("long_line.pd",
             "format_version = 1\n# " + std::string(1 << 20, 'x') + "\ntrait \"q\" { slot = }\n");
    root.add("future.pd", "format_version = 99\n");
    root.add("loc/ru/empty.pd", "");
    root.add("loc/ru/junk.pd", raw("format_version = 1\nui.x = \"{\xFF\x00}\"\n\x01\x02 = = =\n"));
    root.add("loc/ru/deep.pd", "format_version = 1\nui.y = \"" + std::string(100000, '{') + "\"\n");
    root.add("loc/xx/unknown.pd", "format_version = 1\nui.z = \"x\"\n");
    root.add("loc/stray.pd", "format_version = 1\n");
    root.add_unreadable("locked.pd");

    Options options;  // defaults: rules and unused-key check on
    options.strict = true;
    const Report report = lint_memory(root, options);
    CHECK(report.exit_code() == 1);
    CHECK(report.errors > 5);
    CHECK(has_code(report, "PD113"));  // unreadable file
    CHECK(has_code(report, "PD021"));  // future format version
    CHECK(has_code(report, "LC001"));
    CHECK(has_code(report, "LC002"));
    CHECK(std::ranges::any_of(
        report.notes, [](const std::string& n) { return n.find("not checked") != std::string::npos; }));
    std::ostringstream text;
    pat::contentlint::render(report, Format::Text, text);
    CHECK(!text.str().empty());
}

TEST_CASE("contentlint: coverage, extra keys, parameter mismatch and missing tables") {
    MemorySource root("m");
    root.add("loc/ru/ui.pd",
             "format_version = 1\n"
             "ui.a = \"один\"\n"
             "ui.b = \"{n} {n:plural(год|года|лет)}\"\n"
             "ui.c = \"три\"\n");
    root.add("loc/en/ui.pd",
             "format_version = 1\n"
             "ui.a = \"one\"\n"
             "ui.b = \"{m} {m:plural(year|years)}\"\n"
             "ui.extra = \"extra\"\n");

    Options options = plain_options();
    options.fallback = {"en", "de"};
    Report report = lint_memory(root, options);
    CHECK(report.exit_code() == 0);  // only warnings
    CHECK(report.errors == 0);
    CHECK(count_code(report, "CL001") == 1);  // ui.c missing in en
    CHECK(count_code(report, "CL002") == 1);  // ui.extra
    CHECK(count_code(report, "CL003") == 1);  // ui.b parameters n vs m
    CHECK(count_code(report, "CL006") == 1);  // no 'de' tables
    REQUIRE(report.coverage.size() == 3);     // de, en, ru
    CHECK(report.coverage[0].language == "de");
    CHECK(!report.coverage[0].has_table);
    CHECK(report.coverage[1].language == "en");
    CHECK(report.coverage[1].missing == 1);
    CHECK(report.coverage[1].extra == 1);

    const auto missing =
        std::ranges::find_if(report.findings, [](const Finding& f) { return f.code == "CL001"; });
    REQUIRE(missing != report.findings.end());
    CHECK(missing->file == "m/loc/ru/ui.pd");  // located at the default-language definition
    CHECK(missing->line == 4);
    CHECK(missing->severity == Severity::Warning);

    options.strict = true;
    report = lint_memory(root, options);
    CHECK(report.exit_code() == 1);
    CHECK(report.warnings == 0);
    CHECK(report.errors == 4);
}

TEST_CASE("contentlint: a mod root without loc tables and without --fallback is silent") {
    MemorySource root("mod");
    root.add("traits.pd", R"(format_version = 1
trait "a" {
    name_key = "trait.a.name"
    desc_key = "trait.a.desc"
    slot = 1
    category = personality
}
)");
    const Report report = lint_memory(root, plain_options());
    CHECK(report.findings.empty());
    CHECK(report.exit_code() == 0);
}

TEST_CASE("contentlint: unused keys, prefixes, ignore list and explicit used keys") {
    MemorySource root("m");
    root.add("loc/ru/ui.pd",
             "format_version = 1\n"
             "ui.used = \"a\"\n"
             "ui.unused = \"b\"\n"
             "ui.month.may = \"c\"\n"
             "ui.month.june = \"d\"\n"
             "ui.listed = \"e\"\n"
             "dbg.hidden = \"f\"\n");
    root.add("traits.pd", "format_version = 1\n# a comment mentioning dbg.hidden.thing\n");
    MemorySource code("code");
    code.add("a.cpp", "auto s = loc.format(\"ui.used\"); auto m = std::string(\"ui.month.\") + name;\n");
    code.add("b.txt", "ui.unused is only in a file with an ignored extension\n");

    Options options = plain_options();
    options.check_unused = true;
    options.ignore_unused_prefixes = {"dbg."};
    options.used_keys.push_back({"ui.listed", "keys.txt", 1});
    options.used_keys.push_back({"ui.gone", "keys.txt", 2});
    const Report report = lint_memory(root, options, &code);

    CHECK(count_code(report, "CL004") == 1);
    const auto unused =
        std::ranges::find_if(report.findings, [](const Finding& f) { return f.code == "CL004"; });
    REQUIRE(unused != report.findings.end());
    CHECK(unused->message.find("ui.unused") != std::string::npos);
    CHECK(unused->file == "m/loc/ru/ui.pd");
    CHECK(unused->line == 3);

    CHECK(count_code(report, "CL005") == 1);
    const auto absent =
        std::ranges::find_if(report.findings, [](const Finding& f) { return f.code == "CL005"; });
    REQUIRE(absent != report.findings.end());
    CHECK(absent->severity == Severity::Error);
    CHECK(absent->file == "keys.txt");
    CHECK(absent->line == 2);
    CHECK(report.exit_code() == 1);
}

TEST_CASE("contentlint: --used-keys and --scan on the command line") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "patriarch_contentlint_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src", ec);
    {
        std::ofstream list(dir / "keys.txt", std::ios::binary);
        list << "# keys used by code\n\n  trait.greedy.desc  \r\ntrait.greedy.name\n";
    }
    {
        std::ofstream code(dir / "src" / "x.cpp", std::ios::binary);
        code << "const char* k = \"trait.generous.name\";\n";
    }
    // The good corpus references its keys from data; with --no-scan and an empty reference set the
    // roots are still scanned, so everything stays used.
    CHECK(
        run({"--root", corpus("good"), "--no-rules", "--strict", "--used-keys", (dir / "keys.txt").string()})
            .code == 0);
    CHECK(run({"--root",
               corpus("good"),
               "--no-rules",
               "--strict",
               "--no-scan",
               "--scan",
               (dir / "src").string()})
              .code == 0);

    {
        std::ofstream list(dir / "keys.txt", std::ios::binary | std::ios::trunc);
        list << "ui.never.defined\n";
    }
    const CliRun r =
        run({"--root", corpus("good"), "--no-rules", "--used-keys", (dir / "keys.txt").string()});
    CHECK(r.code == 1);
    CHECK(r.out.find("keys.txt:1:1: error[CL005]") != std::string::npos);
    fs::remove_all(dir, ec);
}

TEST_CASE("contentlint: scan_text_for_keys finds exact keys and run-time prefixes only") {
    const std::vector<std::string> known = {"ui.a.b", "ui.month.may", "ui.x"};
    pat::contentlint::KeyScan scan;
    pat::contentlint::scan_text_for_keys(
        "f(\"ui.a.b\"); g(ui.x); h(\"ui.month.\" + m); \"ui.\"; src/ui.hpp; x.ui.a.b.c; ui.x.; ...ui.x",
        known,
        scan);
    CHECK(scan.keys == std::vector<std::string>{"ui.a.b", "ui.x"});
    CHECK(scan.prefixes == std::vector<std::string>{"ui.month.", "ui.x."});

    pat::contentlint::KeyScan empty;
    pat::contentlint::scan_text_for_keys("", known, empty);
    pat::contentlint::scan_text_for_keys(std::string(5000, '.'), known, empty);
    pat::contentlint::scan_text_for_keys(std::string("\0\xFF.\0", 4), known, empty);
    CHECK(empty.keys.empty());
}

TEST_CASE("contentlint: valid files without the economy and demography objects fail the rule builders") {
    MemorySource root("m");
    root.add("traits.pd", R"(format_version = 1
trait "a" {
    name_key = "trait.a.name"
    desc_key = "trait.a.desc"
    slot = 1
    category = personality
}
)");
    Options options;
    options.check_unused = false;
    const Report report = lint_memory(root, options);
    CHECK(has_code(report, "PD115"));
    CHECK(report.exit_code() == 1);
    options.run_rules = false;
    CHECK(lint_memory(root, options).exit_code() == 0);
}

TEST_CASE("contentlint: a load error skips the rule builders with a note") {
    MemorySource root("m");
    root.add("x.pd", "format_version = 1\nnonsense \"a\" { }\n");
    Options options;
    options.check_unused = false;
    const Report report = lint_memory(root, options);
    CHECK(has_code(report, "PD108"));
    CHECK(!has_code(report, "PD115"));
    CHECK(std::ranges::any_of(
        report.notes, [](const std::string& n) { return n.find("not checked") != std::string::npos; }));
}
