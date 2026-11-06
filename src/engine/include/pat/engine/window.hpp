#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/events.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pat::engine {

struct WindowDesc {
    std::string title = "Patriarch";
    /// Initial client size in *logical* units (96-dpi pixels); multiplied by the display scale of the
    /// primary display, so the window has the same apparent size at 100%-200% scaling.
    int logical_width = 1280;
    int logical_height = 720;
    bool resizable = true;
    bool fullscreen = false;  ///< Borderless desktop fullscreen.
    bool hidden = false;      ///< Create without showing (tests, headless smoke runs).
};

/// Operating system window with per-monitor DPI awareness and an event queue. Main thread only.
/// SDL3 is an implementation detail; the renderer gets the native handle (`HWND` on Windows).
class Window {
public:
    /// Initializes the video subsystem (reference counted) and creates the window.
    [[nodiscard]] static core::Result<Window> create(const WindowDesc& desc);

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    /// Pumps the OS message queue and appends the resulting events to `out`.
    void poll_events(std::vector<Event>& out);

    /// Asks the application to quit: a `Quit` event is delivered by the next poll_events().
    void request_close();

    /// Client area in physical pixels (the swapchain size).
    [[nodiscard]] PixelSize pixel_size() const;
    /// OS display scale of the monitor the window is on (1.0 = 100%).
    [[nodiscard]] float display_scale() const;

    void set_title(std::string_view title);
    void show();
    void set_text_input(bool enabled);
    void set_fullscreen(bool enabled);
    [[nodiscard]] bool is_fullscreen() const;

    /// Native window handle (`HWND` on Windows) for the renderer; null if unavailable.
    [[nodiscard]] void* native_handle() const;

    /// Name of the active video backend (e.g. "windows"; "dummy" in headless tests).
    [[nodiscard]] std::string_view video_driver() const;

private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::engine
