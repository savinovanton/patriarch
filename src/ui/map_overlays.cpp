#include "pat/ui/map_overlays.hpp"

#include "pat/ui/text.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "overlays";
// The area of the minimap picture on board 01 (222 x 104 dp) inside the panel.
constexpr float kAreaW = 222.0f;
constexpr float kAreaH = 104.0f;

using loc::Arg;

struct OverlayTexts {
    Rml::String season;
    Rml::String region_title;
    Rml::String region_sub;
    Rml::String plate;
    Rml::String minimap_title;
    Rml::String scale;
    Rml::String zoom_in;
    Rml::String zoom_out;
};

std::string dp(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fdp", static_cast<double>(value));
    return buf;
}

std::string px(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fpx", static_cast<double>(value));
    return buf;
}

}  // namespace

struct MapOverlays::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string doc_dir;  // directory of the document, to make file paths relative to it
    std::string icons_rel;

    OverlayTexts t;
    Rml::String castle_icon;
    Rml::String marker_icon;
    bool has_region = false;
    bool has_plate = false;
    bool has_scale = false;
    Season season = Season::Winter;
    int year = 1703;
    std::string region;
    std::string selection;
    int scale_km = 0;
    float aspect = 2.0f;
    MinimapView view;
    std::function<void(int)> on_zoom;
    std::function<void(float, float)> on_jump;

    [[nodiscard]] Rml::ElementDocument* find_document() const {
        Rml::Context* ctx = ui->context();
        for (int i = 0; i < ctx->GetNumDocuments(); ++i) {
            Rml::ElementDocument* doc = ctx->GetDocument(i);
            if (doc != nullptr && doc->GetSourceURL() == source_url) {
                return doc;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Rml::Element* el(const char* id) const {
        Rml::ElementDocument* doc = find_document();
        return doc != nullptr ? doc->GetElementById(id) : nullptr;
    }

    [[nodiscard]] std::string text(std::string_view key, std::initializer_list<Arg> args = {}) const {
        return loc->format(key, args);
    }

    void refresh() {
        t.season =
            text("ui.overlay.season",
                 {Arg("season",
                      text(std::string("ui.overlay.season.") + std::string(MapOverlays::season_key(season)))),
                  Arg("year", std::to_string(year))});
        t.region_title = upper_text(region);
        t.region_sub = text("ui.overlay.owned");
        t.plate = selection;
        t.minimap_title = text("ui.overlay.minimap");
        t.scale = scale_km > 0 ? text("ui.overlay.scale", {Arg("km", scale_km)}) : std::string();
        t.zoom_in = text("ui.overlay.zoom_in");
        t.zoom_out = text("ui.overlay.zoom_out");
        has_region = !region.empty();
        has_plate = !selection.empty();
        has_scale = scale_km > 0;
        castle_icon = icons_rel + "/castle@gold.svg";
        marker_icon = icons_rel + "/diamond@green.svg";
        handle.DirtyVariable("t");
        handle.DirtyVariable("has_region");
        handle.DirtyVariable("has_plate");
        handle.DirtyVariable("has_scale");
        handle.DirtyVariable("castle_icon");
        handle.DirtyVariable("marker_icon");
    }

    [[nodiscard]] std::string relative_to_document(const std::filesystem::path& file) const {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(std::filesystem::absolute(file), std::filesystem::path(doc_dir), ec);
        return (ec || rel.empty()) ? file.generic_string() : rel.generic_string();
    }

    /// The rectangle (dp, inside the picture area) the map occupies there: the map is fitted into the area.
    void fit(float& x, float& y, float& w, float& h) const {
        const float area_aspect = kAreaW / kAreaH;
        if (aspect >= area_aspect) {
            w = kAreaW;
            h = kAreaW / aspect;
        } else {
            h = kAreaH;
            w = kAreaH * aspect;
        }
        x = (kAreaW - w) * 0.5f;
        y = (kAreaH - h) * 0.5f;
    }

    void place_view() {
        Rml::Element* rect = el("mm-view");
        Rml::Element* area = el("mm-area");
        if (rect == nullptr || area == nullptr) {
            return;
        }
        float fx = 0.0f;
        float fy = 0.0f;
        float fw = 0.0f;
        float fh = 0.0f;
        fit(fx, fy, fw, fh);
        // The rectangle of the view, clipped to the map; a view larger than the map covers all of it.
        const float x0 = std::clamp(view.center_x - view.width * 0.5f, 0.0f, 1.0f);
        const float x1 = std::clamp(view.center_x + view.width * 0.5f, 0.0f, 1.0f);
        const float y0 = std::clamp(view.center_y - view.height * 0.5f, 0.0f, 1.0f);
        const float y1 = std::clamp(view.center_y + view.height * 0.5f, 0.0f, 1.0f);
        rect->SetProperty("left", dp(fx + x0 * fw));
        rect->SetProperty("top", dp(fy + y0 * fh));
        rect->SetProperty("width", dp(std::max(2.0f, (x1 - x0) * fw)));
        rect->SetProperty("height", dp(std::max(2.0f, (y1 - y0) * fh)));
    }

    void place_anchor(const char* id, float x, float y, float dx_dp, float dy_dp) {
        Rml::Element* e = el(id);
        if (e == nullptr) {
            return;
        }
        const float ratio = ui->context()->GetDensityIndependentPixelRatio();
        e->SetProperty("left", px(x + dx_dp * ratio));
        e->SetProperty("top", px(y + dy_dp * ratio));
    }
};

MapOverlays::MapOverlays(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MapOverlays::~MapOverlays() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<MapOverlays>> MapOverlays::create(engine::UiContext& ui,
                                                               const std::filesystem::path& document,
                                                               const std::filesystem::path& icons,
                                                               const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<MapOverlays>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;
    impl->doc_dir = std::filesystem::absolute(document).parent_path().string();

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    impl->icons_rel = ec ? icons.generic_string() : rel.generic_string();

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{100, "cannot create the map overlays data model"});
    }
    if (auto texts = ctor.RegisterStruct<OverlayTexts>()) {
        texts.RegisterMember("season", &OverlayTexts::season);
        texts.RegisterMember("region_title", &OverlayTexts::region_title);
        texts.RegisterMember("region_sub", &OverlayTexts::region_sub);
        texts.RegisterMember("plate", &OverlayTexts::plate);
        texts.RegisterMember("minimap_title", &OverlayTexts::minimap_title);
        texts.RegisterMember("scale", &OverlayTexts::scale);
        texts.RegisterMember("zoom_in", &OverlayTexts::zoom_in);
        texts.RegisterMember("zoom_out", &OverlayTexts::zoom_out);
    }
    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("castle_icon", &m->castle_icon);
    ctor.Bind("marker_icon", &m->marker_icon);
    ctor.Bind("has_region", &m->has_region);
    ctor.Bind("has_plate", &m->has_plate);
    ctor.Bind("has_scale", &m->has_scale);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("zoom", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_zoom) {
            m->on_zoom(args[0].Get<int>());
        }
    });
    ctor.BindEventCallback("jump", [m](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* area = m->el("mm-area");
        if (area == nullptr || !m->on_jump) {
            return;
        }
        float fx = 0.0f;
        float fy = 0.0f;
        float fw = 0.0f;
        float fh = 0.0f;
        m->fit(fx, fy, fw, fh);
        const float ratio = m->ui->context()->GetDensityIndependentPixelRatio();
        const float mx = ev.GetParameter<float>("mouse_x", 0.0f);
        const float my = ev.GetParameter<float>("mouse_y", 0.0f);
        const float local_x = (mx - area->GetAbsoluteLeft()) / ratio - fx;
        const float local_y = (my - area->GetAbsoluteTop()) / ratio - fy;
        m->on_jump(std::clamp(local_x / fw, 0.0f, 1.0f), std::clamp(local_y / fh, 0.0f, 1.0f));
    });
    m->refresh();

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{101, "cannot load the map overlays " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    m->place_view();
    return R::ok(std::unique_ptr<MapOverlays>(new MapOverlays(std::move(impl))));
}

