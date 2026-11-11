// The UI theme (issue #256): data/ui/theme.pd -> Theme -> generated RCSS and `$token` expansion. The
// stylesheet of every window must be free of hex colours and refer only to tokens the theme knows.

#include "pat/data/pd_parser.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/theme.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ui_theme_fixture.hpp"

using namespace pat::ui;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path theme_file() {
    return std::filesystem::path(PATRIARCH_DATA_DIR) / "ui" / "theme.pd";
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const char kMiniTheme[] = R"(format_version = 1
theme "mini" {
  color {
    ink = "#1B2523"
    panel = "#25312D"
    raised = "#34423B"
    paper = "#EEE5D2"
    paper2 = "#E1D6BF"
    text = "#242E29"
    muted = "#626658"
    light = "#F3EAD6"
    quiet = "#BAC1B1"
    gold = "#D2B77D"
    rule = "#6E7158"
    line = "#C3B695"
    green = "#365847"
    green_hover = "#436B56"
    success = "#B5C9A0"
    red = "#8D4235"
    softred = "#E6B59F"
  }
  spacing { space_8 = 8 }
  font_size { type_14 = 14 }
  font {
    serif = { family = "Serif Face" }
    sans = { family = "Sans Face" }
    line_height = 1.35
  }
  shape {
    radius = 2
    stroke_hairline = 1
    stroke_icon = 1.5
    shadow = none
  }
})";

}  // namespace

TEST_CASE("theme: theme.pd loads with the 17 colours, scales, families and shape") {
    auto loaded = load_theme(theme_file());
    REQUIRE(loaded.has_value());
    const Theme& theme = loaded.value();
    CHECK(theme.id == "patriarch-concept-01");
    CHECK(theme.reference_width == 1920);
    CHECK(theme.reference_height == 1080);
    CHECK(theme.colors.size() == 17);
    CHECK(color_hex(*theme.find_color("gold")) == "#D2B77D");
    CHECK(theme.find_number("space_48")->milli == 48000);
    CHECK(theme.find_number("type_40")->milli == 40000);
    CHECK(theme.find_number("type_13") != nullptr);  // the extra scale
    CHECK(theme.find_number("stroke_icon")->milli == 1500);
    CHECK(theme.find_number("radius")->milli == 2000);
    CHECK(theme.find_font("serif")->family == "PT Serif");
    CHECK(theme.find_font("sans")->family == "PT Sans Caption");
    CHECK(theme.line_height_milli == 1350);
}

TEST_CASE("theme: a file that lacks a token or has a bad colour is rejected with a message") {
    CHECK_FALSE(parse_theme("format_version = 1\n").has_value());
    CHECK_FALSE(parse_theme("this is not pd").has_value());
    std::string bad = kMiniTheme;
    bad.replace(bad.find("#1B2523"), 7, "#1B25ZZ");
    auto result = parse_theme(bad);
    REQUIRE_FALSE(result.has_value());
    CHECK(contains(result.error().message, "ink"));
    std::string missing = kMiniTheme;
    missing.replace(missing.find("    softred = \"#E6B59F\"\n"), 23, "");
    result = parse_theme(missing);
    REQUIRE_FALSE(result.has_value());
    CHECK(contains(result.error().message, "softred"));
    CHECK(parse_theme(kMiniTheme).has_value());
}

