#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/ui/explanation.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// The page of the shared components (UI-D02, #257): every state of the button, badges, tabs, cards, the
/// search field and filter chips, panels, the progress track and key-value rows from boards 00 and 09 of the
/// mockup, on a paper page (apps/spike/ui/gallery.rml). A sample for the designer and the test bed of the
/// components: tabs, cards and chips react to clicks and the keyboard; every text comes from the string table
/// (`ui.gallery.*`).
class ComponentGallery {
public:
    /// Creates the data model and loads `document`; `icons` is the directory with the SVG icons
    /// (assets/ui/patriarch/icons), written into the document relative to it. `loc` must outlive the page.
    [[nodiscard]] static core::Result<std::unique_ptr<ComponentGallery>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~ComponentGallery();
    ComponentGallery(const ComponentGallery&) = delete;
    ComponentGallery& operator=(const ComponentGallery&) = delete;

    void relocalize();

    /// The explanation behind a tooltip key of the page ("gallery:cost", "gallery:treasury",
    /// "gallery:reason", "gallery:skill"); null for other keys. Meant as a TooltipManager provider.
    [[nodiscard]] const Explanation* explanation(std::string_view key) const;

    /// The page of the gallery: "components" (default) or "tips" (the samples of the tooltip boards).
    void set_page(std::string_view page);
    [[nodiscard]] std::string_view page() const;

    /// "overview", "income" or "buildings".
    void set_tab(std::string_view tab);
    [[nodiscard]] std::string_view tab() const;
    /// 1 or 2: the selected card.
    [[nodiscard]] int selected_card() const;
    /// How many times a button with an action was pressed.
    [[nodiscard]] int presses() const;

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit ComponentGallery(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
