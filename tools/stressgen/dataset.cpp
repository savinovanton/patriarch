#include "pat/stress/dataset.hpp"

#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace pat::stress {
namespace {

constexpr std::uint32_t kMaxRecords = 1'000'000;
constexpr std::array<std::uint8_t, 4> kMagic{'P', 'S', 'L', '1'};

std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes) {
    core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

// Draw another valid index without rejection sampling or a self-reference.
std::uint32_t other_index(core::Pcg32& rng, std::uint32_t index, std::uint32_t count) {
    const auto other = rng.bounded(count - 1);
    return other >= index ? other + 1 : other;
}

}  // namespace

core::Result<Config> profile_config(std::string_view name) {
    using R = core::Result<Config>;
    Config config;
    if (name == "year1") {
        config.world.characters = 1500;
        config.world.date = sim::Date::from_ymd(1703, 1, 1);
        config.armies = 100;
        config.schemes = 20;
        config.contracts = 100;
        config.events = 200;
    } else if (name == "year60x3") {
        config.world.provinces *= 3;
        config.world.characters *= 3;
        config.world.dynasties *= 3;
        config.armies *= 3;
        config.schemes *= 3;
        config.contracts *= 3;
        config.events *= 3;
    } else if (name != "year60") {
        return R::err({1, "profile must be year1, year60 or year60x3"});
    }
    return R::ok(config);
}

core::Result<Dataset> generate(const Config& config) {
    using R = core::Result<Dataset>;
    if (config.armies > kMaxRecords || config.schemes > kMaxRecords || config.contracts > kMaxRecords ||
        config.events > kMaxRecords) {
        return R::err({1, "workload counts exceed the generator limit"});
    }
    if ((config.armies > 0 && (config.world.dynasties == 0 || config.world.provinces == 0)) ||
        (config.schemes > 0 && config.world.characters < 2) ||
        (config.contracts > 0 && config.world.dynasties < 2) ||
        (config.events > 0 && (config.world.characters == 0 || config.world.provinces == 0))) {
        return R::err({1, "requested workload requires missing world entities"});
    }
    auto generated = generate_world(config.world);
    if (!generated) {
        return R::err(generated.error());
    }
    Dataset dataset{std::move(generated).value(), {}};
    auto& world = dataset.world;
    if (config.economy.enabled) {
        // Before the workload streams below, so that the workload header binds the final world hash.
        if (const auto filled = populate_economy(world, config.economy); !filled) {
            return R::err(filled.error());
        }
    }
    auto& workload = dataset.workload;
    auto& armies_rng = world.rng().stream("stress.armies");
    auto& schemes_rng = world.rng().stream("stress.schemes");
    auto& contracts_rng = world.rng().stream("stress.contracts");
    auto& events_rng = world.rng().stream("stress.events");

    auto& armies = workload.armies;
    armies.owner.reserve(config.armies);
    armies.province.reserve(config.armies);
    armies.destination.reserve(config.armies);
    armies.soldiers.reserve(config.armies);
    armies.progress.reserve(config.armies);
    for (std::uint32_t i = 0; i < config.armies; ++i) {
        armies.owner.emplace_back(armies_rng.bounded(config.world.dynasties));
        armies.province.emplace_back(armies_rng.bounded(config.world.provinces));
        armies.destination.emplace_back(armies_rng.bounded(config.world.provinces));
        armies.soldiers.push_back(static_cast<std::uint32_t>(armies_rng.range(100, 10000)));
        armies.progress.push_back(core::Fixed32::from_raw(armies_rng.range(0, core::Fixed32::one_raw - 1)));
    }
    auto& schemes = workload.schemes;
    schemes.actor.reserve(config.schemes);
    schemes.target.reserve(config.schemes);
    schemes.progress_permille.reserve(config.schemes);
    schemes.next_check.reserve(config.schemes);
    for (std::uint32_t i = 0; i < config.schemes; ++i) {
        const auto actor = schemes_rng.bounded(config.world.characters);
        schemes.actor.emplace_back(actor);
        schemes.target.emplace_back(other_index(schemes_rng, actor, config.world.characters));
        schemes.progress_permille.push_back(schemes_rng.bounded(1001));
        schemes.next_check.push_back(world.today() + schemes_rng.range(1, 28));
    }
    auto& contracts = workload.contracts;
    contracts.first.reserve(config.contracts);
    contracts.second.reserve(config.contracts);
    contracts.annual_payment.reserve(config.contracts);
    contracts.expires.reserve(config.contracts);
    for (std::uint32_t i = 0; i < config.contracts; ++i) {
        const auto first = contracts_rng.bounded(config.world.dynasties);
        contracts.first.emplace_back(first);
        contracts.second.emplace_back(other_index(contracts_rng, first, config.world.dynasties));
        contracts.annual_payment.push_back(contracts_rng.range(1000, 1000000));
        contracts.expires.push_back(world.today() + contracts_rng.range(30, 3650));
    }
    auto& events = workload.events;
    events.character.reserve(config.events);
    events.province.reserve(config.events);
    events.trigger.reserve(config.events);
    events.due.reserve(config.events);
    for (std::uint32_t i = 0; i < config.events; ++i) {
        events.character.emplace_back(events_rng.bounded(config.world.characters));
        events.province.emplace_back(events_rng.bounded(config.world.provinces));
        events.trigger.push_back(events_rng.bounded(16));
        events.due.push_back(world.today() + events_rng.range(1, 365));
    }
    if (const auto violations = check_workload(world, workload); !violations.empty()) {
        return R::err({2, violations.front()});
    }
    return R::ok(std::move(dataset));
}

