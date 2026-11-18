#pragma once

#include "pat/game/save_service.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

// Autosaves and the quick save (E13-T06, #371, docs/dev/save-format.md 9.6). The scheduler decides WHEN to
// ask SaveService for a save and WHICH file it goes to; it owns no thread and reads no clock (the host
// passes a monotonic time in milliseconds, tests pass a fake one). Rotation: `_auto1..3.psv`, the slot
// with the oldest `saved_at` is replaced (a missing slot first). A save is skipped when nothing changed
// since the last one of any kind (same tick and same number of applied commands). On exit a separate
// `_autoexit.psv` is written, outside the rotation. The quick save is one slot, `_quick.psv`.
// Platform-free: the directory comes from the host. Settings are plain values read from settings.pd.

namespace pat::game {

inline constexpr int kAutosaveSlots = 3;

struct AutosaveSettings {
    int interval_min = 10;  ///< 0 = no timer autosave; the menu offers 0, 5, 10, 20
    bool on_exit = true;    ///< write `_autoexit.psv` when the game is left
};

/// What identifies the state of the world for "did anything change": the tick and the count of commands
/// applied so far (a command on a paused world changes it without moving the tick).
struct SaveMark {
    std::int64_t tick = 0;
    std::uint64_t commands = 0;
    [[nodiscard]] bool operator==(const SaveMark&) const = default;
};

/// What the host knows at the moment of asking: the summary fields only it knows (see SaveRequest) and the
/// mark of the world.
struct SaveContext {
    sim::SaveSummary summary;  ///< `kind` is set by the scheduler, `saved_at` by the service
    SaveMark mark;
};

/// `_auto1.psv` .. `_auto3.psv` (slot is 1-based), `_autoexit.psv`, `_quick.psv` inside `dir`.
[[nodiscard]] std::filesystem::path autosave_slot_path(const std::filesystem::path& dir, int slot);
[[nodiscard]] std::filesystem::path autosave_exit_path(const std::filesystem::path& dir);
[[nodiscard]] std::filesystem::path quick_save_path(const std::filesystem::path& dir);

/// The rotation slot (1..kAutosaveSlots) the next autosave goes to: a missing slot first (lowest number),
/// otherwise the one with the oldest save time; an unreadable slot counts as the oldest; ties go to the
/// lower number.
[[nodiscard]] int next_autosave_slot(const std::filesystem::path& dir);

class AutosaveScheduler {
public:
    AutosaveScheduler(SaveService& service, std::filesystem::path dir, AutosaveSettings settings = {});

    void set_settings(AutosaveSettings settings);
    [[nodiscard]] const AutosaveSettings& settings() const { return settings_; }

    /// A game starts or a save was loaded: the timer begins at `now_ms`, and the world as it is counts as
    /// saved (a freshly loaded game is not re-saved until something changes).
    void start(std::int64_t now_ms, SaveMark mark);

    /// Call as often as you like (every frame is fine). When the interval is over and the world changed,
    /// asks for a rotation autosave; nullopt = nothing asked. Dropped by the service while a save runs
    /// (RequestOutcome::Skipped): asked again on the next call.
    std::optional<RequestOutcome> poll(std::int64_t now_ms, const SaveContext& context);

    /// The game is left: asks for `_autoexit.psv` when enabled and the world changed since the last save.
    std::optional<RequestOutcome> save_on_exit(const SaveContext& context);

    /// F5: `_quick.psv`, always (no "unchanged" check: the player asked).
    RequestOutcome quick_save(const SaveContext& context);

    /// F9: the quick save file if it exists (hand it to load_game), otherwise nullopt.
    [[nodiscard]] std::optional<std::filesystem::path> quick_load_path() const;

    /// A manual save the host requested itself: it counts as "the last save".
    void note_saved(SaveMark mark) { last_saved_ = mark; }

private:
    RequestOutcome request(const SaveContext& context, sim::SaveKind kind, const std::filesystem::path& path);

    SaveService& service_;
    std::filesystem::path dir_;
    AutosaveSettings settings_;
    std::int64_t last_ms_ = 0;
    SaveMark last_saved_;
};

}  // namespace pat::game
