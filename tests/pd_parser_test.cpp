#include "pat/core/rng.hpp"
#include "pat/data/pd_parser.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pat::data;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path corpus_dir() {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "pd";
}

std::vector<std::filesystem::path> files_with_extension(const std::filesystem::path& dir,
                                                        const std::string& ext) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ext)
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string with_header(const std::string& body) {
    return "format_version = 1\n" + body;
}

std::vector<std::string> error_lines(const ParseResult& r) {
    std::vector<std::string> lines;
    for (const Diagnostic& d : r.errors) {
        lines.push_back(std::format("{}:{} {}", d.pos.line, d.pos.column, error_code_name(d.code)));
    }
    return lines;
}

}  // namespace

TEST_CASE("pd corpus: every valid file parses to the expected canonical tree") {
    const auto files = files_with_extension(corpus_dir() / "valid", ".pd");
    REQUIRE(files.size() >= 6);
    for (const auto& path : files) {
        CAPTURE(path.filename().string());
        const std::string source = read_file(path);
        const ParseResult r = parse_pd(source);
        for (const Diagnostic& d : r.errors)
            MESSAGE(format_diagnostic(path.filename().string(), d));
        CHECK(r.ok());
        CHECK(r.file.format_version == 1);
        auto tree_path = path;
        tree_path.replace_extension(".tree");
        CHECK(dump_tree(r.file) == read_file(tree_path));
    }
}

TEST_CASE("pd corpus: every invalid file yields exactly the expected errors") {
    const auto files = files_with_extension(corpus_dir() / "invalid", ".pd");
    REQUIRE(files.size() >= 20);
    for (const auto& path : files) {
        CAPTURE(path.filename().string());
        const ParseResult r = parse_pd(read_file(path));
        auto expect_path = path;
        expect_path.replace_extension(".expect");
        std::vector<std::string> expected;
        std::istringstream in(read_file(expect_path));
        for (std::string line; std::getline(in, line);) {
            if (!line.empty())
                expected.push_back(line);
        }
        CHECK(error_lines(r) == expected);
    }
}

TEST_CASE("pd parser: scalar values keep exact numbers") {
    const ParseResult r =
        parse_pd(with_header("a = 1.5\nb = -0.025\nc = 123456789012345678\nd = 0.000000001\n"));
    REQUIRE(r.ok());
    const Value& a = r.file.entries[1].value;
    CHECK(a.kind == ValueKind::Decimal);
    CHECK(a.integer == 15);
    CHECK(a.scale == 1);
    const Value& b = r.file.entries[2].value;
    CHECK(b.integer == -25);
    CHECK(b.scale == 3);
    CHECK(r.file.entries[3].value.integer == 123456789012345678);
    CHECK(r.file.entries[4].value.integer == 1);
    CHECK(r.file.entries[4].value.scale == 9);
}

TEST_CASE("pd parser: dates, refs and keywords") {
    const ParseResult r = parse_pd(with_header("d = 1703.05.27\nr = scope:governor\ny = yes\nn = no\n"));
    REQUIRE(r.ok());
    const Value& d = r.file.entries[1].value;
    CHECK(d.kind == ValueKind::Date);
    CHECK(d.year == 1703);
    CHECK(d.month == 5);
    CHECK(d.day == 27);
    const Value& ref = r.file.entries[2].value;
    CHECK(ref.kind == ValueKind::Ref);
    CHECK(ref_prefix(ref) == "scope");
    CHECK(ref_name(ref) == "governor");
    CHECK(r.file.entries[3].value.integer == 1);
    CHECK(r.file.entries[4].value.integer == 0);
}

TEST_CASE("pd parser: positions count code points and tabs as one column") {
    const ParseResult r = parse_pd("format_version = 1\n\t\"Ж\" = 1\n");  // string key is not allowed
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].pos.line == 2);
    CHECK(r.errors[0].pos.column == 2);
    CHECK(r.errors[0].code == ErrorCode::ExpectedKey);

    const ParseResult s = parse_pd("format_version = 1\nname = \"Жук\" @\n");
    REQUIRE(s.errors.size() == 1);
    CHECK(s.errors[0].pos.column == 14);  // name = "Жук" @  -> '@' is the 14th code point
}

