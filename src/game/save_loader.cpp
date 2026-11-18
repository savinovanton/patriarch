#include "pat/game/save_loader.hpp"

#include "pat/core/log.hpp"
#include "pat/game/save_service.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace pat::game {

namespace {

constexpr int kCodeNoMigration = 21;
constexpr int kCodeDiskFirst = 30;
constexpr int kCodeDiskLast = 39;

class Reporter {
public:
    explicit Reporter(const LoadOptions& options) : options_(options) {}

    void report(LoadStage stage, int percent) {
        percent_ = std::max(percent_, percent);
        if (options_.on_progress) {
            options_.on_progress(LoadProgress{stage, percent_});
        }
    }
    [[nodiscard]] bool cancelled() const { return options_.cancelled && options_.cancelled(); }

private:
    const LoadOptions& options_;
    int percent_ = 0;
};

LoadResult fail(LoadResult base, LoadOutcome outcome, LoadReason reason, std::string message, int code = 0) {
    base.outcome = outcome;
    base.reason = reason;
    base.message = std::move(message);
    base.error_code = code;
    base.simulation.reset();
    if (outcome == LoadOutcome::Corrupt) {
        std::error_code ec;
        base.backup_available = std::filesystem::exists(backup_path_of(base.path), ec) && !ec;
    }
    PAT_LOG_WARN("save", "load of a save failed: {}", base.message);
    return base;
}

LoadResult fail_from_error(LoadResult base, const core::Error& error) {
    if (is_newer_save_error(error.code)) {
        return fail(
            std::move(base), LoadOutcome::NeedsNewerGame, LoadReason::NewerFormat, error.message, error.code);
    }
    if (error.code == kCodeNoMigration) {
        return fail(
            std::move(base), LoadOutcome::Incompatible, LoadReason::NoMigration, error.message, error.code);
    }
    const bool disk = error.code >= kCodeDiskFirst && error.code <= kCodeDiskLast;
    return fail(std::move(base),
                LoadOutcome::Corrupt,
                disk ? LoadReason::UnreadableDisk : LoadReason::Damaged,
                error.message,
                error.code);
}

LoadResult cancelled_result(LoadResult base) {
    base.outcome = LoadOutcome::Cancelled;
    base.reason = LoadReason::None;
    base.message = "cancelled by the caller";
    base.simulation.reset();
    return base;
}

}  // namespace

LoadResult load_game(const std::filesystem::path& path,
                     const content::SessionContent& content,
                     const LoadOptions& options) {
    LoadResult result;
    result.path = path;
    Reporter progress(options);

    // Stage 1: the file and the world (docs/dev/save-format.md 2.4, steps 1-7).
    progress.report(LoadStage::ReadingHistory, 0);
    auto bytes = sim::read_file(path);
    if (!bytes.has_value()) {
        return fail_from_error(std::move(result), bytes.error());
    }
    progress.report(LoadStage::ReadingHistory, 10);
    if (progress.cancelled()) {
        return cancelled_result(std::move(result));
    }

    // The summary decides compatibility before anything is decompressed.
    auto info = sim::read_save_summary(bytes.value(), bytes.value().size());
    if (!info.has_value()) {
        return fail_from_error(std::move(result), info.error());
    }
    result.summary = info.value().summary;
    if (const auto& summary = info.value().summary; summary && !options.ignore_content_hash &&
                                                    content.content_hash != 0 && summary->content_hash != 0 &&
                                                    summary->content_hash != content.content_hash) {
        return fail(std::move(result),
                    LoadOutcome::Incompatible,
                    LoadReason::DifferentContent,
                    "the save was made with another content set");
    }

    auto data = sim::deserialize_save(bytes.value());
    if (!data.has_value()) {
        return fail_from_error(std::move(result), data.error());
    }
    std::vector<std::uint8_t>().swap(bytes.value());  // the file image is not needed any more
    progress.report(LoadStage::ReadingHistory, 60);
    if (progress.cancelled()) {
        return cancelled_result(std::move(result));
    }

    // Stage 2: the content check (step 8) and the session assembly (step 9).
    progress.report(LoadStage::BuildingWorld, 60);
    // The demo systems cannot advance an economy world (simcli refuses it the same way).
    std::string error = content.save_error(data.value().world, options.ignore_content_hash);
    if (error.empty() && !content.has_content && data.value().world.economy_present()) {
        error = "the demo systems cannot advance an economy save";
    }
    if (!error.empty()) {
        return fail(std::move(result), LoadOutcome::Incompatible, LoadReason::DifferentContent, error);
    }
    result.summary = data.value().summary;
    const std::uint64_t saved_world_hash = data.value().summary.world_hash;
    sim::Simulation simulation = content::assemble_session(
        content, std::move(data.value().world), std::move(data.value().pending_internal));
    progress.report(LoadStage::BuildingWorld, 90);
    if (progress.cancelled()) {
        return cancelled_result(std::move(result));
    }

    // Stage 3: the restored world must hash to what was written.
    progress.report(LoadStage::CheckingIntegrity, 90);
    if (saved_world_hash != 0 && simulation.world().compute_hash().total != saved_world_hash) {
        return fail(std::move(result),
                    LoadOutcome::Corrupt,
                    LoadReason::Damaged,
                    "the restored world does not match the hash of the save");
    }
    progress.report(LoadStage::CheckingIntegrity, 100);

    result.outcome = LoadOutcome::Ok;
    result.reason = LoadReason::None;
    result.simulation.emplace(std::move(simulation));
    return result;
}

std::optional<std::filesystem::path> pick_continue_save(std::span<const SaveEntry> entries) {
    // The list is sorted newest first, so the first compatible entry is the newest one.
    for (const SaveEntry& entry : entries) {
        if (entry.cls == SaveClass::Compatible) {
            return entry.path;
        }
    }
    return std::nullopt;
}

LoadResult continue_game(const std::filesystem::path& dir,
                         const content::SessionContent& content,
                         const LoadOptions& options) {
    ScanOptions scan;
    scan.expected_content_hash = options.ignore_content_hash ? 0 : content.content_hash;
    const std::vector<SaveEntry> entries = scan_saves(dir, scan);
    const auto path = pick_continue_save(entries);
    if (!path) {
        LoadResult result;
        result.outcome = LoadOutcome::NothingToLoad;
        result.reason = LoadReason::None;
        result.message = "no compatible save to continue";
        return result;
    }
    return load_game(*path, content, options);
}

}  // namespace pat::game
