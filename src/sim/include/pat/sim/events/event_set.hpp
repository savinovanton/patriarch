#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The compiled form of the events (docs/dev/event-engine.md sections 3, 4 and 20; ADR-0008 D1, D8):
// the result of the compiler of pat::content (E11-T06) and the input of the dispatcher (E11-T05).
// `sim` owns the type so that it can be built and tested by hand, without `data`; the World does not
// hold an EventSet (it is content, like the economy rules).
//
// EventSet::hash() identifies the compiled rules: it covers what changes the behaviour (areas, actions,
// ppm, windows, pools, the programs with their constants, the layout of the options) and not the
// texts, `review`, `history`, `sources`, `notes`, `image`, `tags` or `category`.

namespace pat::sim::events {

/// The 64-bit key of an event, an option, a modifier, a flag or a variable: FNV-1a 64 of the UTF-8 id.
/// Never changes (it is stored in saves); a collision of two ids is a compile error (EV105).
[[nodiscard]] std::uint64_t event_key(std::string_view id) noexcept;

enum class EventType : std::uint8_t {
    Personal,
    Family,
    Provincial,
    Court,
    Imperial,
    Church,
    Military,
    Background,
    Hidden,
};
enum class Display : std::uint8_t { Modal, Notification, Silent };
enum class Urgency : std::uint8_t { Critical, Important, Info };
enum class AutoPause : std::uint8_t { Auto, Always, Never };
enum class OnceMode : std::uint8_t { None, Scope, Global };
enum class StackMode : std::uint8_t { Refresh, Replace, Ignore };
enum class ReviewStatus : std::uint8_t { Draft, Reviewed };
enum class HistoryStatus : std::uint8_t { None, Unchecked, Checked };

/// One subscription of an event to an on-action: the action code and which of its roots the event runs on.
struct OnSub {
    std::uint16_t action = 0;
    std::uint8_t role = 0;  // index in OnActionEntry::roles (0 for pulses)

    [[nodiscard]] friend constexpr bool operator==(const OnSub&, const OnSub&) noexcept = default;
};

/// A factor of mean_time_to_happen: the interval is multiplied when the trigger holds (section 12.4).
struct MtthModifier {
    std::int32_t factor_permille = 1000;
    Program trigger;
};

struct OptionDef {
    std::string name;
    std::uint64_t key = 0;
    std::optional<Program> available;  // trigger: false = visible but unavailable
    std::optional<Program> visible;    // show_if
    std::optional<Program> ai_chance;  // accumulator, default weight 10 when absent
    std::optional<Program> effect;
    bool is_default = false;
    bool neutral = false;
    std::uint8_t local_saved = 0;  // saved-scope slots this option's effect uses beyond the event's own
    std::string text_key;
    std::string detail_key;  // empty: none (an automatic line is built by the bridge)
};

struct EventDef {
    std::uint64_t key = 0;
    std::string id;
    ScopeKind scope = ScopeKind::None;
    EventType type = EventType::Personal;
    std::string category;
    std::string image;
    std::vector<std::string> tags;

    std::vector<OnSub> on;
    std::int32_t priority = 50;
    Urgency urgency = Urgency::Important;
    Display display = Display::Modal;
    AutoPause autopause = AutoPause::Auto;
    bool demotable = true;
    bool rumor = false;
    bool survives_death = false;
    std::string group_key;
    bool single_option = false;

    OnceMode once = OnceMode::None;
    std::int32_t cooldown_days = 0;
    std::uint64_t cooldown_group = 0;  // 0: none
    bool has_window = false;
    std::int32_t window_from = 0;  // day numbers of pat::sim::Date
    std::int32_t window_to = 0;
    bool anchor = false;
    std::uint64_t pool = 0;  // 0: none

    /// mean_time_to_happen: the interval in days and the probability per pulse in ppm for each kind of
    /// pulse (section 12.4); 0 when the event has no mean_time_to_happen.
    std::int32_t mtth_days = 0;
    std::array<std::uint32_t, kPulseKindCount> mtth_ppm{};
    std::vector<MtthModifier> mtth_modifiers;

    std::optional<Program> chance;  // percent accumulator
    std::optional<Program> weight;  // pool weight accumulator
    std::optional<Program> trigger;
    std::optional<Program> immediate;
    std::optional<Program> after;
    std::optional<Program> speaker;  // yields a scope word
    std::vector<OptionDef> options;

