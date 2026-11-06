#include "pat/ui/tooltip_state.hpp"

#include <algorithm>

namespace pat::ui {

void TooltipState::set_hover(const HoverInfo& hover) {
    hover_ = hover;
}

void TooltipState::resize_levels(std::size_t depth) {
    if (levels_.size() > depth) {
        levels_.resize(depth);
        ++revision_;
    }
}

void TooltipState::close_all() {
    resize_levels(0);
    pending_path_.clear();
    pending_time_ = 0.0f;
    close_time_ = 0.0f;
    pinned_ = false;
}

void TooltipState::set_pinned(bool pinned) {
    pinned_ = pinned && !levels_.empty();
}

void TooltipState::tick(float dt) {
    // How many of the open levels the cursor is currently "inside" (they must stay open).
    std::size_t keep = 0;
    if (hover_.level >= 0) {
        keep = std::min(levels_.size(), static_cast<std::size_t>(hover_.level) + 1);
    }

    // A hovered element that has a tooltip is a candidate for the next level.
    bool has_candidate = false;
    std::size_t cand_level = 0;
    if (!hover_.path.empty()) {
        cand_level = static_cast<std::size_t>(hover_.level + 1);
        if (cand_level < static_cast<std::size_t>(kMaxDepth)) {
            if (cand_level < levels_.size() && levels_[cand_level].path == hover_.path) {
                keep = std::max(keep, cand_level + 1);  // already open: keep it (and what leads to it)
            } else if (!(pinned_ && cand_level == 0 && !levels_.empty())) {
                has_candidate = true;  // a pinned stack is not replaced by hovering something else
            }
        }
    }

    if (has_candidate) {
        const int level = static_cast<int>(cand_level);
        if (pending_path_ == hover_.path && pending_level_ == level) {
            pending_time_ += dt;
        } else {
            pending_path_ = hover_.path;
            pending_level_ = level;
            pending_time_ = dt;
        }
        pending_anchor_ = hover_.anchor;
        const float delay = levels_.empty() ? timing_.show_delay : timing_.nested_delay;
        if (pending_time_ >= delay) {
            resize_levels(cand_level);
            levels_.push_back({pending_path_, pending_anchor_});
            ++revision_;
            keep = std::max(keep, cand_level + 1);
            pending_path_.clear();
            pending_time_ = 0.0f;
            close_time_ = 0.0f;
        }
    } else {
        pending_path_.clear();
        pending_time_ = 0.0f;
    }

    // Levels the cursor has left close after a short grace period (not while pinned).
    if (!pinned_ && levels_.size() > keep) {
        close_time_ += dt;
        if (close_time_ >= timing_.hide_grace) {
            resize_levels(keep);
            close_time_ = 0.0f;
        }
    } else {
        close_time_ = 0.0f;
    }
}

namespace {

float clamp_axis(float value, float lo, float hi) {
    return std::clamp(value, lo, std::max(lo, hi));
}

}  // namespace

RectF place_tooltip(const PlaceRequest& r) {
    const float margin = r.margin;
    const float w = r.size.w;
    const float h = r.size.h;
    const float cx = r.cursor_x;
    const float cy = r.cursor_y;

    const auto fits = [&](const RectF& t) {
        return t.x >= margin && t.y >= margin && t.right() <= r.viewport.w - margin &&
               t.bottom() <= r.viewport.h - margin;
    };
    const auto covers = [&](const RectF& t) {
        const RectF cursor{cx - 4.0f, cy - 4.0f, 8.0f, 8.0f};
        if (t.intersects(cursor) || (r.parent != nullptr && t.intersects(*r.parent))) {
            return true;
        }
        for (const RectF& other : r.avoid) {
            if (t.intersects(other)) {
                return true;
            }
        }
        return false;
    };
    const auto clamp_y = [&](RectF t) {
        t.y = clamp_axis(t.y, margin, r.viewport.h - margin - h);
        return t;
    };
    const auto clamp_both = [&](RectF t) {
        t.x = clamp_axis(t.x, margin, r.viewport.w - margin - w);
        t.y = clamp_axis(t.y, margin, r.viewport.h - margin - h);
        return t;
    };

    // Candidates in order of preference; the bool says whether only the vertical position may be adjusted.
    std::vector<std::pair<RectF, bool>> candidates;
    if (r.parent != nullptr) {
        const RectF& p = *r.parent;
        candidates.push_back({{p.right() + r.gap, r.anchor.y - 8.0f, w, h}, true});  // right of the parent
        candidates.push_back({{p.x - r.gap - w, r.anchor.y - 8.0f, w, h}, true});    // left of the parent
        candidates.push_back({{p.x, p.bottom() + r.gap, w, h}, false});              // below
        candidates.push_back({{p.x, p.y - r.gap - h, w, h}, false});                 // above
    } else {
        candidates.push_back({{cx + 14.0f, cy + 20.0f, w, h}, false});          // below-right of the cursor
        candidates.push_back({{cx - 14.0f - w, cy + 20.0f, w, h}, false});      // below-left
        candidates.push_back({{cx + 14.0f, cy - 20.0f - h, w, h}, false});      // above-right
        candidates.push_back({{cx - 14.0f - w, cy - 20.0f - h, w, h}, false});  // above-left
        candidates.push_back(
            {{cx + 14.0f, cy - h * 0.5f, w, h}, true});  // right of the cursor, shifted up/down
        candidates.push_back({{cx - 14.0f - w, cy - h * 0.5f, w, h}, true});           // left of the cursor
        candidates.push_back({{r.anchor.x, r.anchor.bottom() + r.gap, w, h}, false});  // below the element
        candidates.push_back({{r.anchor.x, r.anchor.y - r.gap - h, w, h}, false});     // above the element
    }

    for (const auto& [rect, vertical_only] : candidates) {
        const RectF t = vertical_only ? clamp_y(rect) : rect;
        if (fits(t) && !covers(t)) {
            return t;
        }
    }

    // Nothing fits cleanly (tiny screen, huge tooltip): clamp the first choice into the screen and, if it
    // landed on the cursor, move it just above or below the cursor.
    RectF t = clamp_both(candidates.front().first);
    if (covers(t)) {
        const float above = cy - 12.0f - h;
        t.y = above >= margin ? above : cy + 12.0f;
        t = clamp_both(t);
    }
    return t;
}

}  // namespace pat::ui
