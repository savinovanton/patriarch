#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Deterministic replays (E1-T09). A replay is the complete input of a run: the seed and starting
// date, the accepted External commands with the day they were applied, and state-hash checkpoints.
// Playing it back against the same scenario must reproduce every checkpoint; the first mismatch
// is reported together with the subsystem whose hash differs.
//
// File layout (little endian):
//   "PRP1" | u16 version | u64 seed | i32 start_day | u64 scenario_id | i64 total_ticks |
//   i64 checkpoint_interval | u32 command_count | { i32 day | command }* |
//   u32 checkpoint_count | { i64 tick | 6 x u64 hash parts }* | u64 checksum

namespace pat::sim {

inline constexpr std::uint16_t kReplayFormatVersion = 1;

struct ReplayCommand {
    Date date;  // the day on which the command was applied (the tick that processed it)
    Command command;
    [[nodiscard]] bool operator==(const ReplayCommand&) const = default;
};

struct ReplayCheckpoint {
    std::int64_t tick = 0;
    WorldHash hash;
    [[nodiscard]] bool operator==(const ReplayCheckpoint&) const = default;
};

struct Replay {
    std::uint64_t seed = 0;
    Date start;
    std::uint64_t scenario_id = 0;  // identifies world + systems; the player only checks that it matches
    std::int64_t total_ticks = 0;
    std::int64_t checkpoint_interval = 30;
    std::vector<ReplayCommand> commands;        // accepted External commands, in application order
    std::vector<ReplayCheckpoint> checkpoints;  // ascending by tick

    [[nodiscard]] bool operator==(const Replay&) const = default;
};

/// Records a replay while a simulation runs: call after_tick() after every tick.
class ReplayRecorder {
public:
    ReplayRecorder(std::uint64_t seed,
                   std::uint64_t scenario_id,
                   Date start,
                   std::int64_t checkpoint_interval = 30);

    void after_tick(const Simulation& simulation);
    /// Finalizes the replay (adds the closing checkpoint and extracts the commands from the log).
    [[nodiscard]] Replay finish(const Simulation& simulation);

private:
    Replay replay_;
};

struct ReplayDivergence {
    std::int64_t tick = 0;
    WorldHash expected;
    WorldHash actual;
    // "calendar", "rng", "dynasties", "provinces" or "characters". A replay checkpoint records only
    // total + these five parts, so when they agree and `total` does not, the cause lies in an
    // extended part and the text lists the parts in use ("extended parts (one of: ...)").
    std::string first_differing_part;
};

struct ReplayResult {
    bool ok = false;
    std::int64_t ticks_run = 0;
    std::string error;  // set when the replay could not be played at all (precondition failure)
    std::optional<ReplayDivergence> divergence;
};

/// Plays a replay on a freshly built simulation of the same scenario: `simulation` must be at tick 0
/// and today() must equal replay.start. Stops at the first checkpoint mismatch.
[[nodiscard]] ReplayResult play_replay(Simulation& simulation, const Replay& replay);

[[nodiscard]] std::vector<std::uint8_t> serialize_replay(const Replay& replay);
[[nodiscard]] core::Result<Replay, core::Error> deserialize_replay(std::span<const std::uint8_t> bytes);

/// Name of the first hash part that differs ("" when all parts are equal). Extended parts are
/// compared last, in the order of their numbers.
[[nodiscard]] std::string first_differing_part(const WorldHash& expected, const WorldHash& actual);

}  // namespace pat::sim
