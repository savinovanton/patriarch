#include "pat/engine/label_layout.hpp"

#include <algorithm>
#include <cmath>

namespace pat::engine {

namespace {

float smoothstep(float lo, float hi, float v) {
    const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Rect {
    float x0;
    float y0;
    float x1;
    float y1;
    [[nodiscard]] bool overlaps(const Rect& o) const noexcept {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
};

struct Candidate {
    const MapLabel* label;
    float x;
    float y;
    float size;
    float alpha;
    float width;
    float rank;  // larger = placed first
};

/// Uniform grid over the screen holding the rectangles accepted so far.
class RectGrid {
public:
    RectGrid(float w, float h)
        : cols_(static_cast<int>(w / kCell) + 1), rows_(static_cast<int>(h / kCell) + 1) {
        cells_.resize(static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_));
    }

    [[nodiscard]] bool overlaps_any(const Rect& r) const {
        const auto [c0, c1, r0, r1] = range(r);
        for (int y = r0; y <= r1; ++y) {
            for (int x = c0; x <= c1; ++x) {
                for (const Rect& other : cells_[index(x, y)]) {
                    if (other.overlaps(r)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void add(const Rect& r) {
        const auto [c0, c1, r0, r1] = range(r);
        for (int y = r0; y <= r1; ++y) {
            for (int x = c0; x <= c1; ++x) {
                cells_[index(x, y)].push_back(r);
            }
        }
    }

private:
    static constexpr float kCell = 128.0f;

    struct Range {
        int c0, c1, r0, r1;
    };
    [[nodiscard]] Range range(const Rect& r) const {
        return {std::clamp(static_cast<int>(r.x0 / kCell), 0, cols_ - 1),
                std::clamp(static_cast<int>(r.x1 / kCell), 0, cols_ - 1),
                std::clamp(static_cast<int>(r.y0 / kCell), 0, rows_ - 1),
                std::clamp(static_cast<int>(r.y1 / kCell), 0, rows_ - 1)};
    }
    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(x);
    }

    int cols_;
    int rows_;
    std::vector<std::vector<Rect>> cells_;
};

}  // namespace

LabelLayoutStats layout_labels(std::span<const MapLabel> labels,
                               const LabelLayoutInput& in,
                               const LabelStyle& style,
                               std::vector<PlacedLabel>& out) {
    out.clear();
    LabelLayoutStats stats;
    stats.candidates = labels.size();

    std::vector<Candidate> candidates;
    candidates.reserve(labels.size());
    for (const MapLabel& label : labels) {
        const Vec4 clip = in.view_proj * Vec4{label.anchor.x, label.anchor.y, label.anchor.z, 1.0f};
        if (clip.w <= 1e-4f) {
            ++stats.hidden_offscreen;  // behind the camera
            continue;
        }
        const float inv_w = 1.0f / clip.w;
        const float sx = (clip.x * inv_w * 0.5f + 0.5f) * in.viewport_w;
        const float sy = (0.5f - clip.y * inv_w * 0.5f) * in.viewport_h;

        const float dist = std::max(length(in.eye - label.anchor), 1e-3f);
        const float extent_px = label.world_extent * in.pixels_per_unit_at_one / dist;

        float alpha = 0.0f;
        float size = 0.0f;
        float priority = 0.0f;
        if (label.kind == LabelKind::Province) {
            alpha = smoothstep(style.province_min_px, style.province_full_px, extent_px);
            size =
                std::clamp(extent_px * style.size_factor, style.province_min_size, style.province_max_size);
            priority = 1.0f;
        } else {
            alpha =
                smoothstep(style.region_min_px, style.region_full_px, extent_px) *
                (1.0f - smoothstep(style.region_fade_out_start_px, style.region_fade_out_end_px, extent_px));
            size = std::clamp(extent_px * style.size_factor, style.region_min_size, style.region_max_size);
            priority = 2.0f;
        }
        if (alpha < style.min_alpha) {
            ++stats.hidden_zoom;
            continue;
        }
        const float width = label.width_per_px * size;
        if (sx + width * 0.5f < 0.0f || sx - width * 0.5f > in.viewport_w || sy + size < 0.0f ||
            sy - size > in.viewport_h) {
            ++stats.hidden_offscreen;
            continue;
        }
        candidates.push_back({&label, sx, sy, size, alpha, width, priority * 1.0e6f + extent_px});
    }

    // Most important first; ties broken by id so the result is stable between frames.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) {
            return a.rank > b.rank;
        }
        return a.label->id < b.label->id;
    });

    RectGrid grid(in.viewport_w, in.viewport_h);
    for (const Candidate& c : candidates) {
        const float half_w = c.width * 0.5f + style.padding;
        const float half_h = c.size * 0.6f + style.padding;
        const Rect rect{c.x - half_w, c.y - half_h, c.x + half_w, c.y + half_h};
        if (grid.overlaps_any(rect)) {
            ++stats.hidden_overlap;
            continue;
        }
        grid.add(rect);
        out.push_back({c.label, c.x, c.y, c.size, c.alpha, c.width});
    }
    stats.placed = out.size();
    return stats;
}

}  // namespace pat::engine
