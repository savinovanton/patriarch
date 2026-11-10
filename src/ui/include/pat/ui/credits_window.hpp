#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// One credited party, already in the language of the moment it was built.
struct CreditsItem {
    std::string name;  // "Российская национальная библиотека"
    std::string meta;  // "поставщик · лицензия"; empty when there is none
    std::string text;  // description or the provider's attribution wording; empty when there is none
};

struct CreditsSection {
    std::string title;
    std::vector<CreditsItem> items;
};

/// What the credits window shows: plain data, no phrases of the interface (those the window formats).
struct CreditsSnapshot {
    std::vector<CreditsSection> sections;
};

/// The "Титры" (credits) window: sections with the people, data sources and assets the game credits.
/// Bound to a CreditsSnapshot through an RmlUi data model. The list is built from data (data/credits,
/// docs/dev/credits.md) by the game bridge; the window only shows it. Its own captions (title, close
/// button, empty list) come from the string table (`ui.credits.*`); a missing key shows as [[key]].
/// Hidden until show(true). Temporary test layout in the style of the other spike windows.
class CreditsWindow {
public:
    /// Creates the data model and loads `document` into `ui`; the window starts hidden. `loc` supplies
    /// the captions and must outlive the window. Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<CreditsWindow>>
    create(engine::UiContext& ui, const std::filesystem::path& document, const loc::Localizer& loc);
    ~CreditsWindow();
    CreditsWindow(const CreditsWindow&) = delete;
    CreditsWindow& operator=(const CreditsWindow&) = delete;

    void set_snapshot(const CreditsSnapshot& snapshot);
    [[nodiscard]] const CreditsSnapshot& snapshot() const;

    /// Re-reads the captions from the Localizer (after a language change). The snapshot keeps the
    /// language it was built in: rebuild it.
    void relocalize();

    /// Called after the close button hid the window.
    void set_close_handler(std::function<void()> handler);

    [[nodiscard]] bool visible() const;
    void show(bool visible);
    void toggle();

    [[nodiscard]] Rml::ElementDocument* document() const;

private:
    struct Impl;
    explicit CreditsWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
