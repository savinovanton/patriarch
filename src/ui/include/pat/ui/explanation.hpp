#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Explanation trees (FR-UI-3, GDD 01 section 8 / GDD 08 section 4.7): every number the game shows can be
// broken down into its terms, and every term can have terms of its own. The simulation (or any system
// that computes a value) produces a tree next to the value; the tooltip engine turns it into UI without
// hand-written texts for each case.

namespace pat::ui {

/// How the title of a tooltip is tinted: `Warning` is the red of an unavailability reason ("Не хватает 40
/// ₽").
enum class TipTone : unsigned char { Normal, Warning };

/// One tooltip (UI-D03, #258): a title, optional lines, a table of terms and a total. The fields map to the
/// layers of the tooltip boards of the mockup (docs/dev/ui-design.md section 6): `label` is the title, `note`
/// the quiet line below it ("Списываются при отправке"), `headline` the big value ("1 248 ₽"), `lines` the
/// body of a reason, `terms` the rows, `total_label` + `value` the sum row ("Итого 120 ₽"; without a total
/// label the value stands next to the title), `footer` the gold line at the bottom.
struct Explanation {
    std::string label;  ///< "Хозяйство", "Образование", "Черта «Щедрый»"
    std::string value;  ///< preformatted: "9", "+3", "-2", "35%"
    std::string note;   ///< optional explanatory line under the title; on a term: "×1,20" after its label
    std::vector<Explanation> terms;
    std::string headline;            ///< optional large value under the title
    std::vector<std::string> lines;  ///< optional paragraphs (the reason an action is unavailable)
    std::string total_label;         ///< label of the sum row; empty: `value` is shown next to the title
    std::string footer;              ///< optional gold line at the bottom
    TipTone tone = TipTone::Normal;
    bool wide = false;  ///< the 410 px variant of the character tooltips instead of 366 px

    Explanation() = default;
    Explanation(std::string label_, std::string value_, std::string note_, std::vector<Explanation> terms_)
        : label(std::move(label_)), value(std::move(value_)), note(std::move(note_)),
          terms(std::move(terms_)) {}

    [[nodiscard]] bool operator==(const Explanation&) const = default;
};

/// Preformatted signed number: "+3", "-2", "0".
[[nodiscard]] std::string signed_value(int value);

/// Follows a path of term indices ("0/2" = third term of the first term) below `root`. An empty path is
/// `root` itself. Returns null for malformed paths or out-of-range indices.
[[nodiscard]] const Explanation* find_term(const Explanation& root, std::string_view path);

/// Splits "key/0/2" into the key ("key") and the term path ("0/2").
struct SplitPath {
    std::string_view key;
    std::string_view terms;
};
[[nodiscard]] SplitPath split_tooltip_path(std::string_view path);

/// Escapes text for use inside RML/HTML element content and attribute values.
[[nodiscard]] std::string escape_markup(std::string_view text);

/// What the generated markup needs besides the explanation.
struct TooltipRender {
    std::string_view style;  ///< RCSS text embedded in a <style> element; not escaped
    std::string_view icons;  ///< directory of the SVG icons (for the close button); empty: no icon
    bool pinned = false;     ///< the close button is visible
};

/// Generates the RML of one tooltip level (the box with title, rows, total and footer). `path` is the tooltip
/// path of `node` ("skill:0", "skill:0/1" ...); terms that have terms of their own get a `data-tip` attribute
/// with their path so that hovering them opens the next level. The box has `id="tip"`, floats at the screen
/// position set by the manager and carries `data-tip-level`.
[[nodiscard]] std::string render_tooltip_rml(const Explanation& node,
                                             std::string_view path,
                                             int level,
                                             const TooltipRender& render = {});

/// The inner markup of the box only (title ... footer), for static samples such as the component gallery.
[[nodiscard]] std::string
render_tooltip_body(const Explanation& node, std::string_view path, std::string_view icons);

}  // namespace pat::ui
