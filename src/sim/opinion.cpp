#include "pat/sim/opinion.hpp"

namespace pat::sim {
namespace {

class OpinionSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return kOpinionSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        context.budget.spend(world.decay_opinions(context.date));
    }
};

}  // namespace

std::unique_ptr<System> make_opinion_system() {
    return std::make_unique<OpinionSystem>();
}

}  // namespace pat::sim
