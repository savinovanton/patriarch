#include "pat/content/m1_economy_content.hpp"

#include "pat/content/economy_content.hpp"
#include "pat/data/m1_schemas.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

namespace pat::content {

namespace {

using namespace sim::economy;
using core::Fixed32;
using data::ContentObject;
using data::ContentTable;
using data::TypedBlock;
using data::TypedMap;
using data::TypedValue;

constexpr std::array<std::string_view, kGroupCount> kGroupNames{
    "nobility", "clergy", "townsfolk", "peasants", "cossacks", "natives"};

std::int64_t integer(const TypedBlock& block, std::string_view key) {
    const auto* value = block.find(key);
    const auto* number = value != nullptr ? value->get_if<std::int64_t>() : nullptr;
    return number != nullptr ? *number : 0;
}

std::string reference(const TypedBlock& block, std::string_view key) {
    const auto* value = block.find(key);
    const auto* ref = value != nullptr ? value->get_if<data::RefValue>() : nullptr;
    return ref != nullptr ? ref->id : std::string();
}

Fixed32 fixed(const TypedBlock& block, std::string_view key, Fixed32 fallback = Fixed32::zero()) {
    const auto* value = block.find(key);
    const auto* number = value != nullptr ? value->get_if<Fixed32>() : nullptr;
    return number != nullptr ? *number : fallback;
}

const TypedMap* map(const TypedBlock& block, std::string_view key) {
    const auto* value = block.find(key);
    return value != nullptr ? value->get_if<TypedMap>() : nullptr;
}

data::SourcePos position(const ContentObject& object, std::string_view key) {
    for (const auto& field : object.object.body.fields) {
        if (field.name == key) {
            return field.pos;
        }
    }
    return object.object.pos;
}

bool populated(const ContentTable* table) {
    return table != nullptr && !table->empty();
}

class Builder {
public:
    Builder(const data::ContentRegistry& registry, const EconomyRules& rules)
        : registry_(registry), rules_(rules) {}

    M1EconomyContentResult build() {
        const auto* holdings = registry_.table(data::kHoldingKind);
        const auto* nodes = registry_.table(data::kTradeNodeKind);
        const auto* starts = registry_.table(data::kEconomyStartKind);
        if (!populated(holdings) && !populated(nodes) && !populated(starts)) {
            return M1EconomyContentResult::ok({});
        }
        const ContentObject& anchor = populated(holdings) ? holdings->objects().front()
                                      : populated(nodes)  ? nodes->objects().front()
                                                          : starts->objects().front();
        if (!populated(holdings) || !populated(nodes) || !populated(starts)) {
            fail(anchor,
                 anchor.object.pos,
                 "M1 economy requires holding, trade_node and economy_start definitions together");
            return finish();
        }
        read_nodes(*nodes);
        read_start(*starts);
        std::set<std::string, std::less<>> covered;
        for (const ContentObject& object : holdings->objects()) {
            HoldingDef holding = read_holding(object);
            if (!covered.insert(holding.province).second) {
                fail(object,
                     position(object, "province"),
                     std::format("duplicate holding for province '{}'", holding.province));
            }
            out_.holdings.push_back(std::move(holding));
        }
        if (const auto* provinces = registry_.table("province")) {
            for (const ContentObject& province : provinces->objects()) {
                if (!covered.contains(province.object.id)) {
                    fail(province,
                         province.object.pos,
                         std::format("province '{}' has no holding in M1 economy", province.object.id));
                }
            }
        }
        std::sort(out_.holdings.begin(), out_.holdings.end(), [](const HoldingDef& a, const HoldingDef& b) {
            return a.province < b.province;
        });
        return finish();
    }

private:
    const data::ContentRegistry& registry_;
    const EconomyRules& rules_;
    M1EconomyContent out_;
    std::vector<data::LoadError> errors_;
    std::map<std::string, std::uint16_t, std::less<>> node_slots_;

    void fail(const ContentObject& object, data::SourcePos pos, std::string message) {
        errors_.push_back({object.root,
                           registry_.root_name(object.root),
                           object.file,
                           data::Diagnostic{pos, data::ErrorCode::ContentRule, std::move(message)}});
    }

