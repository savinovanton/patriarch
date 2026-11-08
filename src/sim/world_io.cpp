#include "pat/sim/world.hpp"

#include <string>
#include <utility>

#include "state_sections.hpp"

// Binary layout of the world state (ADR-0006 D6). All integers are little endian.
//
//   u32 schema                       1..kStateSchema
//   core body                        clock, random streams, dynasties and index lists, provinces,
//                                    character slot table and one E1 record per slot  (schema 1)
//   -- extension block, schema >= 2 --
//   u32 world_flags                  bit 0 = dynasty mode, other bits must be 0
//   u16 section_count                at most kMaxSections
//   section_count x { u16 id | u16 version | u32 byte_length | bytes }   ids strictly ascending
//
// A section is read by its own reader and must be consumed completely. An unknown id, an
// unsupported version, a section newer than the file's schema or a broken order is an error.
// A section whose content equals the defaults is not written at all.
//
//   section 1 "person"   u32 rows | rows x person row (90 bytes), only rows that differ from the
//                        defaults, ascending by id
//   section 2 "dynasty"  u32 rows | rows x { u32 id | i32 unity | u8 law }  (rows that differ from the
//                        defaults) | u32 branches | branches x { u32 name | u32 dynasty }
//   section 3 "kin"      u32 rows | rows x kin row (29 bytes), only rows that differ from the defaults,
//                        ascending by id: u32 id | u32 spouse | u8 marriages | 3 x u32 former spouse |
//                        2 x u32 godparent.  Parents are the father/mother of the core body; the child
//                        lists are derived.  (schema >= 3)
//
// Schema 4 (E7-T04) changes no byte layout: the slot table of the core body may now hold the state 2
// (archived, a dead person in dynasty mode). An archived row keeps its core record (health 0), its
// rows in sections 1 (identity, death date and cause, group S at the defaults) and 3 (marriages that
// ended); it appears in no member list. Older schemas cannot contain the state 2.
//
// Schema 5 (E9-T03) adds the economy as sections 32..39 (world_economy_io.cpp). They are written only
// when the economy is on, so every world that does not use it saves the same bytes as under schema 4.
// A file of an older schema has none and migrates to a world without economy.
//
// Derived data (the list of the living, the child lists, the life-event queue, the running hash of the
// archive) is never stored; it is rebuilt after loading and checked together with all other invariants.

