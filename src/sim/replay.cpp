#include "pat/sim/replay.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/sim/command_log.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace pat::sim {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'R', 'P', '1'};

core::Error replay_error(const char* message) {
    return core::Error{40, message};
}

std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

void write_hash(core::ByteWriter& out, const WorldHash& h) {
    out.u64(h.total);
    out.u64(h.calendar);
    out.u64(h.rng);
    out.u64(h.dynasties);
    out.u64(h.provinces);
    out.u64(h.characters);
}

WorldHash read_hash(core::ByteReader& in) {
    WorldHash h;
    h.total = in.u64();
    h.calendar = in.u64();
    h.rng = in.u64();
    h.dynasties = in.u64();
    h.provinces = in.u64();
    h.characters = in.u64();
    return h;
}

}  // namespace

std::string first_differing_part(const WorldHash& expected, const WorldHash& actual) {
    if (expected.calendar != actual.calendar) {
        return "calendar";
    }
    if (expected.rng != actual.rng) {
        return "rng";
    }
    if (expected.dynasties != actual.dynasties) {
        return "dynasties";
    }
    if (expected.provinces != actual.provinces) {
        return "provinces";
    }
    if (expected.characters != actual.characters) {
        return "characters";
    }
    return {};
}

ReplayRecorder::ReplayRecorder(std::uint64_t seed,
                               std::uint64_t scenario_id,
                               Date start,
                               std::int64_t checkpoint_interval) {
    replay_.seed = seed;
    replay_.scenario_id = scenario_id;
    replay_.start = start;
    replay_.checkpoint_interval = std::max<std::int64_t>(checkpoint_interval, 1);
}

void ReplayRecorder::after_tick(const Simulation& simulation) {
    const std::int64_t tick = simulation.tick_count();
    if (tick % replay_.checkpoint_interval == 0) {
        replay_.checkpoints.push_back(ReplayCheckpoint{tick, simulation.world().compute_hash()});
    }
}

Replay ReplayRecorder::finish(const Simulation& simulation) {
    Replay out = replay_;
    out.total_ticks = simulation.tick_count();
    if (out.checkpoints.empty() || out.checkpoints.back().tick != out.total_ticks) {
        out.checkpoints.push_back(ReplayCheckpoint{out.total_ticks, simulation.world().compute_hash()});
    }
    for (const LoggedCommand& e : simulation.command_log().entries()) {
        if (e.source == CommandSource::External && e.accepted()) {
            out.commands.push_back(ReplayCommand{e.date, e.command});
        }
    }
    return out;
}

ReplayResult play_replay(Simulation& simulation, const Replay& replay) {
    ReplayResult result;
    if (simulation.tick_count() != 0 || simulation.world().today() != replay.start) {
        result.error = "the simulation must be freshly built and start on the replay's start date";
        return result;
    }

    std::size_t next_command = 0;
    std::size_t next_checkpoint = 0;
    for (std::int64_t t = 1; t <= replay.total_ticks; ++t) {
        const Date day = simulation.world().today() + 1;  // the date this tick will process
        while (next_command < replay.commands.size() && replay.commands[next_command].date == day) {
            if (!simulation.submit(replay.commands[next_command].command)) {
                result.error =
                    "the external command queue overflowed during playback (increase its capacity)";
                result.ticks_run = t - 1;
                return result;
            }
            ++next_command;
        }
        (void)simulation.tick();
        result.ticks_run = t;

        while (next_checkpoint < replay.checkpoints.size() && replay.checkpoints[next_checkpoint].tick == t) {
            const WorldHash actual = simulation.world().compute_hash();
            const WorldHash& expected = replay.checkpoints[next_checkpoint].hash;
            if (actual != expected) {
                result.divergence =
                    ReplayDivergence{t, expected, actual, first_differing_part(expected, actual)};
                return result;
            }
            ++next_checkpoint;
        }
    }

    if (next_command != replay.commands.size()) {
        result.error = "the replay contains commands dated after its last tick";
        return result;
    }
    result.ok = true;
    return result;
}

std::vector<std::uint8_t> serialize_replay(const Replay& replay) {
    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kReplayFormatVersion);
    out.u64(replay.seed);
    out.i32(replay.start.days());
    out.u64(replay.scenario_id);
    out.i64(replay.total_ticks);
    out.i64(replay.checkpoint_interval);
    out.u32(static_cast<std::uint32_t>(replay.commands.size()));
    for (const ReplayCommand& c : replay.commands) {
        out.i32(c.date.days());
        write_command(out, c.command);
    }
    out.u32(static_cast<std::uint32_t>(replay.checkpoints.size()));
    for (const ReplayCheckpoint& c : replay.checkpoints) {
        out.i64(c.tick);
        write_hash(out, c.hash);
    }
    out.u64(checksum(out.data()));
    return out.take();
}

core::Result<Replay, core::Error> deserialize_replay(std::span<const std::uint8_t> bytes) {
    using R = core::Result<Replay, core::Error>;
    if (bytes.size() < kMagic.size() + 2 + 8) {
        return R::err(replay_error("replay is too short"));
    }
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 8);
    core::ByteReader tail(bytes.last(8));
    if (tail.u64() != checksum(body)) {
        return R::err(replay_error("replay checksum mismatch"));
    }

    core::ByteReader in(body);
    const auto magic = in.bytes(kMagic.size());
    if (!in.ok() || !std::equal(magic.begin(), magic.end(), kMagic.begin())) {
        return R::err(replay_error("not a replay file (bad magic)"));
    }
    if (in.u16() != kReplayFormatVersion) {
        return R::err(replay_error("unsupported replay version"));
    }

    Replay replay;
    replay.seed = in.u64();
    replay.start = Date::from_days(in.i32());
    replay.scenario_id = in.u64();
    replay.total_ticks = in.i64();
    replay.checkpoint_interval = in.i64();
    if (!in.ok() || replay.total_ticks < 0 || replay.checkpoint_interval < 1) {
        return R::err(replay_error("invalid replay header"));
    }

    const std::uint32_t command_count = in.u32();
    if (!in.ok() || static_cast<std::uint64_t>(command_count) * 9 > in.remaining()) {
        return R::err(replay_error("invalid command count"));
    }
    replay.commands.reserve(command_count);
    for (std::uint32_t i = 0; i < command_count; ++i) {
        ReplayCommand c;
        c.date = Date::from_days(in.i32());
        if (!read_command(in, c.command)) {
            return R::err(replay_error("corrupt command in replay"));
        }
        if (!replay.commands.empty() && c.date < replay.commands.back().date) {
            return R::err(replay_error("replay commands are not in date order"));
        }
        replay.commands.push_back(c);
    }

    const std::uint32_t checkpoint_count = in.u32();
    if (!in.ok() || static_cast<std::uint64_t>(checkpoint_count) * 56 > in.remaining()) {
        return R::err(replay_error("invalid checkpoint count"));
    }
    replay.checkpoints.reserve(checkpoint_count);
    for (std::uint32_t i = 0; i < checkpoint_count; ++i) {
        ReplayCheckpoint c;
        c.tick = in.i64();
        c.hash = read_hash(in);
        if (!replay.checkpoints.empty() && c.tick <= replay.checkpoints.back().tick) {
            return R::err(replay_error("checkpoints are not in ascending order"));
        }
        replay.checkpoints.push_back(c);
    }
    if (!in.ok() || !in.at_end()) {
        return R::err(replay_error("replay is truncated or has trailing bytes"));
    }
    return R::ok(std::move(replay));
}

}  // namespace pat::sim
