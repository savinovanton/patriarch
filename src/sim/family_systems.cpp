#include "pat/sim/family_systems.hpp"

#include "pat/sim/marriage_negotiations.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/simulation.hpp"

#include <utility>

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

void install_family_systems(Simulation& simulation,
                            std::shared_ptr<const MarriageRules> rules,
                            std::shared_ptr<const FaithPolicy> faith) {
    rules = ensure_rules(std::move(rules));
    simulation.set_family_content(rules, faith);
    simulation.add_system(make_marriage_negotiations_system(std::move(rules), std::move(faith)));
}

}  // namespace pat::sim
