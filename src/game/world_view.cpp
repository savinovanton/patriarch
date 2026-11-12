#include "pat/game/world_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace pat::game {

namespace {

// The month names themselves live in the string table as ui.month.<name>.
constexpr std::array<const char*, 12> kMonthKeys = {"january",
                                                    "february",
                                                    "march",
                                                    "april",
                                                    "may",
                                                    "june",
                                                    "july",
                                                    "august",
                                                    "september",
                                                    "october",
                                                    "november",
                                                    "december"};

// Clan names (real Russian families of the period; the people are fictional).
constexpr std::array<const char*, 24> kClans = {
    "Шереметевы", "Голицыны",     "Долгоруковы",   "Апраксины",    "Салтыковы",  "Нарышкины",
    "Репнины",    "Стрешневы",    "Матвеевы",      "Бутурлины",    "Троекуровы", "Одоевские",
    "Воронцовы",  "Головины",     "Ромодановские", "Трубецкие",    "Черкасские", "Хованские",
    "Куракины",   "Прозоровские", "Шеины",         "Милославские", "Лопухины",   "Стрешневы"};
constexpr std::array<const char*, 24> kSurnames = {
    "Шереметев", "Голицын",      "Долгоруков",    "Апраксин",     "Салтыков",   "Нарышкин",
    "Репнин",    "Стрешнев",     "Матвеев",       "Бутурлин",     "Троекуров",  "Одоевский",
    "Воронцов",  "Головин",      "Ромодановский", "Трубецкой",    "Черкасский", "Хованский",
    "Куракин",   "Прозоровский", "Шеин",          "Милославский", "Лопухин",    "Стрешнев"};

constexpr std::array<const char*, 12> kGivenMale = {"Иван",
                                                    "Пётр",
                                                    "Фёдор",
                                                    "Алексей",
                                                    "Михаил",
                                                    "Борис",
                                                    "Никита",
                                                    "Андрей",
                                                    "Дмитрий",
                                                    "Василий",
                                                    "Григорий",
                                                    "Семён"};
constexpr std::array<const char*, 12> kGivenFemale = {"Анна",
                                                      "Мария",
                                                      "Евдокия",
                                                      "Прасковья",
                                                      "Наталья",
                                                      "Софья",
                                                      "Ирина",
                                                      "Ксения",
                                                      "Татьяна",
                                                      "Елена",
                                                      "Аграфена",
                                                      "Феодосия"};
constexpr std::array<const char*, 12> kPatronymicMale = {"Иванович",
                                                         "Петрович",
                                                         "Фёдорович",
                                                         "Алексеевич",
                                                         "Михайлович",
                                                         "Борисович",
                                                         "Никитич",
                                                         "Андреевич",
                                                         "Дмитриевич",
                                                         "Васильевич",
                                                         "Григорьевич",
                                                         "Семёнович"};
constexpr std::array<const char*, 12> kPatronymicFemale = {"Ивановна",
                                                           "Петровна",
                                                           "Фёдоровна",
                                                           "Алексеевна",
                                                           "Михайловна",
                                                           "Борисовна",
                                                           "Никитична",
                                                           "Андреевна",
                                                           "Дмитриевна",
                                                           "Васильевна",
                                                           "Григорьевна",
                                                           "Семёновна"};

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

std::uint32_t rgba(float r, float g, float b) {
    const auto q = [](float c) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    return q(r) | (q(g) << 8U) | (q(b) << 16U) | (255U << 24U);
}

std::string person_name(const sim::World& world, sim::CharacterId id) {
    const std::size_t i = id.index();
    const bool female = world.characters().sex[i] == sim::Sex::Female;
    const std::uint32_t h = hash32(static_cast<std::uint32_t>(id.value()) + 17U);
    const sim::DynastyId dynasty = world.characters().dynasty[i];
    std::string surname = dynasty.valid() ? dynasty_surname(dynasty) : "Безродный";
    if (female && dynasty.valid()) {
        // Feminine form: -ов/-ев/-ин -> -ова/-ева/-ина; adjectival -ский/-цкий/-ой -> -ская/-цкая/-ая.
        if (surname.size() > 4 && surname.compare(surname.size() - 4, 4, "ский") == 0) {
            surname.replace(surname.size() - 4, 4, "ская");
        } else if (surname.size() > 4 && surname.compare(surname.size() - 4, 4, "цкий") == 0) {
            surname.replace(surname.size() - 4, 4, "цкая");
        } else if (surname.size() > 4 && surname.compare(surname.size() - 4, 4, "ской") == 0) {
            surname.replace(surname.size() - 4, 4, "ская");
        } else {
            surname += "а";
        }
    }
    const auto& given = female ? kGivenFemale : kGivenMale;
    const auto& patronymic = female ? kPatronymicFemale : kPatronymicMale;
    return std::string(given[h % given.size()]) + " " + patronymic[(h >> 8U) % patronymic.size()] + " " +
           surname;
}

// String-table key of the health word for a health value.
const char* health_key(core::Fixed32 health) {
    const int v = health.to_int();
    if (v >= 80) {
        return "ui.health.strong";
    }
    if (v >= 55) {
        return "ui.health.good";
    }
    if (v >= 30) {
        return "ui.health.weak";
    }
    return "ui.health.sickly";
}

// The denominators used here (Fixed32::one_raw and 1000) both have finite decimal expansions.
// Keeping them exact makes the displayed additive terms agree with the displayed root.
std::string decimal_value(std::int64_t numerator, std::int64_t denominator, bool signed_value) {
    std::string text;
    if (numerator < 0) {
        text = "-";
        numerator = -numerator;
    } else if (signed_value && numerator > 0) {
        text = "+";
    }
    text += std::to_string(numerator / denominator);
    std::int64_t remainder = numerator % denominator;
    if (remainder != 0) {
        text += '.';
        while (remainder != 0) {
            remainder *= 10;
            text += static_cast<char>('0' + remainder / denominator);
            remainder %= denominator;
        }
    }
    return text;
}

std::string point_value(core::Fixed32 value, bool signed_value = false) {
    return decimal_value(value.raw(), core::Fixed32::one_raw, signed_value);
}

sim::ProvinceId
world_province(const sim::World& world, std::size_t map_id, std::span<const sim::ProvinceId> map_to_world) {
    if (map_id == 0) {
        return {};
    }
    if (map_to_world.empty()) {
        return map_id <= world.provinces().size() ? sim::ProvinceId(static_cast<std::uint32_t>(map_id - 1))
                                                  : sim::ProvinceId{};
    }
    if (map_id < map_to_world.size() && world.exists(map_to_world[map_id])) {
        return map_to_world[map_id];
    }
    return {};
}

template <class ColorForProvince>
std::vector<std::uint32_t> province_palette(const sim::World& world,
                                            std::size_t map_province_count,
                                            std::span<const sim::ProvinceId> map_to_world,
                                            ColorForProvince color_for_province) {
    std::vector<std::uint32_t> palette(map_province_count + 1, kUnownedColor);
    const sim::ProvinceTable& provinces = world.provinces();
    for (std::size_t map_id = 1; map_id < palette.size(); ++map_id) {
        const sim::ProvinceId province = world_province(world, map_id, map_to_world);
        if (province.valid()) {
            palette[map_id] = color_for_province(provinces, province.index());
        }
    }
    return palette;
}

/// Integer interpolation keeps each channel monotone after quantization to RGBA8.
std::uint32_t economy_color(std::int64_t good, std::int64_t range) {
    good = std::clamp<std::int64_t>(good, 0, range);
    const auto red = static_cast<std::uint32_t>(217 - 166 * good / range);
    const auto green = static_cast<std::uint32_t>(64 + 140 * good / range);
    return red | (green << 8U) | (56U << 16U) | 0xFF000000U;
}

}  // namespace

