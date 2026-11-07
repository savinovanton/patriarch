#include "pat/data/content_registry.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pat::data {

namespace {

// ---- content hash ----------------------------------------------------------------------------
//
// Canonical byte form fed to core::HashStream (FNV-1a 64). Every integer is little-endian of a fixed
// width, strings are length-prefixed, and every value starts with an explicit tag byte, so the form
// does not depend on the order of std::variant alternatives. Changing this form requires a new
// magic below.

constexpr std::uint32_t kHashMagic = 0x31484350;  // "PCH1" little-endian

enum class Tag : std::uint8_t {
    Int = 1,
    String = 2,
    Fixed32 = 3,
    Fixed64 = 4,
    Bool = 5,
    Enum = 6,
    Ref = 7,
    Date = 8,
    List = 9,
    Block = 10,
    Map = 11,
};

void feed_string(core::HashStream& h, std::string_view s) {
    h.u64(s.size());
    for (const char c : s)
        h.u8(static_cast<std::uint8_t>(c));
}

void feed_value(core::HashStream& h, const TypedValue& value);

void feed_fields(core::HashStream& h, const std::vector<TypedField>& fields) {
    h.u64(fields.size());
    for (const TypedField& f : fields) {
        // Source positions and the `defaulted` flag are deliberately not hashed: an explicit value
        // equal to the schema default is the same effective content as an omitted one.
        feed_string(h, f.name);
        feed_value(h, f.value);
    }
}

void feed_value(core::HashStream& h, const TypedValue& value) {
    if (const auto* integer = value.get_if<std::int64_t>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Int));
        h.i64(*integer);
    } else if (const auto* text = value.get_if<std::string>()) {
        h.u8(static_cast<std::uint8_t>(Tag::String));
        feed_string(h, *text);
    } else if (const auto* f32 = value.get_if<core::Fixed32>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Fixed32));
        h.fixed(*f32);
    } else if (const auto* f64 = value.get_if<core::Fixed64>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Fixed64));
        h.fixed(*f64);
    } else if (const auto* flag = value.get_if<bool>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Bool));
        h.boolean(*flag);
    } else if (const auto* en = value.get_if<EnumValue>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Enum));
        feed_string(h, en->name);
        h.u64(en->index);
    } else if (const auto* ref = value.get_if<RefValue>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Ref));
        feed_string(h, ref->kind);
        feed_string(h, ref->id);
    } else if (const auto* date = value.get_if<DateValue>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Date));
        h.i32(date->year);
        h.i32(date->month);
        h.i32(date->day);
    } else if (const auto* list = value.get_if<TypedList>()) {
        h.u8(static_cast<std::uint8_t>(Tag::List));
        h.u64(list->size());
        for (const TypedValue& item : *list)
            feed_value(h, item);
    } else if (const auto* block = value.get_if<TypedBlock>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Block));
        feed_fields(h, block->fields);
    } else if (const auto* map = value.get_if<TypedMap>()) {
        h.u8(static_cast<std::uint8_t>(Tag::Map));
        feed_fields(h, map->entries);
    }
}

bool less_object(const ContentObject& a, const ContentObject& b) {
    if (a.object.kind != b.object.kind)
        return a.object.kind < b.object.kind;
    return a.object.id < b.object.id;
}

}  // namespace

// ---- ContentTable ----------------------------------------------------------------------------

const ContentObject* ContentTable::find(std::string_view id) const noexcept {
    const auto it = std::lower_bound(
        objects_.begin(), objects_.end(), id, [](const ContentObject& o, std::string_view key) {
            return std::string_view(o.object.id) < key;
        });
    if (it == objects_.end() || it->object.id != id)
        return nullptr;
    return &*it;
}

const ContentObject* ContentTable::find(core::StringId id, const core::StringInterner& ids) const noexcept {
    if (!id.valid() || id.index() >= ids.size())
        return nullptr;
    return find(ids.view(id));
}

// ---- ContentRegistry -------------------------------------------------------------------------

ContentRegistry ContentRegistry::build(std::vector<std::string> root_names,
                                       const std::vector<std::string>& kinds,
                                       std::vector<ContentObject> objects) {
    ContentRegistry registry;
    registry.root_names_ = std::move(root_names);

    std::sort(objects.begin(), objects.end(), less_object);
    for (std::size_t i = 1; i < objects.size(); ++i) {
        PAT_ASSERT(less_object(objects[i - 1], objects[i]), "ContentRegistry::build: duplicate (kind, id)");
    }

    std::vector<std::string> all_kinds = kinds;
    for (const ContentObject& o : objects)
        all_kinds.push_back(o.object.kind);
    std::sort(all_kinds.begin(), all_kinds.end());
    all_kinds.erase(std::unique(all_kinds.begin(), all_kinds.end()), all_kinds.end());

    registry.tables_.reserve(all_kinds.size());
    for (std::string& kind : all_kinds)
        registry.tables_.emplace_back(std::move(kind));

    // Objects are sorted by (kind, id), so one forward pass fills the tables in order and interns
    // the ids in a content-determined order.
    std::size_t table = 0;
    for (ContentObject& o : objects) {
        while (registry.tables_[table].kind_ != o.object.kind)
            ++table;
        o.id = registry.ids_.intern(o.object.id);
        registry.tables_[table].objects_.push_back(std::move(o));
    }

    core::HashStream h;
    h.u32(kHashMagic);
    std::uint64_t non_empty = 0;
    for (const ContentTable& t : registry.tables_)
        non_empty += t.empty() ? 0U : 1U;
    h.u64(non_empty);
    for (const ContentTable& t : registry.tables_) {
        if (t.empty())
            continue;
        feed_string(h, t.kind_);
        h.u64(t.objects_.size());
        for (const ContentObject& o : t.objects_) {
            feed_string(h, o.object.id);
            feed_fields(h, o.object.body.fields);
        }
    }
    registry.content_hash_ = h.value();
    return registry;
}

const ContentTable* ContentRegistry::table(std::string_view kind) const noexcept {
    const auto it = std::lower_bound(
        tables_.begin(), tables_.end(), kind, [](const ContentTable& t, std::string_view key) {
            return std::string_view(t.kind_) < key;
        });
    if (it == tables_.end() || it->kind_ != kind)
        return nullptr;
    return &*it;
}

const ContentObject* ContentRegistry::find(std::string_view kind, std::string_view id) const noexcept {
    const ContentTable* t = table(kind);
    return t == nullptr ? nullptr : t->find(id);
}

std::size_t ContentRegistry::object_count() const noexcept {
    std::size_t n = 0;
    for (const ContentTable& t : tables_)
        n += t.size();
    return n;
}

}  // namespace pat::data