std::string_view MapOverlays::season_key(Season season) {
    switch (season) {
        case Season::Winter:
            return "winter";
        case Season::Spring:
            return "spring";
        case Season::Summer:
            return "summer";
        case Season::Autumn:
            return "autumn";
    }
    return "winter";
}

void MapOverlays::set_season(Season season, int year) {
    if (impl_->season == season && impl_->year == year) {
        return;
    }
    impl_->season = season;
    impl_->year = year;
    impl_->refresh();
}

void MapOverlays::set_region(std::string_view name) {
    if (impl_->region == name) {
        return;
    }
    impl_->region = std::string(name);
    impl_->refresh();
}

void MapOverlays::set_region_anchor(float x, float y) {
    // The caption block is 760 x 70 dp, centred on the anchor.
    impl_->place_anchor("region", x, y, -380.0f, -35.0f);
}

void MapOverlays::set_selection(std::string_view name) {
    if (impl_->selection == name) {
        return;
    }
    impl_->selection = std::string(name);
    impl_->refresh();
}

void MapOverlays::set_selection_anchor(float x, float y) {
    // The marker (24 dp) is centred on the point, the plate (207 dp wide) hangs 14 dp below it.
    impl_->place_anchor("plate", x, y, -103.5f, 14.0f);
    impl_->place_anchor("marker", x, y, -12.0f, -12.0f);
}

