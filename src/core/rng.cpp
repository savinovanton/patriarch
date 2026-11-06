#include "pat/core/rng.hpp"

namespace pat::core {

namespace {

/// Mixes an index into a stream-name hash (SplitMix64 finalizer style); stable across platforms.
std::uint64_t combine_key(std::uint64_t name_hash, std::uint64_t index) noexcept {
    std::uint64_t state = name_hash ^ (index * 0x9E3779B97F4A7C15ULL);
    return split_mix64_next(state);
}

}  // namespace

Pcg32& RngStreams::stream_for_key(std::uint64_t key) {
    const auto it = streams_.find(key);
    if (it != streams_.end()) {
        return it->second;
    }
    std::uint64_t seeder = master_seed_ ^ key;
    const std::uint64_t seed = split_mix64_next(seeder);
    const std::uint64_t sequence = split_mix64_next(seeder);
    return streams_.emplace(key, Pcg32(seed, sequence)).first->second;
}

Pcg32& RngStreams::stream(std::string_view name) {
    return stream_for_key(fnv1a64(name));
}

Pcg32& RngStreams::stream(std::string_view prefix, std::uint64_t index) {
    return stream_for_key(combine_key(fnv1a64(prefix), index));
}

RngSnapshot RngStreams::save() const {
    RngSnapshot snapshot;
    snapshot.master_seed = master_seed_;
    snapshot.streams.reserve(streams_.size());
    for (const auto& [key, rng] : streams_) {
        snapshot.streams.push_back(RngStreamState{key, rng.state(), rng.inc()});
    }
    return snapshot;
}

void RngStreams::restore(const RngSnapshot& snapshot) {
    master_seed_ = snapshot.master_seed;
    streams_.clear();
    for (const RngStreamState& s : snapshot.streams) {
        streams_.emplace(s.key, Pcg32::from_state(s.state, s.inc));
    }
}

}  // namespace pat::core
