#include "pat/core/file_io.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

using namespace pat;

namespace {

namespace fs = std::filesystem;

fs::path scratch_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string("patriarch_core_file_io_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("write_file_atomic: writes, replaces and leaves no temporary file") {
    const fs::path dir = scratch_dir("write");
    const fs::path file = dir / "data.bin";
    const std::vector<std::uint8_t> first{1, 2, 3, 4};
    const std::vector<std::uint8_t> second{9, 8};

    REQUIRE(core::write_file_atomic(file, first).has_value());
    CHECK(slurp(file) == first);
    REQUIRE(core::write_file_atomic(file, second).has_value());
    CHECK(slurp(file) == second);
    REQUIRE(core::write_file_atomic(dir / "empty.bin", {}).has_value());
    CHECK(fs::file_size(dir / "empty.bin") == 0);
    CHECK_FALSE(fs::exists(dir / "data.bin.tmp"));
    fs::remove_all(dir);
}

TEST_CASE("write_file_atomic: reports the failing stage") {
    const fs::path dir = scratch_dir("stages");
    const std::vector<std::uint8_t> bytes{1};

    const auto open_fail = core::write_file_atomic(dir / "no_such_dir" / "f.bin", bytes);
    REQUIRE_FALSE(open_fail.has_value());
    CHECK(open_fail.error().stage == core::FileIoStage::kOpen);

    // The destination is a non-empty directory: the rename fails and the temporary file is removed.
    const fs::path target = dir / "target";
    fs::create_directories(target / "inner");
    const auto rename_fail = core::write_file_atomic(target, bytes);
    REQUIRE_FALSE(rename_fail.has_value());
    CHECK(rename_fail.error().stage == core::FileIoStage::kRename);
    CHECK_FALSE(fs::exists(dir / "target.tmp"));
    fs::remove_all(dir);
}
