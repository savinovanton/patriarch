#include "pat/game/autosave.hpp"

#include "pat/game/save_catalog.hpp"

#include <array>
#include <string>
#include <utility>

namespace pat::game {

namespace {

constexpr std::int64_t kMsPerMinute = 60'000;

std::filesystem::path system_save_path(const std::filesystem::path& dir, const std::string& stem) {
    return dir / (stem + std::string(kSaveExtension));
}

}  // namespace

std::filesystem::path autosave_slot_path(const std::filesystem::path& dir, int slot) {
    return system_save_path(dir, "_auto" + std::to_string(slot));
}

std::filesystem::path autosave_exit_path(const std::filesystem::path& dir) {
    return system_save_path(dir, "_autoexit");
}

std::filesystem::path quick_save_path(const std::filesystem::path& dir) {
    return system_save_path(dir, "_quick");
}

int next_autosave_slot(const std::filesystem::path& dir) {
    // Reuses the catalog scan (summary only, no decompression): a save time per slot file.
    std::array<std::optional<std::int64_t>, kAutosaveSlots> times{};
    for (const SaveEntry& entry : scan_saves(dir)) {
        for (int slot = 1; slot <= kAutosaveSlots; ++slot) {
            if (entry.path.filename() == autosave_slot_path({}, slot).filename()) {
                times[static_cast<std::size_t>(slot - 1)] =
                    entry.cls == SaveClass::Corrupt ? std::int64_t{0} : entry.sort_time;
            }
        }
    }
    int best = 1;
    std::int64_t best_time = 0;
    for (int slot = 1; slot <= kAutosaveSlots; ++slot) {
        const auto& t = times[static_cast<std::size_t>(slot - 1)];
        if (!t) {
            return slot;
        }
        if (slot == 1 || *t < best_time) {
            best = slot;
            best_time = *t;
        }
    }
    return best;
}

AutosaveScheduler::AutosaveScheduler(SaveService& service,
                                     std::filesystem::path dir,
                                     AutosaveSettings settings)
    : service_(service), dir_(std::move(dir)), settings_(settings) {}

void AutosaveScheduler::set_settings(AutosaveSettings settings) {
    settings_ = settings;
}

void AutosaveScheduler::start(std::int64_t now_ms, SaveMark mark) {
    last_ms_ = now_ms;
    last_saved_ = mark;
}

RequestOutcome AutosaveScheduler::request(const SaveContext& context,
                                          sim::SaveKind kind,
                                          const std::filesystem::path& path) {
    SaveRequest r;
    r.path = path;
    r.summary = context.summary;
    r.summary.kind = kind;
    const RequestOutcome outcome = service_.request(std::move(r));
    if (outcome == RequestOutcome::Queued || outcome == RequestOutcome::Merged) {
        last_saved_ = context.mark;
    }
    return outcome;
}

std::optional<RequestOutcome> AutosaveScheduler::poll(std::int64_t now_ms, const SaveContext& context) {
    if (settings_.interval_min <= 0 || now_ms - last_ms_ < settings_.interval_min * kMsPerMinute) {
        return std::nullopt;
    }
    if (context.mark == last_saved_) {
        last_ms_ = now_ms;  // nothing to save: look again after another interval
        return std::nullopt;
    }
    const RequestOutcome outcome =
        request(context, sim::SaveKind::Auto, autosave_slot_path(dir_, next_autosave_slot(dir_)));
    if (outcome != RequestOutcome::Skipped) {
        last_ms_ = now_ms;
    }
    return outcome;
}

std::optional<RequestOutcome> AutosaveScheduler::save_on_exit(const SaveContext& context) {
    if (!settings_.on_exit || context.mark == last_saved_) {
        return std::nullopt;
    }
    return request(context, sim::SaveKind::AutoExit, autosave_exit_path(dir_));
}

RequestOutcome AutosaveScheduler::quick_save(const SaveContext& context) {
    return request(context, sim::SaveKind::Quick, quick_save_path(dir_));
}

std::optional<std::filesystem::path> AutosaveScheduler::quick_load_path() const {
    std::error_code ec;
    const std::filesystem::path path = quick_save_path(dir_);
    if (std::filesystem::is_regular_file(path, ec)) {
        return path;
    }
    return std::nullopt;
}

}  // namespace pat::game
