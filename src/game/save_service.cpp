#include "pat/game/save_service.hpp"

#include "pat/core/log.hpp"
#include "pat/game/save_catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>
#include <utility>
#include <vector>

namespace pat::game {

namespace {

constexpr int kMaxBusyRetries = 3;
constexpr std::size_t kTempSuffixLength = 4;  // ".tmp" and ".bak"

bool is_busy_error(const std::error_code& ec) {
    if (ec == std::errc::device_or_resource_busy || ec == std::errc::text_file_busy) {
        return true;
    }
#ifdef _WIN32
    return ec.category() == std::system_category() && ec.value() == 32;  // ERROR_SHARING_VIOLATION
#else
    return false;
#endif
}

bool path_fits(const std::filesystem::path& path) {
    return path.native().size() + kTempSuffixLength <= kMaxSavePathLength;
}

std::int64_t system_unix_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::uint64_t> filesystem_space(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::space_info info = std::filesystem::space(dir, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.available);
}

/// Caller-only fields over what the world knows.
sim::SaveSummary
merged_summary(const sim::World& world, const sim::SaveSummary& from_caller, std::int64_t now) {
    sim::SaveSummary s = sim::summary_from_world(world);
    if (from_caller.content_hash != 0) {
        s.content_hash = from_caller.content_hash;
    }
    s.scenario = from_caller.scenario;
    s.player_dynasty = from_caller.player_dynasty;
    s.dynasty_key = from_caller.dynasty_key;
    s.head_name = from_caller.head_name;
    s.head_age = from_caller.head_age;
    s.treasury = from_caller.treasury;
    s.goal_key = from_caller.goal_key;
    s.goal_progress_ppm = from_caller.goal_progress_ppm;
    s.play_time_s = from_caller.play_time_s;
    s.kind = from_caller.kind;
    s.name = from_caller.name;
    s.saved_at = now;
    s.ironman = from_caller.ironman;
    s.preview = from_caller.preview;
    return s;
}

}  // namespace

struct SaveService::Job {
    SaveRequest request;
    std::shared_ptr<const sim::World> snapshot;
    std::vector<sim::Command> pending_internal;
    std::uint64_t sequence = 0;
};

SaveErrorClass classify_save_error(const std::error_code& ec) {
    if (!ec) {
        return SaveErrorClass::Other;
    }
    if (ec == std::errc::no_space_on_device) {
        return SaveErrorClass::DiskFull;
    }
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted ||
        ec == std::errc::read_only_file_system || is_busy_error(ec)) {
        return SaveErrorClass::AccessDenied;
    }
    if (ec == std::errc::filename_too_long) {
        return SaveErrorClass::PathTooLong;
    }
#ifdef _WIN32
    if (ec.category() == std::system_category() && (ec.value() == 112 || ec.value() == 39)) {
        return SaveErrorClass::DiskFull;  // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL
    }
#endif
    return SaveErrorClass::Other;
}

bool save_keeps_backup(sim::SaveKind kind) {
    return kind == sim::SaveKind::Manual || kind == sim::SaveKind::Quick;
}

std::filesystem::path backup_path_of(const std::filesystem::path& path) {
    std::filesystem::path backup = path;
    backup += ".bak";
    return backup;
}

SaveService::SaveService(SaveServiceOptions options) : options_(std::move(options)) {
    if (!options_.writer) {
        options_.writer = [](const std::filesystem::path& path,
                             std::span<const std::uint8_t> bytes,
                             const core::AtomicWriteOptions& write_options) {
            return core::write_file_atomic(path, bytes, write_options);
        };
    }
    if (!options_.free_space) {
        options_.free_space = filesystem_space;
    }
    if (!options_.clock) {
        options_.clock = system_unix_time;
    }
}

SaveService::~SaveService() {
    wait_idle();
    if (worker_.joinable()) {
        worker_.join();
    }
}

RequestOutcome SaveService::request(SaveRequest request) {
    std::lock_guard lock(mutex_);
    const bool occupied = writing_ || pending_.has_value();

    if (!path_fits(request.path)) {
        if (!occupied) {
            status_ = SaveStatus{};
            status_.state = SaveState::Failed;
            status_.error = SaveErrorClass::PathTooLong;
            status_.message = "the save path is too long";
            status_.path = request.path;
            status_.kind = request.summary.kind;
            status_.sequence = ++sequence_;
        }
        return RequestOutcome::Rejected;
    }
    const sim::SaveKind kind = request.summary.kind;
    if (occupied && (kind == sim::SaveKind::Auto || kind == sim::SaveKind::AutoExit)) {
        return RequestOutcome::Skipped;  // an autosave never waits behind another save
    }

    const bool merged = pending_.has_value();
    if (!writing_) {
        status_ = SaveStatus{};
        status_.state = SaveState::Queued;
        status_.path = request.path;
        status_.kind = kind;
        status_.sequence = ++sequence_;
    }
    pending_ = std::move(request);
    return merged ? RequestOutcome::Merged : RequestOutcome::Queued;
}

