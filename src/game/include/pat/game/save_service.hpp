#pragma once

#include "pat/core/file_io.hpp"
#include "pat/sim/save.hpp"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

// Writing saves while the game runs (E13-T05, #370, docs/dev/save-format.md 9.3). The UI asks with
// request(); the simulation thread calls capture() at a tick boundary, which takes the immutable world
// snapshot and the pending internal commands and hands them to a background thread (serialize + zstd +
// atomic write). One save at a time, repeated requests merge, the previous file survives as `.bak` (manual
// and quick saves), the result and the class of a disk error land in SaveStatus for the toast / the
// "save ready" screen. A save never changes the world and never enters the CommandLog. Platform-free:
// paths come from the host (save_catalog.hpp builds them).

namespace pat::game {

enum class SaveState : std::uint8_t {
    Idle,     ///< nothing requested yet
    Queued,   ///< requested, waits for the next tick boundary (or for the running save to finish)
    Writing,  ///< the snapshot is taken, the background thread works
    Done,     ///< the last save finished: the file is in place
    Failed,   ///< the last save failed: the previous file is intact, see SaveStatus::error
};

enum class SaveErrorClass : std::uint8_t {
    None,
    DiskFull,      ///< no space left (also: free space known to be below 2x the last save)
    AccessDenied,  ///< no permission, read-only medium, file locked
    PathTooLong,   ///< the path leaves no room for `.tmp` / `.bak` (kMaxSavePathLength) or the OS refused the
                   ///< name
    Other,         ///< anything else (including a written file that failed verification)
};

/// The outcome of the last save for the UI. `message` is an English diagnostic for the log; the player
/// sees a text chosen by `error`.
struct SaveStatus {
    SaveState state = SaveState::Idle;
    SaveErrorClass error = SaveErrorClass::None;
    int os_error = 0;  ///< the OS error value behind `error`, 0 if none
    std::string message;
    std::filesystem::path path;
    sim::SaveKind kind = sim::SaveKind::Manual;
    std::uint64_t sequence = 0;  ///< grows with every accepted save; lets the UI notice a new result
    std::uint64_t bytes_written = 0;
};

struct SaveRequest {
    /// Target file (host-built, see unique_save_path / the system names `_auto1`, `_quick`, ...).
    std::filesystem::path path;
    /// What only the caller knows: scenario, player, head, treasury, goal, play time, kind, name. The
    /// world-derived fields (schema, date, tick, hashes, version) and `saved_at` are filled by the service.
    sim::SaveSummary summary;
};

enum class RequestOutcome : std::uint8_t {
    Queued,    ///< accepted, will be written at the next tick boundary
    Merged,    ///< another request was waiting: replaced by this one (one save is written)
    Skipped,   ///< an autosave while a save is running or queued: dropped
    Rejected,  ///< the path is unusable (too long); status() is Failed right away
};

struct SaveServiceOptions {
    using Writer = std::function<core::Result<void, core::FileIoError>(
        const std::filesystem::path&, std::span<const std::uint8_t>, const core::AtomicWriteOptions&)>;
    using SpaceProbe = std::function<std::optional<std::uint64_t>(const std::filesystem::path& dir)>;

    /// Replaceable for tests (a full disk, a locked file). Default: core::write_file_atomic.
    Writer writer;
    /// Free bytes on the volume of a directory; nullopt = unknown. Default: std::filesystem::space.
    SpaceProbe free_space;
    /// Unix time for `saved_at`. Default: the system clock.
    std::function<std::int64_t()> clock;
    /// Pause between retries of a busy file (antivirus, cloud sync), at most 3 retries. 0 in tests.
    int busy_retry_delay_ms = 200;
};

/// OS error -> class shown to the player.
[[nodiscard]] SaveErrorClass classify_save_error(const std::error_code& ec);

/// Manual and quick saves keep the previous file as `<file>.bak`; autosaves do not (the rotation is
/// their backup).
[[nodiscard]] bool save_keeps_backup(sim::SaveKind kind);

/// `<path>.bak`.
[[nodiscard]] std::filesystem::path backup_path_of(const std::filesystem::path& path);

class SaveService {
public:
    explicit SaveService(SaveServiceOptions options = {});
    /// Waits for a running save.
    ~SaveService();
    SaveService(const SaveService&) = delete;
    SaveService& operator=(const SaveService&) = delete;

    /// Any thread. Does not read the world.
    RequestOutcome request(SaveRequest request);

    /// Simulation thread, between ticks: if a request waits and no save runs, takes the snapshot
    /// (`snapshot`, `pending_internal` as of this boundary) and starts the background write.
    /// Returns true when a write was started. Cheap when nothing is queued.
    bool capture(std::shared_ptr<const sim::World> snapshot, std::span<const sim::Command> pending_internal);

    /// Any thread; a copy of the current state.
    [[nodiscard]] SaveStatus status() const;
    [[nodiscard]] bool busy() const;  ///< Queued or Writing
    /// Blocks until the running save has finished (not for a request that only waits). For tests,
    /// shutdown and "save and exit".
    void wait_idle();

private:
    struct Job;
    void run(Job job);
    void finish(SaveStatus result, std::uint64_t size);

    SaveServiceOptions options_;
    mutable std::mutex mutex_;
    std::optional<SaveRequest> pending_;
    SaveStatus status_;
    bool writing_ = false;
    std::uint64_t last_size_ = 0;
    std::uint64_t sequence_ = 0;
    std::condition_variable idle_cv_;
    std::thread worker_;  ///< touched only by capture() and the destructor
};

}  // namespace pat::game
