#include "pat/ui/tooltip_manager.hpp"

#include <RmlUi/Core.h>

#include <cstdio>
#include <iterator>
#include <variant>
#include <vector>

namespace pat::ui {

namespace {

constexpr int kScancodeEscape = 41;
constexpr int kScancodeTab = 43;
constexpr int kScancodeRight = 79;
constexpr int kScancodeLeft = 80;
constexpr int kScancodeDown = 81;
constexpr int kScancodeUp = 82;
constexpr int kScancodeLeftAlt = 226;
constexpr int kScancodeRightAlt = 230;

std::string px_string(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fpx", static_cast<double>(value));
    return buf;
}

RectF element_rect(Rml::Element* el) {
    return {el->GetAbsoluteLeft(), el->GetAbsoluteTop(), el->GetOffsetWidth(), el->GetOffsetHeight()};
}

}  // namespace

struct TooltipManager::Impl {
    engine::UiContext* ui = nullptr;
    TooltipState state;
    Provider provider;
    std::filesystem::path
        stylesheet;     // tooltip.rcss, read whenever a tooltip is created (edits show up live)
    std::string icons;  // directory of the SVG icons (the close button), forward slashes
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    bool keyboard = false;  // the last input was Tab / arrows: the focused element gets the tooltip
    std::uint64_t synced_revision = 0;

    struct Open {
        std::string path;
        Rml::ElementDocument* document = nullptr;
        RectF rect;
    };
    std::vector<Open> open;

    explicit Impl(TooltipTiming timing) : state(timing) {}

    [[nodiscard]] const Explanation* resolve(std::string_view path) const {
        if (!provider) {
            return nullptr;
        }
        const SplitPath split = split_tooltip_path(path);
        const Explanation* root = provider(split.key);
        return root != nullptr ? find_term(*root, split.terms) : nullptr;
    }

    [[nodiscard]] HoverInfo find_hover() const {
        HoverInfo hover;
        // Keyboard focus (Tab, arrows) shows the tooltip of the focused element, like hovering it does.
        Rml::Element* start = keyboard ? ui->context()->GetFocusElement() : ui->context()->GetHoverElement();
        for (Rml::Element* el = start; el != nullptr; el = el->GetParentNode()) {
            if (hover.path.empty() && el->HasAttribute("data-tip")) {
                const std::string path = el->GetAttribute<Rml::String>("data-tip", "");
                if (!path.empty() && resolve(path) != nullptr) {
                    hover.path = path;
                    hover.anchor = element_rect(el);
                }
            }
            if (el->HasAttribute("data-tip-level")) {
                hover.level = el->GetAttribute<int>("data-tip-level", -1);
                break;
            }
        }
        return hover;
    }

    [[nodiscard]] std::string read_style() const {
        // Through the UI context, so the theme tokens of the stylesheet are expanded like in linked
        // stylesheets.
        return ui->read_text_file(stylesheet).value_or(std::string());
    }

    /// The close button is visible while the tooltips are pinned.
    void sync_pinned() const {
        for (const Open& o : open) {
            if (o.document != nullptr) {
                if (Rml::Element* box = o.document->GetElementById("tip")) {
                    box->SetClass("pinned", state.pinned());
                }
            }
        }
    }

    [[nodiscard]] bool hover_is(const char* attribute) const {
        for (Rml::Element* el = ui->context()->GetHoverElement(); el != nullptr; el = el->GetParentNode()) {
            if (el->HasAttribute(attribute)) {
                return true;
            }
        }
        return false;
    }

    void close_documents_from(std::size_t first) {
        for (std::size_t i = first; i < open.size(); ++i) {
            if (open[i].document != nullptr) {
                open[i].document->Close();
            }
        }
        open.resize(std::min(first, open.size()));
    }

