#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/character_snapshot.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// The "character" window (E3-T07): portrait placeholder, name, age, five skill bars, traits, tabs
/// (overview / family / relations), command buttons and a virtualised list for the relations tab.
///
/// The window is bound to a CharacterSnapshot through an RmlUi data model: set_snapshot() replaces the
/// data and the document updates itself. The relations list can hold thousands of rows because only
/// the rows around the scroll position exist in the document (fixed row height, spacer elements).
class CharacterWindow {
public:
    /// Row height of the virtual list in dp; the document's CSS must use the same value.
    static constexpr float kRowHeightDp = 24.0f;

    /// Creates the data model and loads `document` into `ui`. Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<CharacterWindow>>
    create(engine::UiContext& ui, const std::filesystem::path& document);
    ~CharacterWindow();
    CharacterWindow(const CharacterWindow&) = delete;
    CharacterWindow& operator=(const CharacterWindow&) = delete;

    void set_snapshot(const CharacterSnapshot& snapshot);
    [[nodiscard]] const CharacterSnapshot& snapshot() const;

    /// Explanation behind a tooltip key of this window ("skill:0" ...); null for unknown keys. Meant as
    /// the provider of a TooltipManager.
    [[nodiscard]] const Explanation* explanation(std::string_view key) const;

    /// "overview", "family" or "relations".
    void set_tab(std::string_view tab);
    [[nodiscard]] std::string_view tab() const;

    /// Called with the command id ("marry", "appoint", "educate") when a command button is pressed.
    void set_command_handler(std::function<void(const std::string&)> handler);

    /// Per-frame work before UiContext::update(): keeps the virtual list in step with its scroll position.
    void update();

    /// Number of relation rows that currently exist in the document (a small window, not all of them).
    [[nodiscard]] std::size_t rows_in_document() const;
    /// Index (0-based) of the first relation row in the document.
    [[nodiscard]] std::size_t first_row_in_document() const;

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit CharacterWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
