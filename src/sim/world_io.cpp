#include "pat/sim/world.hpp"

#include <string>
#include <utility>

namespace pat::sim {

namespace {

using R = core::Result<World, core::Error>;

R fail(const char* message) {
    return R::err(core::Error{10, message});
}

/// True when `count` entries of at least `bytes_each` bytes could still be present in the input;
/// guards allocations against absurd counts in damaged files.
bool plausible(const core::ByteReader& in, std::uint32_t count, std::size_t bytes_each) {
    return in.ok() && static_cast<std::uint64_t>(count) * bytes_each <= in.remaining();
}

}  // namespace

void World::save_state(core::ByteWriter& out) const {
    out.u32(kStateSchema);

    // clock and randomness
    out.i32(calendar_.today().days());
    out.i64(calendar_.tick_count());
    const core::RngSnapshot rng = rng_.save();
    out.u64(rng.master_seed);
    out.u32(static_cast<std::uint32_t>(rng.streams.size()));
    for (const core::RngStreamState& s : rng.streams) {
        out.u64(s.key);
        out.u64(s.state);
        out.u64(s.inc);
    }

    // dynasties and their index lists (dense order matters)
    out.u32(static_cast<std::uint32_t>(dynasties_.size()));
    for (std::size_t i = 0; i < dynasties_.size(); ++i) {
        out.u32(dynasties_.name[i].value());
        out.u32(dynasties_.head[i].value());
        out.i32(dynasties_.honor[i]);
    }
    for (std::size_t i = 0; i < dynasties_.size(); ++i) {
        out.u32(static_cast<std::uint32_t>(members_[i].size()));
        for (const CharacterId c : members_[i].values()) {
            out.u32(c.value());
        }
        out.u32(static_cast<std::uint32_t>(owned_provinces_[i].size()));
        for (const ProvinceId p : owned_provinces_[i].values()) {
            out.u32(p.value());
        }
    }

    // provinces
    out.u32(static_cast<std::uint32_t>(provinces_.size()));
    for (std::size_t i = 0; i < provinces_.size(); ++i) {
        out.u32(provinces_.name[i].value());
        out.u32(provinces_.owner[i].value());
        out.i32(provinces_.population[i].raw());
        out.u8(provinces_.development[i]);
    }

    // characters: slot table first, then one record per slot (dead slots hold reset values)
    const std::size_t capacity = characters_.ids.capacity();
    out.u32(static_cast<std::uint32_t>(capacity));
    out.bytes(characters_.ids.alive_flags());
    for (const std::uint32_t g : characters_.ids.generations()) {
        out.u32(g);
    }
    out.u32(static_cast<std::uint32_t>(characters_.ids.free_list().size()));
    for (const std::uint32_t slot : characters_.ids.free_list()) {
        out.u32(slot);
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        out.u32(characters_.dynasty[i].value());
        out.i32(characters_.birth_date[i].days());
        out.u8(static_cast<std::uint8_t>(characters_.sex[i]));
        out.i32(characters_.health[i].raw());
        out.u32(characters_.father[i].value());
        out.u32(characters_.mother[i].value());
    }
}

core::Result<World, core::Error> World::load_state(core::ByteReader& in) {
    if (in.u32() != kStateSchema || !in.ok()) {
        return fail("unsupported or missing world state schema");
    }

    const Date today = Date::from_days(in.i32());
    const std::int64_t ticks = in.i64();
    if (!in.ok() || ticks < 0) {
        return fail("invalid clock in world state");
    }

    core::RngSnapshot rng;
    rng.master_seed = in.u64();
    const std::uint32_t stream_count = in.u32();
    if (!plausible(in, stream_count, 24)) {
        return fail("invalid random stream table");
    }
    rng.streams.reserve(stream_count);
    for (std::uint32_t i = 0; i < stream_count; ++i) {
        core::RngStreamState s;
        s.key = in.u64();
        s.state = in.u64();
        s.inc = in.u64();
        if (!rng.streams.empty() && s.key <= rng.streams.back().key) {
            return fail("random streams are not sorted by key");
        }
        rng.streams.push_back(s);
    }

    World world(today, rng.master_seed);
    world.calendar_ = TickCalendar::restore(today, ticks);
    world.rng_.restore(rng);

    // dynasties
    const std::uint32_t dynasty_count = in.u32();
    if (!plausible(in, dynasty_count, 12 + 8)) {
        return fail("invalid dynasty table");
    }
    for (std::uint32_t i = 0; i < dynasty_count; ++i) {
        world.dynasties_.name.emplace_back(in.u32());
        world.dynasties_.head.emplace_back(in.u32());
        world.dynasties_.honor.push_back(in.i32());
        world.members_.emplace_back();
        world.owned_provinces_.emplace_back();
    }
    std::vector<std::vector<std::uint32_t>> member_lists(dynasty_count);
    std::vector<std::vector<std::uint32_t>> owned_lists(dynasty_count);
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        for (auto* list : {&member_lists[d], &owned_lists[d]}) {
            const std::uint32_t n = in.u32();
            if (!plausible(in, n, 4)) {
                return fail("invalid dynasty index list");
            }
            list->reserve(n);
            for (std::uint32_t k = 0; k < n; ++k) {
                list->push_back(in.u32());
            }
        }
    }

