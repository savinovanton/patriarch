#pragma once

#include "pat/core/bytes.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

// Helpers of the tests of the saved world state: the bytes of World::save_state, the sections of its
// extension block as editable rows, and "this damaged file must be refused" (family, events).

namespace pat::test {

using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::sim::World;

inline std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

inline auto load_state(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    auto loaded = World::load_state(in);
    if (loaded.has_value()) {
        REQUIRE(in.at_end());
    }
    return loaded;
}

struct Section {
    std::uint16_t id = 0;
    std::uint16_t version = 0;
    std::vector<std::uint8_t> body;
};

struct Sections {
    std::size_t count_at = 0;
    std::size_t end = 0;
    std::vector<Section> rows;
};

inline Sections sections_of(std::span<const std::uint8_t> bytes) {
    Sections sections;
    ByteReader in(bytes);
    (void)in.u32();
    (void)in.i32();
    (void)in.i64();
    (void)in.u64();
    const auto streams = in.u32();
    (void)in.bytes(std::size_t{streams} * 24);
    const auto dynasties = in.u32();
    (void)in.bytes(std::size_t{dynasties} * 12);
    for (std::uint32_t i = 0; i < dynasties; ++i) {
        for (int list = 0; list < 2; ++list) {
            const auto n = in.u32();
            (void)in.bytes(std::size_t{n} * 4);
        }
    }
    const auto provinces = in.u32();
    (void)in.bytes(std::size_t{provinces} * 13);
    const auto capacity = in.u32();
    (void)in.bytes(capacity);
    (void)in.bytes(std::size_t{capacity} * 4);
    const auto free = in.u32();
    (void)in.bytes(std::size_t{free} * 4);
    (void)in.bytes(std::size_t{capacity} * 21);
    (void)in.u32();
    sections.count_at = in.position();
    const auto count = in.u16();
    for (std::uint16_t i = 0; i < count; ++i) {
        Section row;
        row.id = in.u16();
        row.version = in.u16();
        const auto length = in.u32();
        const auto body = in.bytes(length);
        row.body.assign(body.begin(), body.end());
        sections.rows.push_back(std::move(row));
    }
    REQUIRE(in.ok());
    sections.end = in.position();
    return sections;
}

inline Section& section(Sections& sections, std::uint16_t id) {
    const auto found = std::find_if(
        sections.rows.begin(), sections.rows.end(), [&](const Section& row) { return row.id == id; });
    REQUIRE(found != sections.rows.end());
    return *found;
}

inline std::vector<std::uint8_t> edit_sections(const std::vector<std::uint8_t>& bytes,
                                               const std::function<void(Sections&)>& edit) {
    auto sections = sections_of(bytes);
    edit(sections);
    ByteWriter out;
    out.bytes(std::span<const std::uint8_t>(bytes).first(sections.count_at));
    out.u16(static_cast<std::uint16_t>(sections.rows.size()));
    for (const auto& row : sections.rows) {
        out.u16(row.id);
        out.u16(row.version);
        out.u32(static_cast<std::uint32_t>(row.body.size()));
        out.bytes(row.body);
    }
    out.bytes(std::span<const std::uint8_t>(bytes).subspan(sections.end));
    return out.take();
}

inline void expect_rejected(const std::vector<std::uint8_t>& bytes) {
    const auto result = load_state(bytes);
    REQUIRE_FALSE(result.has_value());
}

}  // namespace pat::test