TEST_CASE("pd parser: BOM, CRLF and entry positions") {
    const std::string source = "\xEF\xBB\xBF"
                               "format_version = 1\r\nkey = \"\"\"\r\nline1\r\nline2\"\"\"\r\nother = 2\r\n";
    const ParseResult r = parse_pd(source);
    REQUIRE(r.ok());
    REQUIRE(r.file.entries.size() == 3);
    CHECK(r.file.entries[0].pos.line == 1);
    CHECK(r.file.entries[0].pos.column == 1);
    CHECK(r.file.entries[1].value.text == "line1\nline2");
    CHECK(r.file.entries[2].pos.line == 5);
}

TEST_CASE("pd parser: escapes") {
    const ParseResult r = parse_pd(with_header("s = \"\\u{1F600}\\u{44F}\"\n"));
    REQUIRE(r.ok());
    CHECK(r.file.entries[1].value.text == "\xF0\x9F\x98\x80\xD1\x8F");
    for (const char* bad : {"s = \"\\u{}\"\n",
                            "s = \"\\u{110000}\"\n",
                            "s = \"\\u{D800}\"\n",
                            "s = \"\\u41\"\n",
                            "s = \"\\u{0}\"\n",
                            "s = \"\\u{1234567}\"\n"}) {
        CAPTURE(bad);
        const ParseResult b = parse_pd(with_header(bad));
        REQUIRE(b.errors.size() == 1);
        CHECK(b.errors[0].code == ErrorCode::BadEscape);
    }
}

TEST_CASE("pd parser: labels are normalised and key { } equals key = { }") {
    const ParseResult a = parse_pd(with_header("t \"x\" { v = 1 }\n"));
    const ParseResult b = parse_pd(with_header("t x = { v = 1 }\n"));
    REQUIRE(a.ok());
    CHECK(dump_tree(a.file).find("t \"x\" = {") != std::string::npos);
    // `t x = {` is not valid: a label must be followed directly by the block.
    CHECK_FALSE(b.ok());
    const ParseResult c = parse_pd(with_header("t x { v = 1 }\n"));
    REQUIRE(c.ok());
    CHECK(dump_tree(a.file) == dump_tree(c.file));
}

TEST_CASE("pd parser: duplicate keys keep their order") {
    const ParseResult r = parse_pd(with_header("b { n = 1 n = 2 n = 3 }\n"));
    REQUIRE(r.ok());
    const auto& entries = r.file.entries[1].value.entries;
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].value.integer == 1);
    CHECK(entries[2].value.integer == 3);
}

TEST_CASE("pd parser: empty and comment-only files report a missing header") {
    for (const char* text : {"", "# only a comment\n", "   \n\n"}) {
        const ParseResult r = parse_pd(text);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == ErrorCode::MissingVersion);
        CHECK(r.errors[0].pos.line == 1);
        CHECK(r.errors[0].pos.column == 1);
    }
    CHECK(parse_pd("format_version = 0\n").errors.size() == 1);
    CHECK(parse_pd("format_version = \"1\"\n").errors.size() == 1);
    CHECK(parse_pd("format_version > 1\n").errors.size() == 1);
}

TEST_CASE("pd parser: the error count is capped with PD022") {
    std::string source = "format_version = 1\n";
    for (int i = 0; i < 300; ++i)
        source += "a = @\n";
    const ParseResult r = parse_pd(source);
    REQUIRE(r.errors.size() == kMaxErrors + 1);
    CHECK(r.errors.back().code == ErrorCode::TooManyErrors);
    CHECK(r.errors[kMaxErrors - 1].code == ErrorCode::BadChar);
}