TEST_CASE("theme: tokens expand to RCSS values") {
    auto theme = parse_theme(kMiniTheme);
    REQUIRE(theme.has_value());
    const auto expand = [&](const char* text) { return expand_tokens(theme.value(), text).text; };
    CHECK(expand("color: $gold;") == "color: #D2B77D;");
    CHECK(expand("padding: $space_8 $space_8;") == "padding: 8dp 8dp;");
    CHECK(expand("font-size: $type_14;") == "font-size: 14dp;");
    CHECK(expand("border: $stroke_hairline $rule;") == "border: 1dp #6E7158;");
    CHECK(expand("width: $stroke_icon;") == "width: 1.5dp;");
    CHECK(expand("font-family: $serif;") == "font-family: \"Serif Face\";");
    CHECK(expand("line-height: $line_height;") == "line-height: 1.35;");
    CHECK(expand("background-color: $panel_a95;") == "background-color: #25312DF2;");
    CHECK(expand("background-color: $light_a0;") == "background-color: #F3EAD600;");
    CHECK(expand("no tokens here, 1dp and #id") == "no tokens here, 1dp and #id");

    const ExpandResult unknown = expand_tokens(theme.value(), "color: $nope; border: $gold_a101 $space_9;");
    CHECK(unknown.text == "color: $nope; border: $gold_a101 $space_9;");
    REQUIRE(unknown.unknown.size() == 3);
    CHECK(unknown.unknown[0] == "nope");
}

TEST_CASE("theme: the generated RCSS matches every colour token of theme.pd") {
    // The expected values are read from the raw file with the .pd parser, not from the Theme under test.
    const auto parsed = pat::data::parse_pd(read_text(theme_file()));
    REQUIRE(parsed.ok());
    const pat::data::Entry* block = nullptr;
    for (const auto& e : parsed.file.entries) {
        if (e.key == "theme") {
            block = &e;
        }
    }
    REQUIRE(block != nullptr);
    const pat::data::Entry* colors = nullptr;
    const pat::data::Entry* sizes = nullptr;
    for (const auto& e : block->value.entries) {
        if (e.key == "color") {
            colors = &e;
        }
        if (e.key == "font_size") {
            sizes = &e;
        }
    }
    REQUIRE(colors != nullptr);
    REQUIRE(sizes != nullptr);

    auto loaded = load_theme(theme_file());
    REQUIRE(loaded.has_value());
    const std::string rcss = generate_theme_rcss(loaded.value());
    std::size_t checked = 0;
    for (const auto& e : colors->value.entries) {
        std::string upper = e.value.text;
        std::transform(
            upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        std::string name = e.key;
        std::replace(name.begin(), name.end(), '_', '-');
        INFO(e.key);
        CHECK(contains(rcss, ".fg-" + name + " { color: " + upper + "; }"));
        CHECK(contains(rcss, ".bg-" + name + " { background-color: " + upper + "; }"));
        CHECK(contains(rcss, ".bd-" + name + " { border-color: " + upper + "; }"));
        ++checked;
    }
    CHECK(checked == 17);
    for (const auto& e : sizes->value.entries) {
        CHECK(contains(
            rcss, ".type-" + e.key.substr(5) + " { font-size: " + std::to_string(e.value.integer) + "dp; }"));
    }
    // The base look uses the tokens of the dark frame.
    CHECK(contains(rcss, "font-family: \"PT Sans Caption\""));
    CHECK(contains(rcss, "background-color: #25312D;"));  // .panel
    CHECK(contains(rcss, "border: 1dp #6E7158;"));
    CHECK_FALSE(contains(rcss, "$"));
}

TEST_CASE("theme: no window stylesheet or document keeps a hex colour or an unknown token") {
    auto loaded = load_theme(theme_file());
    REQUIRE(loaded.has_value());
    const std::regex hex("#[0-9a-fA-F]{3,8}\\b");
    std::size_t files = 0;
    for (const auto& item : std::filesystem::directory_iterator(PATRIARCH_SPIKE_UI_DIR)) {
        const std::string ext = item.path().extension().string();
        if (ext != ".rcss" && ext != ".rml") {
            continue;
        }
        ++files;
        const std::string text = read_text(item.path());
        INFO(item.path().filename().string());
        // A `#affairs-title` selector is not a colour: a colour is not followed by a name character.
        std::sregex_iterator it(text.begin(), text.end(), hex);
        for (; it != std::sregex_iterator(); ++it) {
            const std::size_t end =
                static_cast<std::size_t>(it->position()) + static_cast<std::size_t>(it->length());
            const bool selector =
                end < text.size() && (text[end] == '-' || text[end] == '_' ||
                                      std::isalnum(static_cast<unsigned char>(text[end])) != 0);
            CHECK_MESSAGE(selector, "hex colour " << it->str() << " outside the theme");
        }
        if (ext == ".rcss") {
            CHECK(expand_tokens(loaded.value(), text).unknown.empty());
        }
    }
    CHECK(files >= 10);
    // The C++ of the windows has no colour literals either.
    for (const char* dir : {"src/ui", "src/game"}) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(
                 std::filesystem::path(PATRIARCH_SOURCE_DIR) / dir)) {
            const std::string name = item.path().filename().string();
            if (!item.is_regular_file() || name == "theme.cpp" || name == "theme.hpp") {
                continue;
            }
            const std::string text = read_text(item.path());
            INFO(item.path().string());
            CHECK_FALSE(std::regex_search(text, std::regex("\"#[0-9a-fA-F]{6,8}\"")));
        }
    }
}

