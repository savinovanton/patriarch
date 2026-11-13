#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/math.hpp"
#include "pat/engine/sdf_font.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/mapdata/map_data.hpp"

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace pat::engine {

/// Straight (non-premultiplied) RGBA colour, components in [0, 1].
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

/// Screen-space rectangle in pixels (origin at the top-left corner of the render target).
struct Quad {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    Color color;
};

struct RendererDesc {
    /// Native window handle (`HWND`). Null creates an offscreen render target of `size` instead
    /// (headless tests, screenshot tools).
    void* native_window = nullptr;
    PixelSize size{1280, 720};
    bool vsync = true;
    /// Ask for the graphics API debug layer; silently skipped if it is not installed.
    bool debug_layer = false;
    /// Force the software rasterizer (WARP). Used by tests on machines without a GPU.
    bool software = false;
    /// Directory with `quad.hlsl`; when set, the shader is loaded from there and can be hot-reloaded.
    /// When empty, the copy embedded into the executable is used.
    std::filesystem::path shader_dir;
};

/// How the province layer colours the map.
enum class MapMode : std::uint8_t {
    Terrain = 0,  ///< No political colouring: just the landscape.
    Owners,       ///< One colour per province (stand-in for owners/realms).
    Mood,         ///< Red-to-green ramp (stand-in for a simulation value such as unrest).
    Random,       ///< Pseudo-random colours (debug).
    Count
};

struct MapViewDesc {
    /// World height of a full-scale height value as a fraction of the map width (vertical exaggeration).
    float relief_fraction = 0.03f;
    LodSettings lod;
};

/// Per-frame inputs of the map renderer.
/// Lighting of the landscape (ADR-0003 phase P2): one directional sun, a sky ambient, soft shadows cast by
/// the relief itself (ray marched through the height texture) and distance haze.
struct MapLighting {
    float sun_azimuth_deg = 315.0f;   ///< Where the sun is, clockwise from north (315 = north-west).
    float sun_elevation_deg = 38.0f;  ///< Height of the sun above the horizon.
    float shadow_strength = 0.85f;    ///< 0 = no shadows, 1 = fully dark shadows.
    int shadow_steps = 24;            ///< Ray march steps per pixel; 0 switches terrain shadows off.
    float haze = 1.0f;                ///< Distance haze scale; 0 = clear air.
};

/// Painterly art direction of the landscape (E5 #269, docs/dev/map-art-direction.md): a season palette,
/// colour grading and a vignette, all done in the terrain pixel shader (no extra pass). The defaults leave
/// the picture exactly as it was before the style existed.
struct MapStyle {
    float winter =
        0.0f;  ///< 0 = summer palette, 1 = winter 1703: snow-beige fields, dark conifers, warm-grey haze.
    float grade = 0.0f;     ///< Colour grading strength: exposure, soft shoulder, split toning, desaturation.
    float vignette = 0.0f;  ///< Darkening of the screen edges (0 = none).
    bool borders =
        false;  ///< Painted borders: smooth thin ink lines, gold contour and fill of the selected province.
};

/// The reference look of the new design (winter terrain concept of board 01): everything on.
[[nodiscard]] inline MapStyle winter_style_1703() noexcept {
    MapStyle style;
    style.winter = 1.0f;
    style.grade = 1.0f;
    style.vignette = 0.35f;
    style.borders = true;
    return style;
}

struct MapFrame {
    Mat4 view_proj = Mat4::identity();
    Vec3 eye;
    float zoom_fraction = 1.0f;  ///< 0 = fully zoomed in, 1 = fully zoomed out; scales the political tint.
    std::uint16_t hovered = 0;   ///< Province under the cursor (0 = none).
    std::uint16_t selected = 0;  ///< Selected province (0 = none).
    float time_s = 0.0f;
    MapLighting light;
    MapStyle style;
};

/// The volumetric landscape with the province layer draped over it (ADR-0003, phase P1).
/// A view must not outlive the renderer that created it; draw it between begin_frame() and end_frame().
class IMapView {
public:
    virtual ~IMapView() = default;

    /// Switches the palette used for the province layer; no geometry is rebuilt.
    virtual void set_mode(MapMode mode) = 0;
    [[nodiscard]] virtual MapMode mode() const = 0;

    /// Replaces the colours of a map mode with values derived from the simulation (ADR-0003: palettes are
    /// updated from snapshots, only when something changed). `colors[id]` is the RGBA8 colour of province
    /// `id` (red in the lowest byte, alpha 255); index 0 is unused. Shorter spans keep the remaining
    /// entries unchanged. The new colours show immediately if `mode` is the current one.
    virtual void set_palette(MapMode mode, std::span<const std::uint32_t> colors) = 0;

    virtual void draw(const MapFrame& frame) = 0;

    [[nodiscard]] virtual const TerrainGeometry& geometry() const = 0;
    /// Number of terrain nodes drawn by the last draw() call.
    [[nodiscard]] virtual std::size_t last_node_count() const = 0;
    /// Rebuilds the terrain shader if its source changed on disk (development hot reload).
    virtual bool reload_shaders_if_changed() = 0;
};

/// One glyph to draw: its geometry (screen pixels + atlas coordinates), fill colour and outline colour.
struct TextQuad {
    TextQuadGeometry geometry;
    Color color{1, 1, 1, 1};
    Color outline{0, 0, 0, 1};
};

/// Shape of the halo (the "outline" colour of a TextQuad) around the glyphs, in fractions of the font size.
/// A negative width keeps the renderer's default: a crisp outline of about 0.08 em.
struct TextHalo {
    float width_em = -1.0f;  ///< solid part: distance from the glyph edge where the halo is fully opaque
    float softness_em =
        0.0f;  ///< extra distance over which the halo fades out (dark ink on a light underlay)
};

/// Draws SDF text quads (E3-T05) with a fill and an outline; created for one atlas.
class ITextRenderer {
public:
    virtual ~ITextRenderer() = default;
    /// Sets the halo shape for the next draw() calls.
    virtual void set_halo(const TextHalo& halo) { (void)halo; }
    /// Draws the glyph quads in order with alpha blending, between begin_frame() and end_frame().
    virtual void draw(std::span<const TextQuad> quads) = 0;
};

struct FrameStats {
    std::uint32_t draw_calls = 0;
    std::uint32_t quads = 0;
};

/// Rendering backend interface (ADR-0001). Nothing from the underlying graphics API appears here.
/// All methods must be called from the thread that created the renderer.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// Resizes the swapchain (or offscreen target). Zero-sized (minimized) windows are allowed.
    virtual void resize(PixelSize size) = 0;
    [[nodiscard]] virtual PixelSize size() const = 0;

    /// Starts a frame and clears the target. Returns false if the frame must be skipped
    /// (zero-sized target, lost device); in that case do not call the other frame methods.
    [[nodiscard]] virtual bool begin_frame(const Color& clear) = 0;
    /// Draws filled rectangles in submission order with alpha blending.
    virtual void draw_quads(std::span<const Quad> quads) = 0;
    /// Copies the current target to RGBA8 pixels (row-major, top row first). Call between the last
    /// draw and end_frame(). Slow; for tests and screenshots only.
    [[nodiscard]] virtual std::vector<std::uint8_t> capture() = 0;
    /// Finishes the frame and presents it.
    virtual void end_frame() = 0;

    virtual void set_vsync(bool enabled) = 0;
    [[nodiscard]] virtual bool vsync() const = 0;

    /// Recompiles the shader if its source file changed on disk. Returns true if a new shader is now
    /// active; on a compile error the previous shader stays in use and the error is logged.
    virtual bool reload_shaders_if_changed() = 0;
    /// Incremented every time a shader was (re)built successfully, starting at 1.
    [[nodiscard]] virtual std::uint32_t shader_generation() const = 0;

    /// Named GPU debug regions shown in RenderDoc/PIX captures.
    virtual void push_marker(std::string_view name) = 0;
    virtual void pop_marker() = 0;

    [[nodiscard]] virtual std::string_view backend_name() const = 0;
    [[nodiscard]] virtual std::string_view adapter_name() const = 0;
    [[nodiscard]] virtual const FrameStats& last_frame_stats() const = 0;

    /// GPU time of a recent frame (begin_frame to end_frame) in milliseconds, measured with timestamp
    /// queries a few frames late; negative while unavailable.
    [[nodiscard]] virtual float last_gpu_ms() const = 0;
    /// Video memory currently used by this process on the adapter, in MiB (0 when unknown).
    [[nodiscard]] virtual float gpu_memory_mb() const = 0;

    /// Creates the RmlUi render interface that draws into this renderer's current target (E3-T06). Draw UI
    /// between begin_frame() and end_frame(); the interface must not outlive the renderer.
    [[nodiscard]] virtual core::Result<std::unique_ptr<Rml::RenderInterface>>
    create_ui_render_interface() = 0;

    /// Uploads an SDF atlas and returns a renderer for text drawn with it. Must not outlive this renderer.
    [[nodiscard]] virtual core::Result<std::unique_ptr<ITextRenderer>>
    create_text_renderer(const SdfAtlas& atlas) = 0;

    /// Uploads the map layers to the GPU and returns a view that draws them.
    [[nodiscard]] virtual core::Result<std::unique_ptr<IMapView>>
    create_map_view(const mapdata::MapData& map, const MapViewDesc& desc) = 0;
};

/// Creates the renderer for the current platform (Direct3D 11 on Windows).
[[nodiscard]] core::Result<std::unique_ptr<IRenderer>> create_renderer(const RendererDesc& desc);

}  // namespace pat::engine