    M1EconomyContentResult finish() {
        if (!errors_.empty()) {
            std::stable_sort(errors_.begin(), errors_.end(), [](const auto& a, const auto& b) {
                return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.message) <
                       std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.message);
            });
            return M1EconomyContentResult::err(std::move(errors_));
        }
        return M1EconomyContentResult::ok(std::move(out_));
    }

    void read_nodes(const ContentTable& table) {
        std::set<std::uint16_t> slots;
        for (const ContentObject& object : table.objects()) {
            const auto slot = static_cast<std::uint16_t>(integer(object.object.body, "slot"));
            if (!slots.insert(slot).second) {
                fail(object, position(object, "slot"), std::format("duplicate trade node slot {}", slot));
            }
            node_slots_.emplace(object.object.id, slot);
            const auto* key = object.object.body.find("name_key")->get_if<std::string>();
            out_.trade_nodes.push_back({slot, object.object.id, *key});
        }
        std::sort(out_.trade_nodes.begin(),
                  out_.trade_nodes.end(),
                  [](const TradeNodeDef& a, const TradeNodeDef& b) { return a.slot < b.slot; });
    }

    void read_start(const ContentTable& table) {
        const ContentObject& object = table.objects().front();
        if (table.size() != 1) {
            for (std::size_t i = 1; i < table.size(); ++i) {
                const ContentObject& extra = table.objects()[i];
                fail(extra, extra.object.pos, "only one economy_start object is allowed");
            }
        }
        out_.crown_treasury_kop = integer(object.object.body, "crown_treasury_kop");
        if (const TypedMap* treasuries = map(object.object.body, "dynasty_treasuries")) {
            std::set<std::string, std::less<>> seen;
            for (const auto& entry : treasuries->entries) {
                if (!seen.insert(entry.name).second) {
                    fail(object, entry.pos, std::format("duplicate dynasty treasury '{}'", entry.name));
                }
                if (registry_.find("dynasty", entry.name) == nullptr) {
                    fail(object, entry.pos, std::format("unknown dynasty treasury '{}'", entry.name));
                } else {
                    out_.dynasty_treasuries.emplace_back(entry.name, *entry.value.get_if<std::int64_t>());
                }
            }
        }
        std::sort(out_.dynasty_treasuries.begin(), out_.dynasty_treasuries.end());
    }

    template <class Assign>
    void read_groups(const ContentObject& object, std::string_view key, Assign assign) {
        if (const TypedMap* values = map(object.object.body, key)) {
            std::set<std::string, std::less<>> seen;
            for (const auto& entry : values->entries) {
                const auto group = std::find(kGroupNames.begin(), kGroupNames.end(), entry.name);
                if (!seen.insert(entry.name).second) {
                    fail(object,
                         entry.pos,
                         std::format("duplicate population group '{}' in {}", entry.name, key));
                }
                if (group == kGroupNames.end()) {
                    fail(object,
                         entry.pos,
                         std::format("unknown population group '{}' in {}", entry.name, key));
                } else {
                    assign(static_cast<std::size_t>(group - kGroupNames.begin()), entry.value);
                }
            }
        }
    }