TEST_CASE("theme: icons are repainted by colour token") {
    const std::string svg =
        read_text(std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons" / "chevron.svg");
    REQUIRE(contains(svg, "stroke=\"#365847\""));
    auto loaded = load_theme(theme_file());
    REQUIRE(loaded.has_value());
    const std::string gold = recolor_svg(svg, *loaded.value().find_color("gold"));
    CHECK(contains(gold, "stroke=\"#D2B77D\""));
    CHECK_FALSE(contains(gold, "#365847"));
    CHECK(contains(gold, "fill=\"none\""));  // `none` is not a colour and stays
    CHECK(contains(gold, "stroke-width=\"1.5\""));
}

#ifdef _WIN32

#include <RmlUi/Core.h>

#include "test_fonts.hpp"

TEST_CASE("theme: an SVG icon requested as name@colour renders in the colour of the token") {
    using namespace pat::engine;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "patriarch-theme-icon";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons" /
                                   "chevron.svg",
                               dir / "chevron.svg");
    {
        std::ofstream out(dir / "icon.rml", std::ios::binary);
        out << "<rml><head><link type=\"text/rcss\" href=\"theme.rcss\"/>"
               "<style>svg { position: absolute; left: 20dp; top: 20dp; width: 96dp; height: 96dp; "
               "}</style></head>"
               "<body class=\"bg-ink\"><svg src=\"chevron@gold.svg\"/></body></rml>";
    }
    RendererDesc rd;
    rd.size = {160, 160};
    rd.software = true;
    auto renderer = create_renderer(rd);
    REQUIRE(renderer.has_value());
    UiDesc ud;
    ud.size = {160, 160};
    ud.font_files = pat::test::game_font_files();
    auto ui = UiContext::create(*renderer.value(), nullptr, ud);
    REQUIRE(ui.has_value());
    pat::test::install_game_theme(*ui.value());
    REQUIRE(ui.value()->load_document(dir / "icon.rml") != nullptr);

    REQUIRE(renderer.value()->begin_frame({0, 0, 0, 1}));
    ui.value()->update();
    ui.value()->render();
    const auto px = renderer.value()->capture();
    renderer.value()->end_frame();
    REQUIRE(px.size() == 160u * 160u * 4u);
    const auto near = [&](std::size_t i, int r, int g, int b) {
        return std::abs(px[i] - r) < 40 && std::abs(px[i + 1] - g) < 40 && std::abs(px[i + 2] - b) < 40;
    };
    std::size_t gold = 0;
    std::size_t original = 0;
    std::size_t ink = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        gold += near(i, 0xD2, 0xB7, 0x7D) ? 1 : 0;
        original += near(i, 0x36, 0x58, 0x47) ? 1 : 0;
        ink += near(i, 0x1B, 0x25, 0x23) ? 1 : 0;
    }
    CHECK(gold > 100);     // the chevron stroke, repainted with the gold token
    CHECK(original == 0);  // the colour baked into the file is gone
    CHECK(ink > 10000);    // the background of the `bg-ink` class
    ui.value().reset();
}

#endif
