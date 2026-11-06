#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace pat::engine {

/// Platform-neutral input/window events. SDL types never leak through this header (ADR-0001).
/// All positions are in physical pixels of the window's client area.
namespace event {

struct Quit {};

struct Resized {
    int pixel_w = 0;
    int pixel_h = 0;
};

struct ScaleChanged {
    float scale = 1.0f;  ///< OS display scale, 1.0 = 100%.
};

struct FocusChanged {
    bool gained = false;
};

/// Modifier bit flags carried by Key events.
namespace mod {
inline constexpr std::uint16_t shift = 1;
inline constexpr std::uint16_t ctrl = 2;
inline constexpr std::uint16_t alt = 4;
inline constexpr std::uint16_t gui = 8;
}  // namespace mod

struct Key {
    int scancode = 0;  ///< Physical key (USB HID usage, layout independent).
    bool down = false;
    bool repeat = false;
    std::uint16_t mods = 0;  ///< Combination of mod::* flags.
};

struct MouseMove {
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
};

struct MouseButton {
    int button = 0;  ///< 1 = left, 2 = middle, 3 = right, 4/5 = extra.
    bool down = false;
    int clicks = 1;
    float x = 0.0f;
    float y = 0.0f;
};

struct MouseWheel {
    float x = 0.0f;  ///< Horizontal scroll, positive to the right.
    float y = 0.0f;  ///< Vertical scroll, positive away from the user.
};

struct TextInput {
    std::string utf8;
};

}  // namespace event

using Event = std::variant<event::Quit,
                           event::Resized,
                           event::ScaleChanged,
                           event::FocusChanged,
                           event::Key,
                           event::MouseMove,
                           event::MouseButton,
                           event::MouseWheel,
                           event::TextInput>;

/// A few physical key codes (USB HID usages) used by the spike and tools.
namespace scancode {
inline constexpr int enter = 40;
inline constexpr int escape = 41;
inline constexpr int space = 44;
inline constexpr int f1 = 58;
inline constexpr int f2 = 59;
inline constexpr int f3 = 60;
inline constexpr int f11 = 68;
}  // namespace scancode

}  // namespace pat::engine
