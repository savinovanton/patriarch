#include "pat/sim/world.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <format>

namespace pat::sim {

namespace {

template <class Id>
void hash_table_slots(core::HashStream& h, const EntityTable<Id>& ids) {
    h.u64(ids.capacity());
    h.u64(ids.alive_count());
    for (const std::uint32_t slot : ids.free_list()) {  // free-list order decides future ids
        h.u32(slot);
    }
    h.bytes(ids.alive_flags());
}

}  // namespace

World::World(Date start, std::uint64_t seed) : calendar_(start), rng_(seed) {}

// ---- dynasties ----------------------------------------------------------------------------------

DynastyId World::create_dynasty(core::StringId name) {
    const DynastyId id(static_cast<std::uint32_t>(dynasties_.size()));
    dynasties_.name.push_back(name);
    dynasties_.head.emplace_back();
    dynasties_.honor.push_back(0);
    members_.emplace_back();
    owned_provinces_.emplace_back();
    return id;
}

bool World::set_dynasty_head(DynastyId dynasty, CharacterId head) {
    if (!exists(dynasty)) {
        return false;
    }
    if (head.valid()) {
        if (!is_alive(head) || characters_.dynasty[head.index()] != dynasty) {
            return false;
        }
    }
    dynasties_.head[dynasty.index()] = head;
    return true;
}

void World::add_honor(DynastyId dynasty, std::int32_t delta) {
    PAT_ASSERT(exists(dynasty), "add_honor: unknown dynasty");
    if (!exists(dynasty)) {
        return;
    }
    const std::int64_t value = static_cast<std::int64_t>(dynasties_.honor[dynasty.index()]) + delta;
    dynasties_.honor[dynasty.index()] =
        static_cast<std::int32_t>(std::clamp<std::int64_t>(value, kMinHonor, kMaxHonor));
}

// ---- provinces ----------------------------------------------------------------------------------

ProvinceId World::create_province(core::StringId name, DynastyId owner, core::Fixed32 population) {
    PAT_ASSERT(!owner.valid() || exists(owner), "create_province: unknown owner");
    const ProvinceId id(static_cast<std::uint32_t>(provinces_.size()));
    provinces_.name.push_back(name);
    provinces_.owner.push_back(owner);
    provinces_.population.push_back(population);
    provinces_.development.push_back(1);
    if (exists(owner)) {
        owned_provinces_[owner.index()].insert(id);
    } else {
        provinces_.owner.back() = DynastyId();
    }
    return id;
}

void World::set_province_owner(ProvinceId province, DynastyId owner) {
    PAT_ASSERT(exists(province), "set_province_owner: unknown province");
    PAT_ASSERT(!owner.valid() || exists(owner), "set_province_owner: unknown owner");
    if (!exists(province)) {
        return;
    }
    DynastyId& current = provinces_.owner[province.index()];
    if (exists(current)) {
        owned_provinces_[current.index()].erase(province);
    }
    current = exists(owner) ? owner : DynastyId();
    if (current.valid()) {
        owned_provinces_[current.index()].insert(province);
    }
}

// ---- characters ---------------------------------------------------------------------------------

CharacterId World::create_character(const CharacterInit& init) {
    PAT_ASSERT(!init.dynasty.valid() || exists(init.dynasty), "create_character: unknown dynasty");
    const CharacterId id = characters_.ids.create();
    const std::size_t index = id.index();
    if (index >= characters_.dynasty.size()) {  // fresh slot: grow every column
        characters_.dynasty.emplace_back();
        characters_.birth_date.emplace_back();
        characters_.sex.emplace_back();
        characters_.health.emplace_back();
        characters_.father.emplace_back();
        characters_.mother.emplace_back();
    }
    const DynastyId dynasty = exists(init.dynasty) ? init.dynasty : DynastyId();
    characters_.dynasty[index] = dynasty;
    characters_.birth_date[index] = init.birth_date;
    characters_.sex[index] = init.sex;
    characters_.health[index] = init.health;
    characters_.father[index] = init.father;
    characters_.mother[index] = init.mother;
    if (dynasty.valid()) {
        members_[dynasty.index()].insert(id);
    }
    return id;
}

bool World::destroy_character(CharacterId id) {
    if (!characters_.ids.is_alive(id)) {
        return false;
    }
    const std::size_t index = id.index();
    const DynastyId dynasty = characters_.dynasty[index];
    if (dynasty.valid()) {
        members_[dynasty.index()].erase(id);
        if (dynasties_.head[dynasty.index()] == id) {
            dynasties_.head[dynasty.index()] = CharacterId();
        }
    }
    // Reset the slot so that a dead record never leaks old values into later reuse.
    characters_.dynasty[index] = DynastyId();
    characters_.birth_date[index] = Date();
    characters_.sex[index] = Sex::Male;
    characters_.health[index] = core::Fixed32();
    characters_.father[index] = CharacterId();
    characters_.mother[index] = CharacterId();
    return characters_.ids.destroy(id);
}

void World::set_character_dynasty(CharacterId id, DynastyId dynasty) {
    PAT_ASSERT(is_alive(id), "set_character_dynasty: dead character");
    PAT_ASSERT(!dynasty.valid() || exists(dynasty), "set_character_dynasty: unknown dynasty");
    if (!is_alive(id)) {
        return;
    }
    DynastyId& current = characters_.dynasty[id.index()];
    if (current.valid()) {
        members_[current.index()].erase(id);
        if (dynasties_.head[current.index()] == id) {
            dynasties_.head[current.index()] = CharacterId();
        }
    }
    current = exists(dynasty) ? dynasty : DynastyId();
    if (current.valid()) {
        members_[current.index()].insert(id);
    }
}

// ---- indexes ------------------------------------------------------------------------------------

std::span<const CharacterId> World::members_of(DynastyId dynasty) const noexcept {
    return exists(dynasty) ? members_[dynasty.index()].values() : std::span<const CharacterId>();
}

std::span<const ProvinceId> World::provinces_of(DynastyId dynasty) const noexcept {
    return exists(dynasty) ? owned_provinces_[dynasty.index()].values() : std::span<const ProvinceId>();
}

// ---- hashing ------------------------------------------------------------------------------------

WorldHash World::compute_hash() const {
    WorldHash out;

    {
        core::HashStream h;
        h.i32(calendar_.today().days());
        h.i64(calendar_.tick_count());
        out.calendar = h.value();
    }
    {
        core::HashStream h;
        const core::RngSnapshot snap = rng_.save();
        h.u64(snap.master_seed);
        for (const core::RngStreamState& s : snap.streams) {
            h.u64(s.key);
            h.u64(s.state);
            h.u64(s.inc);
        }
        out.rng = h.value();
    }
    {
        core::HashStream h;
        h.u64(dynasties_.size());
        for (std::size_t i = 0; i < dynasties_.size(); ++i) {
            h.id(dynasties_.name[i]);
            h.id(dynasties_.head[i]);
            h.i32(dynasties_.honor[i]);
            // index contents in their (deterministic) order
            h.u64(members_[i].size());
            for (const CharacterId c : members_[i].values()) {
                h.id(c);
            }
            h.u64(owned_provinces_[i].size());
            for (const ProvinceId p : owned_provinces_[i].values()) {
                h.id(p);
            }
        }
        out.dynasties = h.value();
    }
    {
        core::HashStream h;
        h.u64(provinces_.size());
        for (std::size_t i = 0; i < provinces_.size(); ++i) {
            h.id(provinces_.name[i]);
            h.id(provinces_.owner[i]);
            h.fixed(provinces_.population[i]);
            h.u8(provinces_.development[i]);
        }
        out.provinces = h.value();
    }
    {
        core::HashStream h;
        hash_table_slots(h, characters_.ids);
        const std::size_t capacity = characters_.ids.capacity();
        for (std::size_t i = 0; i < capacity; ++i) {
            if (characters_.ids.alive_flags()[i] == 0) {
                continue;
            }
            h.id(characters_.dynasty[i]);
            h.i32(characters_.birth_date[i].days());
            h.u8(static_cast<std::uint8_t>(characters_.sex[i]));
            h.fixed(characters_.health[i]);
            h.id(characters_.father[i]);
            h.id(characters_.mother[i]);
        }
        out.characters = h.value();
    }

    core::HashStream total;
    total.u64(out.calendar);
    total.u64(out.rng);
    total.u64(out.dynasties);
    total.u64(out.provinces);
    total.u64(out.characters);
    out.total = total.value();
    return out;
}

// ---- invariants ---------------------------------------------------------------------------------

std::vector<std::string> World::check_invariants() const {
    std::vector<std::string> bad;
    auto fail = [&](std::string message) { bad.push_back(std::move(message)); };

    const std::size_t capacity = characters_.ids.capacity();
    if (characters_.dynasty.size() != capacity || characters_.birth_date.size() != capacity ||
        characters_.sex.size() != capacity || characters_.health.size() != capacity ||
        characters_.father.size() != capacity || characters_.mother.size() != capacity) {
        fail("character columns differ in size from the id table");
        return bad;  // everything below indexes the columns
    }
    if (dynasties_.head.size() != dynasties_.size() || dynasties_.honor.size() != dynasties_.size() ||
        members_.size() != dynasties_.size() || owned_provinces_.size() != dynasties_.size()) {
        fail("dynasty columns or indexes differ in size");
        return bad;
    }
    if (provinces_.owner.size() != provinces_.size() || provinces_.population.size() != provinces_.size() ||
        provinces_.development.size() != provinces_.size()) {
        fail("province columns differ in size");
        return bad;
    }

    // free list: unique, dead slots only
    std::vector<std::uint32_t> free_sorted(characters_.ids.free_list().begin(),
                                           characters_.ids.free_list().end());
    std::sort(free_sorted.begin(), free_sorted.end());
    if (std::adjacent_find(free_sorted.begin(), free_sorted.end()) != free_sorted.end()) {
        fail("free list contains duplicates");
    }
    for (const std::uint32_t slot : free_sorted) {
        if (slot >= capacity || characters_.ids.alive_flags()[slot] != 0) {
            fail(std::format("free list entry {} is out of range or alive", slot));
        }
    }
    if (characters_.ids.alive_count() + free_sorted.size() != capacity) {
        fail("alive count + free list size != capacity");
    }

    // characters vs member index
    std::size_t indexed = 0;
    for (std::size_t d = 0; d < dynasties_.size(); ++d) {
        for (const CharacterId c : members_[d].values()) {
            ++indexed;
            if (!is_alive(c)) {
                fail(std::format("dynasty {} indexes dead character {}", d, c.value()));
            } else if (characters_.dynasty[c.index()].value() != d) {
                fail(std::format(
                    "character {} is indexed under dynasty {} but belongs elsewhere", c.value(), d));
            }
        }
    }
    std::size_t expected_indexed = 0;
    characters_.ids.for_each_alive([&](CharacterId c) {
        const DynastyId d = characters_.dynasty[c.index()];
        if (d.valid()) {
            ++expected_indexed;
            if (!exists(d)) {
                fail(std::format("character {} has unknown dynasty {}", c.value(), d.value()));
            } else if (!members_[d.index()].contains(c)) {
                fail(std::format(
                    "character {} missing from the member index of dynasty {}", c.value(), d.value()));
            }
        }
    });
    if (indexed != expected_indexed) {
        fail(std::format(
            "member index holds {} entries but {} characters have a dynasty", indexed, expected_indexed));
    }

    // dynasty heads
    for (std::size_t d = 0; d < dynasties_.size(); ++d) {
        const CharacterId head = dynasties_.head[d];
        if (head.valid() && (!is_alive(head) || characters_.dynasty[head.index()].value() != d)) {
            fail(std::format("dynasty {} head {} is dead or not a member", d, head.value()));
        }
        if (dynasties_.honor[d] < kMinHonor || dynasties_.honor[d] > kMaxHonor) {
            fail(std::format("dynasty {} honor {} out of range", d, dynasties_.honor[d]));
        }
    }

    // provinces vs ownership index
    std::size_t owned_indexed = 0;
    for (std::size_t d = 0; d < dynasties_.size(); ++d) {
        for (const ProvinceId p : owned_provinces_[d].values()) {
            ++owned_indexed;
            if (!exists(p) || provinces_.owner[p.index()].value() != d) {
                fail(std::format("dynasty {} indexes province {} it does not own", d, p.value()));
            }
        }
    }
    std::size_t expected_owned = 0;
    for (std::size_t p = 0; p < provinces_.size(); ++p) {
        const DynastyId owner = provinces_.owner[p];
        if (owner.valid()) {
            ++expected_owned;
            if (!exists(owner) ||
                !owned_provinces_[owner.index()].contains(ProvinceId(static_cast<std::uint32_t>(p)))) {
                fail(std::format("province {} owner {} index mismatch", p, owner.value()));
            }
        }
        if (provinces_.population[p].is_negative()) {
            fail(std::format("province {} has negative population", p));
        }
    }
    if (owned_indexed != expected_owned) {
        fail("ownership index size differs from the number of owned provinces");
    }

    return bad;
}

}  // namespace pat::sim