std::vector<std::string> check_workload(const sim::World& world, const Workload& workload) {
    std::vector<std::string> errors;
    const auto& a = workload.armies;
    if (a.owner.size() != a.province.size() || a.owner.size() != a.destination.size() ||
        a.owner.size() != a.soldiers.size() || a.owner.size() != a.progress.size()) {
        errors.emplace_back("army columns differ in size");
    } else {
        for (std::size_t i = 0; i < a.owner.size(); ++i) {
            if (!world.exists(a.owner[i]) || !world.exists(a.province[i]) ||
                !world.exists(a.destination[i]) || a.soldiers[i] == 0 || a.progress[i].raw() < 0 ||
                a.progress[i].raw() >= core::Fixed32::one_raw) {
                errors.emplace_back("invalid army record");
                break;
            }
        }
    }
    const auto& s = workload.schemes;
    if (s.actor.size() != s.target.size() || s.actor.size() != s.progress_permille.size() ||
        s.actor.size() != s.next_check.size()) {
        errors.emplace_back("scheme columns differ in size");
    } else {
        for (std::size_t i = 0; i < s.actor.size(); ++i) {
            if (!world.is_alive(s.actor[i]) || !world.is_alive(s.target[i]) || s.actor[i] == s.target[i] ||
                s.progress_permille[i] > 1000 || s.next_check[i] < world.today()) {
                errors.emplace_back("invalid scheme record");
                break;
            }
        }
    }
    const auto& c = workload.contracts;
    if (c.first.size() != c.second.size() || c.first.size() != c.annual_payment.size() ||
        c.first.size() != c.expires.size()) {
        errors.emplace_back("contract columns differ in size");
    } else {
        for (std::size_t i = 0; i < c.first.size(); ++i) {
            if (!world.exists(c.first[i]) || !world.exists(c.second[i]) || c.first[i] == c.second[i] ||
                c.annual_payment[i] < 0 || c.expires[i] < world.today()) {
                errors.emplace_back("invalid contract record");
                break;
            }
        }
    }
    const auto& e = workload.events;
    if (e.character.size() != e.province.size() || e.character.size() != e.trigger.size() ||
        e.character.size() != e.due.size()) {
        errors.emplace_back("event columns differ in size");
    } else {
        for (std::size_t i = 0; i < e.character.size(); ++i) {
            if (!world.is_alive(e.character[i]) || !world.exists(e.province[i]) || e.trigger[i] >= 16 ||
                e.due[i] < world.today()) {
                errors.emplace_back("invalid event record");
                break;
            }
        }
    }
    return errors;
}

