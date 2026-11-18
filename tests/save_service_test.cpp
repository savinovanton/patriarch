// SaveService (E13-T05, #370): the snapshot at a tick boundary, one save at a time, merged requests,
// the `.bak` copy, disk errors by class with the previous file intact. No wall-clock thresholds:
// everything waits on wait_idle().

#include "pat/game/save_catalog.hpp"
#include "pat/game/save_service.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "test_files.hpp"

using namespace pat;
using game::RequestOutcome;
using game::SaveErrorClass;
using game::SaveRequest;
using game::SaveService;
using game::SaveServiceOptions;
using game::SaveState;

namespace {

sim::Simulation make_sim(std::uint64_t seed = 11) {
    sim::demo::Config config;
    config.seed = seed;
    return sim::demo::make_simulation(config);
}

SaveRequest request_for(const std::filesystem::path& path, sim::SaveKind kind, const std::string& name = {}) {
    SaveRequest r;
    r.path = path;
    r.summary.kind = kind;
    r.summary.name = name;
    r.summary.scenario = "m1";
    return r;
}

bool capture(SaveService& service, const sim::Simulation& sim) {
    return service.capture(sim.snapshot(), sim.pending_internal());
}

std::vector<std::uint8_t> slurp(const std::filesystem::path& path) {
    auto bytes = sim::read_file(path);
    REQUIRE(bytes.has_value());
    return std::move(bytes).value();
}

SaveServiceOptions fast_options() {
    SaveServiceOptions o;
    o.busy_retry_delay_ms = 0;
    o.clock = [] { return std::int64_t{1700000000}; };
    return o;
}

core::Result<void, core::FileIoError> fail_with(std::errc code,
                                                core::FileIoStage stage = core::FileIoStage::kWrite) {
    return core::Result<void, core::FileIoError>::err(core::FileIoError{stage, std::make_error_code(code)});
}

}  // namespace

TEST_CASE("save service: saving while the simulation runs gives the same file as stopping it") {
    pat::test::TempDir dir("savesvc_run");
    sim::Simulation sim = make_sim();
    sim.run_days(60);
    const sim::WorldHash at_capture = sim.world().compute_hash();

    SaveService service(fast_options());
    REQUIRE(service.request(request_for(dir.path() / "a.psv", sim::SaveKind::Manual, "A")) ==
            RequestOutcome::Queued);
    CHECK(service.status().state == SaveState::Queued);
    REQUIRE(capture(service, sim));
    sim.run_days(90);  // the simulation moves on during the write
    service.wait_idle();

    const auto status = service.status();
    CHECK(status.state == SaveState::Done);
    CHECK(status.error == SaveErrorClass::None);
    CHECK(status.bytes_written > 0);

    const auto loaded = sim::deserialize_save(slurp(dir.path() / "a.psv"));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.compute_hash() == at_capture);
    CHECK(loaded.value().summary.world_hash == at_capture.total);
    CHECK(loaded.value().summary.kind == sim::SaveKind::Manual);
    CHECK(loaded.value().summary.name == "A");
    CHECK(loaded.value().summary.scenario == "m1");
    CHECK(loaded.value().summary.saved_at == 1700000000);

    // The same moment saved synchronously with the simulation stopped: the same world state.
    sim::Simulation stopped = make_sim();
    stopped.run_days(60);
    const auto sync = sim::deserialize_save(sim::serialize_save(stopped.world(), stopped.pending_internal()));
    REQUIRE(sync.has_value());
    CHECK(sync.value().world.compute_hash() == loaded.value().world.compute_hash());

    // A run that saved evolves exactly like one that did not, and the log gets no save entries.
    sim::Simulation control = make_sim();
    control.run_days(60 + 90);
    CHECK(sim.world().compute_hash() == control.world().compute_hash());
    CHECK(sim.command_log().size() == control.command_log().size());
}

TEST_CASE("save service: two requests in a row merge into one save") {
    pat::test::TempDir dir("savesvc_merge");
    sim::Simulation sim = make_sim();
    sim.run_days(5);

    SaveService service(fast_options());
    CHECK(service.request(request_for(dir.path() / "one.psv", sim::SaveKind::Manual, "one")) ==
          RequestOutcome::Queued);
    CHECK(service.request(request_for(dir.path() / "two.psv", sim::SaveKind::Manual, "two")) ==
          RequestOutcome::Merged);
    CHECK(capture(service, sim));
    CHECK_FALSE(capture(service, sim));  // nothing is waiting any more
    service.wait_idle();

    CHECK(service.status().state == SaveState::Done);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "one.psv"));
    CHECK(std::filesystem::exists(dir.path() / "two.psv"));
    CHECK_FALSE(service.busy());
}

