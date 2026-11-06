#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Behaviour of nested tooltips without any UI toolkit (E3-T08): when they open and close, how deep they
// nest, pinning, and where they are placed. The RmlUi glue (tooltip_manager) only feeds this state with
// what is under the cursor and shows the resulting levels.

namespace pat::ui {

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    [[nodiscard]] float right() const noexcept { return x + w; }
    [[nodiscard]] float bottom() const noexcept { return y + h; }
    [[nodiscard]] bool contains(float px, float py) const noexcept {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }
    [[nodiscard]] bool intersects(const RectF& o) const noexcept {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    [[nodiscard]] bool operator==(const RectF&) const = default;
};

struct TooltipTiming {
    float show_delay = 0.35f;    ///< seconds of hovering before the first tooltip opens
    float nested_delay = 0.20f;  ///< before a nested tooltip (or a replacement while one is open) opens
    float hide_grace = 0.25f;    ///< how long levels stay after the cursor left them
};

/// What the cursor is over, as determined by the UI each frame.
struct HoverInfo {
    /// Tooltip path of the hovered element that has a tooltip ("skill:0", "skill:0/1"), or empty.
    std::string path;
    /// -1: the element is in the normal UI; k >= 0: inside tooltip level k (so `path` opens level k + 1).
    int level = -1;
    RectF anchor;  ///< screen rectangle of the hovered element
};

class TooltipState {
public:
    struct Level {
        std::string path;
        RectF anchor;
        [[nodiscard]] bool operator==(const Level&) const = default;
    };

    static constexpr int kMaxDepth = 3;

    explicit TooltipState(TooltipTiming timing = {}) : timing_(timing) {}

    /// Reports what is under the cursor now (call every frame, with an empty path when nothing is).
    void set_hover(const HoverInfo& hover);
    /// Advances the timers; opens and closes levels.
    void tick(float dt);

    /// Pinning keeps the open tooltips on screen while the cursor wanders (to read or to reach nested
    /// terms); they close when unpinned and the cursor is elsewhere.
    void set_pinned(bool pinned);
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }
    void close_all();

    [[nodiscard]] const std::vector<Level>& levels() const noexcept { return levels_; }
    /// Incremented whenever the list of levels changes.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    void resize_levels(std::size_t depth);

    TooltipTiming timing_;
    std::vector<Level> levels_;
    std::uint64_t revision_ = 0;
    bool pinned_ = false;

    HoverInfo hover_;
    // A candidate level waiting for its delay.
    std::string pending_path_;
    int pending_level_ = -1;
    RectF pending_anchor_;
    float pending_time_ = 0.0f;
    // Levels deeper than the cursor is currently "in" wait for the grace period before closing.
    float close_time_ = 0.0f;
};

struct SizeF {
    float w = 0.0f;
    float h = 0.0f;
};

struct PlaceRequest {
    SizeF size;             ///< tooltip size
    SizeF viewport;         ///< screen size
    float cursor_x = 0.0f;  ///< cursor position (the tooltip must not cover it)
    float cursor_y = 0.0f;
    RectF anchor;                   ///< element that was hovered
    const RectF* parent = nullptr;  ///< tooltip level this one nests in (must not be covered), or null
    float margin = 8.0f;            ///< minimum distance to the screen edges
    float gap = 6.0f;               ///< distance to the anchor / parent
    std::vector<RectF> avoid;       ///< other open tooltips (ancestors) that must not be covered either
};

/// Chooses where a tooltip goes: inside the screen, not covering the cursor and (for nested tooltips) not
/// covering the parent tooltip. Root tooltips prefer the area below-right of the cursor and flip to the
/// other sides at the screen edges; nested ones prefer the side of the parent tooltip.
[[nodiscard]] RectF place_tooltip(const PlaceRequest& request);

}  // namespace pat::ui