    // provinces
    const std::uint32_t province_count = in.u32();
    if (!plausible(in, province_count, 13)) {
        return fail("invalid province table");
    }
    for (std::uint32_t i = 0; i < province_count; ++i) {
        world.provinces_.name.emplace_back(in.u32());
        world.provinces_.owner.emplace_back(in.u32());
        world.provinces_.population.push_back(core::Fixed32::from_raw(in.i32()));
        world.provinces_.development.push_back(in.u8());
    }

    // characters
    const std::uint32_t capacity = in.u32();
    if (!plausible(in, capacity, 1 + 4 + 21)) {
        return fail("invalid character table");
    }
    const auto alive_span = in.bytes(capacity);
    std::vector<std::uint8_t> alive(alive_span.begin(), alive_span.end());
    std::vector<std::uint32_t> generation(capacity);
    for (auto& g : generation) {
        g = in.u32();
    }
    const std::uint32_t free_count = in.u32();
    if (!plausible(in, free_count, 4)) {
        return fail("invalid free list");
    }
    std::vector<std::uint32_t> free_list(free_count);
    for (auto& slot : free_list) {
        slot = in.u32();
    }
    if (!in.ok() ||
        !world.characters_.ids.restore(std::move(alive), std::move(generation), std::move(free_list))) {
        return fail("inconsistent character slot table");
    }
    CharacterTable& chars = world.characters_;
    chars.dynasty.reserve(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        chars.dynasty.emplace_back(in.u32());
        chars.birth_date.push_back(Date::from_days(in.i32()));
        const std::uint8_t sex = in.u8();
        if (sex > 1) {
            return fail("invalid sex value");
        }
        chars.sex.push_back(static_cast<Sex>(sex));
        chars.health.push_back(core::Fixed32::from_raw(in.i32()));
        const CharacterId father(in.u32());
        const CharacterId mother(in.u32());
        if ((father.valid() && father.value() >= capacity) ||
            (mother.valid() && mother.value() >= capacity)) {
            return fail("parent reference out of range");
        }
        chars.father.push_back(father);
        chars.mother.push_back(mother);
    }
    if (!in.ok()) {
        return fail("world state is truncated");
    }

    // Rebuild the index lists in the saved order.
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        for (const std::uint32_t c : member_lists[d]) {
            if (c >= capacity || !world.characters_.ids.is_alive(CharacterId(c)) ||
                chars.dynasty[c].value() != d || !world.members_[d].insert(CharacterId(c))) {
                return fail("inconsistent member index");
            }
        }
        for (const std::uint32_t p : owned_lists[d]) {
            if (p >= province_count || world.provinces_.owner[p].value() != d ||
                !world.owned_provinces_[d].insert(ProvinceId(p))) {
                return fail("inconsistent province ownership index");
            }
        }
    }

    const std::vector<std::string> problems = world.check_invariants();
    if (!problems.empty()) {
        return fail("world state violates invariants");
    }
    return R::ok(std::move(world));
}

}  // namespace pat::sim