std::string format_date(const loc::Localizer& loc, sim::Date date) {
    const sim::YearMonthDay v = date.ymd();
    const std::string month =
        loc.format("ui.month." + std::string(kMonthKeys[static_cast<std::size_t>(v.month - 1)]));
    return loc.format("ui.date.long",
                      {loc::Arg("day", v.day), loc::Arg("month", month), loc::Arg("year", v.year)});
}

std::string dynasty_name(sim::DynastyId dynasty) {
    if (!dynasty.valid()) {
        return "Без рода";
    }
    const std::size_t i = dynasty.index();
    std::string name = kClans[i % kClans.size()];
    if (i >= kClans.size()) {
        name += " " + std::to_string(i / kClans.size() + 1);
    }
    return name;
}

std::string dynasty_surname(sim::DynastyId dynasty) {
    return dynasty.valid() ? kSurnames[dynasty.index() % kSurnames.size()] : "Безродный";
}

std::uint32_t dynasty_color(sim::DynastyId dynasty) {
    if (!dynasty.valid()) {
        return kUnownedColor;
    }
    // Golden-ratio hue steps keep neighbouring dynasties visually distinct.
    const float hue = std::fmod(static_cast<float>(dynasty.index()) * 0.61803398875f + 0.08f, 1.0f);
    const float sat = 0.55f;
    const float val = 0.82f;
    const float h6 = hue * 6.0f;
    const int sector = static_cast<int>(h6) % 6;
    const float f = h6 - std::floor(h6);
    const float p = val * (1.0f - sat);
    const float q = val * (1.0f - sat * f);
    const float t = val * (1.0f - sat * (1.0f - f));
    switch (sector) {
        case 0:
            return rgba(val, t, p);
        case 1:
            return rgba(q, val, p);
        case 2:
            return rgba(p, val, t);
        case 3:
            return rgba(p, q, val);
        case 4:
            return rgba(t, p, val);
        default:
            return rgba(val, p, q);
    }
}

