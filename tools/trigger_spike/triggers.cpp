#include "triggers.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/stress/world_generator.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace pat::trigger_spike {
namespace {

bool predicate(const Definition& event, const Table& table, SubjectId subject) {
    const auto i = subject.index();
    return table.fields[event.fields[0]][i] >= event.lower &&
           table.fields[event.fields[1]][i] <= event.upper &&
           (table.fields[event.fields[2]][i] & event.flag) == event.flag;
}

bool selected(const Fixture& fixture, Scope scope, SubjectId subject) {
    if (fixture.config.sample_permille == 1000) {
        return true;
    }
    const auto date = fixture.world.today().ymd();
    const auto month = static_cast<std::uint64_t>(date.year * 12 + date.month);
    auto key = fixture.sample_key ^ (month * 0x9E3779B97F4A7C15ULL) ^
               (static_cast<std::uint64_t>(scope) << 48U) ^ subject.value();
    return core::split_mix64_next(key) % 1000 < fixture.config.sample_permille;
}

}  // namespace

Index build_index(const std::vector<Definition>& definitions) {
    Index index;
    for (std::size_t rank = 0; rank < definitions.size(); ++rank) {
        const auto& event = definitions[rank];
        const auto scope = static_cast<std::size_t>(event.scope);
        const EventId id(static_cast<std::uint32_t>(rank));
        if (event.monthly) {
            index.monthly[scope].push_back(id);
        } else {
            for (std::size_t action = 0; action < kActions; ++action) {
                if ((event.actions & (1U << action)) != 0) {
                    index.action[scope][action].push_back(id);
                }
            }
        }
    }
    return index;
}

core::Result<Fixture> make_fixture(const Config& config) {
    using R = core::Result<Fixture>;
    if (config.characters == 0 || config.characters > 15000 || config.provinces == 0 ||
        config.provinces > 1200 || config.events == 0 || config.events > 1000 ||
        config.changed_permille > 1000 || config.sample_permille > 1000) {
        return R::err({1, "invalid trigger fixture sizes or permille"});
    }
    stress::WorldConfig world_config;
    world_config.seed = config.seed;
    world_config.characters = config.characters;
    world_config.provinces = config.provinces;
    world_config.dynasties = std::min(100U, config.characters);
    auto world = stress::generate_world(world_config);
    if (!world) {
        return R::err(world.error());
    }
    Fixture fixture{std::move(world).value(), config};
    auto& rng = fixture.world.rng().stream("trigger.fixture");
    for (std::size_t scope = 0; scope < fixture.tables.size(); ++scope) {
        auto& table = fixture.tables[scope];
        const auto size = scope == 0 ? config.characters : config.provinces;
        table.actions.resize(size, 0);
        table.changed.reserve(size);
        table.permutation.reserve(size);
        for (auto& field : table.fields) {
            field.resize(size);
            for (auto& value : field) {
                value = rng.bounded(101);
            }
        }
        for (std::uint32_t i = 0; i < size; ++i) {
            table.permutation.emplace_back(i);
        }
        rng.shuffle<SubjectId>(table.permutation);
        // Four inputs are projections of real E1 data, four are synthetic attributes.
        for (std::size_t i = 0; i < size; ++i) {
            if (scope == 0) {
                const auto& characters = fixture.world.characters();
                table.fields[0][i] =
                    static_cast<std::uint32_t>(characters.health[i].raw() / core::Fixed32::one_raw);
                table.fields[1][i] = static_cast<std::uint32_t>(fixture.world.today().year() -
                                                                characters.birth_date[i].year());
                table.fields[2][i] = characters.sex[i] == sim::Sex::Female ? 100U : 0U;
                table.fields[3][i] = static_cast<std::uint32_t>(
                    fixture.world.dynasties().honor[characters.dynasty[i].index()] / 50);
            } else {
                const auto& provinces = fixture.world.provinces();
                table.fields[0][i] = static_cast<std::uint32_t>(
                    std::min(100, provinces.population[i].raw() / core::Fixed32::one_raw));
                table.fields[1][i] = provinces.development[i];
                table.fields[2][i] = static_cast<std::uint32_t>(
                    fixture.world.dynasties().honor[provinces.owner[i].index()] / 50);
                table.fields[3][i] = provinces.owner[i].value() % 100;
            }
        }
    }
    fixture.definitions.reserve(config.events);
    for (std::uint32_t i = 0; i < config.events; ++i) {
        Definition event;
        event.scope = (i / 5) % 5 == 0 ? Scope::Province : Scope::Character;
        event.monthly = i % 5 == 0;
        event.priority = static_cast<std::uint8_t>(rng.bounded(4));
        const auto action = rng.bounded(kActions);
        event.actions = static_cast<std::uint16_t>(1U << action);
        if (i % 3 == 0) {
            event.actions |= static_cast<std::uint16_t>(1U << ((action + 1 + rng.bounded(15)) % kActions));
        }
        event.fields[0] = static_cast<std::uint8_t>(rng.bounded(kFields));
        event.fields[1] = static_cast<std::uint8_t>((event.fields[0] + 1 + rng.bounded(7)) % kFields);
        do {
            event.fields[2] = static_cast<std::uint8_t>(rng.bounded(kFields));
        } while (event.fields[2] == event.fields[0] || event.fields[2] == event.fields[1]);
        event.lower = static_cast<std::uint32_t>(rng.range(25, 75));
        event.upper = static_cast<std::uint32_t>(rng.range(25, 75));
        event.flag = 1U << rng.bounded(6);
        fixture.definitions.push_back(event);
    }
    std::stable_sort(fixture.definitions.begin(),
                     fixture.definitions.end(),
                     [](const Definition& a, const Definition& b) { return a.priority < b.priority; });
    for (auto& event : fixture.definitions) {
        if (event.monthly) {
            event.audit_offset = fixture.audit_size;
            fixture.audit_size += fixture.tables[static_cast<std::size_t>(event.scope)].size();
        }
    }
    fixture.index = build_index(fixture.definitions);
    fixture.sample_key = fixture.world.rng().stream("trigger.sample").next_u64();
    return R::ok(std::move(fixture));
}