    HoldingDef read_holding(const ContentObject& object) {
        const TypedBlock& body = object.object.body;
        HoldingDef holding;
        holding.province = reference(body, "province");
        if (registry_.find("province", holding.province) == nullptr) {
            fail(object,
                 position(object, "province"),
                 std::format("unknown holding province '{}'", holding.province));
        }
        const auto kind = province_kind_from_key(reference(body, "kind"));
        if (!kind) {
            fail(object, position(object, "kind"), "unknown holding province kind");
        } else {
            holding.kind = *kind;
        }
        const auto* owner_kind = body.find("owner_kind")->get_if<data::EnumValue>();
        holding.owner_kind = static_cast<OwnerKind>(owner_kind->index);
        holding.owner = reference(body, "owner");
        if (holding.owner_kind == OwnerKind::Dynasty) {
            if (holding.owner.empty() || registry_.find("dynasty", holding.owner) == nullptr) {
                fail(object, position(object, "owner"), "dynasty holding needs an existing owner dynasty");
            }
        } else if (!holding.owner.empty()) {
            fail(object, position(object, "owner"), "owner is allowed only for owner_kind = dynasty");
        }
        const auto node = node_slots_.find(reference(body, "trade_node"));
        if (node == node_slots_.end()) {
            fail(object, position(object, "trade_node"), "unknown holding trade node");
        } else {
            holding.trade_node = node->second;
        }
        holding.development = static_cast<std::uint8_t>(integer(body, "development"));
        holding.slots_total = static_cast<std::uint8_t>(integer(body, "slots_total"));
        holding.wealth.fill(kDefaultWealth);
        read_groups(object, "population", [&](std::size_t group, const TypedValue& value) {
            holding.population[group] = static_cast<std::uint32_t>(*value.get_if<std::int64_t>());
        });
        std::uint64_t total = 0;
        for (const auto population : holding.population) {
            total += population;
        }
        if (total > kMaxProvincePersons) {
            fail(object, position(object, "population"), "holding population total exceeds 32000000 persons");
        }
        read_groups(object, "mood", [&](std::size_t group, const TypedValue& value) {
            holding.mood[group] = *value.get_if<Fixed32>();
        });
        read_groups(object, "wealth", [&](std::size_t group, const TypedValue& value) {
            holding.wealth[group] = *value.get_if<Fixed32>();
        });
        holding.control = fixed(body, "control", kDefaultControl);
        holding.corruption = fixed(body, "corruption");
        holding.unrest = fixed(body, "unrest");
        holding.grain_stock = fixed(body, "grain_stock");
        if (const TypedMap* capacities = map(body, "capacity")) {
            std::set<std::string, std::less<>> seen;
            for (const auto& entry : capacities->entries) {
                const ContentObject* good = registry_.find("good", entry.name);
                if (!seen.insert(entry.name).second) {
                    fail(object, entry.pos, std::format("duplicate capacity good '{}'", entry.name));
                }
                const auto slot = good != nullptr ? integer(good->object.body, "slot") : -1;
                if (slot < 0 || slot >= static_cast<std::int64_t>(kMaxGoods)) {
                    fail(object, entry.pos, std::format("unknown capacity good '{}'", entry.name));
                } else {
                    holding.capacity[static_cast<std::size_t>(slot)] = *entry.value.get_if<Fixed32>();
                }
            }
        }
        read_buildings(object, holding);
        return holding;
    }

    void read_buildings(const ContentObject& object, HoldingDef& holding) {
        const auto* value = object.object.body.find("buildings");
        const auto* buildings = value != nullptr ? value->get_if<data::TypedList>() : nullptr;
        if (buildings == nullptr) {
            return;
        }
        if (buildings->size() > holding.slots_total) {
            fail(object, position(object, "buildings"), "holding buildings exceed its slots_total");
        }
        std::array<std::uint32_t, 256> counts{};
        for (const TypedValue& building : *buildings) {
            const auto& id = building.get_if<data::RefValue>()->id;
            const ContentObject* definition = registry_.find("building", id);
            const auto slot = definition != nullptr ? integer(definition->object.body, "slot") : 0;
            const auto found =
                std::find_if(rules_.buildings.defs.begin(),
                             rules_.buildings.defs.end(),
                             [slot](const BuildingDef& def) { return slot > 0 && def.slot == slot; });
            if (slot < 1 || slot > 255 || found == rules_.buildings.defs.end()) {
                fail(object, building.pos, std::format("unknown holding building '{}'", id));
                continue;
            }
            const auto index = static_cast<std::uint8_t>(slot);
            holding.buildings.push_back(index);
            const bool allowed_kind =
                found->allowed_kinds == 0 || (found->allowed_kinds & (1U << index_of(holding.kind))) != 0;
            if (!allowed_kind || holding.development < found->min_development) {
                fail(object,
                     building.pos,
                     std::format("building '{}' is not allowed at this holding kind/development", id));
            }
            if (++counts[index] > static_cast<std::uint32_t>(found->max_per_province)) {
                fail(object, building.pos, std::format("holding has too many buildings of type '{}'", id));
            }
        }
    }
};

}  // namespace

M1EconomyContentResult build_m1_economy_content(const data::ContentRegistry& registry,
                                                const sim::economy::EconomyRules& rules) {
    return Builder(registry, rules).build();
}

}  // namespace pat::content