std::vector<std::uint32_t> owner_palette(const sim::World& world,
                                         std::size_t map_province_count,
                                         std::span<const sim::ProvinceId> map_to_world) {
    return province_palette(
        world, map_province_count, map_to_world, [](const sim::ProvinceTable& provinces, std::size_t index) {
            return dynasty_color(provinces.owner[index]);
        });
}

std::vector<std::uint32_t> development_palette(const sim::World& world,
                                               std::size_t map_province_count,
                                               std::span<const sim::ProvinceId> map_to_world) {
    return province_palette(
        world, map_province_count, map_to_world, [](const sim::ProvinceTable& provinces, std::size_t index) {
            const float v = std::clamp(static_cast<float>(provinces.development[index]) / 10.0f, 0.0f, 1.0f);
            return rgba(0.85f - 0.65f * v, 0.25f + 0.55f * v, 0.22f);
        });
}

std::vector<std::uint32_t> unrest_palette(const sim::World& world,
                                          std::size_t map_province_count,
                                          std::span<const sim::ProvinceId> map_to_world) {
    if (!world.economy_present()) {
        return std::vector<std::uint32_t>(map_province_count + 1, kUnownedColor);
    }
    constexpr std::int64_t kRange = 100LL * core::Fixed32::one_raw;
    return province_palette(
        world, map_province_count, map_to_world, [&world](const sim::ProvinceTable&, std::size_t index) {
            if (world.population_persons(sim::ProvinceId(static_cast<std::uint32_t>(index))) == 0) {
                return kUnownedColor;
            }
            const std::int64_t unrest = world.economy().provinces.unrest[index].raw();
            return economy_color(kRange - unrest, kRange);
        });
}

std::vector<std::uint32_t> mood_palette(const sim::World& world,
                                        std::size_t map_province_count,
                                        std::span<const sim::ProvinceId> map_to_world) {
    if (!world.economy_present()) {
        return std::vector<std::uint32_t>(map_province_count + 1, kUnownedColor);
    }
    constexpr std::int64_t kHalfRange = 100LL * core::Fixed32::one_raw;
    return province_palette(
        world, map_province_count, map_to_world, [&world](const sim::ProvinceTable&, std::size_t index) {
            const auto& groups = world.economy().groups;
            const std::size_t first = index * sim::economy::kGroupCount;
            std::int64_t weighted = 0;
            std::uint64_t people = 0;
            for (std::size_t group = 0; group < sim::economy::kGroupCount; ++group) {
                const std::uint32_t size = groups.size[first + group];
                weighted += static_cast<std::int64_t>(size) * groups.mood[first + group].raw();
                people += size;
            }
            if (people == 0) {
                return kUnownedColor;
            }
            // Even at kMaxProvincePersons the full weighted numerator fits int64.
            const std::int64_t mood = weighted / static_cast<std::int64_t>(people);
            return economy_color(mood + kHalfRange, 2 * kHalfRange);
        });
}