TEST_CASE("pd parser: oversize input and limits") {
    const std::string huge(kMaxFileBytes + 1, ' ');
    const ParseResult r = parse_pd(huge);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == ErrorCode::FileTooLarge);

    const std::string long_ident = with_header("k" + std::string(kMaxIdentBytes, 'a') + " = 1\n");
    const ParseResult i = parse_pd(long_ident);
    REQUIRE_FALSE(i.errors.empty());
    CHECK(i.errors[0].code == ErrorCode::OutOfRange);

    const std::string long_string = with_header("s = \"" + std::string(kMaxStringBytes + 1, 'x') + "\"\n");
    const ParseResult s = parse_pd(long_string);
    REQUIRE(s.errors.size() == 1);
    CHECK(s.errors[0].code == ErrorCode::OutOfRange);
}

TEST_CASE("pd parser: nesting of 64 levels is accepted, 65 is a fatal error") {
    std::string ok = "format_version = 1\n";
    for (int i = 0; i < kMaxNesting; ++i)
        ok += "a = { ";
    for (int i = 0; i < kMaxNesting; ++i)
        ok += "} ";
    CHECK(parse_pd(ok).ok());

    std::string deep = "format_version = 1\nk = ";
    for (int i = 0; i < kMaxNesting + 1; ++i)
        deep += "[ ";
    const ParseResult r = parse_pd(deep);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == ErrorCode::TooDeep);
}

TEST_CASE("pd parser: diagnostics format") {
    Diagnostic d;
    d.pos = {14, 5};
    d.code = ErrorCode::BadEscape;
    d.message = "invalid escape sequence";
    CHECK(format_diagnostic("data/a.pd", d) == "data/a.pd:14:5: error[PD003]: invalid escape sequence");
}

TEST_CASE("pd parser: parsing is deterministic") {
    const std::string source = read_file(corpus_dir() / "valid" / "blocks.pd");
    CHECK(dump_tree(parse_pd(source).file) == dump_tree(parse_pd(source).file));
}

TEST_CASE("pd parser: random garbage never crashes and stays inside the file") {
    pat::core::Pcg32 rng(2024, 1);
    const std::vector<std::filesystem::path> seeds = [] {
        std::vector<std::filesystem::path> all = files_with_extension(corpus_dir() / "valid", ".pd");
        const auto bad = files_with_extension(corpus_dir() / "invalid", ".pd");
        all.insert(all.end(), bad.begin(), bad.end());
        return all;
    }();
    const std::string alphabet = "{}[]()=!<>,#\"\\: \t\r\n.-_09azAZ\xD0\xB6\xFF@";

    auto check = [](const std::string& text) {
        const ParseResult r = parse_pd(text);
        std::uint32_t lines = 1;
        for (const char c : text)
            lines += c == '\n' ? 1u : 0u;
        SourcePos last{0, 0};
        for (const Diagnostic& d : r.errors) {
            CHECK(d.pos.line >= 1);
            CHECK(d.pos.column >= 1);
            CHECK(d.pos.line <= lines);
            CHECK(last <= d.pos);
            last = d.pos;
        }
        (void)dump_tree(r.file);
    };

    for (int round = 0; round < 400; ++round) {
        // pure noise
        std::string noise;
        const auto len = rng.bounded(200);
        for (std::uint32_t i = 0; i < len; ++i)
            noise += alphabet[rng.bounded(static_cast<std::uint32_t>(alphabet.size()))];
        check(noise);
        check(with_header(noise));

        // mutated corpus file
        std::string text = read_file(seeds[rng.bounded(static_cast<std::uint32_t>(seeds.size()))]);
        const auto edits = 1 + rng.bounded(4);
        for (std::uint32_t e = 0; e < edits && !text.empty(); ++e) {
            const auto at = rng.bounded(static_cast<std::uint32_t>(text.size()));
            switch (rng.bounded(3)) {
                case 0:
                    text.erase(at, 1 + rng.bounded(3));
                    break;
                case 1:
                    text.insert(at, 1, alphabet[rng.bounded(static_cast<std::uint32_t>(alphabet.size()))]);
                    break;
                default:
                    text[at] = alphabet[rng.bounded(static_cast<std::uint32_t>(alphabet.size()))];
                    break;
            }
        }
        check(text);
    }

    // every prefix of every corpus file
    for (const auto& path : seeds) {
        const std::string text = read_file(path);
        for (std::size_t n = 0; n <= text.size(); n += 3)
            check(text.substr(0, n));
    }
}
