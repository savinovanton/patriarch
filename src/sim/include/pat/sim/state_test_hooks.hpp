#pragma once

#include <cstdint>

// TEST-ONLY hook of the world state extension block (E13-T03, docs/dev/save-format.md section 5.1).
//
// Real sections are listed in the table of world_io.cpp and there are far fewer of them than the
// section limit allows (16 of 96 today), so a test cannot reach a file with more than 32 sections
// through the game's own data. With a range set here, save_state() appends `count` extra sections
// with consecutive ids starting at `first_id`, each holding a single u32 (its own id), and
// load_state() accepts and checks exactly such sections. The default (count 0) changes nothing: the
// bytes of every file are the same as without the hook. Nothing but tests calls it.
//
// Not thread safe: set it before any save or load and reset it (ScopedTestSections) afterwards.

namespace pat::sim::testing {

struct TestSections {
    std::uint16_t first_id = 0;  ///< must be above the last real section id (40) when count > 0
    std::uint16_t count = 0;
};

void set_test_sections(TestSections range) noexcept;
[[nodiscard]] TestSections test_sections() noexcept;

/// Sets a range for the lifetime of the object and restores "none" afterwards.
class ScopedTestSections {
public:
    explicit ScopedTestSections(TestSections range) noexcept { set_test_sections(range); }
    ~ScopedTestSections() { set_test_sections({}); }
    ScopedTestSections(const ScopedTestSections&) = delete;
    ScopedTestSections& operator=(const ScopedTestSections&) = delete;
};

}  // namespace pat::sim::testing