bool SaveService::capture(std::shared_ptr<const sim::World> snapshot,
                          std::span<const sim::Command> pending_internal) {
    Job job;
    {
        std::lock_guard lock(mutex_);
        if (!pending_.has_value() || writing_) {
            return false;
        }
        job.request = std::move(*pending_);
        pending_.reset();
        writing_ = true;
        status_ = SaveStatus{};
        status_.state = SaveState::Writing;
        status_.path = job.request.path;
        status_.kind = job.request.summary.kind;
        status_.sequence = ++sequence_;
        job.sequence = status_.sequence;
    }
    job.snapshot = std::move(snapshot);
    job.pending_internal.assign(pending_internal.begin(), pending_internal.end());
    if (worker_.joinable()) {
        worker_.join();  // the previous run has finished: writing_ was false
    }
    worker_ = std::thread([this, job = std::move(job)]() mutable { run(std::move(job)); });
    return true;
}

SaveStatus SaveService::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

bool SaveService::busy() const {
    std::lock_guard lock(mutex_);
    return writing_ || pending_.has_value();
}

void SaveService::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] { return !writing_; });
}

void SaveService::run(Job job) {
    SaveStatus result;
    result.state = SaveState::Failed;
    result.path = job.request.path;
    result.kind = job.request.summary.kind;
    result.sequence = job.sequence;

    std::uint64_t last_size = 0;
    {
        std::lock_guard lock(mutex_);
        last_size = last_size_;
    }

    // Too little room is known in advance: do not even start, nothing on the disk is touched.
    if (last_size > 0) {
        const auto free = options_.free_space(job.request.path.parent_path());
        if (free.has_value() && *free < 2 * last_size) {
            result.error = SaveErrorClass::DiskFull;
            result.message = "not enough free disk space for a save";
            finish(std::move(result), 0);
            return;
        }
    }

    const sim::SaveSummary summary = merged_summary(*job.snapshot, job.request.summary, options_.clock());
    const std::vector<std::uint8_t> bytes = sim::serialize_save(*job.snapshot, job.pending_internal, summary);
    job.snapshot.reset();

    core::AtomicWriteOptions write_options;
    write_options.verify = [size = bytes.size()](const std::filesystem::path& temp) {
        const auto info = sim::read_save_summary(temp);
        std::error_code ec;
        const auto on_disk = std::filesystem::file_size(temp, ec);
        return info.has_value() && !ec && on_disk == size;
    };
    if (save_keeps_backup(summary.kind)) {
        write_options.backup_path = backup_path_of(job.request.path);
    }

    core::Result<void, core::FileIoError> written = options_.writer(job.request.path, bytes, write_options);
    for (int attempt = 0; !written && is_busy_error(written.error().ec) && attempt < kMaxBusyRetries;
         ++attempt) {
        if (options_.busy_retry_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options_.busy_retry_delay_ms));
        }
        written = options_.writer(job.request.path, bytes, write_options);
    }

    if (written) {
        result.state = SaveState::Done;
        result.bytes_written = bytes.size();
        finish(std::move(result), bytes.size());
        return;
    }
    const core::FileIoError& failure = written.error();
    result.error = classify_save_error(failure.ec);
    result.os_error = failure.ec.value();
    result.message = failure.stage == core::FileIoStage::kVerify
                         ? "the written file failed verification"
                         : "writing the save failed: " + failure.ec.message();
    finish(std::move(result), 0);
}

void SaveService::finish(SaveStatus result, std::uint64_t size) {
    if (result.state == SaveState::Failed) {
        PAT_LOG_ERROR("save", "save failed (class {}): {}", static_cast<int>(result.error), result.message);
    } else {
        PAT_LOG_INFO("save", "saved {} bytes", size);
    }
    {
        std::lock_guard lock(mutex_);
        if (size > 0) {
            last_size_ = size;
        }
        status_ = std::move(result);
        writing_ = false;
    }
    idle_cv_.notify_all();
}

}  // namespace pat::game
