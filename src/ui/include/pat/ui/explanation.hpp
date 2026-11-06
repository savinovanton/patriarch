#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Explanation trees (FR-UI-3, GDD 01 section 8 / GDD 08 section 4.7): every number the game shows can be
// broken down into its terms, and every term can have terms of its own. The simulation (or any system
// that computes a value) produces a tree next to the value; the tooltip engine turns it into UI without
// hand-written texts for each case.

namespace pat::ui {

struct Explanation {
    std::string label;  ///< "Хозяйство", "Образование", "Черта «Щедрый»"
    std::string value;  ///< preformatted: "9", "+3", "-2", "35%"
    std::string note;   ///< optional explanatory line
    std::vector<Explanation> terms;

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

/// Generates the RML of one tooltip level (the box with title, total, one row per term and the note).
/// `path` is the tooltip path of `node` ("skill:0", "skill:0/1" ...); terms that have terms of their own
/// get a `data-tip` attribute with their path so that hovering them opens the next level. `inline_style`
/// (RCSS text) is embedded in a <style> element; it is not escaped.
[[nodiscard]] std::string render_tooltip_rml(const Explanation& node,
                                             std::string_view path,
                                             int level,
                                             std::string_view inline_style = {});

}  // namespace pat::ui
