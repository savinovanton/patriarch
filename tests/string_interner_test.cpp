#include "pat/core/string_interner.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace pat::core;

TEST_CASE("string_interner: ids are sequential and deduplicated") {
    StringInterner interner;
    const StringId a = interner.intern("alpha");
    const StringId b = interner.intern("beta");
    const StringId a_again = interner.intern("alpha");

    CHECK(a.value() == 0);
    CHECK(b.value() == 1);
    CHECK(a_again == a);
    CHECK(interner.size() == 2);
}

TEST_CASE("string_interner: view and find") {
    StringInterner interner;
    const StringId a = interner.intern("province.tula");
    CHECK(interner.view(a) == "province.tula");
    CHECK(interner.find("province.tula") == a);
    CHECK_FALSE(interner.find("province.kursk").has_value());
}

TEST_CASE("string_interner: the empty string is a valid entry") {
    StringInterner interner;
    const StringId empty = interner.intern("");
    CHECK(interner.view(empty).empty());
    CHECK(interner.intern("") == empty);
}

TEST_CASE("string_interner: views stay valid while the table grows") {
    StringInterner interner;
    const StringId first = interner.intern("a-rather-long-string-that-does-not-fit-in-sso-buffers");
    const std::string_view before = interner.view(first);
    const char* address = before.data();

    std::vector<StringId> ids;
    for (int i = 0; i < 5000; ++i) {
        ids.push_back(interner.intern("string-number-" + std::to_string(i)));
    }
    CHECK(interner.view(first).data() == address);
    CHECK(interner.view(first) == "a-rather-long-string-that-does-not-fit-in-sso-buffers");
    for (int i = 0; i < 5000; ++i) {
        REQUIRE(interner.view(ids[static_cast<std::size_t>(i)]) == "string-number-" + std::to_string(i));
    }
    CHECK(interner.size() == 5001);
}

TEST_CASE("string_interner: clear and rebuild in the same order gives the same ids") {
    StringInterner interner;
    const std::vector<std::string> words = {"x", "y", "z", "y"};
    std::vector<StringId> first;
    for (const auto& w : words) {
        first.push_back(interner.intern(w));
    }
    interner.clear();
    CHECK(interner.size() == 0);
    for (std::size_t i = 0; i < words.size(); ++i) {
        CHECK(interner.intern(words[i]) == first[i]);
    }
}

TEST_CASE("string_interner: can be moved") {
    StringInterner a;
    const StringId id = a.intern("moved");
    StringInterner b = std::move(a);
    CHECK(b.view(id) == "moved");
    CHECK(b.find("moved") == id);
}
