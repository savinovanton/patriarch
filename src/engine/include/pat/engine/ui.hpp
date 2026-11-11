#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/events.hpp"
#include "pat/engine/renderer.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
}  // namespace Rml

namespace pat::engine {

class Window;

struct UiDesc {
    /// Font files (TTF/OTF) loaded at startup; the first one also becomes the fallback face for glyphs a
    /// font lacks. Faces are matched by the family/style stored in the files (regular, italic, bold ...).
    std::vector<std::filesystem::path> font_files;
    /// Initial size of the UI surface in pixels and the UI scale (display scale x user scale).
    PixelSize size{1280, 720};
    float ui_scale = 1.0f;
};

/// Game UI on top of RmlUi (ADR-0001): owns the RmlUi system/file/render interfaces, one context, input
/// translation, DPI scaling and development hot reload of documents. RmlUi has global state, so only one
/// UiContext can exist at a time. Main thread only; must be destroyed before the renderer.
class UiContext {
public:
    /// `window` is optional (null in headless tests): when given it provides the clipboard, mouse cursors
    /// and text-input activation for the platform.
    [[nodiscard]] static core::Result<std::unique_ptr<UiContext>>
    create(IRenderer& renderer, Window* window, const UiDesc& desc);
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    /// Feeds a window/input event to the UI. Returns true if the UI consumed it, i.e. the game must not
    /// react to it (mouse over a UI element, keyboard focus in a text field, ...).
    bool handle_event(const Event& event);

    /// Updates the UI surface size (pixels) and scale (1.0 = 100%).
    void resize(PixelSize pixels, float ui_scale);

    /// Content for files RmlUi asks for (stylesheets, images, ...): given the path RmlUi wants, returns the
    /// text to serve instead of the file, or nullopt to read the file from disk as usual. The UI theme uses
    /// it to generate `theme.rcss`, expand its tokens in stylesheets and repaint icons
    /// (pat::ui::install_theme).
    using FileProvider = std::function<std::optional<std::string>(const std::filesystem::path&)>;
    void set_file_provider(FileProvider provider);
    /// Reads a text file the way RmlUi would (through the file provider, then from disk).
    [[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path);

    /// Loads and shows a document; hot reload watches the document's directory. Returns null on failure.
    [[nodiscard]] Rml::ElementDocument* load_document(const std::filesystem::path& path);

    /// Reloads documents whose .rml/.rcss files changed on disk. Returns true if anything was reloaded.
    bool reload_if_changed();

    /// Runs layout/animation. Call once per frame before render().
    void update();
    /// Draws the UI into the renderer's current target (between begin_frame and end_frame).
    void render();

    /// True while the cursor is over UI elements (the game should not pan the map etc.).
    [[nodiscard]] bool mouse_over_ui() const;
    [[nodiscard]] Rml::Context* context() const;
    [[nodiscard]] float ui_scale() const;

private:
    struct Impl;
    explicit UiContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::engine
