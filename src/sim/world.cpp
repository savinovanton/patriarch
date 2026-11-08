#include "pat/sim/world.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace pat::sim {

namespace {

constexpr core::Fixed32 kZero = core::Fixed32::zero();
constexpr core::Fixed32 kNegativeScaleMax = core::Fixed32::from_int(-100);

template <class Id>
void hash_table_slots(core::HashStream& h, const EntityTable<Id>& ids) {
    h.u64(ids.capacity());
    h.u64(ids.alive_count());
    for (const std::uint32_t slot : ids.free_list()) {  // free-list order decides future ids
        h.u32(slot);
    }
    h.bytes(ids.alive_flags());
}

[[nodiscard]] core::Fixed32 clamp_skill(core::Fixed32 v) noexcept {
    return std::clamp(v, kZero, kSkillMax);
}
[[nodiscard]] core::Fixed32 clamp_scale(core::Fixed32 v) noexcept {
    return std::clamp(v, kZero, kScaleMax);
}
[[nodiscard]] core::Fixed32 clamp_signed_scale(core::Fixed32 v) noexcept {
    return std::clamp(v, kNegativeScaleMax, kSignedScaleMax);
}
[[nodiscard]] bool in_range(core::Fixed32 v, core::Fixed32 lo, core::Fixed32 hi) noexcept {
    return v >= lo && v <= hi;
}

/// Due date of the first birthday entry of a character created now: a newborn gets its "year 0"
/// roll on the birth date, everyone else on the next anniversary (ADR-0006 D4).
[[nodiscard]] Date first_birthday_due(Date birth, Date today) noexcept {
    return birth >= today ? birth : next_anniversary_on_or_after(birth, today);
}

/// Due date of the birthday entry in a state at a tick boundary: everything up to today has been
/// processed, so the next entry is the first anniversary after today.
[[nodiscard]] Date rebuilt_birthday_due(Date birth, Date today) noexcept {
    return next_anniversary_after(birth, today);
}

[[nodiscard]] std::uint64_t nonzero(std::uint64_t value) noexcept {
    return value == 0 ? 1 : value;
}

}  // namespace

std::string ext_part_name(std::size_t number) {
    switch (static_cast<ExtPart>(number)) {
        case ExtPart::Mode:
            return "mode";
        case ExtPart::CharacterExt:
            return "character_ext";
        case ExtPart::Archive:
            return "archive";
        case ExtPart::DynastyExt:
            return "dynasty_ext";
        case ExtPart::Pregnancy:
            return "pregnancy";
        case ExtPart::Chronicle:
            return "chronicle";
        case ExtPart::Opinion:
            return "opinion";
        case ExtPart::Kinship:
            return "kinship";
    }
    return std::format("ext[{}]", number);
}

World::World(Date start, std::uint64_t seed) : calendar_(start), rng_(seed) {}

// ---- mode ---------------------------------------------------------------------------------------

bool World::enable_dynasty_mode() {
    if (dynasty_mode()) {
        return true;
    }
    if (!characters_.ids.free_list().empty()) {
        return false;  // a freed id would be reused: the one thing dynasty mode must never do
    }
    // Dynasty mode keeps the family tree consistent; E1 worlds may carry parent references that
    // would break it (a parent of the wrong sex or born after the child, a stale slot).
    for (const CharacterId child : living_) {
        const std::size_t i = child.index();
        const CharacterId father = characters_.father[i];
        const CharacterId mother = characters_.mother[i];
        if ((father.valid() && !parent_link_is_valid(child, father, Sex::Male)) ||
            (mother.valid() && !parent_link_is_valid(child, mother, Sex::Female))) {
            return false;
        }
    }
    flags_ |= kWorldFlagDynastyMode;
    rebuild_derived_indexes();
    return true;
}

// ---- dynasties ----------------------------------------------------------------------------------

