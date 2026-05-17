#include "gamepad_input_remapper.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/XKBlib.h>
// X11 / Xi headers like to #define short names like None, Bool, Status,
// Button1..5, COUNT, etc. that clobber our own enum members. Scrub the ones
// that actually collide so the rest of this TU sees plain identifiers.
#ifdef COUNT
#undef COUNT
#endif
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#endif

namespace gcpad {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Convert a virtual key code to the corresponding hardware scan code.
// Many games read scan codes via DirectInput or RawInput instead of virtual
// key codes, so we must provide both for reliable injection.
static uint16_t vkToScanCode(uint16_t vk) {
#ifdef _WIN32
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    return static_cast<uint16_t>(sc);
#else
    (void)vk;
    return 0;
#endif
}

// Apply deadzone removal and a power response curve to a raw axis value.
// Returns a signed value in [-1, 1] with the deadzone range collapsed to 0.
static float applyDeadzoneAndCurve(float value, float deadzone, float curve) {
    float absVal = std::abs(value);
    if (absVal <= deadzone) {
        return 0.0f;
    }
    // Normalize from [deadzone, 1] to [0, 1]
    float normalized = (absVal - deadzone) / (1.0f - deadzone);
    normalized = std::fmin(normalized, 1.0f);
    // Apply power curve for acceleration
    float curved = std::pow(normalized, curve);
    // Restore original sign
    return (value < 0.0f) ? -curved : curved;
}

// Returns the floating-point pixel contribution for this frame.
// The caller accumulates the fractional remainder so that gentle deflections
// don't vanish on truncation — that was the root cause of the "stiff" feel.
static float axisToMouseMotionF(float value, const AxisMouseMapping& mapping) {
    float processed = applyDeadzoneAndCurve(value, mapping.deadzone, mapping.curve);
    if (processed == 0.0f) return 0.0f;
    if (mapping.invert) processed = -processed;
    return processed * mapping.sensitivity;
}

#ifdef __linux__
// Global error handler state for X11 async error handling
static std::atomic<int> g_x11_error_occurred{0};

static int x11_error_handler(Display* /*display*/, XErrorEvent* /*event*/) {
    // Don't crash on X11 errors - game may have grabbed focus
    g_x11_error_occurred.store(1, std::memory_order_relaxed);
    return 0;
}
#endif

// ── Construction / reset ─────────────────────────────────────────────────────

GamepadInputRemapper::GamepadInputRemapper() {
    clearAllButtonMappings();
    clearAllAxisMappings();
    resetState();
}

void GamepadInputRemapper::resetState() {
    mouse_accum_x_ = 0.0f;
    mouse_accum_y_ = 0.0f;
    wheel_accum_.fill(0.0f);
    axis_key_pos_active_.fill(false);
    axis_key_neg_active_.fill(false);
    axis_mouse_btn_active_.fill(false);
}

// ── Button mapping ───────────────────────────────────────────────────────────

void GamepadInputRemapper::mapButtonToKey(Button button, uint16_t virtual_key) {
    button_to_key[static_cast<size_t>(button)] = virtual_key;
}

void GamepadInputRemapper::mapButtonToMouseButton(Button button, MouseButton mouse_button) {
    button_to_mouse[static_cast<size_t>(button)] = mouse_button;
}

void GamepadInputRemapper::mapButtonToWheel(Button button, int delta) {
    auto& m = button_to_wheel[static_cast<size_t>(button)];
    m.enabled = true;
    m.delta = delta;
}

void GamepadInputRemapper::clearButtonMapping(Button button) {
    size_t idx = static_cast<size_t>(button);
    button_to_key[idx].reset();
    button_to_mouse[idx].reset();
    button_to_wheel[idx].enabled = false;
}

void GamepadInputRemapper::clearAllButtonMappings() {
    for (auto& b : button_to_key)  b.reset();
    for (auto& b : button_to_mouse) b.reset();
    for (auto& b : button_to_wheel) b.enabled = false;
}

// ── Axis mapping ─────────────────────────────────────────────────────────────

void GamepadInputRemapper::mapAxisToMouse(Axis axis, float sensitivity, float deadzone,
                                           bool invert, float curve) {
    auto& m = axis_to_mouse[static_cast<size_t>(axis)];
    m.enabled     = true;
    m.sensitivity = sensitivity;
    m.deadzone    = deadzone;
    m.invert      = invert;
    m.curve       = curve;
}

void GamepadInputRemapper::mapAxisToKey(Axis axis, uint16_t virtual_key,
                                         float threshold, bool negative_direction) {
    size_t idx = static_cast<size_t>(axis);
    AxisKeyMapping akm;
    akm.enabled             = true;
    akm.virtual_key         = virtual_key;
    akm.threshold           = std::abs(threshold);
    akm.negative_direction  = negative_direction;

    if (negative_direction) {
        axis_to_key_negative[idx] = akm;
    } else {
        axis_to_key_positive[idx] = akm;
    }
}

void GamepadInputRemapper::mapAxisToMouseButton(Axis axis, MouseButton mouse_button,
                                                 float threshold) {
    size_t idx = static_cast<size_t>(axis);
    AxisMouseButtonMapping abm;
    abm.enabled   = true;
    abm.button    = mouse_button;
    abm.threshold = threshold;
    axis_to_mouse_button[idx] = abm;
}

void GamepadInputRemapper::mapAxisToWheel(Axis axis, int delta, float deadzone,
                                           bool invert, float tick_rate) {
    size_t idx = static_cast<size_t>(axis);
    AxisWheelMapping awm;
    awm.enabled        = true;
    awm.delta_per_tick = delta;
    awm.deadzone       = deadzone;
    awm.invert         = invert;
    awm.tick_rate      = tick_rate;
    axis_to_wheel[idx] = awm;
}

void GamepadInputRemapper::clearAxisMapping(Axis axis) {
    size_t idx = static_cast<size_t>(axis);
    axis_to_mouse[idx]        = AxisMouseMapping{};
    axis_to_key_positive[idx].reset();
    axis_to_key_negative[idx].reset();
    axis_to_mouse_button[idx].reset();
    axis_to_wheel[idx].reset();
}

void GamepadInputRemapper::clearAllAxisMappings() {
    for (auto& am : axis_to_mouse) am = AxisMouseMapping{};
    for (auto& ak : axis_to_key_positive)  ak.reset();
    for (auto& ak : axis_to_key_negative)  ak.reset();
    for (auto& ab : axis_to_mouse_button)  ab.reset();
    for (auto& aw : axis_to_wheel)         aw.reset();
}

// ── Event generation ─────────────────────────────────────────────────────────

std::vector<GamepadInputEvent> GamepadInputRemapper::remap(
        const GamepadState& current, const GamepadState& previous) const {
    std::vector<GamepadInputEvent> events;

    // ── Button -> keyboard / mouse button / mouse wheel events ───────────────
    for (size_t i = 0; i < static_cast<size_t>(Button::COUNT); ++i) {
        bool curPressed  = current.buttons[i];
        bool prevPressed = previous.buttons[i];

        if (curPressed != prevPressed) {
            // Button -> keyboard key
            if (button_to_key[i]) {
                GamepadInputEvent ev{};
                ev.type = GamepadInputEvent::Type::Keyboard;
                ev.keyboard.virtual_key = *button_to_key[i];
                ev.keyboard.scan_code   = vkToScanCode(*button_to_key[i]);
                ev.keyboard.pressed     = curPressed;
                events.push_back(ev);
            }
            // Button -> mouse button
            if (button_to_mouse[i]) {
                GamepadInputEvent ev{};
                ev.type = GamepadInputEvent::Type::MouseButton;
                ev.mouse_button.button  = *button_to_mouse[i];
                ev.mouse_button.pressed = curPressed;
                events.push_back(ev);
            }
            // Button -> mouse wheel (only on press, not release)
            if (button_to_wheel[i].enabled && curPressed) {
                GamepadInputEvent ev{};
                ev.type = GamepadInputEvent::Type::MouseWheel;
                ev.mouse_wheel.delta = button_to_wheel[i].delta;
                events.push_back(ev);
            }
        }
    }

    // ── Axis -> mouse motion ─────────────────────────────────────────────────
    // Accumulate fractional pixels across frames so gentle deflections are
    // never silently discarded by integer truncation (the "stiff stick" fix).
    float fdx = 0.0f, fdy = 0.0f;

    auto accumulateAxisF = [&](Axis axis, float& tx, float& ty, bool isX) {
        size_t idx = static_cast<size_t>(axis);
        if (!axis_to_mouse[idx].enabled) return;
        float motion = axisToMouseMotionF(current.axes[idx], axis_to_mouse[idx]);
        if (isX) tx += motion; else ty += motion;
    };

    accumulateAxisF(Axis::LeftX,  fdx, fdy, true);
    accumulateAxisF(Axis::LeftY,  fdx, fdy, false);
    accumulateAxisF(Axis::RightX, fdx, fdy, true);
    accumulateAxisF(Axis::RightY, fdx, fdy, false);
    accumulateAxisF(Axis::LeftTrigger,  fdx, fdy, true);
    accumulateAxisF(Axis::RightTrigger, fdx, fdy, true);

    mouse_accum_x_ += fdx;
    mouse_accum_y_ += fdy;
    int dx = static_cast<int>(mouse_accum_x_);
    int dy = static_cast<int>(mouse_accum_y_);
    mouse_accum_x_ -= static_cast<float>(dx);
    mouse_accum_y_ -= static_cast<float>(dy);

    if (dx != 0 || dy != 0) {
        GamepadInputEvent ev{};
        ev.type = GamepadInputEvent::Type::MouseMove;
        ev.mouse_move.dx = dx;
        ev.mouse_move.dy = dy;
        events.push_back(ev);
    }

    // ── Axis -> keyboard key (threshold-based) ──────────────────────────────
    for (size_t i = 0; i < static_cast<size_t>(Axis::COUNT); ++i) {
        float value = current.axes[i];

        // Positive direction
        if (axis_to_key_positive[i]) {
            const auto& akm = *axis_to_key_positive[i];
            if (akm.enabled) {
                bool shouldBeActive = (value >= akm.threshold);
                if (shouldBeActive && !axis_key_pos_active_[i]) {
                    GamepadInputEvent ev{};
                    ev.type = GamepadInputEvent::Type::Keyboard;
                    ev.keyboard.virtual_key = akm.virtual_key;
                    ev.keyboard.scan_code   = vkToScanCode(akm.virtual_key);
                    ev.keyboard.pressed     = true;
                    events.push_back(ev);
                    axis_key_pos_active_[i] = true;
                } else if (!shouldBeActive && axis_key_pos_active_[i]) {
                    GamepadInputEvent ev{};
                    ev.type = GamepadInputEvent::Type::Keyboard;
                    ev.keyboard.virtual_key = akm.virtual_key;
                    ev.keyboard.scan_code   = vkToScanCode(akm.virtual_key);
                    ev.keyboard.pressed     = false;
                    events.push_back(ev);
                    axis_key_pos_active_[i] = false;
                }
            }
        }

        // Negative direction
        if (axis_to_key_negative[i]) {
            const auto& akm = *axis_to_key_negative[i];
            if (akm.enabled) {
                bool shouldBeActive = (value <= -akm.threshold);
                if (shouldBeActive && !axis_key_neg_active_[i]) {
                    GamepadInputEvent ev{};
                    ev.type = GamepadInputEvent::Type::Keyboard;
                    ev.keyboard.virtual_key = akm.virtual_key;
                    ev.keyboard.scan_code   = vkToScanCode(akm.virtual_key);
                    ev.keyboard.pressed     = true;
                    events.push_back(ev);
                    axis_key_neg_active_[i] = true;
                } else if (!shouldBeActive && axis_key_neg_active_[i]) {
                    GamepadInputEvent ev{};
                    ev.type = GamepadInputEvent::Type::Keyboard;
                    ev.keyboard.virtual_key = akm.virtual_key;
                    ev.keyboard.scan_code   = vkToScanCode(akm.virtual_key);
                    ev.keyboard.pressed     = false;
                    events.push_back(ev);
                    axis_key_neg_active_[i] = false;
                }
            }
        }
    }

    // ── Axis -> mouse button (threshold-based) ──────────────────────────────
    for (size_t i = 0; i < static_cast<size_t>(Axis::COUNT); ++i) {
        if (!axis_to_mouse_button[i]) continue;
        const auto& abm = *axis_to_mouse_button[i];
        if (!abm.enabled) continue;

        float value = current.axes[i];
        bool shouldBeActive = (value >= abm.threshold);

        if (shouldBeActive && !axis_mouse_btn_active_[i]) {
            GamepadInputEvent ev{};
            ev.type = GamepadInputEvent::Type::MouseButton;
            ev.mouse_button.button  = abm.button;
            ev.mouse_button.pressed = true;
            events.push_back(ev);
            axis_mouse_btn_active_[i] = true;
        } else if (!shouldBeActive && axis_mouse_btn_active_[i]) {
            GamepadInputEvent ev{};
            ev.type = GamepadInputEvent::Type::MouseButton;
            ev.mouse_button.button  = abm.button;
            ev.mouse_button.pressed = false;
            events.push_back(ev);
            axis_mouse_btn_active_[i] = false;
        }
    }

    // ── Axis -> mouse wheel (accumulator-based) ─────────────────────────────
    for (size_t i = 0; i < static_cast<size_t>(Axis::COUNT); ++i) {
        if (!axis_to_wheel[i]) continue;
        const auto& awm = *axis_to_wheel[i];
        if (!awm.enabled) continue;

        float value = current.axes[i];
        if (std::abs(value) <= awm.deadzone) {
            wheel_accum_[i] = 0.0f;
            continue;
        }

        float contribution = value * awm.tick_rate;
        if (awm.invert) contribution = -contribution;
        wheel_accum_[i] += contribution;

        if (std::abs(wheel_accum_[i]) >= 1.0f) {
            int ticks = static_cast<int>(wheel_accum_[i]);
            wheel_accum_[i] -= static_cast<float>(ticks);

            GamepadInputEvent ev{};
            ev.type = GamepadInputEvent::Type::MouseWheel;
            ev.mouse_wheel.delta = ticks * awm.delta_per_tick;
            events.push_back(ev);
        }
    }

    return events;
}

// ── OS input injection ───────────────────────────────────────────────────────

bool GamepadInputRemapper::sendInput(const GamepadState& current,
                                      const GamepadState& previous) {
    auto events = remap(current, previous);
    if (events.empty()) {
        return true;
    }

#ifdef _WIN32
    std::vector<INPUT> native;
    native.reserve(events.size());

    for (const auto& e : events) {
        switch (e.type) {
            case GamepadInputEvent::Type::Keyboard: {
                INPUT input = {};
                input.type = INPUT_KEYBOARD;
                input.ki.wVk   = e.keyboard.virtual_key;
                input.ki.wScan = e.keyboard.scan_code;
                // Use both virtual key and scan code for maximum compatibility.
                // Games using DirectInput/RawInput read the scan code;
                // games using GetAsyncKeyState read the virtual key.
                input.ki.dwFlags = KEYEVENTF_SCANCODE;
                if (!e.keyboard.pressed) {
                    input.ki.dwFlags |= KEYEVENTF_KEYUP;
                }
                // Extended keys (arrows, numpad enter, etc.) need the extended flag
                if (e.keyboard.scan_code > 0xFF ||
                    e.keyboard.virtual_key == VK_RIGHT ||
                    e.keyboard.virtual_key == VK_LEFT ||
                    e.keyboard.virtual_key == VK_UP ||
                    e.keyboard.virtual_key == VK_DOWN ||
                    e.keyboard.virtual_key == VK_INSERT ||
                    e.keyboard.virtual_key == VK_DELETE ||
                    e.keyboard.virtual_key == VK_HOME ||
                    e.keyboard.virtual_key == VK_END ||
                    e.keyboard.virtual_key == VK_PRIOR ||
                    e.keyboard.virtual_key == VK_NEXT ||
                    e.keyboard.virtual_key == VK_NUMLOCK) {
                    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
                native.push_back(input);
                break;
            }
            case GamepadInputEvent::Type::MouseButton: {
                INPUT input = {};
                input.type = INPUT_MOUSE;
                switch (e.mouse_button.button) {
                    case MouseButton::Left:
                        input.mi.dwFlags = e.mouse_button.pressed
                            ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                        break;
                    case MouseButton::Right:
                        input.mi.dwFlags = e.mouse_button.pressed
                            ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                        break;
                    case MouseButton::Middle:
                        input.mi.dwFlags = e.mouse_button.pressed
                            ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                        break;
                }
                native.push_back(input);
                break;
            }
            case GamepadInputEvent::Type::MouseMove: {
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx = e.mouse_move.dx;
                input.mi.dy = e.mouse_move.dy;
                native.push_back(input);
                break;
            }
            case GamepadInputEvent::Type::MouseWheel: {
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                input.mi.mouseData = static_cast<DWORD>(e.mouse_wheel.delta);
                native.push_back(input);
                break;
            }
        }
    }

    if (!native.empty()) {
        UINT sent = ::SendInput(static_cast<UINT>(native.size()),
                                native.data(), sizeof(INPUT));
        return sent == static_cast<UINT>(native.size());
    }

    return true;
#elif defined(__linux__)
    // FIX: Open and close X11 display per-call instead of caching.
    // When a game launches fullscreen, it takes control of X11 and the
    // cached display handle becomes invalid, causing X11 calls to block.
    // Also set up error handler to prevent crashes on X11 errors.
    g_x11_error_occurred.store(0, std::memory_order_relaxed);
    XSetErrorHandler(x11_error_handler);
    
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        XSetErrorHandler(nullptr);
        return false;
    }

    // Check if error occurred during display open
    if (g_x11_error_occurred.load(std::memory_order_relaxed)) {
        XCloseDisplay(display);
        XSetErrorHandler(nullptr);
        return false;
    }

    // Try to get input focus - this may fail during game launch
    Window root = DefaultRootWindow(display);
    Window focus = 0;
    int revert_to = 0;
    
    // Use XGetInputFocus with error checking
    XGetInputFocus(display, &focus, &revert_to);
    int focus_error = XSync(display, False);
    if (g_x11_error_occurred.load(std::memory_order_relaxed)) {
        XCloseDisplay(display);
        XSetErrorHandler(nullptr);
        return false;
    }

    for (const auto& e : events) {
        // Skip further processing if error occurred
        if (g_x11_error_occurred.load(std::memory_order_relaxed)) {
            break;
        }
        
        switch (e.type) {
            case GamepadInputEvent::Type::Keyboard: {
                KeyCode keycode = static_cast<KeyCode>(e.keyboard.virtual_key);
                if (e.keyboard.pressed) {
                    XTestFakeKeyEvent(display, keycode, True, CurrentTime);
                    XTestFakeKeyEvent(display, keycode, False, CurrentTime);
                }
                break;
            }
            case GamepadInputEvent::Type::MouseButton: {
                unsigned int button;
                switch (e.mouse_button.button) {
                    case MouseButton::Left: button = Button1; break;
                    case MouseButton::Right: button = Button2; break;
                    case MouseButton::Middle: button = Button3; break;
                    default: continue;
                }
                XTestFakeButtonEvent(display, button,
                    e.mouse_button.pressed ? True : False, CurrentTime);
                break;
            }
            case GamepadInputEvent::Type::MouseMove: {
                XTestFakeRelativeMotionEvent(display,
                    static_cast<double>(e.mouse_move.dx),
                    static_cast<double>(e.mouse_move.dy),
                    CurrentTime);
                break;
            }
            case GamepadInputEvent::Type::MouseWheel: {
                int direction = (e.mouse_wheel.delta > 0) ? 4 : 5;
                int count = std::abs(e.mouse_wheel.delta) / 120;
                for (int i = 0; i < count; ++i) {
                    XTestFakeButtonEvent(display, direction, True, CurrentTime);
                    XTestFakeButtonEvent(display, direction, False, CurrentTime);
                }
                break;
            }
        }
    }

    XFlush(display);
    XCloseDisplay(display);
    XSetErrorHandler(nullptr);
    return !g_x11_error_occurred.load(std::memory_order_relaxed);
#else
    (void)events;
    return false;
#endif
}

// ── Utility ──────────────────────────────────────────────────────────────────

std::string GamepadInputRemapper::virtualKeyName(uint16_t vk) {
#ifdef _WIN32
    // Use GetKeyNameTextA via scan code for readable names
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc == 0) {
        // Fallback for unmapped keys
        char buf[32];
        snprintf(buf, sizeof(buf), "VK_0x%02X", vk);
        return buf;
    }
    // GetKeyNameTextA expects scan code in bits 16-23, extended flag in bit 24
    LONG lparam = static_cast<LONG>(sc) << 16;
    char name[64] = {};
    int len = GetKeyNameTextA(lparam, name, sizeof(name));
    if (len > 0) {
        return std::string(name, static_cast<size_t>(len));
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "VK_0x%02X", vk);
    return buf;
#elif defined(__linux__)
    // FIX: Open and close display per-call instead of caching.
    // This ensures we get a fresh connection that works even after
    // a game has taken control of X11.
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        char buf[32];
        snprintf(buf, sizeof(buf), "VK_0x%02X", vk);
        return buf;
    }
    
    g_x11_error_occurred.store(0, std::memory_order_relaxed);
    XSetErrorHandler(x11_error_handler);
    
    KeyCode keycode = static_cast<KeyCode>(vk);
    // Use XGetKeyboardMapping as fallback for XKeycodeToKeysym deprecation
    KeySym keysym = 0;
    int keysyms_per_keycode_return = 0;
    KeySym* keysym_map = XGetKeyboardMapping(display, keycode, 1, &keysyms_per_keycode_return);
    if (keysym_map && keysyms_per_keycode_return > 0) {
        keysym = keysym_map[0];
        XFree(keysym_map);
    }
    const char* name = XKeysymToString(keysym);
    
    XSetErrorHandler(nullptr);
    XCloseDisplay(display);
    
    if (name && !g_x11_error_occurred.load(std::memory_order_relaxed)) {
        return std::string(name);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "VK_0x%02X", vk);
    return buf;
#else
    char buf[32];
    snprintf(buf, sizeof(buf), "VK_0x%02X", vk);
    return buf;
#endif
}

} // namespace gcpad