#include "pat/core/ids.hpp"

#include <doctest/doctest.h>

#include <type_traits>
#include <unordered_set>

using namespace pat::core;

namespace {
using ProvinceId = StrongId<struct ProvinceTag>;
using DynastyId = StrongId<struct DynastyTag>;
using ProvinceHandle = Handle<struct ProvinceTag>;

static_assert(!std::is_convertible_v<ProvinceId, DynastyId>, "ids of different kinds must not mix");
static_assert(!std::is_convertible_v<std::uint32_t, ProvinceId>, "construction from integers is explicit");
static_assert(std::is_trivially_copyable_v<ProvinceId>);
static_assert(sizeof(ProvinceId) == sizeof(std::uint32_t));
}  // namespace

TEST_CASE("ids: default id is invalid") {
    const ProvinceId none;
    CHECK_FALSE(none.valid());
    CHECK(none == ProvinceId::invalid());
    CHECK(none.value() == ProvinceId::kInvalid);
}

TEST_CASE("ids: values, ordering and equality") {
    const ProvinceId a(3);
    const ProvinceId b(7);
    CHECK(a.valid());
    CHECK(a.value() == 3);
    CHECK(a.index() == 3);
    CHECK(a < b);
    CHECK(a != b);
    CHECK(a == ProvinceId(3));
}

TEST_CASE("ids: usable as hash map keys for lookup") {
    std::unordered_set<ProvinceId> set;
    set.insert(ProvinceId(1));
    set.insert(ProvinceId(2));
    set.insert(ProvinceId(1));
    CHECK(set.size() == 2);
    CHECK(set.contains(ProvinceId(2)));
    CHECK_FALSE(set.contains(ProvinceId(3)));
}

TEST_CASE("ids: handles compare by index and generation") {
    const ProvinceHandle none;
    CHECK_FALSE(none.valid());

    const ProvinceHandle a{4, 1};
    const ProvinceHandle b{4, 2};
    CHECK(a.valid());
    CHECK(a != b);
    CHECK(a == ProvinceHandle{4, 1});
    CHECK(std::hash<ProvinceHandle>{}(a) != std::hash<ProvinceHandle>{}(b));
}
