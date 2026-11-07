// schemadoc - prints the Markdown reference of the game content schemas (E4-T09).
//
//   schemadoc                         print docs/dev/data-reference.md to stdout (LF line endings)
//   schemadoc --write FILE            write the reference to FILE (LF line endings, UTF-8, no BOM)
//   schemadoc --check FILE            exit 1 with a diff-like message when FILE differs from the
//                                     generated reference (CR characters are ignored)
//
// The source of truth is pat::data::content_schemas(); to add a kind, register it there and run
// `schemadoc --write docs/dev/data-reference.md`.
//
// Exit codes: 0 ok / up to date, 1 the file is out of date, 2 usage or I/O error.

#include "pat/data/content_schemas.hpp"
#include "pat/data/schema_doc.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitStale = 1;
constexpr int kExitError = 2;

void usage() {
    std::fputs("usage: schemadoc [--write FILE | --check FILE]\n"
               "  (no arguments)  print the schema reference to stdout\n"
               "  --write FILE    write the reference to FILE\n"
               "  --check FILE    fail (exit 1) when FILE differs from the generated reference\n",
               stderr);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

std::string strip_cr(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\r')
            out += c;
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

int check(const std::string& path, const std::string& expected) {
    std::string actual_raw;
    if (!read_file(path, actual_raw)) {
        std::fprintf(stderr, "schemadoc: cannot read '%s'\n", path.c_str());
        return kExitError;
    }
    const std::string actual = strip_cr(actual_raw);
    if (actual == expected)
        return kExitOk;

    const std::vector<std::string> want = split_lines(expected);
    const std::vector<std::string> have = split_lines(actual);
    std::size_t i = 0;
    while (i < want.size() && i < have.size() && want[i] == have[i])
        ++i;
    std::fprintf(stderr, "schemadoc: '%s' is out of date (generated reference differs)\n", path.c_str());
    std::fprintf(stderr, "  first difference at line %zu:\n", i + 1);
    std::fprintf(stderr, "  - expected: %s\n", i < want.size() ? want[i].c_str() : "<end of file>");
    std::fprintf(stderr, "  + actual:   %s\n", i < have.size() ? have[i].c_str() : "<end of file>");
    std::fprintf(stderr, "  expected %zu lines, found %zu\n", want.size(), have.size());
    std::fprintf(stderr, "  regenerate it with: schemadoc --write %s\n", path.c_str());
    return kExitStale;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string text = pat::data::render_reference_document(pat::data::content_schemas());

    if (argc == 1) {
#if defined(_WIN32)
        (void)_setmode(_fileno(stdout), _O_BINARY);  // keep LF; text mode would write CRLF
#endif
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), stdout);
        return (written == text.size() && std::fflush(stdout) == 0) ? kExitOk : kExitError;
    }

    if (argc == 3 && std::strcmp(argv[1], "--write") == 0) {
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::fprintf(stderr, "schemadoc: cannot write '%s'\n", argv[2]);
            return kExitError;
        }
        return kExitOk;
    }

    if (argc == 3 && std::strcmp(argv[1], "--check") == 0)
        return check(argv[2], text);

    usage();
    return kExitError;
}
