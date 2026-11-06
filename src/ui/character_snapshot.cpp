#include "pat/ui/character_snapshot.hpp"

#include "pat/core/rng.hpp"
#include "pat/ui/explanation.hpp"

#include <algorithm>
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

/// Builds a three-level breakdown whose terms sum exactly to `total`.
Explanation make_skill_explanation(core::Pcg32& rng,
                                   const std::string& name,
                                   int total,
                                   const std::vector<std::string>& traits) {
    Explanation root;
    root.label = name;
    root.value = std::to_string(total);
    root.note = "Навыки лежат в диапазоне 0–20.";

    int remaining = total;
    const auto add = [&](Explanation term) {
        remaining -= std::stoi(term.value);
        root.terms.push_back(std::move(term));
    };

    const int base = std::min(total, static_cast<int>(rng.range(2, 6)));
    add({"Базовое значение", signed_value(base), "", {}});

    // Education, with a mentor (who has a breakdown of their own) and optionally study abroad.
    const int education = std::min(std::max(remaining - 1, 0), static_cast<int>(rng.range(1, 8)));
    if (education > 0) {
        Explanation edu{"Образование", signed_value(education), "", {}};
        const int abroad = education >= 3 ? 1 : 0;
        const int mentor = education - abroad;
        Explanation mentor_term{"Наставник", signed_value(mentor), "", {}};
        const int mentor_skill = mentor / 2;
        mentor_term.terms.push_back({"Навык наставника", signed_value(mentor_skill), "", {}});
        if (mentor - mentor_skill != 0) {
            mentor_term.terms.push_back({"Усердие ученика", signed_value(mentor - mentor_skill), "", {}});
        }
        edu.terms.push_back(std::move(mentor_term));
        if (abroad > 0) {
            edu.terms.push_back({"Обучение за границей", signed_value(abroad), "", {}});
        }
        add(std::move(edu));
    }

    // Traits.
    const int trait_value = static_cast<int>(rng.range(0, 4)) - 1;
    if (trait_value != 0 && !traits.empty()) {
        Explanation tr{"Черты", signed_value(trait_value), "", {}};
        tr.terms.push_back({"Черта «" + traits.front() + "»", signed_value(trait_value), "", {}});
        add(std::move(tr));
    }

    // Whatever is left is attributed to age and experience.
    if (remaining != 0) {
        add({"Возраст и опыт", signed_value(remaining), "", {}});
    }
    return root;
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
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        c.skill_explanations[i] = make_skill_explanation(rng, kSkillNames[i], c.skills[i], c.traits);
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
