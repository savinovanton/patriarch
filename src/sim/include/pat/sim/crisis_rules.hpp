#pragma once

#include "pat/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Crisis rules (E12-T04, docs/dev/throne-crisis-model.md sections 8 and 11, ADR-0010): the CONTENT of the
// stability model, the crisis phases, the emperor's projects, claims, suspicion, plots and the coup.
//
// Rules are content, not state: they are not part of the World and are passed to the pure functions of
// the later E12 tasks (stability T05, phases T06, emperor T07, ...). `sim` does not know about data
// files: pat::content::build_crisis_rules() turns the loaded `data/balance/crisis.pd` and
// `data/crisis/projects.pd` into this value. CrisisRules::defaults() is the ONLY place in the code with
// numeric constants of the crisis (the tables below and crisis_rules.cpp); a test keeps the files equal
// to it. Every number is a [predl.] proposal of the specification (calibration is E12-T21).
//
// No floating point: weights are per-mille of one, hundredths of a point carry the suffix `_x100`,
// money is kopecks. Nothing here depends on whether executions exist (#594): punishment tiers are data
// that the consequences of E12-T15 read.

namespace pat::sim::crisis {

/// The eight weighted components of stability (section 8.1); the order is stable (saved by T03).
enum class Component : std::uint8_t { Fiscal, War, Recruit, Mood, Court, Church, Clarity, Authority };
inline constexpr std::size_t kComponentCount = 8;

/// The emperor's projects (section 8.5); the value is the stable `slot` of the project object.
enum class ProjectKind : std::uint8_t { War = 1, Taxes = 2, Purges = 3 };
inline constexpr std::size_t kProjectCount = 3;

/// Crisis phases 0...3 (section 8.2); phase 3 is the coup window.
inline constexpr std::size_t kPhaseCount = 4;

/// Points of the sigmoid table of the coup (section 8.9): diff -100...100 in steps of `sigmoid_step`.
inline constexpr std::size_t kSigmoidPoints = 21;
/// Claim strength tables (section 8.4): kinship degrees 1...5 (5 stands for "5 and more"), places 1...3.
inline constexpr std::size_t kBloodDegrees = 5;
inline constexpr std::size_t kLegalPlaces = 3;
/// Levels of a project are at most this many (war 0...3, taxes -2...3, purges 0...3 fit).
inline constexpr std::size_t kMaxProjectLevels = 8;

// ---- the scalar balance ------------------------------------------------------------------------------
// X(group, type, name, default, min, max). The group is the block of `crisis_balance` in
// data/balance/crisis.pd and the prefix of a diagnostic path; every name is unique. `min`/`max` are the
// corridor that keeps the formulas free of overflow (validate()). One list builds the struct, the
// defaults, the checks and the reader of content/crisis_content.cpp, so a field cannot be added to one
// of them only. Suffixes: _pm per-mille, _x100 hundredths of a point, _kop kopecks, _k a multiplier.
// clang-format off
#define PAT_CRISIS_BALANCE_FIELDS(X)                                                                         \
    X(stability, std::int32_t, base, 50, 0, 100)                                                             \
    X(stability, std::int32_t, step_cap_x100, 500, 1, 10'000)                                                \
    X(stability, std::int32_t, band_up_margin, 3, 0, 20)                                                     \
    X(stability, std::int32_t, legacy_decay_pm, 40, 0, 1'000)                                                \
    X(stability, std::int32_t, legacy_snap_x100, 50, 0, 1'000)                                               \
    X(bands, std::int32_t, order_min, 70, 1, 100)                                                            \
    X(bands, std::int32_t, tension_min, 50, 1, 100)                                                          \
    X(bands, std::int32_t, crisis_min, 30, 1, 100)                                                           \
    X(bands, std::int32_t, plot_ready_order_pm, 500, 100, 5'000)                                             \
    X(bands, std::int32_t, plot_ready_tension_pm, 1'000, 100, 5'000)                                         \
    X(bands, std::int32_t, plot_ready_crisis_pm, 1'250, 100, 5'000)                                          \
    X(bands, std::int32_t, plot_ready_collapse_pm, 1'500, 100, 5'000)                                        \
    X(bands, std::int32_t, detect_order_pm, 1'300, 100, 5'000)                                               \
    X(components, std::int32_t, weight_fiscal, 200, 0, 1'000)                                                \
    X(components, std::int32_t, weight_war, 150, 0, 1'000)                                                   \
    X(components, std::int32_t, weight_recruit, 120, 0, 1'000)                                               \
    X(components, std::int32_t, weight_mood, 200, 0, 1'000)                                                  \
    X(components, std::int32_t, weight_court, 130, 0, 1'000)                                                 \
    X(components, std::int32_t, weight_church, 80, 0, 1'000)                                                 \
    X(components, std::int32_t, weight_clarity, 70, 0, 1'000)                                                \
    X(components, std::int32_t, weight_authority, 50, 0, 1'000)                                              \
    X(components, std::int32_t, fiscal_tax_per_level, 25, 0, 1'000)                                          \
    X(components, std::int32_t, fiscal_arrears_share_k, 40, 0, 1'000)                                        \
    X(components, std::int32_t, fiscal_short_quota_k, 6, 0, 1'000)                                           \
    X(components, std::int32_t, fiscal_short_quota_cap, 30, 0, 100)                                          \
    X(components, std::int32_t, fiscal_deficit, 20, 0, 100)                                                  \
    X(components, std::int32_t, recruit_level_k, 25, 0, 1'000)                                               \
    X(components, std::int32_t, recruit_short_share_k, 40, 0, 1'000)                                         \
    X(components, std::int32_t, mood_gain_pm, 1'500, 0, 10'000)                                              \
    X(components, std::int32_t, estate_nobles, 1'000, 0, 10'000)                                             \
    X(components, std::int32_t, estate_clergy, 600, 0, 10'000)                                               \
    X(components, std::int32_t, estate_townsfolk, 1'000, 0, 10'000)                                          \
    X(components, std::int32_t, estate_peasants, 1'500, 0, 10'000)                                           \
    X(components, std::int32_t, estate_cossacks, 800, 0, 10'000)                                             \
    X(components, std::int32_t, estate_old_believers, 400, 0, 10'000)                                        \
    X(components, std::int32_t, court_base, 30, -100, 100)                                                   \
    X(components, std::int32_t, court_opposition_k, 80, 0, 1'000)                                            \
    X(components, std::int32_t, court_known_plot_k, 15, 0, 1'000)                                            \
    X(components, std::int32_t, court_known_plot_cap, 3, 0, 100)                                             \
    X(components, std::int32_t, court_purge_heat_k_pm, 400, 0, 10'000)                                       \
    X(components, std::int32_t, clarity_vacant, -100, -100, 100)                                             \
    X(components, std::int32_t, clarity_dispute, -70, -100, 100)                                             \
    X(components, std::int32_t, clarity_appointed, 100, -100, 100)                                           \
    X(components, std::int32_t, clarity_lawful_adult, 80, -100, 100)                                         \
    X(components, std::int32_t, clarity_minor, 30, -100, 100)                                               \
    X(components, std::int32_t, clarity_none, -50, -100, 100)                                                \
    X(components, std::int32_t, clarity_rival_k, 20, 0, 1'000)                                               \
    X(components, std::int32_t, clarity_rival_cap, 2, 0, 100)                                                \
    X(components, std::int32_t, clarity_rival_margin, -10, -100, 100)                                        \
    X(components, std::int32_t, authority_k, 2, 0, 100)                                                      \
    X(phases, std::int32_t, p1_enter, 55, 0, 100)                                                            \
    X(phases, std::int32_t, p1_exit, 62, 0, 100)                                                             \
    X(phases, std::int32_t, p1_opposition_share, 25, 0, 100)                                                 \
    X(phases, std::int32_t, p1_opposition_opinion, -20, -100, 100)                                           \
    X(phases, std::int32_t, p2_members, 2, 1, 12)                                                            \
    X(phases, std::int32_t, p2_susp_enter, 60, 0, 100)                                                       \
    X(phases, std::int32_t, p2_susp_exit, 45, 0, 100)                                                        \
    X(phases, std::int32_t, p3_enter, 35, 0, 100)                                                            \
    X(phases, std::int32_t, p3_exit, 42, 0, 100)                                                             \
    X(phases, std::int32_t, p3_split_court, -70, -100, 100)                                                  \
    X(phases, std::int32_t, after_throne, 2, 0, 3)                                                           \
    X(protect, std::int32_t, years, 5, 0, 50)                                                                \
    X(protect, std::int32_t, health_floor, 55, 0, 100)                                                       \
    X(emperor, std::int32_t, ill_enter, 50, 1, 100)                                                          \
    X(emperor, std::int32_t, ill_exit, 58, 1, 100)                                                           \
    X(emperor, std::int32_t, heavy_enter, 30, 1, 100)                                                        \
    X(emperor, std::int32_t, heavy_exit, 38, 1, 100)                                                         \
    X(emperor, std::int32_t, authority_base, 60, 0, 100)                                                     \
    X(emperor, std::int32_t, authority_pull_pm, 80, 0, 1'000)                                                \
    X(emperor, std::int32_t, authority_ill_x100, -150, -10'000, 0)                                           \
    X(emperor, std::int32_t, authority_heavy_x100, -400, -10'000, 0)                                         \
    X(emperor, std::int32_t, authority_riot_x100, -40, -10'000, 0)                                           \
    X(emperor, std::int32_t, authority_riot_cap_x100, -300, -10'000, 0)                                      \
    X(emperor, std::int32_t, authority_minor, 40, 0, 100)                                                    \
    X(emperor, std::int32_t, calm_min_stability, 30, 0, 100)                                                 \
    X(emperor, std::int32_t, challenge_margin, 10, 0, 100)                                                   \
    X(emperor, std::int32_t, interregnum_days, 90, 1, 730)                                                   \
    X(emperor, std::int32_t, auto_claimants, 3, 1, 12)                                                       \
    X(emperor, std::int32_t, interregnum_legacy_x100, -1'000, -10'000, 0)                                    \
    X(emperor, std::int32_t, interregnum_loser_opinion, -10, -100, 0)                                        \
    X(projects, std::int32_t, noise_max, 8, 0, 100)                                                          \
    X(projects, std::int32_t, war_need, 60, 0, 200)                                                          \
    X(projects, std::int32_t, taxes_need_max, 60, 0, 200)                                                    \
    X(projects, std::int32_t, taxes_need_min, -20, -200, 0)                                                  \
    X(projects, std::int64_t, taxes_low_kop, 200'000, 1, 1'000'000'000)                                      \
    X(projects, std::int32_t, purges_susp_pm, 500, 0, 5'000)                                                 \
    X(projects, std::int32_t, purges_plot_k, 25, 0, 1'000)                                                   \
    X(projects, std::int32_t, purges_plot_cap, 2, 0, 100)                                                    \
    X(projects, std::int32_t, recruit_quota_from_year, 1'705, 1'703, 1'800)                                  \
    X(suspicion, std::int32_t, offices_cap, 3, 0, 100)                                                       \
    X(suspicion, std::int32_t, quota_short_x100, 600, 0, 10'000)                                             \
    X(suspicion, std::int32_t, danger_x100, 150, 0, 10'000)                                                  \
    X(suspicion, std::int32_t, danger_guard_below, 40, 0, 100)                                               \
    X(suspicion, std::int32_t, favor_k_pm, 50, 0, 1'000)                                                     \
    X(suspicion, std::int32_t, favor_pivot, 50, 0, 100)                                                      \
    X(suspicion, std::int32_t, decay_pct, 3, 0, 100)                                                         \
    X(suspicion, std::int32_t, decay_min_x100, 20, 0, 10'000)                                                \
    X(suspicion, std::int32_t, expose_organizer, 40, 0, 100)                                                 \
    X(suspicion, std::int32_t, expose_member, 25, 0, 100)                                                    \
    X(suspicion, std::int32_t, confirmed_accused, 50, 0, 100)                                                \
    X(suspicion, std::int32_t, confirmed_informer, -15, -100, 0)                                             \
    X(suspicion, std::int32_t, false_informer, 20, 0, 100)                                                   \
    X(suspicion, std::int32_t, open_claim, 25, 0, 100)                                                       \
    X(suspicion, std::int32_t, band_low, 30, 1, 100)                                                         \
    X(suspicion, std::int32_t, band_mid, 60, 1, 100)                                                         \
    X(suspicion, std::int32_t, band_high, 80, 1, 100)                                                        \
    X(suspicion, std::int32_t, band_hysteresis, 5, 0, 20)                                                    \
    X(suspicion, std::int32_t, denounce_cooldown_days, 90, 0, 3'650)                                         \
    X(suspicion, std::int32_t, investigation_days, 30, 1, 365)                                               \
    X(suspicion, std::int32_t, credibility_favor_k, 4, 0, 100)                                               \
    X(suspicion, std::int32_t, credibility_honor_div, 10, 1, 10'000)                                         \
    X(suspicion, std::int32_t, credibility_cap, 150, 0, 1'000)                                               \
    X(suspicion, std::int32_t, confirm_base, 500, 0, 1'000)                                                  \
    X(suspicion, std::int32_t, confirm_secrecy_k, 4, 0, 100)                                                 \
    X(suspicion, std::int32_t, confirm_min, 100, 0, 1'000)                                                   \
    X(suspicion, std::int32_t, confirm_max, 950, 0, 1'000)                                                   \
    X(suspicion, std::int32_t, false_expose_base, 600, 0, 1'000)                                             \
    X(suspicion, std::int32_t, false_expose_min, 300, 0, 1'000)                                              \
    X(suspicion, std::int32_t, false_expose_max, 900, 0, 1'000)                                              \
    X(suspicion, std::int32_t, informer_leniency_pm, 500, 0, 1'000)                                          \
    X(suspicion, std::int32_t, informer_honor, 10, 0, 1'000)                                                 \
    X(suspicion, std::int32_t, false_honor, -20, -1'000, 0)                                                  \
    X(suspicion, std::int32_t, false_opinion, -20, -100, 0)                                                  \
    X(guard, std::int32_t, base, 60, 0, 100)                                                                 \
    X(guard, std::int32_t, empty_seat, -20, -100, 0)                                                         \
    X(guard, std::int32_t, loyalty_pm, 150, 0, 1'000)                                                        \
    X(guard, std::int32_t, unpaid, -10, -100, 0)                                                             \
    X(guard, std::int32_t, shift_decay, 3, 0, 100)                                                           \
    X(guard, std::int32_t, access_to_guard_pm, 1'000, 0, 10'000)                                             \
    X(plot, std::int32_t, max_members, 12, 2, 12)                                                            \
    X(plot, std::int32_t, prep_per_week_x100, 250, 0, 10'000)                                                \
    X(plot, std::int32_t, strength_lo_pm, 400, 0, 5'000)                                                     \
    X(plot, std::int32_t, strength_hi_pm, 1'600, 0, 5'000)                                                   \
    X(plot, std::int32_t, secrecy_loss_x100, 50, 0, 10'000)                                                  \
    X(plot, std::int32_t, secrecy_member_loss_x100, 40, 0, 10'000)                                           \
    X(plot, std::int32_t, secrecy_info_gain_x100, 10, 0, 10'000)                                             \
    X(plot, std::int32_t, secrecy_info_cap, 30, 0, 100)                                                      \
    X(plot, std::int32_t, detect_base, 4, 0, 1'000)                                                          \
    X(plot, std::int32_t, detect_member, 3, 0, 1'000)                                                        \
    X(plot, std::int32_t, detect_age, 1, 0, 1'000)                                                           \
    X(plot, std::int32_t, detect_age_cap, 30, 0, 1'000)                                                      \
    X(plot, std::int32_t, detect_susp_div, 5, 1, 1'000)                                                      \
    X(plot, std::int32_t, detect_secrecy_div, 10, 1, 1'000)                                                  \
    X(plot, std::int32_t, detect_cap, 250, 1, 1'000)                                                         \
    X(plot, std::int32_t, exposure_base, 1, 0, 100)                                                          \
    X(plot, std::int32_t, exposure_member_div, 2, 1, 100)                                                    \
    X(plot, std::int32_t, idle_days, 60, 1, 3'650)                                                           \
    X(plot, std::int64_t, money_per_point_kop, 2'000, 1, 1'000'000'000)                                      \
    X(plot, std::int32_t, forces_div, 10, 1, 10'000)                                                         \
    X(plot, std::int32_t, honor_div, 50, 1, 10'000)                                                          \
    X(plot, std::int32_t, access_per_guard_seat, 20, 0, 100)                                                 \
    X(plot, std::int32_t, access_other, 5, 0, 100)                                                           \
    X(plot, std::int32_t, accept_threshold, 20, -1'000, 1'000)                                               \
    X(plot, std::int32_t, stance_plotter, 40, -1'000, 1'000)                                                 \
    X(plot, std::int32_t, stance_loyalist, -60, -1'000, 1'000)                                               \
    X(plot, std::int32_t, loyalty_k_pm, 300, 0, 10'000)                                                      \
    X(plot, std::int32_t, disgraced_member, 15, 0, 1'000)                                                    \
    X(plot, std::int32_t, party_candidate, 10, 0, 1'000)                                                     \
    X(plot, std::int32_t, order_penalty, 10, 0, 1'000)                                                       \
    X(coup, std::int32_t, w_guard, 350, 0, 1'000)                                                            \
    X(coup, std::int32_t, w_strength, 300, 0, 1'000)                                                         \
    X(coup, std::int32_t, w_legit, 200, 0, 1'000)                                                            \
    X(coup, std::int32_t, w_authority, 250, 0, 1'000)                                                        \
    X(coup, std::int32_t, w_factions, 200, 0, 1'000)                                                         \
    X(coup, std::int32_t, surprise_max, 12, 0, 100)                                                          \
    X(coup, std::int32_t, alert_known, 10, 0, 100)                                                           \
    X(coup, std::int32_t, alert_susp, 5, 0, 100)                                                             \
    X(coup, std::int32_t, alert_susp_from, 60, 0, 100)                                                       \
    X(coup, std::int32_t, success_min, 20, 0, 1'000)                                                         \
    X(coup, std::int32_t, success_max, 970, 0, 1'000)                                                        \
    X(coup, std::int32_t, partial_base, 350, 0, 1'000)                                                       \
    X(coup, std::int32_t, partial_slope, 4, 0, 100)                                                          \
    X(coup, std::int32_t, partial_min, 100, 0, 1'000)                                                        \
    X(coup, std::int32_t, partial_max, 650, 0, 1'000)                                                        \
    X(coup, std::int32_t, hard_margin, 15, 0, 100)                                                           \
    X(coup, std::int32_t, ai_coup_min_pm, 600, 0, 1'000)                                                     \
    X(coup, std::int32_t, sigmoid_step, 10, 1, 50)                                                           \
    X(stance, std::int32_t, decay_pm, 20, 0, 1'000)                                                          \
    X(stance, std::int32_t, favor_div, 10, 1, 1'000)                                                         \
    X(stance, std::int32_t, offices_cap_x100, 450, 0, 10'000)                                                \
    X(stance, std::int32_t, offices_k_x100, 150, 0, 10'000)                                                  \
    X(stance, std::int32_t, disgrace_loss, 8, 0, 100)                                                        \
    X(stance, std::int32_t, disgrace_cap, 2, 0, 100)                                                         \
    X(stance, std::int32_t, quota_loss, 4, 0, 100)                                                           \
    X(stance, std::int32_t, suspicion_div, 12, 1, 1'000)                                                     \
    X(stance, std::int32_t, party_cap, 3, 0, 100)                                                            \
    X(stance, std::int32_t, authority_div, 20, 1, 1'000)                                                     \
    X(stance, std::int32_t, loyalist_enter, 20, -100, 100)                                                   \
    X(stance, std::int32_t, loyalist_exit, 10, -100, 100)                                                    \
    X(stance, std::int32_t, plotter_enter, -35, -100, 100)                                                   \
    X(stance, std::int32_t, plotter_exit, -20, -100, 100)                                                    \
    X(stance, std::int32_t, plotter_ambition, 40, 0, 100)                                                    \
    X(stance, std::int32_t, min_months, 3, 0, 120)                                                           \
    X(stance, std::int32_t, ai_candidate_min, 35, 0, 100)                                                    \
    X(stance, std::int32_t, ai_contrib_share, 25, 0, 100)                                                    \
    X(stance, std::int32_t, loyalist_denounce_delay_months, 2, 0, 120)                                       \
    X(consequences, std::int32_t, ok_emperor_honor, -100, -10'000, 0)                                        \
    X(consequences, std::int32_t, ok_legacy_x100, -800, -10'000, 0)                                          \
    X(consequences, std::int32_t, ok_candidate_honor, 40, 0, 10'000)                                         \
    X(consequences, std::int32_t, ok_authority, 45, 0, 100)                                                  \
    X(consequences, std::int32_t, ok_organizer_honor, 20, 0, 10'000)                                         \
    X(consequences, std::int32_t, ok_member_honor, 20, 0, 10'000)                                           \
    X(consequences, std::int32_t, ok_reward_offices, 4, 0, 64)                                               \
    X(consequences, std::int32_t, ok_loyalist_susp, 20, 0, 100)                                              \
    X(consequences, std::int32_t, ok_disgrace_days, 365, 0, 3'650)                                           \
    X(consequences, std::int32_t, part_authority, -10, -100, 0)                                              \
    X(consequences, std::int32_t, part_legacy_x100, -500, -10'000, 0)                                        \
    X(consequences, std::int32_t, part_organizer_honor, -10, -10'000, 0)                                     \
    X(consequences, std::int32_t, part_organizer_susp, 15, 0, 100)                                           \
    X(consequences, std::int32_t, part_member_susp, 15, 0, 100)                                              \
    X(consequences, std::int32_t, part_concession_offices, 1, 0, 64)                                         \
    X(consequences, std::int32_t, fail_authority, 5, 0, 100)                                                 \
    X(consequences, std::int32_t, fail_legacy_x100, -300, -10'000, 0)                                        \
    X(consequences, std::int32_t, fail_candidate_honor, -60, -10'000, 0)                                     \
    X(consequences, std::int32_t, fail_organizer_disgrace_days, 730, 0, 3'650)                               \
    X(consequences, std::int32_t, fail_organizer_honor, -60, -10'000, 0)                                     \
    X(consequences, std::int32_t, fail_organizer_confiscate_pm, 300, 0, 1'000)                               \
    X(consequences, std::int32_t, fail_organizer_unity, -10, -100, 0)                                        \
    X(consequences, std::int32_t, fail_organizer_susp, 40, 0, 100)                                           \
    X(consequences, std::int32_t, fail_member_disgrace_days, 365, 0, 3'650)                                  \
    X(consequences, std::int32_t, fail_member_honor, -30, -10'000, 0)                                        \
    X(consequences, std::int32_t, fail_member_confiscate_pm, 100, 0, 1'000)                                  \
    X(consequences, std::int32_t, fail_member_susp, 25, 0, 100)                                              \
    X(consequences, std::int32_t, fail_house_opinion, -20, -100, 0)                                          \
    X(consequences, std::int32_t, hard_authority, 10, 0, 100)                                                \
    X(consequences, std::int32_t, hard_legacy_x100, -500, -10'000, 0)                                        \
    X(consequences, std::int32_t, hard_candidate_honor, -120, -10'000, 0)                                    \
    X(consequences, std::int32_t, hard_organizer_confiscate_pm, 500, 0, 1'000)                               \
    X(consequences, std::int32_t, hard_organizer_unity, -20, -100, 0)                                        \
    X(consequences, std::int32_t, exposed_authority, 3, 0, 100)                                              \
    X(consequences, std::int32_t, exposed_legacy_x100, -300, -10'000, 0)                                     \
    X(consequences, std::int32_t, exposed_candidate_honor, -30, -10'000, 0)                                  \
    X(consequences, std::int32_t, exposed_mild_pm, 500, 0, 1'000)                                            \
    X(consequences, std::int32_t, purge_honor, -20, -10'000, 0)                                              \
    X(consequences, std::int32_t, purge_after_susp, -10, -100, 0)
// clang-format on

/// Scalar balance numbers; member names are the keys of the blocks of `crisis_balance`.
struct CrisisBalance {
#define PAT_CRISIS_FIELD_DECL(group, type, name, def, lo, hi) type group##_##name = def;
    PAT_CRISIS_BALANCE_FIELDS(PAT_CRISIS_FIELD_DECL)
#undef PAT_CRISIS_FIELD_DECL

