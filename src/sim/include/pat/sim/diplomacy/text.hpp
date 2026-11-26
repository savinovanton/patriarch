#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pat::sim::diplomacy {

struct TextKey {
    std::uint16_t code;
    std::string_view key;
};

// Stable codes are reserved by diplomacy-model.md §§3, 7, 10, 12 and 14.
inline constexpr std::array<TextKey, 4> kTreatyTexts{{
    {1, "dipl.treaty.alliance"},
    {2, "dipl.treaty.pledge"},
    {3, "dipl.treaty.marriage_pact"},
    {4, "dipl.treaty.claim_support"},
}};

inline constexpr std::array<TextKey, 9> kRelationReasonTexts{{
    {1, "dipl.reason.gift"},
    {2, "dipl.reason.favor_granted"},
    {3, "dipl.reason.ingratitude"},
    {4, "dipl.reason.offer_refused"},
    {5, "dipl.reason.treaty_honoured"},
    {6, "dipl.reason.treaty_broken"},
    {7, "dipl.reason.denounced"},
    {8, "dipl.reason.false_denunciation"},
    {9, "dipl.reason.scenario"},
}};

inline constexpr std::array<TextKey, 5> kReplyReasonTexts{{
    {1, "dipl.reply.score"},
    {2, "dipl.reply.feud"},
    {3, "dipl.reply.limit"},
    {4, "dipl.reply.no_answer"},
    {5, "dipl.reply.player"},
}};

inline constexpr std::array<TextKey, 28> kDenialTexts{{
    {1, "dipl.denial.disabled"},           {2, "dipl.denial.self_target"},
    {3, "dipl.denial.crown_target"},       {4, "dipl.denial.no_house"},
    {5, "dipl.denial.not_party"},          {6, "dipl.denial.no_economy"},
    {7, "dipl.denial.no_money"},           {8, "dipl.denial.gift_too_small"},
    {9, "dipl.denial.gift_too_large"},     {10, "dipl.denial.gift_cooldown"},
    {11, "dipl.denial.relation_limit"},    {12, "dipl.denial.treaty_limit"},
    {13, "dipl.denial.duplicate_kind"},    {14, "dipl.denial.pair_offer_limit"},
    {15, "dipl.denial.player_inbox_full"}, {16, "dipl.denial.proposal_blocked"},
    {17, "dipl.denial.bad_terms"},         {18, "dipl.denial.no_claim"},
    {19, "dipl.denial.supporter_busy"},    {20, "dipl.denial.favor_debt_full"},
    {21, "dipl.denial.offer_gone"},        {22, "dipl.denial.not_responder"},
    {23, "dipl.denial.not_proposer"},      {24, "dipl.denial.counter_not_allowed"},
    {25, "dipl.denial.treaty_gone"},       {26, "dipl.denial.not_breakable"},
    {27, "dipl.denial.kind_disabled"},     {28, "dipl.denial.system_only"},
}};

inline constexpr std::array<TextKey, 49> kBreakdownTexts{{
    {20000, "dipl.term.code_20000"}, {20001, "dipl.term.code_20001"}, {20002, "dipl.term.code_20002"},
    {20003, "dipl.term.code_20003"}, {20004, "dipl.term.code_20004"}, {20005, "dipl.term.code_20005"},
    {20006, "dipl.term.code_20006"}, {20021, "dipl.term.code_20021"}, {20022, "dipl.term.code_20022"},
    {20023, "dipl.term.code_20023"}, {20024, "dipl.term.code_20024"}, {20100, "dipl.term.code_20100"},
    {20101, "dipl.term.code_20101"}, {20102, "dipl.term.code_20102"}, {20103, "dipl.term.code_20103"},
    {20110, "dipl.term.code_20110"}, {20200, "dipl.term.code_20200"}, {20201, "dipl.term.code_20201"},
    {20202, "dipl.term.code_20202"}, {20203, "dipl.term.code_20203"}, {20204, "dipl.term.code_20204"},
    {20205, "dipl.term.code_20205"}, {20206, "dipl.term.code_20206"}, {20207, "dipl.term.code_20207"},
    {20220, "dipl.term.code_20220"}, {20221, "dipl.term.code_20221"}, {20222, "dipl.term.code_20222"},
    {20223, "dipl.term.code_20223"}, {20224, "dipl.term.code_20224"}, {20225, "dipl.term.code_20225"},
    {20226, "dipl.term.code_20226"}, {20230, "dipl.term.code_20230"}, {20231, "dipl.term.code_20231"},
    {20250, "dipl.term.code_20250"}, {20251, "dipl.term.code_20251"}, {20252, "dipl.term.code_20252"},
    {20253, "dipl.term.code_20253"}, {20300, "dipl.term.code_20300"}, {20400, "dipl.term.code_20400"},
    {20401, "dipl.term.code_20401"}, {20402, "dipl.term.code_20402"}, {20450, "dipl.term.code_20450"},
    {20451, "dipl.term.code_20451"}, {20452, "dipl.term.code_20452"}, {20500, "dipl.term.code_20500"},
    {20501, "dipl.term.code_20501"}, {20502, "dipl.term.code_20502"}, {20503, "dipl.term.code_20503"},
    {20504, "dipl.term.code_20504"},
}};

inline constexpr std::array<TextKey, 13> kAiReasonTexts{{
    {450, "ai.reason.code_450"},
    {451, "ai.reason.code_451"},
    {452, "ai.reason.code_452"},
    {453, "ai.reason.code_453"},
    {454, "ai.reason.code_454"},
    {455, "ai.reason.code_455"},
    {456, "ai.reason.code_456"},
    {457, "ai.reason.code_457"},
    {458, "ai.reason.code_458"},
    {459, "ai.reason.code_459"},
    {460, "ai.reason.code_460"},
    {461, "ai.reason.code_461"},
    {462, "ai.reason.code_462"},
}};

[[nodiscard]] constexpr std::string_view text_key(std::span<const TextKey> entries,
                                                  std::uint16_t code) noexcept {
    for (const auto& entry : entries) {
        if (entry.code == code)
            return entry.key;
    }
    return {};
}

inline constexpr std::string_view kUiDiplomacyKey = "ui.dipl.title";
inline constexpr std::string_view kUiRelationsKey = "ui.dipl.relations";
inline constexpr std::string_view kUiTreatiesKey = "ui.dipl.treaties";
inline constexpr std::string_view kUiOffersKey = "ui.dipl.offers";
inline constexpr std::string_view kUiFavorsKey = "ui.dipl.favors";
inline constexpr std::string_view kUiProposeKey = "ui.dipl.propose";
inline constexpr std::string_view kUiAcceptKey = "ui.dipl.accept";
inline constexpr std::string_view kUiRefuseKey = "ui.dipl.refuse";
inline constexpr std::string_view kUiCounterKey = "ui.dipl.counter";
inline constexpr std::string_view kUiWithdrawKey = "ui.dipl.withdraw";
inline constexpr std::string_view kUiBreakKey = "ui.dipl.break";
inline constexpr std::string_view kUiGiftKey = "ui.dipl.gift";
inline constexpr std::string_view kUiAskFavorKey = "ui.dipl.ask_favor";

}  // namespace pat::sim::diplomacy