DynastyId World::create_dynasty(core::StringId name) {
    const DynastyId id(static_cast<std::uint32_t>(dynasties_.size()));
    dynasties_.name.push_back(name);
    dynasties_.head.emplace_back();
    dynasties_.honor.push_back(0);
    dynasties_.unity.push_back(kDefaultUnity);
    dynasties_.succession_law.push_back(SuccessionLaw::Majorat);
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

void World::set_dynasty_unity(DynastyId dynasty, core::Fixed32 unity) {
    PAT_ASSERT(exists(dynasty), "set_dynasty_unity: unknown dynasty");
    if (exists(dynasty)) {
        dynasties_.unity[dynasty.index()] = clamp_scale(unity);
    }
}

void World::set_dynasty_succession_law(DynastyId dynasty, SuccessionLaw law) {
    PAT_ASSERT(exists(dynasty), "set_dynasty_succession_law: unknown dynasty");
    PAT_ASSERT(static_cast<std::uint8_t>(law) < kSuccessionLawCount, "set_dynasty_succession_law: bad law");
    if (exists(dynasty) && static_cast<std::uint8_t>(law) < kSuccessionLawCount) {
        dynasties_.succession_law[dynasty.index()] = law;
    }
}

// ---- branches -----------------------------------------------------------------------------------

BranchId World::create_branch(DynastyId dynasty, core::StringId name) {
    PAT_ASSERT(exists(dynasty), "create_branch: unknown dynasty");
    if (!exists(dynasty)) {
        return BranchId();
    }
    const BranchId id(static_cast<std::uint32_t>(branches_.size()));
    branches_.name.push_back(name);
    branches_.dynasty.push_back(dynasty);
    return id;
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

void World::grow_character_columns() {
    characters_.dynasty.emplace_back();
    characters_.birth_date.emplace_back();
    characters_.sex.emplace_back();
    characters_.health.emplace_back();
    characters_.father.emplace_back();
    characters_.mother.emplace_back();
    characters_.name.emplace_back();
    characters_.branch.emplace_back();
    characters_.portrait.push_back(0);
    characters_.legitimacy.push_back(Legitimacy::Legitimate);
    characters_.cause_of_death.push_back(0);
    characters_.death_date.push_back(kNoDate);
    characters_.skills.emplace_back();
    characters_.fertility.emplace_back();
    characters_.stress.emplace_back();
    characters_.piety.emplace_back();
    characters_.ambition.emplace_back();
    characters_.loyalty_head.emplace_back();
    characters_.loyalty_emperor.emplace_back();
    characters_.traits.emplace_back();
    characters_.education_focus.push_back(0);
    characters_.education_tutor.emplace_back();
    characters_.education_level.push_back(0);
    characters_.planned_death.push_back(kNoDate);
    characters_.spouse.emplace_back();
    characters_.marriages.push_back(0);
    characters_.ex_spouses.emplace_back();
    characters_.godparent.emplace_back();
    characters_.first_child.emplace_back();
    characters_.next_by_father.emplace_back();
    characters_.next_by_mother.emplace_back();
}

bool World::kin_is_default(std::size_t i) const {
    const CharacterTable& c = characters_;
    return !c.spouse[i].valid() && c.marriages[i] == 0 &&
           c.ex_spouses[i] == std::array<CharacterId, kMaxExSpouses>{} &&
           c.godparent[i] == std::array<CharacterId, kGodparentSlots>{};
}

void World::reset_kinship(std::size_t i) {
    characters_.spouse[i] = CharacterId();
    characters_.marriages[i] = 0;
    characters_.ex_spouses[i] = {};
    characters_.godparent[i] = {};
    characters_.first_child[i] = CharacterId();
    characters_.next_by_father[i] = CharacterId();
    characters_.next_by_mother[i] = CharacterId();
}

bool World::parent_link_is_valid(CharacterId child, CharacterId parent, Sex role) const noexcept {
    return exists(parent) && child.valid() && child.index() < characters_.birth_date.size() &&
           parent != child && characters_.sex[parent.index()] == role &&
           characters_.birth_date[parent.index()] < characters_.birth_date[child.index()];
}

namespace {

/// Order of the child lists: eldest first, ties by id.
[[nodiscard]] bool born_before(const CharacterTable& c, CharacterId a, CharacterId b) noexcept {
    const Date da = c.birth_date[a.index()];
    const Date db = c.birth_date[b.index()];
    return da < db || (da == db && a < b);
}

}  // namespace

void World::link_child(CharacterId child) {
    CharacterTable& c = characters_;
    for (const Sex role : {Sex::Male, Sex::Female}) {
        const CharacterId parent = role == Sex::Male ? c.father[child.index()] : c.mother[child.index()];
        if (!parent_link_is_valid(child, parent, role)) {
            continue;
        }
        std::vector<CharacterId>& next = role == Sex::Male ? c.next_by_father : c.next_by_mother;
        CharacterId* link = &c.first_child[parent.index()];
        while (link->valid() && born_before(c, *link, child)) {
            link = &next[link->index()];
        }
        next[child.index()] = *link;
        *link = child;
    }
}

void World::rebuild_child_lists() {
    CharacterTable& c = characters_;
    const std::size_t capacity = c.ids.capacity();
    c.first_child.assign(capacity, CharacterId());
    c.next_by_father.assign(capacity, CharacterId());
    c.next_by_mother.assign(capacity, CharacterId());
    if (!dynasty_mode()) {
        return;
    }
    // Sort once and append: linear in the number of links however many children one parent has.
    std::vector<CharacterId> order;
    for (std::size_t i = 0; i < capacity; ++i) {
        if (c.ids.alive_flags()[i] == 0) {
            continue;
        }
        const CharacterId child(static_cast<std::uint32_t>(i));
        if (parent_link_is_valid(child, c.father[i], Sex::Male) ||
            parent_link_is_valid(child, c.mother[i], Sex::Female)) {
            order.push_back(child);
        }
    }
    std::sort(order.begin(), order.end(), [&](CharacterId a, CharacterId b) { return born_before(c, a, b); });
    std::vector<CharacterId> tail(capacity);
    for (const CharacterId child : order) {
        for (const Sex role : {Sex::Male, Sex::Female}) {
            const CharacterId parent = role == Sex::Male ? c.father[child.index()] : c.mother[child.index()];
            if (!parent_link_is_valid(child, parent, role)) {
                continue;
            }
            std::vector<CharacterId>& next = role == Sex::Male ? c.next_by_father : c.next_by_mother;
            CharacterId& last = tail[parent.index()];
            if (last.valid()) {
                next[last.index()] = child;
            } else {
                c.first_child[parent.index()] = child;
            }
            last = child;
        }
    }
}

void World::reset_character_extension(std::size_t i) {
    characters_.name[i] = core::StringId();
    characters_.branch[i] = BranchId();
    characters_.portrait[i] = 0;
    characters_.legitimacy[i] = Legitimacy::Legitimate;
    characters_.cause_of_death[i] = 0;
    characters_.death_date[i] = kNoDate;
    characters_.skills[i] = {};
    characters_.fertility[i] = kZero;
    characters_.stress[i] = kZero;
    characters_.piety[i] = kZero;
    characters_.ambition[i] = kZero;
    characters_.loyalty_head[i] = kZero;
    characters_.loyalty_emperor[i] = kZero;
    characters_.traits[i] = {};
    characters_.education_focus[i] = 0;
    characters_.education_tutor[i] = CharacterId();
    characters_.education_level[i] = 0;
    characters_.planned_death[i] = kNoDate;
}

bool World::character_extension_is_default(std::size_t i) const {
    const CharacterTable& c = characters_;
    return !c.name[i].valid() && !c.branch[i].valid() && c.portrait[i] == 0 &&
           c.legitimacy[i] == Legitimacy::Legitimate && c.cause_of_death[i] == 0 &&
           c.death_date[i] == kNoDate && c.skills[i] == std::array<core::Fixed32, kSkillCount>{} &&
           c.fertility[i] == kZero && c.stress[i] == kZero && c.piety[i] == kZero && c.ambition[i] == kZero &&
           c.loyalty_head[i] == kZero && c.loyalty_emperor[i] == kZero &&
           c.traits[i] == std::array<std::uint16_t, kTraitSlots>{} && c.education_focus[i] == 0 &&
           !c.education_tutor[i].valid() && c.education_level[i] == 0 && c.planned_death[i] == kNoDate;
}

void World::insert_living(CharacterId id) {
    if (living_.empty() || living_.back() < id) {
        living_.push_back(id);
    } else {
        living_.insert(std::lower_bound(living_.begin(), living_.end(), id), id);
    }
}

void World::push_life_events(CharacterId id, Date birthday_due) {
    life_queue_.push(birthday_due, LifeEvent{LifeEventKind::Birthday, id});
    const Date planned = characters_.planned_death[id.index()];
    if (planned != kNoDate) {
        life_queue_.push(planned, LifeEvent{LifeEventKind::PlannedDeath, id});
    }
}

CharacterId World::create_character(const CharacterInit& init) {
    PAT_ASSERT(!init.dynasty.valid() || exists(init.dynasty), "create_character: unknown dynasty");
    PAT_ASSERT(can_create_character(), "create_character: the character table is full");
    if (!can_create_character()) {
        return CharacterId();
    }
    const CharacterId id = characters_.ids.create();
    const std::size_t index = id.index();
    if (index >= characters_.dynasty.size()) {  // fresh slot: grow every column
        grow_character_columns();
    }
    const DynastyId dynasty = exists(init.dynasty) ? init.dynasty : DynastyId();
    characters_.dynasty[index] = dynasty;
    characters_.birth_date[index] = init.birth_date;
    characters_.sex[index] = init.sex;
    characters_.health[index] = init.health;
    characters_.father[index] = init.father;
    characters_.mother[index] = init.mother;

    // Columns added in E7-T02: clamp and clear whatever would break an invariant.
    reset_character_extension(index);
    reset_kinship(index);
    characters_.name[index] = init.name;
    if (exists(init.branch) && branches_.dynasty[init.branch.index()] == dynasty) {
        characters_.branch[index] = init.branch;
    }
    characters_.portrait[index] = init.portrait;
    if (static_cast<std::uint8_t>(init.legitimacy) < kLegitimacyCount) {
        characters_.legitimacy[index] = init.legitimacy;
    }
    for (std::size_t s = 0; s < kSkillCount; ++s) {
        characters_.skills[index][s] = clamp_skill(init.skills[s]);
    }
    characters_.fertility[index] = clamp_scale(init.fertility);
    characters_.stress[index] = clamp_scale(init.stress);
    characters_.piety[index] = clamp_scale(init.piety);
    characters_.ambition[index] = clamp_signed_scale(init.ambition);
    characters_.loyalty_head[index] = clamp_signed_scale(init.loyalty_head);
    characters_.loyalty_emperor[index] = clamp_signed_scale(init.loyalty_emperor);
    characters_.traits[index] = sanitize_traits(init.traits);
    if (init.education.focus <= kEducationFocusCount) {
        characters_.education_focus[index] = init.education.focus;
    }
    if (is_alive(init.education.tutor) && init.education.tutor != id) {
        characters_.education_tutor[index] = init.education.tutor;
    }
    characters_.education_level[index] = std::min(init.education.level, kMaxEducationLevel);
    if (planned_death_is_valid(init.birth_date, init.planned_death)) {
        characters_.planned_death[index] = init.planned_death;
    }

    if (dynasty.valid()) {
        members_[dynasty.index()].insert(id);
    }
    insert_living(id);
    if (dynasty_mode()) {
        // Kinship columns of a new row are at their defaults (reset above); the family tree only
        // keeps parents that can really be parents, then the child joins their lists.
        if (characters_.father[index].valid() &&
            !parent_link_is_valid(id, characters_.father[index], Sex::Male)) {
            characters_.father[index] = CharacterId();
        }
        if (characters_.mother[index].valid() &&
            !parent_link_is_valid(id, characters_.mother[index], Sex::Female)) {
            characters_.mother[index] = CharacterId();
        }
        link_child(id);
        if (is_sane_date(init.birth_date) && is_sane_date(today())) {
            push_life_events(id, first_birthday_due(init.birth_date, today()));
        }
    }
    return id;
}

bool World::destroy_character(CharacterId id) {
    PAT_ASSERT(!dynasty_mode(), "destroy_character is not available in dynasty mode");
    if (dynasty_mode() || !characters_.ids.is_alive(id)) {
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
    reset_character_extension(index);
    reset_kinship(index);
    living_.erase(std::lower_bound(living_.begin(), living_.end(), id));
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
    BranchId& branch = characters_.branch[id.index()];
    if (branch.valid() && branches_.dynasty[branch.index()] != current) {
        branch = BranchId();  // a branch belongs to one dynasty
    }
}

void World::set_character_name(CharacterId id, core::StringId name) {
    if (is_alive(id)) {
        characters_.name[id.index()] = name;
    }
}

void World::set_character_portrait(CharacterId id, std::uint16_t portrait) {
    if (is_alive(id)) {
        characters_.portrait[id.index()] = portrait;
    }
}

void World::set_character_legitimacy(CharacterId id, Legitimacy legitimacy) {
    PAT_ASSERT(static_cast<std::uint8_t>(legitimacy) < kLegitimacyCount,
               "set_character_legitimacy: bad value");
    if (is_alive(id) && static_cast<std::uint8_t>(legitimacy) < kLegitimacyCount) {
        characters_.legitimacy[id.index()] = legitimacy;
    }
}

bool World::set_character_branch(CharacterId id, BranchId branch) {
    if (!is_alive(id)) {
        return false;
    }
    if (branch.valid() &&
        (!exists(branch) || branches_.dynasty[branch.index()] != characters_.dynasty[id.index()])) {
        return false;
    }
    characters_.branch[id.index()] = branch;
    return true;
}

void World::set_character_skill(CharacterId id, Skill skill, core::Fixed32 value) {
    PAT_ASSERT(static_cast<std::size_t>(skill) < kSkillCount, "set_character_skill: bad skill");
    if (is_alive(id) && static_cast<std::size_t>(skill) < kSkillCount) {
        characters_.skills[id.index()][static_cast<std::size_t>(skill)] = clamp_skill(value);
    }
}

void World::set_character_fertility(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.fertility[id.index()] = clamp_scale(value);
    }
}

void World::set_character_stress(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.stress[id.index()] = clamp_scale(value);
    }
}

void World::set_character_piety(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.piety[id.index()] = clamp_scale(value);
    }
}