    /// Probability of success (per-mille) by `diff` in steps of `coup_sigmoid_step` from -100: a
    /// logistic with scale 20 points; p(d) + p(-d) = 1000 (section 8.9).
    std::array<std::int32_t, kSigmoidPoints> sigmoid{7,   11,  18,  29,  47,  76,  119, 182, 269, 378, 500,
                                                     622, 731, 818, 881, 924, 953, 971, 982, 989, 993};

    [[nodiscard]] bool operator==(const CrisisBalance&) const = default;
};

/// Number of scalar balance fields (for the consistency test with the schema).
inline constexpr std::size_t kCrisisBalanceFieldCount = 0
#define PAT_CRISIS_FIELD_COUNT(group, type, name, def, lo, hi) +1
    PAT_CRISIS_BALANCE_FIELDS(PAT_CRISIS_FIELD_COUNT)
#undef PAT_CRISIS_FIELD_COUNT
    ;

// ---- claim_rules (section 8.4) -----------------------------------------------------------------------
// The same X-list scheme as above; the group is the block of `claim_rules`.
// clang-format off
#define PAT_CLAIM_RULE_FIELDS(X)                                                                             \
    X(basis, std::int32_t, marriage, 15, 0, 100)                                                             \
    X(basis, std::int32_t, designation_appointment, 30, 0, 100)                                              \
    X(basis, std::int32_t, designation_other, 10, 0, 100)                                                    \
    X(basis, std::int32_t, kin_max_degree, 4, 1, 8)                                                          \
    X(basis, std::int32_t, force_min, 25, 0, 100)                                                            \
    X(basis, std::int32_t, force_supporters, 2, 1, 64)                                                       \
    X(basis, std::int32_t, max_per_house, 3, 1, 64)                                                          \
    X(support, std::int32_t, guard_k_pm, 150, 0, 10'000)                                                     \
    X(support, std::int32_t, guard_cap, 15, 0, 100)                                                          \
    X(support, std::int32_t, houses_max, 20, 0, 100)                                                         \
    X(support, std::int32_t, council_max, 15, 0, 100)                                                        \
    X(support, std::int32_t, prestige_div, 10, 1, 1'000)                                                     \
    X(support, std::int32_t, prestige_cap, 10, 0, 100)                                                       \
    X(penalty, std::int32_t, outside_line, -30, -100, 0)                                                     \
    X(penalty, std::int32_t, relief_marriage, 15, 0, 100)                                                    \
    X(penalty, std::int32_t, relief_appointment, 30, 0, 100)                                                 \
    X(penalty, std::int32_t, relief_council_max, 20, 0, 100)                                                 \
    X(penalty, std::int32_t, relief_church, 10, 0, 100)                                                      \
    X(penalty, std::int32_t, relief_guard, 10, 0, 100)                                                       \
    X(penalty, std::int32_t, relief_guard_from, 40, 0, 100)                                                  \
    X(penalty, std::int32_t, phase4_pm, 500, 0, 1'000)                                                       \
    X(penalty, std::int32_t, illegitimate, -15, -100, 0)                                                     \
    X(penalty, std::int32_t, disgraced, -10, -100, 0)
// clang-format on

/// Claim strength rules; member names are `<group>_<name>`.
struct ClaimRules {
#define PAT_CLAIM_FIELD_DECL(group, type, name, def, lo, hi) type group##_##name = def;
    PAT_CLAIM_RULE_FIELDS(PAT_CLAIM_FIELD_DECL)
#undef PAT_CLAIM_FIELD_DECL

    /// Blood term by kinship degree 1...5 (5 means "5 and more"); no kinship is 0.
    std::array<std::int32_t, kBloodDegrees> blood{40, 32, 24, 16, 8};
    /// Legal term by place in the succession order of E8 (1st, 2nd, 3rd); later places are 0.
    std::array<std::int32_t, kLegalPlaces> legal{15, 8, 4};

    [[nodiscard]] bool operator==(const ClaimRules&) const = default;
};

inline constexpr std::size_t kClaimRuleFieldCount = 0
#define PAT_CLAIM_FIELD_COUNT(group, type, name, def, lo, hi) +1
    PAT_CLAIM_RULE_FIELDS(PAT_CLAIM_FIELD_COUNT)
#undef PAT_CLAIM_FIELD_COUNT
    ;

// ---- the emperor's projects (section 8.5) ------------------------------------------------------------

/// What a level of a project does: one named list of numbers, one per level from `level_min`.
struct ProjectEffect {
    std::string name;
    std::vector<std::int64_t> per_level;

    [[nodiscard]] bool operator==(const ProjectEffect&) const = default;
};

/// One project of the emperor. The level is chosen from a score `s`: it is `level_min + k` where `k`
/// counts the thresholds with `s >= threshold` (ascending); a tie goes to the lower level.
struct ProjectDef {
    std::uint8_t slot = 0;  ///< stable ProjectKind 1...3
    std::string id;         ///< content id, e.g. "war"
    std::string name_key;
    std::string desc_key;
    std::int32_t level_min = 0;
    std::int32_t level_max = 0;
    /// Score thresholds between neighbouring levels: `level_max - level_min` ascending numbers.
    std::vector<std::int32_t> thresholds;
    /// `response = response_k_pm * max(0, response_pivot - stability) / 1000` (section 8.5).
    std::int32_t response_pivot = 50;
    std::int32_t response_k_pm = 0;
    std::vector<ProjectEffect> effects;

    [[nodiscard]] const ProjectEffect* find_effect(std::string_view name) const noexcept;
    [[nodiscard]] bool operator==(const ProjectDef&) const = default;
};

/// The effects each project must define (names are part of the data contract with E12-T09).
[[nodiscard]] const std::vector<std::string_view>& required_project_effects(ProjectKind kind);

struct CrisisRules {
    CrisisBalance balance;
    ClaimRules claims;
    std::vector<ProjectDef> projects;  ///< ascending `slot`, exactly kProjectCount

    /// The default rules: the one table of numeric constants (crisis_rules.cpp and the lists above).
    [[nodiscard]] static CrisisRules defaults();

    [[nodiscard]] const ProjectDef* find_project(ProjectKind kind) const noexcept;

    /// Stability component weights in per-mille, in the order of `Component`.
    [[nodiscard]] std::array<std::int32_t, kComponentCount> component_weights() const noexcept;

    [[nodiscard]] bool operator==(const CrisisRules&) const = default;
};

struct CrisisRulesError {
    std::string message;
    /// Stable path for content diagnostics: `<group>.<name>`, `sigmoid[i]`, `claim_rules <group>.<name>`
    /// or `emperor_project <id>.<field>`.
    std::string field;

    [[nodiscard]] bool operator==(const CrisisRulesError&) const = default;
};

/// Checks a rules instance and returns the first problem: corridors, weights summing to 1000, ordered
/// thresholds of bands and phases, a monotone symmetric sigmoid, three well-formed projects. Rules that
/// pass can be used by the formulas without further checks.
[[nodiscard]] core::Result<void, CrisisRulesError> validate(const CrisisRules& rules);

}  // namespace pat::sim::crisis
