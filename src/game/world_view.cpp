#include "pat/game/world_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace pat::game {

namespace {

constexpr std::array<const char*, 12> kMonthsGenitive = {"января",
                                                         "февраля",
                                                         "марта",
                                                         "апреля",
                                                         "мая",
                                                         "июня",
                                                         "июля",
                                                         "августа",
                                                         "сентября",
                                                         "октября",
                                                         "ноября",
                                                         "декабря"};

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

const char* health_text(core::Fixed32 health) {
    const int v = health.to_int();
    if (v >= 80) {
        return "крепкое";
    }
    if (v >= 55) {
        return "хорошее";
    }
    if (v >= 30) {
        return "слабое";
    }
    return "тяжёлое";
}

}  // namespace

std::string format_date_ru(sim::Date date) {
    const sim::YearMonthDay v = date.ymd();
    return std::to_string(v.day) + " " + kMonthsGenitive[static_cast<std::size_t>(v.month - 1)] + " " +
           std::to_string(v.year);
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

std::vector<std::uint32_t> owner_palette(const sim::World& world, std::size_t map_province_count) {
    std::vector<std::uint32_t> palette(map_province_count + 1, kUnownedColor);
    const sim::ProvinceTable& provinces = world.provinces();
    const std::size_t n = std::min(map_province_count, provinces.size());
    for (std::size_t i = 0; i < n; ++i) {
        palette[i + 1] = dynasty_color(provinces.owner[i]);
    }
    return palette;
}

std::vector<std::uint32_t> development_palette(const sim::World& world, std::size_t map_province_count) {
    std::vector<std::uint32_t> palette(map_province_count + 1, kUnownedColor);
    const sim::ProvinceTable& provinces = world.provinces();
    const std::size_t n = std::min(map_province_count, provinces.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float v = std::clamp(static_cast<float>(provinces.development[i]) / 10.0f, 0.0f, 1.0f);
        palette[i + 1] = rgba(0.85f - 0.65f * v, 0.25f + 0.55f * v, 0.22f);
    }
    return palette;
}

int age_in_years(sim::Date born, sim::Date today) {
    const double days = static_cast<double>(today.days()) - static_cast<double>(born.days());
    return std::max(0, static_cast<int>(std::floor(days / 365.25)));
}

ui::CharacterSnapshot character_snapshot(const sim::World& world, sim::CharacterId character) {
    const sim::CharacterTable& t = world.characters();
    const std::size_t i = character.index();
    const sim::Date today = world.today();

    // Skills, traits and their breakdowns are placeholders (deterministic per character) until the
    // dynasty systems provide real ones.
    ui::CharacterSnapshot s =
        ui::make_test_character(hash32(static_cast<std::uint32_t>(character.value()) + 101U), 0);

    const std::string full = person_name(world, character);
    // person_name() is "Given Patronymic Surname"; split it for the window.
    const std::size_t first_space = full.find(' ');
    const std::size_t second_space = full.find(' ', first_space + 1);
    s.given_name = full.substr(0, first_space);
    s.patronymic = full.substr(first_space + 1, second_space - first_space - 1);
    s.surname = full.substr(second_space + 1);

    s.age = age_in_years(t.birth_date[i], today);
    s.health = health_text(t.health[i]);
    const sim::DynastyId dynasty = t.dynasty[i];
    const bool is_head = dynasty.valid() && world.dynasties().head[dynasty.index()] == character;
    if (is_head) {
        s.role = "глава рода";
    } else if (s.age < 16) {
        s.role = t.sex[i] == sim::Sex::Female ? "отроковица" : "отрок";
    } else {
        s.role = t.sex[i] == sim::Sex::Female ? "боярыня" : "стольник";
    }

    // Family: parents and children.
    s.family.clear();
    const auto describe = [&](sim::CharacterId other) {
        return person_name(world, other) + ", " +
               std::to_string(age_in_years(t.birth_date[other.index()], today)) + " л.";
    };
    if (t.father[i].valid() && world.is_alive(t.father[i])) {
        s.family.push_back("отец: " + describe(t.father[i]));
    }
    if (t.mother[i].valid() && world.is_alive(t.mother[i])) {
        s.family.push_back("мать: " + describe(t.mother[i]));
    }
    s.family.push_back("род: " + dynasty_name(dynasty));
    if (dynasty.valid()) {
        for (const sim::CharacterId member : world.members_of(dynasty)) {
            if (member == character) {
                continue;
            }
            const std::size_t m = member.index();
            if (t.father[m] == character || t.mother[m] == character) {
                s.family.push_back(std::string(t.sex[m] == sim::Sex::Female ? "дочь: " : "сын: ") +
                                   describe(member));
            }
        }
    }

    // Relations: kin first, then everybody else alive (capped to keep the list a reasonable size).
    s.relations.clear();
    int number = 0;
    const auto add_relation = [&](sim::CharacterId other, const char* kind) {
        s.relations.push_back(std::to_string(++number) + ". " + person_name(world, other) + " - " + kind);
    };
    if (dynasty.valid()) {
        for (const sim::CharacterId member : world.members_of(dynasty)) {
            if (member != character) {
                add_relation(member, "родич");
            }
        }
    }
    const std::span<const std::uint8_t> alive = t.ids.alive_flags();
    for (std::size_t other = 0; other < alive.size() && s.relations.size() < 400; ++other) {
        const sim::CharacterId id(static_cast<std::uint32_t>(other));
        if (alive[other] == 0 || id == character || (dynasty.valid() && t.dynasty[other] == dynasty)) {
            continue;
        }
        add_relation(id, "знакомый");
    }
    return s;
}

sim::CharacterId head_for_province(const sim::World& world, std::size_t map_province) {
    if (map_province == 0 || map_province > world.provinces().size()) {
        return sim::CharacterId{};
    }
    const sim::DynastyId owner = world.provinces().owner[map_province - 1];
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
        count += a != 0 ? 1U : 0U;
    }
    if (count == 0) {
        return sim::CharacterId{};
    }
    std::size_t target = hash32(seed) % count;
    for (std::size_t i = 0; i < alive.size(); ++i) {
        if (alive[i] != 0 && target-- == 0) {
            return sim::CharacterId(static_cast<std::uint32_t>(i));
        }
    }
    return sim::CharacterId{};
}

}  // namespace pat::game