std::vector<std::uint8_t> serialize_workload(const sim::World& world, const Workload& workload) {
    core::ByteWriter payload;
    const auto& a = workload.armies;
    payload.u32(static_cast<std::uint32_t>(a.owner.size()));
    for (std::size_t i = 0; i < a.owner.size(); ++i) {
        payload.u32(a.owner[i].value());
        payload.u32(a.province[i].value());
        payload.u32(a.destination[i].value());
        payload.u32(a.soldiers[i]);
        payload.i32(a.progress[i].raw());
    }
    const auto& s = workload.schemes;
    payload.u32(static_cast<std::uint32_t>(s.actor.size()));
    for (std::size_t i = 0; i < s.actor.size(); ++i) {
        payload.u32(s.actor[i].value());
        payload.u32(s.target[i].value());
        payload.u32(s.progress_permille[i]);
        payload.i32(s.next_check[i].days());
    }
    const auto& c = workload.contracts;
    payload.u32(static_cast<std::uint32_t>(c.first.size()));
    for (std::size_t i = 0; i < c.first.size(); ++i) {
        payload.u32(c.first[i].value());
        payload.u32(c.second[i].value());
        payload.i64(c.annual_payment[i]);
        payload.i32(c.expires[i].days());
    }
    const auto& e = workload.events;
    payload.u32(static_cast<std::uint32_t>(e.character.size()));
    for (std::size_t i = 0; i < e.character.size(); ++i) {
        payload.u32(e.character[i].value());
        payload.u32(e.province[i].value());
        payload.u32(e.trigger[i]);
        payload.i32(e.due[i].days());
    }
    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kWorkloadFormatVersion);
    out.u16(0);
    out.u64(world.compute_hash().total);
    out.u64(hash_bytes(payload.data()));
    out.bytes(payload.data());
    return out.take();
}

core::Result<Workload> deserialize_workload(std::span<const std::uint8_t> bytes, const sim::World& world) {
    using R = core::Result<Workload>;
    core::ByteReader in(bytes);
    const auto magic = in.bytes(kMagic.size());
    if (magic.size() != kMagic.size() || !std::equal(magic.begin(), magic.end(), kMagic.begin()) ||
        in.u16() != kWorkloadFormatVersion || in.u16() != 0) {
        return R::err({3, "invalid stress workload header or version"});
    }
    const auto world_hash = in.u64();
    const auto payload_hash = in.u64();
    if (!in.ok() || world_hash != world.compute_hash().total) {
        return R::err({3, "stress workload belongs to a different world"});
    }
    if (hash_bytes(bytes.subspan(in.position())) != payload_hash) {
        return R::err({3, "stress workload checksum mismatch"});
    }
    auto read_count = [&](std::uint32_t record_size) {
        const auto count = in.u32();
        if (count > kMaxRecords || static_cast<std::uint64_t>(count) * record_size > in.remaining()) {
            in.fail();
            return 0U;
        }
        return count;
    };
    Workload workload;
    const auto armies = read_count(20);
    for (std::uint32_t i = 0; i < armies; ++i) {
        workload.armies.owner.emplace_back(in.u32());
        workload.armies.province.emplace_back(in.u32());
        workload.armies.destination.emplace_back(in.u32());
        workload.armies.soldiers.push_back(in.u32());
        workload.armies.progress.push_back(core::Fixed32::from_raw(in.i32()));
    }
    const auto schemes = read_count(16);
    for (std::uint32_t i = 0; i < schemes; ++i) {
        workload.schemes.actor.emplace_back(in.u32());
        workload.schemes.target.emplace_back(in.u32());
        workload.schemes.progress_permille.push_back(in.u32());
        workload.schemes.next_check.push_back(sim::Date::from_days(in.i32()));
    }
    const auto contracts = read_count(20);
    for (std::uint32_t i = 0; i < contracts; ++i) {
        workload.contracts.first.emplace_back(in.u32());
        workload.contracts.second.emplace_back(in.u32());
        workload.contracts.annual_payment.push_back(in.i64());
        workload.contracts.expires.push_back(sim::Date::from_days(in.i32()));
    }
    const auto events = read_count(16);
    for (std::uint32_t i = 0; i < events; ++i) {
        workload.events.character.emplace_back(in.u32());
        workload.events.province.emplace_back(in.u32());
        workload.events.trigger.push_back(in.u32());
        workload.events.due.push_back(sim::Date::from_days(in.i32()));
    }
    if (!in.ok() || !in.at_end()) {
        return R::err({3, "invalid stress workload counts or trailing data"});
    }
    if (const auto violations = check_workload(world, workload); !violations.empty()) {
        return R::err({3, violations.front()});
    }
    return R::ok(std::move(workload));
}

std::uint64_t compute_hash(const Dataset& dataset) {
    return hash_bytes(serialize_workload(dataset.world, dataset.workload));
}

}  // namespace pat::stress
