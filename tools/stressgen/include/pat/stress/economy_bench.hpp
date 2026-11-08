#pragma once

#include "pat/core/result.hpp"
#include "pat/stress/dataset.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace pat::stress {

/// Measurements of the economy state of a stress world (E9-T05). Wall-clock timing lives here, in the
/// tool, and never reaches the simulation: the numbers are not saved, hashed or used to decide
/// anything. Timings are only meaningful in a Release build on the target PC.
struct EconomyBenchOptions {
    /// Repeats of the cheap operations (hash, invariants, a copy of the world, the sweep).
    std::uint32_t repeats = 30;
    /// Repeats of the expensive ones (serializing, compressing, writing, reading, loading a save).
    std::uint32_t heavy_repeats = 5;
    /// Days the skeleton of the economy systems runs on the loaded world (1..16000).
    std::uint32_t days = 400;
    /// Where the save is written for the disk timings; empty = no disk timings.
    std::filesystem::path scratch;
    /// Delete the scratch file afterwards (set when the path is a temporary one).
    bool remove_scratch = false;
};

/// Builds the world of `config` with and without the economy and measures the economy: memory by
/// table, the time to fill it, to verify and hash it, to copy it (World::snapshot), to save and load
/// it, a read-only sweep as a floor for the rebuild of the modifier cache (E9-T17), and the skeleton
/// of the economy systems with telemetry (`system.economy.*`) on the loaded world. Returns the report
/// as a JSON object. The economy is switched on whatever `config.economy.enabled` says.
[[nodiscard]] core::Result<std::string> run_economy_bench(const Config& config,
                                                          const EconomyBenchOptions& options);

}  // namespace pat::stress
