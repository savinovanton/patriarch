#include "pat/ui/character_snapshot.hpp"

#include "pat/core/rng.hpp"

#include <array>

namespace pat::ui {

namespace {

constexpr std::array<const char*, 12> kGivenNames = {"Иван",
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
constexpr std::array<const char*, 12> kPatronymics = {"Иванович",
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
constexpr std::array<const char*, 10> kSurnames = {"Шереметев",
                                                   "Голицын",
                                                   "Долгоруков",
                                                   "Апраксин",
                                                   "Салтыков",
                                                   "Нарышкин",
                                                   "Репнин",
                                                   "Стрешнев",
                                                   "Матвеев",
                                                   "Бутурлин"};
constexpr std::array<const char*, 6> kRoles = {
    "стольник", "боярин", "воевода", "окольничий", "стряпчий", "дьяк"};
constexpr std::array<const char*, 4> kHealth = {"крепкое", "хорошее", "слабое", "болезненное"};
constexpr std::array<const char*, 12> kTraits = {"Честолюбивый",
                                                 "Осторожный",
                                                 "Щедрый",
                                                 "Набожный",
                                                 "Хозяйственный",
                                                 "Храбрый",
                                                 "Скрытный",
                                                 "Учёный",
                                                 "Вспыльчивый",
                                                 "Верный",
                                                 "Корыстолюбивый",
                                                 "Тщеславный"};
constexpr std::array<const char*, 6> kKin = {"отец", "мать", "жена", "сын", "дочь", "брат"};

template <class A>
const char* pick(core::Pcg32& rng, const A& list) {
    return list[rng.bounded(static_cast<std::uint32_t>(list.size()))];
}

}  // namespace

std::string CharacterSnapshot::full_name() const {
    return given_name + " " + patronymic + " " + surname;
}

CharacterSnapshot make_test_character(std::uint32_t seed, std::size_t relation_count) {
    core::Pcg32 rng(seed, 0xC4A5ULL);
    CharacterSnapshot c;
    c.given_name = pick(rng, kGivenNames);
    c.patronymic = pick(rng, kPatronymics);
    c.surname = pick(rng, kSurnames);
    c.role = pick(rng, kRoles);
    c.health = pick(rng, kHealth);
    c.age = static_cast<int>(rng.range(18, 70));
    for (int& s : c.skills) {
        s = static_cast<int>(rng.range(2, 18));
    }
    const int trait_count = static_cast<int>(rng.range(2, 4));
    for (int i = 0; i < trait_count; ++i) {
        const std::string t = pick(rng, kTraits);
        bool dup = false;
        for (const std::string& have : c.traits) {
            dup = dup || have == t;
        }
        if (!dup) {
            c.traits.push_back(t);
        }
    }
    const int kin_count = static_cast<int>(rng.range(3, 6));
    for (int i = 0; i < kin_count; ++i) {
        c.family.push_back(std::string(pick(rng, kKin)) + ": " + pick(rng, kGivenNames) + " " +
                           pick(rng, kPatronymics) + " " + c.surname);
    }
    c.relations.reserve(relation_count);
    for (std::size_t i = 0; i < relation_count; ++i) {
        const int opinion = static_cast<int>(rng.range(0, 100)) - 50;
        c.relations.push_back(std::to_string(i + 1) + ". " + pick(rng, kGivenNames) + " " +
                              pick(rng, kPatronymics) + " " + pick(rng, kSurnames) + " — мнение " +
                              (opinion > 0 ? "+" : "") + std::to_string(opinion));
    }
    return c;
}

}  // namespace pat::ui