void prepare_tick(Fixture& fixture) {
    auto& rng = fixture.world.rng().stream("trigger.changes");
    for (auto& table : fixture.tables) {
        for (const auto& notification : table.changed) {
            table.actions[notification.subject.index()] = 0;
        }
        table.changed.clear();
        const auto count = (table.size() * fixture.config.changed_permille + 999) / 1000;
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = table.permutation[(table.cursor + i) % table.size()];
            const auto first = rng.bounded(kActions);
            std::uint16_t actions = static_cast<std::uint16_t>(1U << first);
            table.fields[first % kFields][id.index()] = rng.bounded(101);
            if (i % 7 == 0) {
                const auto second = (first + 1 + rng.bounded(15)) % kActions;
                actions |= static_cast<std::uint16_t>(1U << second);
                table.fields[second % kFields][id.index()] = rng.bounded(101);
            }
            table.actions[id.index()] = actions;
            table.changed.push_back({id, actions});
        }
        table.cursor = (table.cursor + count) % table.size();
    }
}

Evaluator::Evaluator(const Fixture& fixture) : seen(fixture.definitions.size(), 0) {
    std::size_t action_pairs = 0;
    std::size_t pulse_pairs = 0;
    for (const auto& event : fixture.definitions) {
        const auto size = fixture.tables[static_cast<std::size_t>(event.scope)].size();
        (event.monthly ? pulse_pairs : action_pairs) += size;
    }
    actions.reserve(action_pairs);
    pulses.reserve(pulse_pairs);
}

void Evaluator::run(const Fixture& fixture, Method method) {
    actions.clear();
    pulses.clear();
    counts = {};
    const auto date = fixture.world.today();
    const auto pulse_day = static_cast<std::uint32_t>(date.day());
    auto evaluate = [&](EventId id, SubjectId subject) {
        const auto& event = fixture.definitions[id.index()];
        ++counts.predicates;
        if (predicate(event, fixture.tables[static_cast<std::size_t>(event.scope)], subject)) {
            (event.monthly ? pulses : actions).push_back({id, subject});
        }
    };
    if (method == Method::Naive) {
        for (std::uint32_t e = 0; e < fixture.definitions.size(); ++e) {
            const auto& event = fixture.definitions[e];
            const auto& table = fixture.tables[static_cast<std::size_t>(event.scope)];
            if (event.monthly && pulse_day > 28) {
                continue;
            }
            counts.visited += table.size();
            for (std::uint32_t i = 0; i < table.size(); ++i) {
                const bool eligible =
                    event.monthly ? i % 28U == pulse_day - 1 : (event.actions & table.actions[i]) != 0;
                if (eligible) {
                    evaluate(EventId(e), SubjectId(i));
                }
            }
        }
    } else {
        for (std::size_t scope = 0; scope < fixture.tables.size(); ++scope) {
            const auto& table = fixture.tables[scope];
            for (const auto& notification : table.changed) {
                if (++stamp == 0) {
                    std::fill(seen.begin(), seen.end(), 0);
                    stamp = 1;
                }
                auto bits = static_cast<std::uint32_t>(notification.actions);
                while (bits != 0) {
                    const auto action = static_cast<std::size_t>(std::countr_zero(bits));
                    bits &= bits - 1;
                    for (const auto event : fixture.index.action[scope][action]) {
                        ++counts.visited;
                        if (seen[event.index()] == stamp) {
                            ++counts.duplicates;
                            continue;
                        }
                        seen[event.index()] = stamp;
                        evaluate(event, notification.subject);
                    }
                }
            }
            const auto start = static_cast<std::size_t>(date.day() - 1);
            if (start < 28) {
                for (auto i = start; i < table.size(); i += 28) {
                    const SubjectId subject(static_cast<std::uint32_t>(i));
                    if (method == Method::Sampled && !selected(fixture, static_cast<Scope>(scope), subject)) {
                        continue;
                    }
                    for (const auto event : fixture.index.monthly[scope]) {
                        ++counts.visited;
                        evaluate(event, subject);
                    }
                }
            }
        }
    }
    // Canonical dispatch order (priority/event rank, then subject), included in evaluation time.
    std::sort(actions.begin(), actions.end());
    std::sort(pulses.begin(), pulses.end());
}

std::uint64_t hit_hash(const std::vector<Hit>& hits) {
    core::HashStream hash;
    hash.u64(hits.size());
    for (const auto hit : hits) {
        hash.u32(hit.event.value());
        hash.u32(hit.subject.value());
    }
    return hash.value();
}

void Audit::observe(const Fixture& fixture, const std::vector<Hit>& hits, int day, bool sampled) {
    auto& first = sampled ? sampled_first : full_first;
    for (const auto hit : hits) {
        auto& entry = first[fixture.definitions[hit.event.index()].audit_offset + hit.subject.index()];
        if (entry < 0) {
            entry = day;
        }
    }
}

}  // namespace pat::trigger_spike
