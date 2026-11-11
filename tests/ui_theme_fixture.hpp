#pragma once

#include "pat/engine/ui.hpp"
#include "pat/ui/theme.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <utility>

// The real theme (data/ui/theme.pd) for the window tests: windows link `theme.rcss` and write `$tokens`, so
// every UiContext that loads them needs the theme installed (pat::ui::install_theme).

namespace pat::test {

inline std::shared_ptr<const ui::Theme> load_game_theme() {
    auto theme = ui::load_theme(std::filesystem::path(PATRIARCH_DATA_DIR) / "ui" / "theme.pd");
    REQUIRE_MESSAGE(theme.has_value(), "theme.pd does not load");
    return std::make_shared<const ui::Theme>(std::move(theme).value());
}

inline void install_game_theme(engine::UiContext& ui) {
    ui::install_theme(ui, load_game_theme());
}

}  // namespace pat::test
