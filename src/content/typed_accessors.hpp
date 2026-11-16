#pragma once

#include "pat/data/content_registry.hpp"
#include "pat/data/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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

// ---- ContentObject overloads: schema validation guarantees presence and type; defaults keep us safe

/// String value of a field, empty when absent or of another type.
inline std::string text_of(const data::ContentObject& object, std::string_view name) {
    const auto* value = value_of<std::string>(object.object.body, name);
    return value != nullptr ? *value : std::string{};
}

/// Integer value of a field narrowed to 32 bits, `fallback` when absent or of another type.
inline std::int32_t
i32_of(const data::ContentObject& object, std::string_view name, std::int32_t fallback = 0) {
    const auto* value = value_of<std::int64_t>(object.object.body, name);
    return value != nullptr ? static_cast<std::int32_t>(*value) : fallback;
}

/// Index of an enum field's value, 0 when absent or of another type.
inline std::size_t enum_index_of(const data::ContentObject& object, std::string_view name) {
    const auto* value = value_of<data::EnumValue>(object.object.body, name);
    return value != nullptr ? value->index : 0;
}

inline bool has_field(const data::ContentObject& object, std::string_view name) {
    return object.object.body.find(name) != nullptr;
}

/// Position of the key of a field (the object itself when the field is absent).
inline data::SourcePos field_pos(const data::ContentObject& object, std::string_view name) {
    return key_pos(object.object.body, name, object.object.pos);
}

}  // namespace pat::content::detail