    /// Names of the saved scopes shared by the programs of the event: slots 0 .. provided-1 are filled
    /// by the on-action (`scope:mother`), the rest by `save_scope_as` in `immediate`.
    std::vector<std::string> saved_names;
    std::uint8_t provided_saved = 0;

    std::string title_key;
    std::string subtitle_key;  // empty: none
    std::string desc_key;
    std::string quote_key;  // empty: none
    ReviewStatus review = ReviewStatus::Draft;
    HistoryStatus history = HistoryStatus::None;
    std::vector<std::string> sources;

    /// Where the event is written (index in EventSet::files, line, column); diagnostics only.
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

struct EventModifierDef {
    std::uint64_t key = 0;
    std::string id;
    ScopeKind scope = ScopeKind::None;
    std::int32_t duration_days = 0;  // 0: permanent
    StackMode stack = StackMode::Refresh;
    std::vector<std::pair<std::uint64_t, core::Fixed64>> values;  // by key, ascending
    std::string tooltip_key;
};

/// The settings of the engine (event-engine.md section 4.5); the defaults are the `[предл.]` values.
struct EventSettings {
    std::int32_t modal_per_month = 6;
    std::int32_t modal_per_month_crisis = 12;
    std::int32_t crisis_phase_from = 2;
    std::int32_t defer_max = 2;
    std::int32_t defer_days = 14;
    std::vector<std::int32_t> frequency_choices{500, 1000, 2000};
    bool frequency_applies_to_all = false;
    std::int32_t max_chain_depth = 8;
    std::int32_t max_timer_chain = 32;
    std::int32_t abort_disable_months = 12;
    std::int32_t fire_cap_per_pulse = 1;
    std::int32_t pulse_month_days = 30;
    std::int32_t pulse_quarter_days = 91;
    std::int32_t pulse_year_days = 365;
    std::int32_t rumor_delay_min_days = 7;
    std::int32_t rumor_delay_max_days = 30;
    std::int32_t notification_ring = 256;
    std::int32_t history_ring = 512;
    /// Limits of the machine, lowered by data (never above limits::).
    std::uint32_t max_trigger_instr = limits::kMaxTriggerInstr;
    std::uint32_t max_effect_instr = limits::kMaxEffectInstr;
    std::uint32_t max_commands = limits::kMaxCommandsPerRun;
    std::uint32_t max_state_ops = limits::kMaxStateOpsPerRun;
    std::uint32_t max_fanout = static_cast<std::uint32_t>(limits::kMaxFanout);

    /// Days of one pulse of a kind (the interval of section 12.4).
    [[nodiscard]] std::int32_t pulse_days(PulseKind kind) const noexcept;
};

/// ppm = 10^6 x pulse_days / mtth_days, rounded to the nearest and clamped to [1, 10^6] (section 12.4).
/// Integer arithmetic only; `mtth_days` and `pulse_days` must be positive (otherwise 0).
[[nodiscard]] std::uint32_t mtth_ppm(std::int32_t pulse_days, std::int64_t mtth_days) noexcept;

class EventSet {
public:
    std::vector<EventDef> events;             // ascending by key
    std::vector<EventModifierDef> modifiers;  // ascending by key
    EventSettings settings;
    /// Names behind the keys of flags, variables, pools and groups used by the programs
    /// (key ascending); for tools and dumps, not part of the hash.
    std::vector<std::pair<std::uint64_t, std::string>> key_names;
    /// Source files of the events, indexed by SourcePos::file of the programs (diagnostics only).
    std::vector<std::string> files;

    [[nodiscard]] const EventDef* find(std::uint64_t key) const noexcept;
    [[nodiscard]] const EventDef* find(std::string_view id) const noexcept;
    [[nodiscard]] const EventModifierDef* find_modifier(std::uint64_t key) const noexcept;
    [[nodiscard]] std::string_view name_of_key(std::uint64_t key) const noexcept;

    /// Canonical bytes of the compiled rules (see the header comment) and their FNV-1a hash.
    [[nodiscard]] std::vector<std::byte> to_bytes() const;
    [[nodiscard]] std::uint64_t hash() const noexcept;
};

}  // namespace pat::sim::events
