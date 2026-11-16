#include "pat/core/path_utf8.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace pat;

TEST_CASE("path_utf8: Cyrillic path round-trips") {
    const std::string dir = "data/\xD0\x9F\xD0\xB0\xD1\x82\xD1\x80\xD0\xB8\xD0\xB0\xD1\x80\xD1\x85";
    const std::string text = dir + "/\xD1\x91.pd";
    const std::filesystem::path p = core::path_from_utf8(text);
    CHECK(core::path_to_utf8(p) == text);
    CHECK(core::path_to_utf8(p.parent_path()) == dir);
}

TEST_CASE("path_utf8: ASCII and empty") {
    CHECK(core::path_to_utf8(core::path_from_utf8("a/b/c.txt")) == "a/b/c.txt");
    CHECK(core::path_to_utf8(core::path_from_utf8("")).empty());
    CHECK(core::path_to_utf8(std::filesystem::path("a") / "b") == "a/b");
}