    void sync() {
        const auto& levels = state.levels();
        std::size_t keep = 0;
        while (keep < open.size() && keep < levels.size() && open[keep].path == levels[keep].path) {
            ++keep;
        }
        close_documents_from(keep);

        const Rml::Vector2i dims = ui->context()->GetDimensions();
        for (std::size_t i = keep; i < levels.size(); ++i) {
            const Explanation* node = resolve(levels[i].path);
            if (node == nullptr) {
                break;
            }
            Rml::ElementDocument* doc = ui->context()->LoadDocumentFromMemory(
                render_tooltip_rml(*node,
                                   levels[i].path,
                                   static_cast<int>(i),
                                   TooltipRender{read_style(), icons, state.pinned()}),
                "[tooltip]");
            if (doc == nullptr) {
                break;
            }
            Rml::Element* box = doc->GetElementById("tip");
            if (box == nullptr) {
                doc->Close();
                break;
            }
            PlaceRequest request;
            request.size = {box->GetOffsetWidth(), box->GetOffsetHeight()};
            request.viewport = {static_cast<float>(dims.x), static_cast<float>(dims.y)};
            request.cursor_x = cursor_x;
            request.cursor_y = cursor_y;
            if (keyboard && i == 0) {
                // No cursor: put the tooltip just below the focused element, flipping at the edges as usual.
                request.cursor_x = levels[i].anchor.x - 14.0f;
                request.cursor_y = levels[i].anchor.bottom() - 14.0f;
            }
            request.anchor = levels[i].anchor;
            request.margin = 8.0f * ui->context()->GetDensityIndependentPixelRatio();
            request.gap = 6.0f * ui->context()->GetDensityIndependentPixelRatio();
            const RectF parent = i > 0 ? open[i - 1].rect : RectF{};
            request.parent = i > 0 ? &parent : nullptr;
            for (std::size_t k = 0; k + 1 < i; ++k) {
                request.avoid.push_back(open[k].rect);  // earlier ancestors stay uncovered too
            }
            const RectF rect = place_tooltip(request);
            box->SetProperty("left", px_string(rect.x));
            box->SetProperty("top", px_string(rect.y));
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            open.push_back({levels[i].path, doc, rect});
        }
        synced_revision = state.revision();
    }
};

TooltipManager::TooltipManager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TooltipManager::~TooltipManager() {
    if (impl_ != nullptr) {
        impl_->close_documents_from(0);
    }
}

core::Result<std::unique_ptr<TooltipManager>> TooltipManager::create(
    engine::UiContext& ui, const std::filesystem::path& stylesheet_dir, TooltipTiming timing) {
    using R = core::Result<std::unique_ptr<TooltipManager>>;
    if (!std::filesystem::exists(stylesheet_dir / "tooltip.rcss")) {
        return R::err(core::Error{95, "tooltip.rcss not found in " + stylesheet_dir.generic_string()});
    }
    auto impl = std::make_unique<Impl>(timing);
    impl->ui = &ui;
    impl->stylesheet = stylesheet_dir / "tooltip.rcss";
    return R::ok(std::unique_ptr<TooltipManager>(new TooltipManager(std::move(impl))));
}

void TooltipManager::set_icons_directory(const std::filesystem::path& icons) {
    impl_->icons = icons.generic_string();
}

void TooltipManager::set_provider(Provider provider) {
    impl_->provider = std::move(provider);
}

void TooltipManager::handle_event(const engine::Event& event) {
    if (const auto* move = std::get_if<engine::event::MouseMove>(&event)) {
        impl_->cursor_x = move->x;
        impl_->cursor_y = move->y;
        impl_->keyboard = false;
    } else if (const auto* button = std::get_if<engine::event::MouseButton>(&event)) {
        impl_->keyboard = false;
        if (button->down && button->button == 1 && impl_->state.levels().size() > 0) {
            if (impl_->state.pinned() && impl_->hover_is("data-tip-close")) {
                close_all();  // the close button of a pinned tooltip
            } else if (impl_->hover_is("data-tip-level")) {
                impl_->state.set_pinned(true);  // a click on the tooltip pins it
                impl_->sync_pinned();
            }
        }
    } else if (const auto* key = std::get_if<engine::event::Key>(&event)) {
        if (key->down) {
            if (key->scancode == kScancodeTab || key->scancode == kScancodeLeft ||
                key->scancode == kScancodeRight || key->scancode == kScancodeUp ||
                key->scancode == kScancodeDown) {
                impl_->keyboard = true;
            }
        }
        if (key->down && !key->repeat) {
            if (key->scancode == kScancodeLeftAlt || key->scancode == kScancodeRightAlt) {
                impl_->state.set_pinned(!impl_->state.pinned());
                impl_->sync_pinned();
            } else if (key->scancode == kScancodeEscape) {
                close_all();
            }
        }
    }
}

void TooltipManager::update(float dt_seconds) {
    impl_->state.set_hover(impl_->find_hover());
    impl_->state.tick(dt_seconds);
    if (impl_->state.revision() != impl_->synced_revision) {
        impl_->sync();
    }
}

void TooltipManager::close_all() {
    impl_->state.close_all();
    impl_->sync();
}

bool TooltipManager::keyboard_mode() const {
    return impl_->keyboard;
}

std::size_t TooltipManager::open_levels() const {
    return impl_->open.size();
}

bool TooltipManager::pinned() const {
    return impl_->state.pinned();
}

const TooltipState& TooltipManager::state() const {
    return impl_->state;
}

Rml::ElementDocument* TooltipManager::level_document(std::size_t level) const {
    return level < impl_->open.size() ? impl_->open[level].document : nullptr;
}

RectF TooltipManager::level_rect(std::size_t level) const {
    return level < impl_->open.size() ? impl_->open[level].rect : RectF{};
}

}  // namespace pat::ui
