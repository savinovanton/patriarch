// contentlint - checks the game content and the localisation tables (E4-T08).
//
//   contentlint [--root DIR]... [--lang ru --fallback en] [--strict] [--format text|github]
//
// See docs/dev/contentlint.md. Exit codes: 0 clean, 1 errors (or warnings under --strict), 2 usage
// or I/O error.

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include "lint.hpp"

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    try {
        const int code = pat::contentlint::run_cli(args, std::cout, std::cerr);
        std::cout.flush();
        return code;
    } catch (const std::exception& e) {
        // The tool must not crash on garbage input; anything unexpected is an I/O-class failure.
        std::cerr << "contentlint: internal error: " << e.what() << '\n';
        return 2;
    }
}
