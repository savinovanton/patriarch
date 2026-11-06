#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/events.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/explanation.hpp"
#include "pat/ui/tooltip_state.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// Nested tooltips over RmlUi (E3-T08, FR-UI-3). Any element with a `data-tip="key"` attribute gets a
/// tooltip generated from the Explanation the provider returns for `key`; terms that have terms of
/// their own open the next level when hovered (up to three levels). Tooltips open after a delay, stay
/// while the cursor is over them, can be pinned with Alt, and are placed inside the screen without
/// covering the cursor or their parent.
class TooltipManager {
public:
    /// Returns the root explanation for a tooltip key ("skill:0"), or null if there is none.
    using Provider = std::function<const Explanation*(std::string_view key)>;

    /// `stylesheet_dir` must contain `tooltip.rcss`. Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<TooltipManager>>
    create(engine::UiContext& ui, const std::filesystem::path& stylesheet_dir, TooltipTiming timing = {});
    ~TooltipManager();
    TooltipManager(const TooltipManager&) = delete;
    TooltipManager& operator=(const TooltipManager&) = delete;

    void set_provider(Provider provider);

    /// Feed every event (after UiContext::handle_event): tracks the cursor, Alt pins/unpins, Esc closes.
    void handle_event(const engine::Event& event);

    /// Per frame, before UiContext::update(): finds what is hovered and opens/closes tooltip documents.
    void update(float dt_seconds);

    void close_all();

    [[nodiscard]] std::size_t open_levels() const;
    [[nodiscard]] bool pinned() const;
    [[nodiscard]] const TooltipState& state() const;
    /// The document of an open level (0 = first), or null.
    [[nodiscard]] Rml::ElementDocument* level_document(std::size_t level) const;
    /// Screen rectangle of an open level.
    [[nodiscard]] RectF level_rect(std::size_t level) const;

private:
    struct Impl;
    explicit TooltipManager(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
