#include "pat/sim/ai/text.hpp"

namespace pat::sim::ai {

std::string_view goal_key(GoalKind value) noexcept {
    switch (value) {
        case GoalKind::GainOffice:
            return "ai.goal.gain_office";
        case GoalKind::SecureLine:
            return "ai.goal.secure_line";
        case GoalKind::GrowWealth:
            return "ai.goal.grow_wealth";
        case GoalKind::GainFavor:
            return "ai.goal.gain_favor";
        case GoalKind::ProtectSelf:
            return "ai.goal.protect_self";
        case GoalKind::PreparePlot:
            return "ai.goal.prepare_plot";
        case GoalKind::KeepOrder:
            return "ai.goal.keep_order";
    }
    return {};
}

std::string_view archetype_key(Archetype value) noexcept {
    switch (value) {
        case Archetype::Ambitious:
            return "ai.archetype.ambitious";
        case Archetype::Loyalist:
            return "ai.archetype.loyalist";
        case Archetype::Steward:
            return "ai.archetype.steward";
        case Archetype::Schemer:
            return "ai.archetype.schemer";
        case Archetype::Acquisitive:
            return "ai.archetype.acquisitive";
        case Archetype::Pious:
            return "ai.archetype.pious";
        case Archetype::Warrior:
            return "ai.archetype.warrior";
        case Archetype::Reformer:
            return "ai.archetype.reformer";
    }
    return {};
}

std::string_view difficulty_key(Difficulty value) noexcept {
    switch (value) {
        case Difficulty::Friendly:
            return "ai.difficulty.friendly";
        case Difficulty::Realistic:
            return "ai.difficulty.realistic";
        case Difficulty::Ruthless:
            return "ai.difficulty.ruthless";
    }
    return {};
}

std::string_view reason_key(std::uint16_t code) noexcept {
    switch (code) {
        case 1:  // Issued
            return "ai.reason.code_1";
        case 2:  // NoAp
            return "ai.reason.code_2";
        case 3:  // NoCandidate
            return "ai.reason.code_3";
        case 4:  // Reserved
            return "ai.reason.code_4";
        case 5:  // BudgetDeferred
            return "ai.reason.code_5";
        case 6:  // Cooldown
            return "ai.reason.code_6";
        case 7:  // Committed
            return "ai.reason.code_7";
        case 8:  // RaceRejected
            return "ai.reason.code_8";
        case 9:  // EmitRejected
            return "ai.reason.code_9";
        case 10:  // GoalChanged
            return "ai.reason.code_10";
        case 11:  // HysteresisHold
            return "ai.reason.code_11";
        case 12:  // LowUtility
            return "ai.reason.code_12";
        case 13:  // CommitDropped
            return "ai.reason.code_13";
        case 14:  // FrameExpired
            return "ai.reason.code_14";
        case 15:  // TierRaised
            return "ai.reason.code_15";
        case 16:  // TierRestored
            return "ai.reason.code_16";
    }
    return {};
}

}  // namespace pat::sim::ai