int age_in_years(sim::Date born, sim::Date today) {
    const double days = static_cast<double>(today.days()) - static_cast<double>(born.days());
    return std::max(0, static_cast<int>(std::floor(days / 365.25)));
}

ui::Explanation character_explanation(const loc::Localizer& loc,
                                      const sim::TraitCatalog& catalog,
                                      const sim::CharacterBreakdown& breakdown,
                                      std::string_view label_key) {
    ui::Explanation root;
    root.label = loc.format(label_key);
    root.value = point_value(breakdown.total);
    for (const sim::CharacterTerm& source : breakdown.terms) {
        ui::Explanation term;
        term.value = point_value(source.value, true);
        switch (source.kind) {
            case sim::CharacterTermKind::Base:
                term.label = loc.format("ui.explain.base");
                break;
            case sim::CharacterTermKind::Trait:
            case sim::CharacterTermKind::Multiplier:
                if (const sim::TraitDef* trait = catalog.find(source.trait)) {
                    term.label =
                        loc.format("ui.explain.trait_named", {loc::Arg("name", loc.format(trait->name_key))});
                } else {
                    term.label = loc.format("ui.explain.traits");
                }
                if (source.kind == sim::CharacterTermKind::Multiplier) {
                    term.note =
                        loc.format("ui.explain.multiplier",
                                   {loc::Arg("factor", decimal_value(source.factor_permille, 1000, false))});
                }
                break;
            case sim::CharacterTermKind::Clamp:
                term.label = loc.format("ui.explain.clamp");
                break;
        }
        root.terms.push_back(std::move(term));
    }
    return root;
}

