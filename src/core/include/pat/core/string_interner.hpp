#pragma once

#include "pat/core/ids.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pat::core {

using StringId = StrongId<struct StringIdTag>;

/// Maps strings to small sequential ids. Ids are assigned in first-seen order, so the table is
/// deterministic and can be serialized as an ordered list. Views returned by view() stay valid for
/// the lifetime of the interner (strings are never moved).
///
/// The hash map is used for lookup only; iteration goes through the id order.
class StringInterner {
public:
    StringInterner() = default;
    StringInterner(const StringInterner&) = delete;
    StringInterner& operator=(const StringInterner&) = delete;
    StringInterner(StringInterner&&) noexcept = default;
    StringInterner& operator=(StringInterner&&) noexcept = default;

    /// Returns the id of `text`, adding it if necessary.
    StringId intern(std::string_view text);

    /// Id of an already interned string, if any.
    [[nodiscard]] std::optional<StringId> find(std::string_view text) const;

    /// The text for a valid id.
    [[nodiscard]] std::string_view view(StringId id) const;

    [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }
    void clear();

private:
    std::deque<std::string> storage_;  // stable element addresses
    std::unordered_map<std::string_view, StringId> lookup_;
};

}  // namespace pat::core