namespace pat::sim {

namespace {

using R = core::Result<World, core::Error>;

R fail(const char* message) {
    return R::err(core::Error{10, message});
}

using detail::kSectionDynasty;
using detail::kSectionKin;
using detail::kSectionPerson;
using detail::plausible;

constexpr std::uint16_t kMaxSections = 32;
constexpr std::size_t kPersonRowBytes = 90;
constexpr std::size_t kDynastyRowBytes = 9;
constexpr std::size_t kBranchRowBytes = 8;
constexpr std::size_t kKinRowBytes = 4 + 4 + 1 + 4 * kMaxExSpouses + 4 * kGodparentSlots;

struct SectionInfo {
    std::uint16_t id;
    std::uint16_t version;     // the one version this build can read
    std::uint32_t min_schema;  // the section cannot appear in older schemas
};
constexpr SectionInfo kSections[] = {
    {kSectionPerson, 1, 2},
    {kSectionDynasty, 1, 2},
    {kSectionKin, 1, 3},
    {detail::kSectionEconomy, 1, 5},
    {detail::kSectionEconomyDynasty, 1, 5},
    {detail::kSectionEconomyProvince, 1, 5},
    {detail::kSectionEconomyPopulation, 1, 5},
    {detail::kSectionEconomyAccounts, 1, 5},
    {detail::kSectionEconomyConstruction, 1, 5},
    {detail::kSectionEconomyCredit, 1, 5},
    {detail::kSectionEconomyIncidents, 1, 5},
};

const SectionInfo* find_section(std::uint16_t id) {
    for (const SectionInfo& info : kSections) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

void write_section(core::ByteWriter& out,
                   std::uint16_t id,
                   std::uint16_t version,
                   const core::ByteWriter& body) {
    out.u16(id);
    out.u16(version);
    out.u32(static_cast<std::uint32_t>(body.size()));
    out.bytes(body.data());
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

    // dynasties and their index lists (dense order matters; members are the living only)
    out.u32(static_cast<std::uint32_t>(dynasties_.size()));
    for (std::size_t i = 0; i < dynasties_.size(); ++i) {
        out.u32(dynasties_.name[i].value());
        out.u32(dynasties_.head[i].value());
        out.i32(dynasties_.honor[i]);
    }
    for (std::size_t i = 0; i < dynasties_.size(); ++i) {
        out.u32(static_cast<std::uint32_t>(members_[i].size()));
        for (const CharacterId c : members_[i]) {
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

    // characters: slot table first (0 free, 1 alive, 2 archived), then one record per slot (free slots
    // hold reset values, archived ones their identity and the facts of the tree)
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

    // ---- extension block (schema 2) ----
    out.u32(flags_);

    using Section = StateSection;
    std::vector<Section> sections;

    {  // section 1: the columns of E7-T02, sparse by id
        std::uint32_t rows = 0;
        for (std::size_t i = 0; i < capacity; ++i) {
            rows += character_extension_is_default(i) ? 0U : 1U;
        }
        if (rows != 0) {
            Section section{kSectionPerson, 1, {}};
            core::ByteWriter& w = section.body;
            const CharacterTable& c = characters_;
            w.u32(rows);
            for (std::size_t i = 0; i < capacity; ++i) {
                if (character_extension_is_default(i)) {
                    continue;
                }
                w.u32(static_cast<std::uint32_t>(i));
                w.u32(c.name[i].value());
                w.u32(c.branch[i].value());
                w.u16(c.portrait[i]);
                w.u8(static_cast<std::uint8_t>(c.legitimacy[i]));
                w.u8(c.cause_of_death[i]);
                w.i32(c.death_date[i].days());
                for (const core::Fixed32 skill : c.skills[i]) {
                    w.i32(skill.raw());
                }
                w.i32(c.fertility[i].raw());
                w.i32(c.stress[i].raw());
                w.i32(c.piety[i].raw());
                w.i32(c.ambition[i].raw());
                w.i32(c.loyalty_head[i].raw());
                w.i32(c.loyalty_emperor[i].raw());
                for (const std::uint16_t trait : c.traits[i]) {
                    w.u16(trait);
                }
                w.u8(c.education_focus[i]);
                w.u32(c.education_tutor[i].value());
                w.u8(c.education_level[i]);
                w.i32(c.planned_death[i].days());
            }
            sections.push_back(std::move(section));
        }
    }
    {  // section 2: unity, succession law, branches
        std::uint32_t rows = 0;
        for (std::size_t d = 0; d < dynasties_.size(); ++d) {
            rows += (dynasties_.unity[d] == kDefaultUnity &&
                     dynasties_.succession_law[d] == SuccessionLaw::Majorat)
                        ? 0U
                        : 1U;
        }
        if (rows != 0 || branches_.size() != 0) {
            Section section{kSectionDynasty, 1, {}};
            core::ByteWriter& w = section.body;
            w.u32(rows);
            for (std::size_t d = 0; d < dynasties_.size(); ++d) {
                if (dynasties_.unity[d] == kDefaultUnity &&
                    dynasties_.succession_law[d] == SuccessionLaw::Majorat) {
                    continue;
                }
                w.u32(static_cast<std::uint32_t>(d));
                w.i32(dynasties_.unity[d].raw());
                w.u8(static_cast<std::uint8_t>(dynasties_.succession_law[d]));
            }
            w.u32(static_cast<std::uint32_t>(branches_.size()));
            for (std::size_t b = 0; b < branches_.size(); ++b) {
                w.u32(branches_.name[b].value());
                w.u32(branches_.dynasty[b].value());
            }
            sections.push_back(std::move(section));
        }
    }

    {  // section 3: kinship, sparse by id
        std::uint32_t rows = 0;
        for (std::size_t i = 0; i < capacity; ++i) {
            rows += kin_is_default(i) ? 0U : 1U;
        }
        if (rows != 0) {
            Section section{kSectionKin, 1, {}};
            core::ByteWriter& w = section.body;
            const CharacterTable& c = characters_;
            w.u32(rows);
            for (std::size_t i = 0; i < capacity; ++i) {
                if (kin_is_default(i)) {
                    continue;
                }
                w.u32(static_cast<std::uint32_t>(i));
                w.u32(c.spouse[i].value());
                w.u8(c.marriages[i]);
                for (const CharacterId ex : c.ex_spouses[i]) {
                    w.u32(ex.value());
                }
                for (const CharacterId godparent : c.godparent[i]) {
                    w.u32(godparent.value());
                }
            }
            sections.push_back(std::move(section));
        }
    }

    economy_write_sections(sections);  // ids 32..39, after the sections above

    out.u16(static_cast<std::uint16_t>(sections.size()));
    for (const Section& section : sections) {
        write_section(out, section.id, section.version, section.body);
    }
}

core::Result<World, core::Error> World::load_state(core::ByteReader& in) {
    const std::uint32_t schema = in.u32();
    if (!in.ok() || schema == 0) {
        return fail("unsupported or missing world state schema");
    }
    if (schema > kStateSchema) {
        return fail("the world state was written by a newer version of the game");
    }

    const Date today = Date::from_days(in.i32());
    const std::int64_t ticks = in.i64();
    if (!in.ok() || ticks < 0 || !is_sane_date(today)) {
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

    // dynasties (schema 1 has no unity or law: they take their defaults, see the dynasty section)
    const std::uint32_t dynasty_count = in.u32();
    if (!plausible(in, dynasty_count, 12 + 8)) {
        return fail("invalid dynasty table");
    }
    for (std::uint32_t i = 0; i < dynasty_count; ++i) {
        world.dynasties_.name.emplace_back(in.u32());
        world.dynasties_.head.emplace_back(in.u32());
        world.dynasties_.honor.push_back(in.i32());
        world.dynasties_.unity.push_back(kDefaultUnity);
        world.dynasties_.succession_law.push_back(SuccessionLaw::Majorat);
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
    if (capacity > kMaxCharacterIds || !plausible(in, capacity, 1 + 4 + 21)) {
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
    // Schema 4 (E7-T04) introduced archived rows (slot state 2); no older file can hold one.
    if (schema < 4 && world.characters_.ids.retired_count() != 0) {
        return fail("archived character rows need state schema 4");
    }
    CharacterTable& chars = world.characters_;
    for (std::uint32_t i = 0; i < capacity; ++i) {
        world.grow_character_columns();  // E7-T02 columns start at their defaults
        chars.dynasty[i] = DynastyId(in.u32());
        chars.birth_date[i] = Date::from_days(in.i32());
        const std::uint8_t sex = in.u8();
        if (sex > 1) {
            return fail("invalid sex value");
        }
        chars.sex[i] = static_cast<Sex>(sex);
        chars.health[i] = core::Fixed32::from_raw(in.i32());
        const CharacterId father(in.u32());
        const CharacterId mother(in.u32());
        if ((father.valid() && father.value() >= capacity) ||
            (mother.valid() && mother.value() >= capacity)) {
            return fail("parent reference out of range");
        }
        chars.father[i] = father;
        chars.mother[i] = mother;
    }
    if (!in.ok()) {
        return fail("world state is truncated");
    }

    // Rebuild the index lists in the saved order.
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        for (const std::uint32_t c : member_lists[d]) {
            if (c >= capacity || !world.characters_.ids.is_alive(CharacterId(c)) ||
                chars.dynasty[c].value() != d || world.member_pos_[c] != kNoMember) {
                return fail("inconsistent member index");
            }
            world.member_add(DynastyId(d), CharacterId(c));
        }
        for (const std::uint32_t p : owned_lists[d]) {
            if (p >= province_count || world.provinces_.owner[p].value() != d ||
                !world.owned_provinces_[d].insert(ProvinceId(p))) {
                return fail("inconsistent province ownership index");
            }
        }
    }

    // ---- migration steps ----
    // Schema 1 -> 2: there is no extension block. Flags are 0 (E1 mode), every E7-T02 column keeps
    // its default and the derived data is built below. Schema 2 -> 3 (E7-T03): there is no kin
    // section; every spouse, marriage counter, former spouse and godparent stays at its default and
    // the child lists are derived from the parent columns that schema 1 already had. Parents of a
    // dynasty-mode world are checked by check_invariants(). Later schemas add their own
    // "if (schema >= N)" block here, in ascending order, so that any old file passes all steps.
    if (schema >= 2) {
        const std::uint32_t flags = in.u32();
        if (!in.ok() || (flags & ~kWorldFlagDynastyMode) != 0) {
            return fail("unknown world flags");
        }
        world.flags_ = flags;

        const std::uint16_t section_count = in.u16();
        if (!in.ok() || section_count > kMaxSections) {
            return fail("invalid section count");
        }
        std::uint16_t previous_id = 0;
        for (std::uint16_t s = 0; s < section_count; ++s) {
            const std::uint16_t id = in.u16();
            const std::uint16_t version = in.u16();
            const std::uint32_t length = in.u32();
            if (!in.ok() || id <= previous_id) {
                return fail("world state sections are malformed or out of order");
            }
            previous_id = id;
            const SectionInfo* info = find_section(id);
            if (info == nullptr || info->version != version || info->min_schema > schema) {
                return fail("unknown or unsupported world state section");
            }
            const std::span<const std::uint8_t> bytes = in.bytes(length);
            if (!in.ok()) {
                return fail("world state section is truncated");
            }
            core::ByteReader sr(bytes);

            if (id == kSectionPerson) {
                const std::uint32_t rows = sr.u32();
                if (!plausible(sr, rows, kPersonRowBytes)) {
                    return fail("invalid person section");
                }
                bool have_previous = false;
                std::uint32_t previous_row = 0;
                for (std::uint32_t r = 0; r < rows; ++r) {
                    const std::uint32_t row = sr.u32();
                    if (!sr.ok() || row >= capacity || (have_previous && row <= previous_row)) {
                        return fail("person rows are out of range or out of order");
                    }
                    have_previous = true;
                    previous_row = row;
                    chars.name[row] = core::StringId(sr.u32());
                    chars.branch[row] = BranchId(sr.u32());
                    chars.portrait[row] = sr.u16();
                    const std::uint8_t legitimacy = sr.u8();
                    if (legitimacy >= kLegitimacyCount) {
                        return fail("invalid legitimacy value");
                    }
                    chars.legitimacy[row] = static_cast<Legitimacy>(legitimacy);
                    chars.cause_of_death[row] = sr.u8();
                    chars.death_date[row] = Date::from_days(sr.i32());
                    for (core::Fixed32& skill : chars.skills[row]) {
                        skill = core::Fixed32::from_raw(sr.i32());
                    }
                    chars.fertility[row] = core::Fixed32::from_raw(sr.i32());
                    chars.stress[row] = core::Fixed32::from_raw(sr.i32());
                    chars.piety[row] = core::Fixed32::from_raw(sr.i32());
                    chars.ambition[row] = core::Fixed32::from_raw(sr.i32());
                    chars.loyalty_head[row] = core::Fixed32::from_raw(sr.i32());
                    chars.loyalty_emperor[row] = core::Fixed32::from_raw(sr.i32());
                    for (std::uint16_t& trait : chars.traits[row]) {
                        trait = sr.u16();
                    }
                    chars.education_focus[row] = sr.u8();
                    chars.education_tutor[row] = CharacterId(sr.u32());
                    chars.education_level[row] = sr.u8();
                    chars.planned_death[row] = Date::from_days(sr.i32());
                    // Range checks that depend on the rest of the world (branch exists and belongs
                    // to the dynasty, scales, traits) are done by check_invariants() below.
                }
            } else if (id == kSectionKin) {
                const std::uint32_t rows = sr.u32();
                if (!plausible(sr, rows, kKinRowBytes)) {
                    return fail("invalid kin section");
                }
                // A reference is either "none" or an id inside the table; whether it points to a
                // sensible person (alive, symmetric, packed) is for check_invariants().
                const auto reference = [&](CharacterId& out_id) {
                    out_id = CharacterId(sr.u32());
                    return !out_id.valid() || out_id.value() < capacity;
                };
                bool have_previous = false;
                std::uint32_t previous_row = 0;
                for (std::uint32_t r = 0; r < rows; ++r) {
                    const std::uint32_t row = sr.u32();
                    if (!sr.ok() || row >= capacity || (have_previous && row <= previous_row)) {
                        return fail("kin rows are out of range or out of order");
                    }
                    have_previous = true;
                    previous_row = row;
                    bool in_range = reference(chars.spouse[row]);
                    chars.marriages[row] = sr.u8();
                    for (CharacterId& ex : chars.ex_spouses[row]) {
                        in_range = reference(ex) && in_range;
                    }
                    for (CharacterId& godparent : chars.godparent[row]) {
                        in_range = reference(godparent) && in_range;
                    }
                    if (!in_range) {
                        return fail("kin row refers to a person outside the table");
                    }
                    if (chars.marriages[row] > kMaxMarriages) {
                        return fail("invalid marriage counter");
                    }
                }
            } else if (id >= detail::kSectionEconomy) {
                if (const char* problem = world.economy_read_section(id, sr)) {
                    return fail(problem);
                }
            } else {  // kSectionDynasty
                const std::uint32_t rows = sr.u32();
                if (!plausible(sr, rows, kDynastyRowBytes)) {
                    return fail("invalid dynasty section");
                }
                bool have_previous = false;
                std::uint32_t previous_row = 0;
                for (std::uint32_t r = 0; r < rows; ++r) {
                    const std::uint32_t row = sr.u32();
                    if (!sr.ok() || row >= dynasty_count || (have_previous && row <= previous_row)) {
                        return fail("dynasty rows are out of range or out of order");
                    }
                    have_previous = true;
                    previous_row = row;
                    world.dynasties_.unity[row] = core::Fixed32::from_raw(sr.i32());
                    const std::uint8_t law = sr.u8();
                    if (law >= kSuccessionLawCount) {
                        return fail("invalid succession law");
                    }
                    world.dynasties_.succession_law[row] = static_cast<SuccessionLaw>(law);
                }
                const std::uint32_t branch_count = sr.u32();
                if (!plausible(sr, branch_count, kBranchRowBytes)) {
                    return fail("invalid branch table");
                }
                for (std::uint32_t b = 0; b < branch_count; ++b) {
                    const core::StringId name(sr.u32());
                    const DynastyId owner(sr.u32());
                    if (!sr.ok() || !owner.valid() || owner.value() >= dynasty_count) {
                        return fail("branch refers to an unknown dynasty");
                    }
                    world.branches_.name.push_back(name);
                    world.branches_.dynasty.push_back(owner);
                }
            }
            if (!sr.ok() || !sr.at_end()) {
                return fail("world state section is malformed");
            }
        }
    }

    world.rebuild_derived_indexes();
    const std::vector<std::string> problems = world.check_invariants();
    if (!problems.empty()) {
        return R::err(core::Error{10, "world state violates invariants: " + problems.front()});
    }
    return R::ok(std::move(world));
}

}  // namespace pat::sim
