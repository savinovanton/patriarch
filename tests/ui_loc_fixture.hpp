#pragma once

#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"

#include <doctest/doctest.h>

#include <string>

// String tables for the UI tests: the real data/loc tables, plus helpers for language and
// missing-key scenarios. Tests that change the chain or the tables load a private copy.

namespace pat::test {

/// A fresh Localizer with the real tables of data/loc (fails the test when they do not load).
inline loc::Localizer load_ui_localizer() {
    data::DirectorySource source(PATRIARCH_DATA_DIR);
    loc::LocLoadResult result = loc::load_localization(source);
    CHECK(result.ok());
    return std::move(result.localizer);
}

/// The real tables, loaded once. Read-only: format/queries only, never set_chain().
inline const loc::Localizer& ui_localizer() {
    static const loc::Localizer shared = load_ui_localizer();
    return shared;
}

/// The real tables plus a small English table (a "mod" root with loc/en/ui.pd) for the keys the HUD,
/// time bar and character window show first; select it with set_chain({"en", "ru"}).
inline loc::Localizer load_ui_localizer_with_english() {
    data::DirectorySource base(PATRIARCH_DATA_DIR);
    data::MemorySource english("english");
    english.add("loc/en/ui.pd", R"EN(format_version = 1

ui.timebar.pause = "Pause"
ui.res.treasury = "Treasury"
ui.res.tooltip = "{label}: {n:group} ({delta})"
ui.hud.holdings.title = "HOLDINGS"
ui.hud.tab.cities = "Cities"
ui.hud.affairs.council.detail = "{n} {n:plural(item|items)} on the agenda"
ui.character.tab.overview = "Overview"
ui.age.years = "{n} {n:plural(year|years)}"
ui.character.subtitle = "{role} - {age} - health: {health}"
ui.character.role.stolnik = "stolnik"
ui.health.strong = "sturdy"
ui.date.long = "{month} {day}, {year}"
ui.month.may = "May"
)EN");
    const data::ContentSource* roots[] = {&base, &english};
    loc::LocLoadResult result = loc::load_localization(roots);
    CHECK(result.ok());
    return std::move(result.localizer);
}

/// A Localizer that knows no key at all: every text shows as [[key]].
inline loc::Localizer load_empty_localizer() {
    data::MemorySource source("empty");
    source.add("loc/ru/ui.pd", "format_version = 1\n");
    loc::LocLoadResult result = loc::load_localization(source);
    CHECK(result.ok());
    return std::move(result.localizer);
}

}  // namespace pat::test
