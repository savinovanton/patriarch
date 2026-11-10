#include "pat/content/credits_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/credits_schemas.hpp"
#include "pat/data/schema.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <tuple>
#include <utility>

namespace pat::content {

namespace {

using data::ContentObject;
using data::ContentTable;
using data::ErrorCode;
using data::LoadError;
using data::SourcePos;
using data::TypedField;
using data::TypedValue;

std::string text_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* s = value->get_if<std::string>()) {
            return *s;
        }
    }
    return {};
}

std::int32_t i32_of(const ContentObject& object, std::string_view name, std::int32_t fallback) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* i = value->get_if<std::int64_t>()) {
            return static_cast<std::int32_t>(*i);
        }
    }
    return fallback;
}

std::size_t enum_index_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* e = value->get_if<data::EnumValue>()) {
            return e->index;
        }
    }
    return 0;
}

bool has_field(const ContentObject& object, std::string_view name) {
    return object.object.body.find(name) != nullptr;
}

/// Position of the key of a field (the object itself when the field is absent).
SourcePos field_pos(const ContentObject& object, std::string_view name) {
    for (const TypedField& field : object.object.body.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return object.object.pos;
}

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry) {}

    CreditEntriesResult build() {
        if (const ContentTable* table = registry_.table(data::kCreditKind)) {
            for (const ContentObject& object : table->objects()) {
                add_entry(object);
            }
        }
        if (!errors_.empty()) {
            std::stable_sort(errors_.begin(), errors_.end(), [](const LoadError& a, const LoadError& b) {
                return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                       std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
            });
            return CreditEntriesResult::err(std::move(errors_));
        }
        std::stable_sort(entries_.begin(), entries_.end(), [](const CreditEntry& a, const CreditEntry& b) {
            return std::tie(a.section, a.order, a.id) < std::tie(b.section, b.order, b.id);
        });
        return CreditEntriesResult::ok(std::move(entries_));
    }

private:
    const data::ContentRegistry& registry_;
    std::vector<CreditEntry> entries_;
    std::vector<LoadError> errors_;

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(LoadError{object.root,
                                    registry_.root_name(object.root),
                                    object.file,
                                    data::Diagnostic{pos, ErrorCode::ContentRule, std::move(message)}});
    }

    /// `first` / `second` are two ways to give the same thing: exactly one is required when `required`,
    /// both together are never allowed, and a given one must not be empty.
    void
    check_pair(const ContentObject& object, std::string_view first, std::string_view second, bool required) {
        const bool a = has_field(object, first);
        const bool b = has_field(object, second);
        if (a && b) {
            fail(object,
                 field_pos(object, second),
                 std::format(
                     "credit '{}' has both {} and {}: give only one", object.object.id, first, second));
        } else if (!a && !b && required) {
            fail(object,
                 object.object.pos,
                 std::format("credit '{}' needs {} or {}", object.object.id, first, second));
        }
        for (const std::string_view field : {first, second}) {
            if (has_field(object, field) && text_of(object, field).empty()) {
                fail(object, field_pos(object, field), std::format("{} must not be empty", field));
            }
        }
    }

    void add_entry(const ContentObject& object) {
        CreditEntry entry;
        entry.id = object.object.id;
        entry.section = static_cast<CreditSection>(enum_index_of(object, "section"));
        entry.order = i32_of(object, "order", 100);
        entry.name_key = text_of(object, "name_key");
        entry.name = text_of(object, "name");
        entry.text_key = text_of(object, "text_key");
        entry.text = text_of(object, "text");
        entry.provider = text_of(object, "provider");
        entry.license = text_of(object, "license");
        entry.source = text_of(object, "source");
        entry.asset = text_of(object, "asset");

        check_pair(object, "name_key", "name", /*required=*/true);
        check_pair(object, "text_key", "text", /*required=*/false);
        for (const std::string_view field : {"provider", "license", "source", "asset"}) {
            if (has_field(object, field) && text_of(object, field).empty()) {
                fail(object, field_pos(object, field), std::format("{} must not be empty", field));
            }
        }
        entries_.push_back(std::move(entry));
    }
};

}  // namespace

CreditEntriesResult build_credits(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

CreditsResult load_credits_content(std::span<const data::ContentSource* const> roots) {
    const data::SchemaSet schemas = data::content_schemas();
    data::LoadResult loaded = data::load_content(roots, schemas);
    if (!loaded.ok()) {
        return CreditsResult::err(std::move(loaded.errors));
    }
    CreditEntriesResult entries = build_credits(loaded.registry);
    if (!entries) {
        return CreditsResult::err(entries.error());
    }
    return CreditsResult::ok(CreditsList{std::move(entries).value(), loaded.registry.content_hash()});
}

CreditsResult load_credits_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_credits_content(roots);
}

}  // namespace pat::content
