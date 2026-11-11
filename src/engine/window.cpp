#include "pat/engine/window.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/log.hpp"
#include "pat/core/profile.hpp"

#include <SDL3/SDL.h>

#include <utility>

namespace pat::engine {

namespace {

core::Error sdl_error(int code, const char* what) {
    return core::Error{code, std::string(what) + ": " + SDL_GetError()};
}

std::uint16_t convert_mods(SDL_Keymod m) {
    std::uint16_t out = 0;
    if ((m & SDL_KMOD_SHIFT) != 0) {
        out |= event::mod::shift;
    }
    if ((m & SDL_KMOD_CTRL) != 0) {
        out |= event::mod::ctrl;
    }
    if ((m & SDL_KMOD_ALT) != 0) {
        out |= event::mod::alt;
    }
    if ((m & SDL_KMOD_GUI) != 0) {
        out |= event::mod::gui;
    }
    return out;
}

}  // namespace

struct Window::Impl {
    SDL_Window* window = nullptr;
    bool video_inited = false;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        if (video_inited) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    [[nodiscard]] float pixel_density() const {
        const float d = SDL_GetWindowPixelDensity(window);
        return d > 0.0f ? d : 1.0f;
    }
};

Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;
Window::~Window() = default;

core::Result<Window> Window::create(const WindowDesc& desc) {
    auto impl = std::make_unique<Impl>();

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return core::Result<Window>::err(sdl_error(1, "SDL video init failed"));
    }
    impl->video_inited = true;

    // Ask for the display scale of the primary display so the initial size looks the same at any DPI.
    float scale = 1.0f;
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    if (primary != 0) {
        const float s = SDL_GetDisplayContentScale(primary);
        if (s > 0.0f) {
            scale = s;
        }
    }
    const PixelSize size = physical_size({desc.logical_width, desc.logical_height}, scale);

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (desc.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    impl->window = SDL_CreateWindow(desc.title.c_str(), size.w, size.h, flags);
    if (impl->window == nullptr) {
        return core::Result<Window>::err(sdl_error(2, "SDL window creation failed"));
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    PAT_LOG_INFO("engine",
                 "window created: {}x{} px, display scale {:.2f}, video driver '{}'",
                 size.w,
                 size.h,
                 scale,
                 driver != nullptr ? driver : "?");
    return core::Result<Window>::ok(Window(std::move(impl)));
}

void Window::poll_events(std::vector<Event>& out) {
    PAT_PROFILE_SCOPE("window.poll_events");
    const float density = impl_->pixel_density();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                out.emplace_back(event::Quit{});
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                out.emplace_back(event::Resized{e.window.data1, e.window.data2});
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                out.emplace_back(event::ScaleChanged{display_scale()});
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                out.emplace_back(event::FocusChanged{true});
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                out.emplace_back(event::FocusChanged{false});
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                out.emplace_back(event::Key{
                    static_cast<int>(e.key.scancode), e.key.down, e.key.repeat, convert_mods(e.key.mod)});
                break;
            case SDL_EVENT_MOUSE_MOTION:
                out.emplace_back(event::MouseMove{e.motion.x * density,
                                                  e.motion.y * density,
                                                  e.motion.xrel * density,
                                                  e.motion.yrel * density});
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                out.emplace_back(event::MouseButton{static_cast<int>(e.button.button),
                                                    e.button.down,
                                                    static_cast<int>(e.button.clicks),
                                                    e.button.x * density,
                                                    e.button.y * density});
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                out.emplace_back(event::MouseWheel{e.wheel.x, e.wheel.y});
                break;
            case SDL_EVENT_TEXT_INPUT:
                out.emplace_back(event::TextInput{e.text.text != nullptr ? e.text.text : ""});
                break;
            default:
                break;
        }
    }
}

void Window::request_close() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

PixelSize Window::pixel_size() const {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(impl_->window, &w, &h);
    return {w, h};
}

float Window::display_scale() const {
    const float s = SDL_GetWindowDisplayScale(impl_->window);
    return s > 0.0f ? s : 1.0f;
}

void Window::set_title(std::string_view title) {
    const std::string copy(title);
    SDL_SetWindowTitle(impl_->window, copy.c_str());
}

void Window::show() {
    SDL_ShowWindow(impl_->window);
}

void Window::set_text_input(bool enabled) {
    if (enabled) {
        SDL_StartTextInput(impl_->window);
    } else {
        SDL_StopTextInput(impl_->window);
    }
}

void Window::set_fullscreen(bool enabled) {
    SDL_SetWindowFullscreen(impl_->window, enabled);
}

bool Window::is_fullscreen() const {
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void* Window::native_handle() const {
#ifdef _WIN32
    const SDL_PropertiesID props = SDL_GetWindowProperties(impl_->window);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    return nullptr;
#endif
}

std::string_view Window::video_driver() const {
    const char* name = SDL_GetCurrentVideoDriver();
    return name != nullptr ? name : "";
}

}  // namespace pat::engine
