#pragma once

#include "pat/stress/world_generator.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pat::stress {

struct Config {
    WorldConfig world;
    std::uint32_t armies = 300;
    std::uint32_t schemes = 100;
    std::uint32_t contracts = 400;
    std::uint32_t events = 500;
};

// Dense synthetic SoA tables, used only by tools/benchmarks until real game systems exist.
// IDs of records are their vector indices; references into World use its strong ID types.
struct ArmyTable {
    std::vector<sim::DynastyId> owner;
    std::vector<sim::ProvinceId> province;
    std::vector<sim::ProvinceId> destination;
    std::vector<std::uint32_t> soldiers;
    std::vector<core::Fixed32> progress;
};

struct SchemeTable {
    std::vector<sim::CharacterId> actor;
    std::vector<sim::CharacterId> target;
    std::vector<std::uint32_t> progress_permille;
    std::vector<sim::Date> next_check;
};

struct ContractTable {
    std::vector<sim::DynastyId> first;
    std::vector<sim::DynastyId> second;
    std::vector<std::int64_t> annual_payment;
    std::vector<sim::Date> expires;
};

struct EventTable {
    std::vector<sim::CharacterId> character;
    std::vector<sim::ProvinceId> province;
    std::vector<std::uint32_t> trigger;
    std::vector<sim::Date> due;
};

struct Workload {
    ArmyTable armies;
    SchemeTable schemes;
    ContractTable contracts;
    EventTable events;
};

struct Dataset {
    sim::World world;
    Workload workload;
};

[[nodiscard]] core::Result<Config> profile_config(std::string_view name);
[[nodiscard]] core::Result<Dataset> generate(const Config& config);
[[nodiscard]] std::vector<std::string> check_workload(const sim::World& world, const Workload& workload);

// PSL1 is a versioned, little-endian companion to an unchanged E1 PSV1 save. Its header binds
// it to the complete World hash. Loading rejects corrupt data and a companion for another save.
inline constexpr std::uint16_t kWorkloadFormatVersion = 1;
// The columns of each table must have equal length; check_workload also validates field values.
[[nodiscard]] std::vector<std::uint8_t> serialize_workload(const sim::World& world, const Workload& workload);
[[nodiscard]] core::Result<Workload> deserialize_workload(std::span<const std::uint8_t> bytes,
                                                          const sim::World& world);
[[nodiscard]] std::uint64_t compute_hash(const Dataset& dataset);

}  // namespace pat::stress
