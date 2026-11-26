#pragma once
#include "court_fixture.hpp"
namespace pat::army_fixture {
inline constexpr std::string_view kBalance = "balance/army.pd";
inline constexpr std::string_view kMilitary = "scenarios/m1/military.pd";
inline data::MemorySource shipped_source(bool with_army = true) {
    auto source = court_fixture::shipped_source(false);
    for (const auto* path : {"provinces/m1.pd", "dynasties/m1.pd"})
        source.add(path, court_fixture::shipped_text(path));
    if (with_army)
        for (const auto path : {kBalance, kMilitary})
            source.add(std::string(path), court_fixture::shipped_text(path));
    return source;
}
}  // namespace pat::army_fixture
