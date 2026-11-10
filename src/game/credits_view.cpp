#include "pat/game/credits_view.hpp"

#include "pat/data/credits_schemas.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace pat::game {

namespace {

using content::CreditEntry;
using content::CreditSection;

constexpr std::string_view kMetaSeparator = " \xC2\xB7 ";  // " · "

std::string_view section_name(CreditSection section) {
    return data::kCreditSections[static_cast<std::size_t>(section)];
}

std::string shown(const loc::Localizer& loc, const std::string& key, const std::string& plain) {
    return key.empty() ? plain : loc.format(key);
}

ui::CreditsItem make_item(const loc::Localizer& loc, const CreditEntry& entry) {
    ui::CreditsItem item;
    item.name = shown(loc, entry.name_key, entry.name);
    item.text = shown(loc, entry.text_key, entry.text);
    item.meta = entry.provider;
    if (!entry.license.empty()) {
        if (!item.meta.empty()) {
            item.meta += kMetaSeparator;
        }
        item.meta += loc.format("ui.credits.license", {loc::Arg("license", entry.license)});
    }
    return item;
}

}  // namespace

ui::CreditsSnapshot credits_snapshot(const loc::Localizer& loc, const content::CreditsList& credits) {
    constexpr std::array kSections = {CreditSection::Authors, CreditSection::Data, CreditSection::Assets};
    ui::CreditsSnapshot snapshot;
    for (const CreditSection section : kSections) {
        ui::CreditsSection out;
        for (const CreditEntry& entry : credits.entries) {
            if (entry.section == section) {
                out.items.push_back(make_item(loc, entry));
            }
        }
        if (!out.items.empty()) {
            out.title = loc.format("ui.credits.section." + std::string(section_name(section)));
            snapshot.sections.push_back(std::move(out));
        }
    }
    return snapshot;
}

}  // namespace pat::game
