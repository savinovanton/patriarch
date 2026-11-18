#pragma once

// Shared helpers of the tests that compile event files (E11-T06 .. T11): the data of the repository plus
// one small file are loaded, the schemas checked and the events compiled against a catalog.

#include "pat/content/event_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/event_set.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "court_fixture.hpp"

namespace pat::event_fixture {

struct Compiled {
    std::string text;  // the whole file, header included (positions refer to it)
    std::vector<pat::data::LoadError> errors;
    std::optional<pat::sim::events::EventSet> set;
    std::uint64_t content_hash = 0;

    [[nodiscard]] bool ok() const { return errors.empty() && set.has_value(); }
    [[nodiscard]] std::vector<pat::data::ErrorCode> codes() const {
        std::vector<pat::data::ErrorCode> out;
        for (const auto& e : errors) {
            out.push_back(e.diagnostic.code);
        }
        return out;
    }
    [[nodiscard]] std::string all_errors() const { return pat::content::format_errors(errors); }
};

inline Compiled compile_event_files(const std::vector<std::pair<std::string, std::string>>& files,
                                    const pat::content::EventBuildOptions& options,
                                    const pat::sim::events::Catalog& cat) {
    auto source = pat::court_fixture::shipped_source(false);
    Compiled out;
    for (const auto& [path, body] : files) {
        out.text = "format_version = 1\n" + body;
        source.add(path, out.text);
    }
    auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    if (!loaded.ok()) {
        out.errors = std::move(loaded.errors);
        return out;
    }
    out.content_hash = pat::content::event_content_hash(loaded.registry);
    auto set = pat::content::build_event_set(loaded.registry, cat, options);
    if (!set) {
        out.errors = std::move(set).error();
    } else {
        out.set = std::move(set).value();
    }
    return out;
}

/// An event of the character scope; `fields` goes before the options, `options` replaces the default one.
inline std::string character_event(std::string_view fields = {},
                                   std::string_view options = "option \"ok\" { }",
                                   std::string_view id = "t.ev") {
    return std::format(R"(event "{}" {{
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    {}
    {}
}}
)",
                       id,
                       fields,
                       options);
}

}  // namespace pat::event_fixture
