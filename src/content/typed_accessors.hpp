#pragma once

#include "pat/data/schema.hpp"

#include <cstdint>
#include <string_view>

namespace pat::content::detail {

template <class T>
const T* value_of(const data::TypedBlock& block, std::string_view name) {
    const auto* value = block.find(name);
    return value != nullptr ? value->get_if<T>() : nullptr;
}

inline std::int64_t int_of(const data::TypedBlock& block, std::string_view name) {
    const auto* value = value_of<std::int64_t>(block, name);
    return value != nullptr ? *value : 0;
}

inline std::int32_t i32_of(const data::TypedBlock& block, std::string_view name) {
    return static_cast<std::int32_t>(int_of(block, name));
}

inline const data::TypedBlock* block_of(const data::TypedBlock& block, std::string_view name) {
    return value_of<data::TypedBlock>(block, name);
}

inline const data::TypedList* list_of(const data::TypedBlock& block, std::string_view name) {
    return value_of<data::TypedList>(block, name);
}

inline data::SourcePos
key_pos(const data::TypedBlock& block, std::string_view name, data::SourcePos fallback) {
    for (const auto& field : block.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return fallback;
}

}  // namespace pat::content::detail