TEST_CASE("save service: one save at a time; an autosave never waits, a manual save does") {
    pat::test::TempDir dir("savesvc_one");
    sim::Simulation sim = make_sim();
    sim.run_days(5);

    // Hold the writer so that the first save stays "running" without any timing guess.
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
    SaveServiceOptions options = fast_options();
    options.writer = [&](const std::filesystem::path& path,
                         std::span<const std::uint8_t> bytes,
                         const core::AtomicWriteOptions& write_options) {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
        return core::write_file_atomic(path, bytes, write_options);
    };
    SaveService service(options);

    REQUIRE(service.request(request_for(dir.path() / "first.psv", sim::SaveKind::Manual)) ==
            RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    while (!entered) {
        std::this_thread::yield();
    }
    CHECK(service.status().state == SaveState::Writing);
    CHECK(service.request(request_for(dir.path() / "_auto1.psv", sim::SaveKind::Auto)) ==
          RequestOutcome::Skipped);
    CHECK(service.request(request_for(dir.path() / "second.psv", sim::SaveKind::Manual)) ==
          RequestOutcome::Queued);
    CHECK_FALSE(capture(service, sim));  // the running save is not interrupted, the second waits
    CHECK(service.status().state == SaveState::Writing);

    release = true;
    service.wait_idle();
    CHECK(service.status().state == SaveState::Done);
    CHECK(std::filesystem::exists(dir.path() / "first.psv"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "second.psv"));

    REQUIRE(capture(service, sim));  // now the waiting request is taken
    service.wait_idle();
    CHECK(std::filesystem::exists(dir.path() / "second.psv"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "_auto1.psv"));
}

TEST_CASE("save service: the previous manual and quick file becomes .bak, autosaves keep none") {
    pat::test::TempDir dir("savesvc_bak");
    sim::Simulation sim = make_sim();
    sim.run_days(10);
    SaveService service(fast_options());
    const auto file = dir.path() / "slot.psv";
    const auto bak = game::backup_path_of(file);

    REQUIRE(service.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    CHECK_FALSE(std::filesystem::exists(bak));
    const std::vector<std::uint8_t> first = slurp(file);

    sim.run_days(10);
    REQUIRE(service.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    CHECK(slurp(bak) == first);
    CHECK(slurp(file) != first);
    CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));

    const auto auto_file = dir.path() / "_auto1.psv";
    for (int i = 0; i < 2; ++i) {
        sim.run_days(3);
        REQUIRE(service.request(request_for(auto_file, sim::SaveKind::Auto)) == RequestOutcome::Queued);
        REQUIRE(capture(service, sim));
        service.wait_idle();
    }
    CHECK(std::filesystem::exists(auto_file));
    CHECK_FALSE(std::filesystem::exists(game::backup_path_of(auto_file)));
    CHECK(game::save_keeps_backup(sim::SaveKind::Quick));
    CHECK_FALSE(game::save_keeps_backup(sim::SaveKind::AutoExit));
}

TEST_CASE("save service: disk errors are classified and the previous file stays intact") {
    pat::test::TempDir dir("savesvc_err");
    sim::Simulation sim = make_sim();
    sim.run_days(10);
    const auto file = dir.path() / "slot.psv";

    {  // a good save first
        SaveService good(fast_options());
        REQUIRE(good.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
        REQUIRE(capture(good, sim));
        good.wait_idle();
    }
    const std::vector<std::uint8_t> before = slurp(file);

    struct Case {
        std::errc code;
        SaveErrorClass expected;
    };
    const Case cases[] = {
        {std::errc::no_space_on_device, SaveErrorClass::DiskFull},
        {std::errc::permission_denied, SaveErrorClass::AccessDenied},
        {std::errc::read_only_file_system, SaveErrorClass::AccessDenied},
        {std::errc::filename_too_long, SaveErrorClass::PathTooLong},
        {std::errc::io_error, SaveErrorClass::Other},
    };
    for (const Case& c : cases) {
        SaveServiceOptions options = fast_options();
        options.writer = [&](const std::filesystem::path&,
                             std::span<const std::uint8_t>,
                             const core::AtomicWriteOptions&) { return fail_with(c.code); };
        SaveService service(options);
        REQUIRE(service.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
        REQUIRE(capture(service, sim));
        service.wait_idle();
        const auto status = service.status();
        CHECK(status.state == SaveState::Failed);
        CHECK(status.error == c.expected);
        CHECK(status.os_error == static_cast<int>(c.code));
        CHECK_FALSE(status.message.empty());
        CHECK(slurp(file) == before);
    }

    // A real failure on the disk: the temporary name is taken by a directory.
    std::filesystem::create_directory(file.string() + ".tmp");
    SaveService real(fast_options());
    REQUIRE(real.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
    REQUIRE(capture(real, sim));
    real.wait_idle();
    CHECK(real.status().state == SaveState::Failed);
    CHECK(slurp(file) == before);
}

TEST_CASE("save service: a file that fails verification is not put in place") {
    pat::test::TempDir dir("savesvc_verify");
    sim::Simulation sim = make_sim();
    sim.run_days(3);
    SaveServiceOptions options = fast_options();
    options.writer = [](const std::filesystem::path& path,
                        std::span<const std::uint8_t> bytes,
                        const core::AtomicWriteOptions& write_options) {
        // Truncated bytes: the service's own verification must refuse them.
        return core::write_file_atomic(path, bytes.first(bytes.size() / 2), write_options);
    };
    SaveService service(options);
    const auto file = dir.path() / "v.psv";
    REQUIRE(service.request(request_for(file, sim::SaveKind::Quick)) == RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    CHECK(service.status().state == SaveState::Failed);
    CHECK(service.status().error == SaveErrorClass::Other);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));
}

TEST_CASE("save service: too little free space refuses before touching the disk") {
    pat::test::TempDir dir("savesvc_space");
    sim::Simulation sim = make_sim();
    sim.run_days(3);
    std::atomic<int> writes{0};
    std::atomic<std::uint64_t> free_bytes{std::uint64_t{1} << 40};
    SaveServiceOptions options = fast_options();
    options.free_space = [&](const std::filesystem::path&) {
        return std::optional<std::uint64_t>(free_bytes.load());
    };
    options.writer = [&](const std::filesystem::path& path,
                         std::span<const std::uint8_t> bytes,
                         const core::AtomicWriteOptions& write_options) {
        ++writes;
        return core::write_file_atomic(path, bytes, write_options);
    };
    SaveService service(options);
    const auto file = dir.path() / "s.psv";

    REQUIRE(service.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    REQUIRE(service.status().state == SaveState::Done);
    const std::vector<std::uint8_t> before = slurp(file);

    free_bytes = 10;
    REQUIRE(service.request(request_for(file, sim::SaveKind::Manual)) == RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    CHECK(service.status().state == SaveState::Failed);
    CHECK(service.status().error == SaveErrorClass::DiskFull);
    CHECK(writes == 1);
    CHECK(slurp(file) == before);
    CHECK_FALSE(std::filesystem::exists(game::backup_path_of(file)));
}

TEST_CASE("save service: a busy file is retried, a path that is too long is rejected") {
    pat::test::TempDir dir("savesvc_busy");
    sim::Simulation sim = make_sim();
    sim.run_days(3);
    std::atomic<int> calls{0};
    SaveServiceOptions options = fast_options();
    options.writer = [&](const std::filesystem::path& path,
                         std::span<const std::uint8_t> bytes,
                         const core::AtomicWriteOptions& write_options) {
        if (++calls <= 2) {
            return fail_with(std::errc::device_or_resource_busy, core::FileIoStage::kRename);
        }
        return core::write_file_atomic(path, bytes, write_options);
    };
    SaveService service(options);
    REQUIRE(service.request(request_for(dir.path() / "b.psv", sim::SaveKind::Manual)) ==
            RequestOutcome::Queued);
    REQUIRE(capture(service, sim));
    service.wait_idle();
    CHECK(service.status().state == SaveState::Done);
    CHECK(calls == 3);

    const std::string long_name(game::kMaxSavePathLength, 'x');
    CHECK(service.request(request_for(dir.path() / (long_name + ".psv"), sim::SaveKind::Manual)) ==
          RequestOutcome::Rejected);
    CHECK(service.status().state == SaveState::Failed);
    CHECK(service.status().error == SaveErrorClass::PathTooLong);
    CHECK_FALSE(capture(service, sim));
}

TEST_CASE("save service: error classes of OS codes") {
    CHECK(game::classify_save_error(std::make_error_code(std::errc::no_space_on_device)) ==
          SaveErrorClass::DiskFull);
    CHECK(game::classify_save_error(std::make_error_code(std::errc::permission_denied)) ==
          SaveErrorClass::AccessDenied);
    CHECK(game::classify_save_error(std::make_error_code(std::errc::filename_too_long)) ==
          SaveErrorClass::PathTooLong);
    CHECK(game::classify_save_error({}) == SaveErrorClass::Other);
}