ui::CharacterSnapshot character_snapshot(const loc::Localizer& loc,
                                         const sim::World& world,
                                         sim::CharacterId character,
                                         const sim::TraitCatalog* traits) {
    const sim::CharacterTable& t = world.characters();
    const std::size_t i = character.index();
    const sim::Date today = world.today();

    // The spike keeps its deterministic placeholders; configured dynasty snapshots use pure queries.
    ui::CharacterSnapshot s =
        ui::make_test_character(loc, hash32(static_cast<std::uint32_t>(character.value()) + 101U), 0);

    const std::string full = person_name(world, character);
    // person_name() is "Given Patronymic Surname"; split it for the window.
    const std::size_t first_space = full.find(' ');
    const std::size_t second_space = full.find(' ', first_space + 1);
    s.given_name = full.substr(0, first_space);
    s.patronymic = full.substr(first_space + 1, second_space - first_space - 1);
    s.surname = full.substr(second_space + 1);

    s.age = age_in_years(t.birth_date[i], today);
    s.health = health_key(t.health[i]);
    if (traits != nullptr && world.dynasty_mode()) {
        static_assert(ui::kSkillCount == sim::kSkillCount);
        for (std::size_t skill = 0; skill < sim::kSkillCount; ++skill) {
            const sim::CharacterBreakdown breakdown =
                sim::skill_breakdown(world, *traits, character, static_cast<sim::Skill>(skill));
            s.skills[skill] = breakdown.total.round_int();
            ui::Explanation explanation =
                character_explanation(loc, *traits, breakdown, ui::kSkillKeys[skill]);
            explanation.value = std::to_string(s.skills[skill]);
            explanation.note =
                loc.format("ui.skill.note", {loc::Arg("min", 0), loc::Arg("max", ui::kSkillMax)});
            const core::Fixed32 rounding = core::Fixed32::from_int(s.skills[skill]) - breakdown.total;
            if (rounding != core::Fixed32::zero()) {
                explanation.terms.push_back(
                    {loc.format("ui.explain.rounding"), point_value(rounding, true), "", {}});
            }
            s.skill_explanations[skill] = std::move(explanation);
        }
        s.traits.clear();
        for (std::uint16_t id : t.traits[i]) {
            if (const sim::TraitDef* trait = traits->find(id)) {
                s.traits.push_back(loc.format(trait->name_key));
            }
        }
        s.health = health_key(
            sim::attribute_breakdown(world, *traits, character, sim::CharacterAttribute::Health).total);
    }
    const sim::DynastyId dynasty = t.dynasty[i];
    const bool is_head = dynasty.valid() && world.dynasties().head[dynasty.index()] == character;
    if (is_head) {
        s.role = "ui.character.role.head";
    } else if (s.age < 16) {
        s.role = t.sex[i] == sim::Sex::Female ? "ui.character.role.youth_f" : "ui.character.role.youth_m";
    } else {
        s.role = t.sex[i] == sim::Sex::Female ? "ui.character.role.noblewoman" : "ui.character.role.stolnik";
    }

    // Family: parents and children.
    s.family.clear();
    const auto describe = [&](sim::CharacterId other) {
        return loc.format("ui.character.person",
                          {loc::Arg("name", person_name(world, other)),
                           loc::Arg("age", age_in_years(t.birth_date[other.index()], today))});
    };
    const auto kin_line = [&](const char* key, sim::CharacterId other) {
        return loc.format(key, {loc::Arg("person", describe(other))});
    };
    if (t.father[i].valid() && world.is_alive(t.father[i])) {
        s.family.push_back(kin_line("ui.character.family.father", t.father[i]));
    }
    if (t.mother[i].valid() && world.is_alive(t.mother[i])) {
        s.family.push_back(kin_line("ui.character.family.mother", t.mother[i]));
    }
    s.family.push_back(loc.format("ui.character.family.dynasty", {loc::Arg("name", dynasty_name(dynasty))}));
    if (dynasty.valid()) {
        for (const sim::CharacterId member : world.members_of(dynasty)) {
            if (member == character) {
                continue;
            }
            const std::size_t m = member.index();
            if (t.father[m] == character || t.mother[m] == character) {
                s.family.push_back(kin_line(t.sex[m] == sim::Sex::Female ? "ui.character.family.daughter"
                                                                         : "ui.character.family.son",
                                            member));
            }
        }
    }

    // Relations: kin first, then everybody else alive (capped to keep the list a reasonable size).
    s.relations.clear();
    int number = 0;
    const auto add_relation = [&](sim::CharacterId other, const char* kind_key) {
        const std::string kind = loc.format(kind_key);
        s.relations.push_back(loc.format(
            "ui.character.relation.row",
            {loc::Arg("n", ++number), loc::Arg("name", person_name(world, other)), loc::Arg("kind", kind)}));
    };
    if (dynasty.valid()) {
        for (const sim::CharacterId member : world.members_of(dynasty)) {
            if (member != character) {
                add_relation(member, "ui.character.relation.kin");
            }
        }
    }
    const std::span<const std::uint8_t> alive = t.ids.alive_flags();
    for (std::size_t other = 0; other < alive.size() && s.relations.size() < 400; ++other) {
        const sim::CharacterId id(static_cast<std::uint32_t>(other));
        if (alive[other] != 1 || id == character || (dynasty.valid() && t.dynasty[other] == dynasty)) {
            continue;
        }
        add_relation(id, "ui.character.relation.acquaintance");
    }
    return s;
}

sim::CharacterId head_for_province(const sim::World& world,
                                   std::size_t map_province,
                                   std::span<const sim::ProvinceId> map_to_world) {
    const sim::ProvinceId province = world_province(world, map_province, map_to_world);
    if (!province.valid()) {
        return sim::CharacterId{};
    }
    const sim::DynastyId owner = world.provinces().owner[province.index()];
    if (!owner.valid() || !world.exists(owner)) {
        return sim::CharacterId{};
    }
    const sim::CharacterId head = world.dynasties().head[owner.index()];
    return (head.valid() && world.is_alive(head)) ? head : sim::CharacterId{};
}

sim::CharacterId pick_character(const sim::World& world, std::uint32_t seed) {
    const std::span<const std::uint8_t> alive = world.characters().ids.alive_flags();
    std::size_t count = 0;
    for (const std::uint8_t a : alive) {
        count += a == 1 ? 1U : 0U;
    }
    if (count == 0) {
        return sim::CharacterId{};
    }
    std::size_t target = hash32(seed) % count;
    for (std::size_t i = 0; i < alive.size(); ++i) {
        if (alive[i] == 1 && target-- == 0) {
            return sim::CharacterId(static_cast<std::uint32_t>(i));
        }
    }
    return sim::CharacterId{};
}

}  // namespace pat::game