void World::set_character_ambition(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.ambition[id.index()] = clamp_signed_scale(value);
    }
}

void World::set_character_loyalty_head(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.loyalty_head[id.index()] = clamp_signed_scale(value);
    }
}

void World::set_character_loyalty_emperor(CharacterId id, core::Fixed32 value) {
    if (is_alive(id)) {
        characters_.loyalty_emperor[id.index()] = clamp_signed_scale(value);
    }
}

bool World::set_character_traits(CharacterId id, std::span<const std::uint16_t> traits) {
    if (!is_alive(id)) {
        return false;
    }
    std::array<std::uint16_t, kTraitSlots> sorted{};
    std::size_t n = 0;
    for (const std::uint16_t t : traits) {
        if (t == 0) {
            continue;
        }
        if (n == kTraitSlots) {
            return false;
        }
        sorted[n++] = t;
    }
    for (std::size_t i = 1; i < n; ++i) {  // insertion sort: n <= 8, and GCC mistrusts std::sort on a prefix
        const std::uint16_t key = sorted[i];
        std::size_t j = i;
        for (; j > 0 && sorted[j - 1] > key; --j) {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = key;
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (sorted[i] == sorted[i - 1]) {
            return false;
        }
    }
    characters_.traits[id.index()] = sorted;  // zeros are at the end: sorted holds the entries first
    return true;
}

bool World::set_character_education(CharacterId id, const Education& education) {
    if (!is_alive(id) || education.focus > kEducationFocusCount || education.level > kMaxEducationLevel ||
        (education.tutor.valid() && (!is_alive(education.tutor) || education.tutor == id))) {
        return false;
    }
    characters_.education_focus[id.index()] = education.focus;
    characters_.education_tutor[id.index()] = education.tutor;
    characters_.education_level[id.index()] = education.level;
    return true;
}

bool World::set_character_planned_death(CharacterId id, Date date) {
    if (!is_alive(id) || !planned_death_is_valid(characters_.birth_date[id.index()], date)) {
        return false;
    }
    characters_.planned_death[id.index()] = date;
    if (dynasty_mode() && date != kNoDate) {
        // Older entries for this character stay in the queue; they are recognised as stale (the
        // date no longer matches the column) and dropped when they come due.
        life_queue_.push(date, LifeEvent{LifeEventKind::PlannedDeath, id});
    }
    return true;
}

// ---- kinship primitives -------------------------------------------------------------------------

bool World::marry(CharacterId a, CharacterId b) {
    if (!dynasty_mode() || a == b || !is_alive(a) || !is_alive(b)) {
        return false;
    }
    CharacterTable& c = characters_;
    const std::size_t ia = a.index();
    const std::size_t ib = b.index();
    if (c.spouse[ia].valid() || c.spouse[ib].valid() || c.marriages[ia] >= kMaxMarriages ||
        c.marriages[ib] >= kMaxMarriages) {
        return false;
    }
    const auto& ex = c.ex_spouses[ia];
    if (std::find(ex.begin(), ex.end(), b) != ex.end()) {
        return false;  // the same two people do not marry twice
    }
    c.spouse[ia] = b;
    c.spouse[ib] = a;
    ++c.marriages[ia];
    ++c.marriages[ib];
    return true;
}

bool World::end_marriage(CharacterId a) {
    if (!dynasty_mode() || !is_alive(a)) {
        return false;
    }
    CharacterTable& c = characters_;
    const CharacterId b = c.spouse[a.index()];
    if (!is_alive(b) || c.spouse[b.index()] != a) {
        return false;
    }
    const auto free_slot = [](std::array<CharacterId, kMaxExSpouses>& ex) {
        return std::find_if(ex.begin(), ex.end(), [](CharacterId x) { return !x.valid(); });
    };
    const auto slot_a = free_slot(c.ex_spouses[a.index()]);
    const auto slot_b = free_slot(c.ex_spouses[b.index()]);
    if (slot_a == c.ex_spouses[a.index()].end() || slot_b == c.ex_spouses[b.index()].end()) {
        return false;  // cannot happen while marriages <= kMaxMarriages, but never lose a record
    }
    *slot_a = b;
    *slot_b = a;
    c.spouse[a.index()] = CharacterId();
    c.spouse[b.index()] = CharacterId();
    return true;
}

bool World::set_character_godparents(CharacterId id,
                                     const std::array<CharacterId, kGodparentSlots>& godparents) {
    if (!dynasty_mode() || !is_alive(id)) {
        return false;
    }
    for (std::size_t k = 0; k < kGodparentSlots; ++k) {
        const CharacterId g = godparents[k];
        if (g.valid() && (!exists(g) || g == id)) {
            return false;
        }
        for (std::size_t j = 0; j < k; ++j) {
            if (g.valid() && g == godparents[j]) {
                return false;
            }
        }
    }
    characters_.godparent[id.index()] = godparents;
    return true;
}

// ---- indexes ------------------------------------------------------------------------------------

std::span<const CharacterId> World::members_of(DynastyId dynasty) const noexcept {
    return exists(dynasty) ? members_[dynasty.index()].values() : std::span<const CharacterId>();
}

std::span<const ProvinceId> World::provinces_of(DynastyId dynasty) const noexcept {
    return exists(dynasty) ? owned_provinces_[dynasty.index()].values() : std::span<const ProvinceId>();
}

void World::rebuild_derived_indexes() {
    living_.clear();
    living_.reserve(characters_.ids.alive_count());
    characters_.ids.for_each_alive([&](CharacterId id) { living_.push_back(id); });

    rebuild_child_lists();
    life_queue_.clear();
    if (!dynasty_mode() || !is_sane_date(today())) {
        return;
    }
    for (const CharacterId id : living_) {
        const Date birth = characters_.birth_date[id.index()];
        if (is_sane_date(birth)) {
            push_life_events(id, rebuilt_birthday_due(birth, today()));
        }
    }
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
            if (characters_.ids.alive_flags()[i] != 1) {
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

    // ---- extended parts: 0 = nothing to hash (ADR-0006 D7) ----
    if (flags_ != 0) {
        core::HashStream h;
        h.u32(flags_);
        out.ext[static_cast<std::size_t>(ExtPart::Mode)] = nonzero(h.value());
    }
    {
        // Living rows that differ from the defaults, by ascending id. A row with default values
        // and no row are the same state (the existence of the row is covered by `characters`).
        core::HashStream h;
        bool any = false;
        for (const CharacterId id : living_) {
            const std::size_t i = id.index();
            if (character_extension_is_default(i)) {
                continue;
            }
            any = true;
            h.u32(id.value());
            h.id(characters_.name[i]);
            h.id(characters_.branch[i]);
            h.u16(characters_.portrait[i]);
            h.u8(static_cast<std::uint8_t>(characters_.legitimacy[i]));
            h.u8(characters_.cause_of_death[i]);
            h.i32(characters_.death_date[i].days());
            for (const core::Fixed32 skill : characters_.skills[i]) {
                h.fixed(skill);
            }
            h.fixed(characters_.fertility[i]);
            h.fixed(characters_.stress[i]);
            h.fixed(characters_.piety[i]);
            h.fixed(characters_.ambition[i]);
            h.fixed(characters_.loyalty_head[i]);
            h.fixed(characters_.loyalty_emperor[i]);
            for (const std::uint16_t trait : characters_.traits[i]) {
                h.u16(trait);
            }
            h.u8(characters_.education_focus[i]);
            h.id(characters_.education_tutor[i]);
            h.u8(characters_.education_level[i]);
            h.i32(characters_.planned_death[i].days());
        }
        if (any) {
            out.ext[static_cast<std::size_t>(ExtPart::CharacterExt)] = nonzero(h.value());
        }
    }
    {
        core::HashStream h;
        bool any = false;
        for (std::size_t d = 0; d < dynasties_.size(); ++d) {
            if (dynasties_.unity[d] == kDefaultUnity &&
                dynasties_.succession_law[d] == SuccessionLaw::Majorat) {
                continue;
            }
            any = true;
            h.u32(static_cast<std::uint32_t>(d));
            h.fixed(dynasties_.unity[d]);
            h.u8(static_cast<std::uint8_t>(dynasties_.succession_law[d]));
        }
        h.u64(branches_.size());
        for (std::size_t b = 0; b < branches_.size(); ++b) {
            any = true;
            h.id(branches_.name[b]);
            h.id(branches_.dynasty[b]);
        }
        if (any) {
            out.ext[static_cast<std::size_t>(ExtPart::DynastyExt)] = nonzero(h.value());
        }
    }

    {
        // Kinship of the living rows that differ from the defaults. Parents are in `characters`
        // already and the child lists are derived from them.
        core::HashStream h;
        bool any = false;
        for (const CharacterId id : living_) {
            const std::size_t i = id.index();
            if (kin_is_default(i)) {
                continue;
            }
            any = true;
            h.u32(id.value());
            h.id(characters_.spouse[i]);
            h.u8(characters_.marriages[i]);
            for (const CharacterId ex : characters_.ex_spouses[i]) {
                h.id(ex);
            }
            for (const CharacterId godparent : characters_.godparent[i]) {
                h.id(godparent);
            }
        }
        if (any) {
            out.ext[static_cast<std::size_t>(ExtPart::Kinship)] = nonzero(h.value());
        }
    }

    core::HashStream total;
    total.u64(out.calendar);
    total.u64(out.rng);
    total.u64(out.dynasties);
    total.u64(out.provinces);
    total.u64(out.characters);
    for (std::size_t part = 0; part < kExtPartCount; ++part) {
        if (out.ext[part] != 0) {  // empty parts do not exist as far as `total` is concerned
            total.u8(static_cast<std::uint8_t>(part));
            total.u64(out.ext[part]);
        }
    }
    out.total = total.value();
    return out;
}

// ---- invariants ---------------------------------------------------------------------------------

/// GDD 01 section 13.3: no cycles in the family, at most two parents (one father, one mother),
/// symmetric spouses, a child is younger than its parents; plus the consistency of the derived
/// child lists. Expects the column sizes to be checked already.
void World::check_kinship_invariants(std::vector<std::string>& bad) const {
    const CharacterTable& ch = characters_;
    const std::size_t capacity = ch.ids.capacity();
    const bool dynasty = dynasty_mode();
    std::size_t expected_links = 0;

    for (std::size_t i = 0; i < capacity; ++i) {
        const std::uint8_t state = ch.ids.alive_flags()[i];
        if (state == 0) {
            if (!kin_is_default(i) || ch.first_child[i].valid() || ch.next_by_father[i].valid() ||
                ch.next_by_mother[i].valid()) {
                bad.push_back(std::format("free character slot {} holds kinship data", i));
            }
            continue;
        }
        if (state != 1) {
            continue;  // retired rows were reported already
        }
        const CharacterId id(static_cast<std::uint32_t>(i));
        if (!dynasty) {
            // E1 mode: ids of the dead are reused, so no reference to a person would stay true.
            if (!kin_is_default(i)) {
                bad.push_back(std::format("character {} has kinship data outside dynasty mode", i));
            }
            continue;
        }

        // parents
        for (const Sex role : {Sex::Male, Sex::Female}) {
            const CharacterId parent = role == Sex::Male ? ch.father[i] : ch.mother[i];
            if (!parent.valid() || parent.index() >= capacity) {
                continue;  // out-of-table parents were reported with the other columns
            }
            if (parent == id) {
                bad.push_back(std::format("character {} is its own parent", i));
            } else if (!exists(parent)) {
                bad.push_back(std::format("character {} has a parent that does not exist", i));
            } else if (ch.sex[parent.index()] != role) {
                bad.push_back(std::format(
                    "character {} has a {} of the wrong sex", i, role == Sex::Male ? "father" : "mother"));
            } else if (ch.birth_date[parent.index()] >= ch.birth_date[i]) {
                bad.push_back(
                    std::format("character {} is not younger than its parent {}", i, parent.value()));
            } else {
                ++expected_links;
            }
        }

        // spouse and former spouses
        const CharacterId spouse = ch.spouse[i];
        if (spouse.valid()) {
            if (spouse == id) {
                bad.push_back(std::format("character {} is married to itself", i));
            } else if (spouse.index() >= capacity || !is_alive(spouse)) {
                bad.push_back(std::format("the spouse of character {} is not alive", i));
            } else if (ch.spouse[spouse.index()] != id) {
                bad.push_back(std::format("the marriage of character {} is not symmetric", i));
            }
        }
        if (ch.marriages[i] > kMaxMarriages) {
            bad.push_back(std::format("character {} has more than {} marriages", i, kMaxMarriages));
        }
        std::size_t partners = spouse.valid() ? 1 : 0;
        bool ended = false;  // an empty slot was seen: nothing may follow it
        for (std::size_t k = 0; k < kMaxExSpouses; ++k) {
            const CharacterId ex = ch.ex_spouses[i][k];
            if (!ex.valid()) {
                ended = true;
                continue;
            }
            ++partners;
            if (ended) {
                bad.push_back(std::format("the former spouses of character {} are not packed", i));
            }
            if (ex == id || ex == spouse || !exists(ex)) {
                bad.push_back(std::format("character {} has an invalid former spouse", i));
                continue;
            }
            for (std::size_t j = 0; j < k; ++j) {
                if (ch.ex_spouses[i][j] == ex) {
                    bad.push_back(std::format("character {} lists a former spouse twice", i));
                }
            }
            const auto& back = ch.ex_spouses[ex.index()];
            if (std::find(back.begin(), back.end(), id) == back.end()) {
                bad.push_back(std::format("the former marriage of character {} is not symmetric", i));
            }
        }
        if (ch.marriages[i] < partners) {
            bad.push_back(std::format("character {} has fewer marriages than partners", i));
        }

        // godparents (reserved)
        for (std::size_t k = 0; k < kGodparentSlots; ++k) {
            const CharacterId g = ch.godparent[i][k];
            if (g.valid() && (g == id || !exists(g) || (k > 0 && ch.godparent[i][0] == g))) {
                bad.push_back(std::format("character {} has an invalid godparent", i));
            }
        }
    }

    // The family has no cycles. Depth-first over the parent links of every row (iterative: a
    // damaged file must not overflow the stack). A self link was reported above.
    {
        std::vector<std::uint8_t> color(capacity, 0);  // 0 unvisited, 1 on the path, 2 done
        std::vector<std::pair<std::uint32_t, std::uint8_t>> path;
        bool cycle = false;
        for (std::size_t start = 0; start < capacity && !cycle; ++start) {
            if (color[start] != 0) {
                continue;
            }
            color[start] = 1;
            path.emplace_back(static_cast<std::uint32_t>(start), 0);
            while (!path.empty() && !cycle) {
                const std::uint32_t node = path.back().first;
                const std::uint8_t edge = path.back().second;
                if (edge == 2) {
                    color[node] = 2;
                    path.pop_back();
                    continue;
                }
                ++path.back().second;
                const CharacterId parent = edge == 0 ? ch.father[node] : ch.mother[node];
                if (!parent.valid() || parent.index() >= capacity || parent.value() == node) {
                    continue;
                }
                if (color[parent.index()] == 1) {
                    cycle = true;
                } else if (color[parent.index()] == 0) {
                    color[parent.index()] = 1;
                    path.emplace_back(parent.value(), 0);
                }
            }
        }
        if (cycle) {
            bad.push_back("the family tree contains a cycle");
        }
    }

    // Child lists: exactly the valid parent links, each list eldest first, no loops.
    if (!dynasty) {
        for (std::size_t i = 0; i < capacity; ++i) {
            if (ch.first_child[i].valid() || ch.next_by_father[i].valid() || ch.next_by_mother[i].valid()) {
                bad.push_back("child lists are not empty outside dynasty mode");
                break;
            }
        }
        return;
    }
    std::size_t listed = 0;
    for (std::size_t p = 0; p < capacity; ++p) {
        if (!ch.first_child[p].valid()) {
            continue;
        }
        if (ch.ids.alive_flags()[p] == 0) {
            bad.push_back(std::format("the child list of the free slot {} is not empty", p));
            continue;
        }
        const CharacterId parent(static_cast<std::uint32_t>(p));
        const Sex role = ch.sex[p];
        const std::vector<CharacterId>& next = role == Sex::Male ? ch.next_by_father : ch.next_by_mother;
        CharacterId previous;
        std::size_t steps = 0;
        for (CharacterId c = ch.first_child[p]; c.valid(); c = next[c.index()]) {
            if (++steps > capacity || c.index() >= capacity || !parent_link_is_valid(c, parent, role) ||
                (role == Sex::Male ? ch.father[c.index()] : ch.mother[c.index()]) != parent ||
                (previous.valid() && !born_before(ch, previous, c))) {
                bad.push_back(std::format("the child list of character {} is damaged", p));
                break;
            }
            ++listed;
            previous = c;
        }
    }
    if (listed != expected_links) {
        bad.push_back(
            std::format("child lists hold {} links but the parent columns have {}", listed, expected_links));
    }
}

std::vector<std::string> World::check_invariants() const {
    std::vector<std::string> bad;
    auto fail = [&](std::string message) { bad.push_back(std::move(message)); };

    const std::size_t capacity = characters_.ids.capacity();
    const CharacterTable& ch = characters_;
    if (ch.dynasty.size() != capacity || ch.birth_date.size() != capacity || ch.sex.size() != capacity ||
        ch.health.size() != capacity || ch.father.size() != capacity || ch.mother.size() != capacity ||
        ch.name.size() != capacity || ch.branch.size() != capacity || ch.portrait.size() != capacity ||
        ch.legitimacy.size() != capacity || ch.cause_of_death.size() != capacity ||
        ch.death_date.size() != capacity || ch.skills.size() != capacity || ch.fertility.size() != capacity ||
        ch.stress.size() != capacity || ch.piety.size() != capacity || ch.ambition.size() != capacity ||
        ch.loyalty_head.size() != capacity || ch.loyalty_emperor.size() != capacity ||
        ch.traits.size() != capacity || ch.education_focus.size() != capacity ||
        ch.education_tutor.size() != capacity || ch.education_level.size() != capacity ||
        ch.planned_death.size() != capacity || ch.spouse.size() != capacity ||
        ch.marriages.size() != capacity || ch.ex_spouses.size() != capacity ||
        ch.godparent.size() != capacity || ch.first_child.size() != capacity ||
        ch.next_by_father.size() != capacity || ch.next_by_mother.size() != capacity) {
        fail("character columns differ in size from the id table");
        return bad;  // everything below indexes the columns
    }
    if (dynasties_.head.size() != dynasties_.size() || dynasties_.honor.size() != dynasties_.size() ||
        dynasties_.unity.size() != dynasties_.size() ||
        dynasties_.succession_law.size() != dynasties_.size() || members_.size() != dynasties_.size() ||
        owned_provinces_.size() != dynasties_.size()) {
        fail("dynasty columns or indexes differ in size");
        return bad;
    }
    if (branches_.dynasty.size() != branches_.size()) {
        fail("branch columns differ in size");
        return bad;
    }
    if (provinces_.owner.size() != provinces_.size() || provinces_.population.size() != provinces_.size() ||
        provinces_.development.size() != provinces_.size()) {
        fail("province columns differ in size");
        return bad;
    }

    if ((flags_ & ~kWorldFlagDynastyMode) != 0) {
        fail("unknown world flags are set");
    }
    if (capacity > kMaxCharacterIds) {
        fail("the character table exceeds kMaxCharacterIds");
    }
    if (characters_.ids.retired_count() != 0) {
        fail("retired character rows are not supported yet (E7-T04)");
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
    if (characters_.ids.alive_count() + characters_.ids.retired_count() + free_sorted.size() != capacity) {
        fail("alive count + retired count + free list size != capacity");
    }
    if (dynasty_mode() && !free_sorted.empty()) {
        fail("dynasty mode requires an empty free list of character ids");
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

    // dynasty heads, honor, unity, succession law
    for (std::size_t d = 0; d < dynasties_.size(); ++d) {
        const CharacterId head = dynasties_.head[d];
        if (head.valid() && (!is_alive(head) || characters_.dynasty[head.index()].value() != d)) {
            fail(std::format("dynasty {} head {} is dead or not a member", d, head.value()));
        }
        if (dynasties_.honor[d] < kMinHonor || dynasties_.honor[d] > kMaxHonor) {
            fail(std::format("dynasty {} honor {} out of range", d, dynasties_.honor[d]));
        }
        if (!in_range(dynasties_.unity[d], kZero, kScaleMax)) {
            fail(std::format("dynasty {} unity {} out of range", d, dynasties_.unity[d].raw()));
        }
        if (static_cast<std::uint8_t>(dynasties_.succession_law[d]) >= kSuccessionLawCount) {
            fail(std::format("dynasty {} has an unknown succession law", d));
        }
    }
    for (std::size_t b = 0; b < branches_.size(); ++b) {
        if (!exists(branches_.dynasty[b])) {
            fail(std::format("branch {} belongs to an unknown dynasty", b));
        }
    }

    // character columns: living rows within bounds, free rows entirely at their defaults
    for (std::size_t i = 0; i < capacity; ++i) {
        const std::uint8_t state = characters_.ids.alive_flags()[i];
        if (state == 0) {
            if (!character_extension_is_default(i)) {
                fail(std::format("free character slot {} holds data", i));
            }
            continue;
        }
        if (state != 1) {
            continue;  // retired rows were reported above
        }
        const std::size_t who = i;
        if (!is_sane_date(ch.birth_date[i])) {
            fail(std::format("character {} has an implausible birth date", who));
        }
        for (const CharacterId parent : {ch.father[i], ch.mother[i]}) {
            if (parent.valid() && parent.index() >= capacity) {
                fail(std::format("character {} has a parent outside the id table", who));
            }
        }
        if (static_cast<std::uint8_t>(ch.legitimacy[i]) >= kLegitimacyCount) {
            fail(std::format("character {} has an unknown legitimacy", who));
        }
        for (const core::Fixed32 skill : ch.skills[i]) {
            if (!in_range(skill, kZero, kSkillMax)) {
                fail(std::format("character {} has a skill out of 0..20", who));
                break;
            }
        }
        if (!in_range(ch.fertility[i], kZero, kScaleMax) || !in_range(ch.stress[i], kZero, kScaleMax) ||
            !in_range(ch.piety[i], kZero, kScaleMax)) {
            fail(std::format("character {} has fertility, stress or piety out of 0..100", who));
        }
        if (!in_range(ch.ambition[i], kNegativeScaleMax, kSignedScaleMax) ||
            !in_range(ch.loyalty_head[i], kNegativeScaleMax, kSignedScaleMax) ||
            !in_range(ch.loyalty_emperor[i], kNegativeScaleMax, kSignedScaleMax)) {
            fail(std::format("character {} has ambition or a loyalty out of -100..100", who));
        }
        if (!traits_are_canonical(ch.traits[i])) {
            fail(std::format("character {} has traits that are not sorted, unique and packed", who));
        }
        if (ch.education_focus[i] > kEducationFocusCount || ch.education_level[i] > kMaxEducationLevel ||
            (ch.education_tutor[i].valid() && ch.education_tutor[i].index() >= capacity)) {
            fail(std::format("character {} has invalid education", who));
        }
        const BranchId branch = ch.branch[i];
        if (branch.valid() && (!exists(branch) || branches_.dynasty[branch.index()] != ch.dynasty[i])) {
            fail(std::format("character {} is in a branch of another dynasty or an unknown branch", who));
        }
        if (ch.death_date[i] != kNoDate || ch.cause_of_death[i] != 0) {
            fail(std::format("living character {} has a death date or cause", who));
        }
        if (!planned_death_is_valid(ch.birth_date[i], ch.planned_death[i])) {
            fail(std::format("character {} has an invalid planned death", who));
        }
    }

    check_kinship_invariants(bad);

    // derived list of the living
    {
        std::size_t position = 0;
        bool same = true;
        characters_.ids.for_each_alive([&](CharacterId c) {
            if (position >= living_.size() || living_[position] != c) {
                same = false;
            }
            ++position;
        });
        if (!same || position != living_.size()) {
            fail("the list of living characters differs from the id table");
        }
    }

    // derived life-event queue. At a tick boundary every living character in dynasty mode has
    // exactly one birthday entry at an anniversary that is not later than the next one after
    // today (earlier is allowed: nothing consumes the queue yet), and its planned death is queued.
    // Entries of characters that are not alive and planned-death entries whose date was changed
    // are stale and tolerated.
    {
        std::vector<std::uint8_t> birthdays(capacity, 0);
        std::vector<std::uint8_t> death_queued(capacity, 0);
        for (const auto& entry : life_queue_.snapshot()) {
            const CharacterId c = entry.item.character;
            if (!c.valid() || c.index() >= capacity) {
                fail("the life-event queue refers to a character outside the id table");
                continue;
            }
            const std::size_t i = c.index();
            if (!dynasty_mode()) {
                fail("the life-event queue is not empty outside dynasty mode");
                break;
            }
            if (!is_alive(c)) {
                continue;
            }
            const Date birth = ch.birth_date[i];
            if (!is_sane_date(birth) || !is_sane_date(entry.due)) {
                continue;  // reported above
            }
            switch (entry.item.kind) {
                case LifeEventKind::Birthday: {
                    ++birthdays[i];
                    const bool anniversary =
                        entry.due == birth ||
                        (entry.due > birth && entry.due == anniversary_in_year(birth, entry.due.year()));
                    if (!anniversary || entry.due > rebuilt_birthday_due(birth, today())) {
                        fail(std::format("character {} has a misplaced birthday entry", c.value()));
                    }
                    break;
                }
                case LifeEventKind::PlannedDeath:
                    if (entry.due == ch.planned_death[i]) {
                        death_queued[i] = 1;
                    }
                    break;
                default:
                    fail("the life-event queue holds an unknown kind of entry");
                    break;
            }
        }
        if (dynasty_mode()) {
            for (const CharacterId c : living_) {
                if (c.index() >= capacity) {
                    continue;
                }
                if (birthdays[c.index()] != 1) {
                    fail(std::format("character {} has {} birthday entries in the life-event queue",
                                     c.value(),
                                     birthdays[c.index()]));
                }
                if (ch.planned_death[c.index()] != kNoDate && death_queued[c.index()] == 0) {
                    fail(std::format("the planned death of character {} is not queued", c.value()));
                }
            }
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
