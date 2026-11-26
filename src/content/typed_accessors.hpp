#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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

/// Position of a dotted path such as `seats.max` or `requirements.skills[2]` inside an object: the key
/// of the deepest field that exists, so an error never points outside the object.
inline data::SourcePos path_position(const data::ContentObject& object, std::string_view path) {
    data::SourcePos pos = object.object.pos;
    const data::TypedBlock* block = &object.object.body;
    while (block != nullptr && !path.empty()) {
        const auto dot = path.find('.');
        std::string_view name = path.substr(0, dot);
        name = name.substr(0, name.find('['));
        pos = key_pos(*block, name, pos);
        block = dot == std::string_view::npos ? nullptr : block_of(*block, name);
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return pos;
}

/// Copies a fixed-size integer list; schemas ensure int32 representation, the caller validates values.
template <std::size_t N>
void read_i32_array(ErrorSink& errors,
                    const data::ContentObject& object,
                    const data::TypedBlock& block,
                    std::string_view name,
                    std::array<std::int32_t, N>& out,
                    std::string_view path = {}) {
    if (path.empty())
        path = name;
    const auto* list = list_of(block, name);
    const auto size = list == nullptr ? 0 : list->size();
    if (size != N) {
        errors.fail(object,
                    path_position(object, path),
                    std::format("`{}` needs exactly {} numbers, found {}", path, N, size));
        return;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (const auto* v = (*list)[i].template get_if<std::int64_t>())
            out[i] = static_cast<std::int32_t>(*v);
    }
}

}  // namespace pat::content::detail