void MapOverlays::set_overview(const MinimapOverview& overview) {
    impl_->aspect = overview.aspect > 0.1f ? overview.aspect : 2.0f;
    if (Rml::Element* land = impl_->el("mm-land")) {
        land->SetAttribute(
            "src", overview.land.empty() ? std::string() : impl_->relative_to_document(overview.land));
    }
    if (Rml::Element* owned = impl_->el("mm-owned")) {
        owned->SetAttribute(
            "src", overview.owned.empty() ? std::string() : impl_->relative_to_document(overview.owned));
    }
    // The pictures keep the proportions of the map inside the fixed area.
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    impl_->fit(x, y, w, h);
    for (const char* id : {"mm-land", "mm-owned"}) {
        if (Rml::Element* e = impl_->el(id)) {
            e->SetProperty("left", dp(x));
            e->SetProperty("top", dp(y));
            e->SetProperty("width", dp(w));
            e->SetProperty("height", dp(h));
        }
    }
    impl_->place_view();
}

void MapOverlays::set_view(const MinimapView& view) {
    if (view.center_x == impl_->view.center_x && view.center_y == impl_->view.center_y &&
        view.width == impl_->view.width && view.height == impl_->view.height) {
        return;
    }
    impl_->view = view;
    impl_->place_view();
}

void MapOverlays::set_scale_km(int km) {
    if (impl_->scale_km == km) {
        return;
    }
    impl_->scale_km = km;
    impl_->refresh();
}

void MapOverlays::set_zoom_handler(std::function<void(int)> handler) {
    impl_->on_zoom = std::move(handler);
}

void MapOverlays::set_jump_handler(std::function<void(float, float)> handler) {
    impl_->on_jump = std::move(handler);
}

void MapOverlays::press_zoom(int direction) {
    if (impl_->on_zoom) {
        impl_->on_zoom(direction);
    }
}

void MapOverlays::relocalize() {
    impl_->refresh();
}

Rml::ElementDocument* MapOverlays::document() const {
    return impl_->find_document();
}

void MapOverlays::show(bool visible) {
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        if (visible) {
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        } else {
            doc->Hide();
        }
    }
}

}  // namespace pat::ui
