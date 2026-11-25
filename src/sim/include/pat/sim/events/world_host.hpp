#pragma once

#include "pat/core/rng.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/vm.hpp"

#include <cstdint>
#include <span>
#include <vector>

// The host of the event machine over a World (E11-T05, docs/dev/event-engine.md sections 5 and 7.1): the
// transitions of section 5.1, the lists of section 5.2, the predicates and values of the character block
// (E11-T07) and the variables of the event tables. Entries of later blocks (E11-T08, E10, E12) are asked
// through the same two functions when they exist; until then a code the host does not know reads as
// "no data" (false / 0 / an empty scope), exactly like a stub of the catalog.
//
// The host only reads the World. The one thing it consumes is the random stream it is given for the run.

namespace pat::sim::events {

class WorldVmHost final : public VmHost {
public:
    WorldVmHost(const World& world, const Catalog& catalog, TriggerEnv env);

    /// The stream the next runs draw from (events.run / events.mtth / events.ai of the event being run).
    /// Null: a throw-away stream (a program that draws without a stream is a bug of the caller).
    void set_rng(core::Pcg32* rng) noexcept { rng_ = rng; }

    // VmHost
    [[nodiscard]] Scope transition(Scope from, std::uint16_t code) override;
    [[nodiscard]] Word load(Scope scope, std::uint16_t code, std::span<const Word> args) override;
    [[nodiscard]] Word load_var(Scope scope, bool global, std::uint64_t key) override;
    [[nodiscard]] bool predicate(Scope scope, std::uint16_t code, std::span<const Word> args) override;
    [[nodiscard]] bool
    list(Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<Scope>& out) override;
    [[nodiscard]] core::Pcg32& rng() override;

    // CatalogInfo
    [[nodiscard]] std::uint32_t cost(Op op, std::uint16_t code) const override;
    [[nodiscard]] std::uint32_t list_bound(std::uint16_t code) const override;
    [[nodiscard]] std::string_view name(NameSpace space, std::uint16_t code) const override;

private:
    const World& world_;
    const Catalog& catalog_;
    TriggerEnv env_;
    core::Pcg32* rng_ = nullptr;
    core::Pcg32 fallback_{0, 0};
};

}  // namespace pat::sim::events
