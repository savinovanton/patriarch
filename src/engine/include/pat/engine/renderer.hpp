#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/dpi.hpp"

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
};

/// Creates the renderer for the current platform (Direct3D 11 on Windows).
[[nodiscard]] core::Result<std::unique_ptr<IRenderer>> create_renderer(const RendererDesc& desc);

}  // namespace pat::engine
