#include "pat/core/string_interner.hpp"

#include "pat/core/assert.hpp"

namespace pat::core {

StringId StringInterner::intern(std::string_view text) {
    if (const auto it = lookup_.find(text); it != lookup_.end()) {
        return it->second;
    }
    const StringId id(static_cast<StringId::value_type>(storage_.size()));
    storage_.emplace_back(text);
    lookup_.emplace(std::string_view(storage_.back()), id);
    return id;
}

std::optional<StringId> StringInterner::find(std::string_view text) const {
    if (const auto it = lookup_.find(text); it != lookup_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string_view StringInterner::view(StringId id) const {
    PAT_ASSERT(id.valid() && id.index() < storage_.size(), "StringInterner::view with an unknown id");
    return storage_[id.index()];
}

void StringInterner::clear() {
    lookup_.clear();
    storage_.clear();
}

}  // namespace pat::core
