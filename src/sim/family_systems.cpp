#include "pat/sim/family_systems.hpp"

#include "pat/sim/marriage_negotiations.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/marriage_weddings.hpp"
#include "pat/sim/simulation.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace pat::sim {

namespace {

class MarriageNegotiationsSystem final : public System {
public:
    MarriageNegotiationsSystem(std::shared_ptr<const MarriageRules> rules,
                               std::shared_ptr<const FaithPolicy> faith)
        : rules_(std::move(rules)), faith_(std::move(faith)) {}

    [[nodiscard]] std::string_view name() const override { return kMarriageNegotiationsSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        for (const auto& negotiation : world.family().negotiations) {
            const auto response = compute_marriage_response(world, *rules_, negotiation.id, faith_.get());
            if (response) {
                context.emit(Command{{}, *response});
            }
        }
    }

private:
    std::shared_ptr<const MarriageRules> rules_;
    std::shared_ptr<const FaithPolicy> faith_;
};

class MarriageWeddingsSystem final : public System {
public:
    MarriageWeddingsSystem(std::shared_ptr<const MarriageRules> rules,
                           std::shared_ptr<const FaithPolicy> faith)
        : rules_(std::move(rules)), faith_(std::move(faith)) {}

    [[nodiscard]] std::string_view name() const override { return kMarriageWeddingsSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        for (const auto& betrothal : world.family().betrothals) {
            if (const auto reason = wedding_break_reason(world, *rules_, betrothal, faith_.get())) {
                const cmd::BreakBetrothal payload{betrothal.id, *reason};
                if (validate_break_betrothal(world, *rules_, {}, payload, faith_.get())) {
                    context.emit(Command{{}, payload});
                }
            } else if (betrothal.wedding_not_before <= world.today()) {
                const cmd::Wed payload{betrothal.id};
                if (validate_wed(world, *rules_, payload, faith_.get())) {
                    context.emit(Command{{}, payload});
                }
            }
        }

        // Marriage rows are ordered by id. Preserve their order while issuing only one settlement
        // for a deceased person who appears in several historical records. No state survives a run.
        std::vector<CharacterId> emitted;
        const auto settle = [&](CharacterId deceased) {
            if (world.is_alive(deceased) ||
                std::find(emitted.begin(), emitted.end(), deceased) != emitted.end()) {
                return;
            }
            const cmd::SettleWidowhood payload{deceased};
            if (validate_settle_widowhood(world, payload)) {
                context.emit(Command{{}, payload});
                emitted.push_back(deceased);
            }
        };
        for (const auto& marriage : world.family().marriages) {
            if (marriage.state == family::MarriageState::Active) {
                settle(marriage.groom);
                settle(marriage.bride);
            } else if (marriage.state == family::MarriageState::Widowed) {
                settle(marriage.bride);
            }
        }
    }

private:
    std::shared_ptr<const MarriageRules> rules_;
    std::shared_ptr<const FaithPolicy> faith_;
};

std::shared_ptr<const MarriageRules> ensure_rules(std::shared_ptr<const MarriageRules> rules) {
    return rules ? std::move(rules) : std::make_shared<const MarriageRules>(MarriageRules::defaults());
}

}  // namespace

std::optional<MarriageAnswered> marriage_answered_action(const family::NegotiationOutcome& outcome) noexcept {
    if (outcome.result != family::NegotiationResult::Accepted &&
        outcome.result != family::NegotiationResult::Refused) {
        return std::nullopt;
    }
    return MarriageAnswered{outcome.initiator,
                            outcome.responder,
                            outcome.groom,
                            outcome.bride,
                            outcome.result == family::NegotiationResult::Accepted,
                            outcome.reason};
}

std::unique_ptr<System> make_marriage_negotiations_system(std::shared_ptr<const MarriageRules> rules,
                                                          std::shared_ptr<const FaithPolicy> faith) {
    return std::make_unique<MarriageNegotiationsSystem>(ensure_rules(std::move(rules)), std::move(faith));
}

std::unique_ptr<System> make_marriage_weddings_system(std::shared_ptr<const MarriageRules> rules,
                                                      std::shared_ptr<const FaithPolicy> faith) {
    return std::make_unique<MarriageWeddingsSystem>(ensure_rules(std::move(rules)), std::move(faith));
}

void install_family_systems(Simulation& simulation,
                            std::shared_ptr<const MarriageRules> rules,
                            std::shared_ptr<const FaithPolicy> faith) {
    rules = ensure_rules(std::move(rules));
    simulation.set_family_content(rules, faith);
    simulation.add_system(make_marriage_negotiations_system(rules, faith));
    simulation.add_system(make_marriage_weddings_system(std::move(rules), std::move(faith)));
}

}  // namespace pat::sim
