// The Android half of the platform seam (see src/window.h). The ws shell
// runs on the thread native_app_glue entered android_main() on; the OS owns
// the window - it hands the ANativeWindow over at APP_CMD_INIT_WINDOW and
// takes it back at APP_CMD_TERM_WINDOW - so "window ready" is a runtime
// state here, not the startup fact it is on desktop. Everything Android
// about the shell lives in this TU: the looper pump, the lifecycle
// commands, and the input translation.
//
// Input contract: every touch contact decodes into the gui frame's touch
// table and the app Touch* events (stable per-contact ids), and the FIRST
// contact of a gesture bridges to mouse button 0 - position, press, release
// - so single-finger use, the gui's mouse path and ImGui keep working
// unchanged. The bridge feeds ImGui's io directly: touch motion never
// reaches ImGui_ImplAndroid_HandleInputEvent, whose decoder follows
// pointer index 0 only and has no POINTER_UP/CANCEL case (a second finger
// lifting used to strand the press in io). AKeyCodes translate to the GLFW
// numbering the event system documents ("printable keys match ASCII") and
// glyphs arrive as TextInputEvent; key and real-mouse events keep first
// refusal through the backend, the same ordering desktop gets from the
// GLFW backend's callback chaining.

#include "platform/window.h"

#include <cstddef>  // std::size_t - the OS pointer indices

#include <ws/core/log.hpp>

#include <imgui.h>
#include <imgui_impl_android.h>

// native_app_glue and the NDK input headers are C - keep them clear of our
// warning settings.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ws::ui {
namespace {

constexpr int kKeyUnknown = -1;  // GLFW_KEY_UNKNOWN; out of InputFrame range

// AKeyEvent code -> the GLFW key numbering the event system documents
// ("printable keys match ASCII" - GLFW_KEY_A == 'A' == 65, etc.). Covers
// the printable set plus the standard editing/navigation keys; unmapped
// keys drop (kKeyUnknown fails the InputFrame range check too).
int glfw_key(int32_t keycode) {
    if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z) {
        return 'A' + (keycode - AKEYCODE_A);
    }
    if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9) {
        return '0' + (keycode - AKEYCODE_0);
    }
    switch (keycode) {
        case AKEYCODE_SPACE: return 32;   // GLFW_KEY_SPACE
        case AKEYCODE_ESCAPE: return 256;  // GLFW_KEY_ESCAPE
        case AKEYCODE_ENTER: return 257;  // GLFW_KEY_ENTER
        case AKEYCODE_TAB: return 258;     // GLFW_KEY_TAB
        case AKEYCODE_DEL: return 259;      // GLFW_KEY_BACKSPACE
        case AKEYCODE_FORWARD_DEL: return 261;  // GLFW_KEY_DELETE
        case AKEYCODE_DPAD_RIGHT: return 262;
        case AKEYCODE_DPAD_LEFT: return 263;
        case AKEYCODE_DPAD_DOWN: return 264;
        case AKEYCODE_DPAD_UP: return 265;
        case AKEYCODE_PAGE_UP: return 266;
        case AKEYCODE_PAGE_DOWN: return 267;
        case AKEYCODE_MOVE_HOME: return 268;
        case AKEYCODE_MOVE_END: return 269;
        case AKEYCODE_CAPS_LOCK: return 280;
        case AKEYCODE_F1: return 290;
        case AKEYCODE_F2: return 291;
        case AKEYCODE_F3: return 292;
        case AKEYCODE_F4: return 293;
        case AKEYCODE_SHIFT_LEFT: return 340;   // GLFW_KEY_LEFT_SHIFT
        case AKEYCODE_CTRL_LEFT: return 341;    // GLFW_KEY_LEFT_CONTROL
        case AKEYCODE_ALT_LEFT: return 342;     // GLFW_KEY_LEFT_ALT
        case AKEYCODE_SHIFT_RIGHT: return 344;  // GLFW_KEY_RIGHT_SHIFT
        case AKEYCODE_COMMA: return ',';
        case AKEYCODE_PERIOD: return '.';
        case AKEYCODE_MINUS: return '-';
        case AKEYCODE_EQUALS: return '=';
        case AKEYCODE_LEFT_BRACKET: return '[';
        case AKEYCODE_RIGHT_BRACKET: return ']';
        case AKEYCODE_BACKSLASH: return '\\';
        case AKEYCODE_SEMICOLON: return ';';
        case AKEYCODE_APOSTROPHE: return '\'';
        case AKEYCODE_SLASH: return '/';
        case AKEYCODE_GRAVE: return '`';
        default: return kKeyUnknown;
    }
}

// WindowHost on native_app_glue. The cmd/input callbacks run on this same
// thread (that is the point of the glue - it wakes our looper), so all
// state below is single-threaded, like the rest of the shell.
class AndroidHost final : public WindowHost {
public:
    ~AndroidHost() override { app_ = nullptr; }  // the glue owns the handle

    bool init(WindowEventSink& sink, const AppConfig& config) override {
        if (config.app_handle == nullptr) {
            core::error("ui: AppConfig::app_handle is null - pass the "
                        "android_app android_main() received");
            return false;
        }
        app_ = config.app_handle;
        sink_ = &sink;
        app_->userData = this;
        app_->onAppCmd = &AndroidHost::app_cmd;
        app_->onInputEvent = &AndroidHost::app_input;
        // The display density, read once: the screen does not change
        // under a live process. density / 160 is the dp convention (a
        // 280 dpi phone reports 1.75); an OS that refuses to name one
        // (NONE, or the ANY wildcard) keeps the unscaled look.
        AConfiguration* aconfig = AConfiguration_new();
        if (aconfig != nullptr) {
            AConfiguration_fromAssetManager(aconfig,
                                            app_->activity->assetManager);
            const int32_t density = AConfiguration_getDensity(aconfig);
            AConfiguration_delete(aconfig);
            ui_scale_ = density > 0 && density != ACONFIGURATION_DENSITY_ANY
                            ? static_cast<float>(density) / 160.0f
                            : 1.0f;
        }
        return true;
    }

    std::vector<const char*> instance_extensions() const override {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    }

    bool create_surface(VkInstance instance, VkSurfaceKHR* out) const override {
        // volk resolves the extension entry point (VK_USE_PLATFORM_ANDROID_KHR
        // is defined on the volk target, so volkLoadInstance filled it); the
        // Android loader exposes VK_KHR_android_surface universally.
        if (vkCreateAndroidSurfaceKHR == nullptr) {
            core::error("ui: vkCreateAndroidSurfaceKHR unavailable");
            return false;
        }
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = app_->window;
        return vkCreateAndroidSurfaceKHR(instance, &info, nullptr, out) == VK_SUCCESS;
    }

    void pump() override { poll_all(0); }

    void wait_for_window() override {
        // Surface-loss sleep: block on the looper (battery-friendly, not a
        // spin) until the OS brings a window back or asks for destroy. One
        // event per round: poll_all(-1) parks inside its drain loop right
        // after INIT_WINDOW - waiting for a further event that never comes -
        // and the window_ready_ that command set would never be observed.
        while (!should_close() && !window_ready_) {
            poll_once(-1);
        }
    }

    bool should_close() const override {
        return close_requested_ || app_->destroyRequested != 0;
    }

    void request_stop() override {
        // The OS then delivers the full pause/term-window/destroy sequence
        // through pump()/wait_for_window() - the shell tears the window
        // stage down on the way, in order, like any other surface loss.
        ANativeActivity_finish(app_->activity);
    }

    bool window_ready() const override { return window_ready_; }

    bool is_hidden() const override {
        return false;  // a lost window reports through window_ready() instead
    }

    void framebuffer_size(int* width, int* height) const override {
        *width = (app_->window != nullptr) ? ANativeWindow_getWidth(app_->window) : 0;
        *height = (app_->window != nullptr) ? ANativeWindow_getHeight(app_->window) : 0;
    }

    float ui_scale() const override { return ui_scale_; }

    bool imgui_platform_init() override {
        // Once per attach: each APP_CMD_INIT_WINDOW can hand over a NEW
        // ANativeWindow, and the backend keeps the pointer it was given.
        return ImGui_ImplAndroid_Init(app_->window);
    }

    void imgui_platform_new_frame() override { ImGui_ImplAndroid_NewFrame(); }

    void imgui_platform_shutdown() override { ImGui_ImplAndroid_Shutdown(); }

private:
    // One round of the glue looper. `timeout` is ALooper_pollOnce's argument:
    // 0 drains what is pending (the running frame loop), -1 blocks until
    // something arrives (surface-loss sleep). pollOnce reports ONE event per
    // call - the while loop is the drain - and is the r27-correct API
    // (pollAll is marked unavailable there: it may ignore wakes).
    void poll_all(int timeout) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeout, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app_, source);
            }
            if (app_->destroyRequested != 0) {
                break;
            }
        }
    }

    // At most one looper event: block, process it, return. The blocking wait
    // (wait_for_window) must re-check window_ready_/should_close() between
    // events - poll_all's drain only ever returns on a 0 timeout, where an
    // idle looper answers "nothing pending" instead of parking.
    void poll_once(int timeout) {
        int events = 0;
        android_poll_source* source = nullptr;
        if (ALooper_pollOnce(timeout, nullptr, &events,
                             reinterpret_cast<void**>(&source)) >= 0 &&
            source != nullptr) {
            source->process(app_, source);
        }
    }

    // Forwards `event` unless the sink is not wired (paranoia; init wires it).
    template <typename E>
    void emit(E&& event) {
        if (sink_->forward) {
            sink_->forward(event);
        }
    }

    // --- input translation --------------------------------------------------

    static void app_cmd(android_app* app, int32_t cmd) {
        AndroidHost* host = static_cast<AndroidHost*>(app->userData);
        if (host == nullptr) {
            return;
        }
        switch (cmd) {
            case APP_CMD_INIT_WINDOW:
                // The OS just handed over the ANativeWindow (app->window);
                // surface creation and the window stage follow in the
                // shell's loop, off window_ready().
                if (app->window == nullptr) {
                    core::error("ui: APP_CMD_INIT_WINDOW without a window");
                    host->close_requested_ = true;
                    return;
                }
                host->window_ready_ = true;
                break;
            case APP_CMD_TERM_WINDOW:
                // The OS takes the window back; the swapchain must come
                // down before anything else touches the surface (the shell
                // observes window_ready() going false).
                host->window_ready_ = false;
                break;
            case APP_CMD_WINDOW_RESIZED: {
                const int32_t width = (app->window != nullptr)
                                          ? ANativeWindow_getWidth(app->window)
                                          : 0;
                const int32_t height = (app->window != nullptr)
                                           ? ANativeWindow_getHeight(app->window)
                                           : 0;
                if (width > 0 && height > 0) {
                    host->emit(WindowResizeEvent{width, height});
                }
                break;
            }
            case APP_CMD_DESTROY:
                host->close_requested_ = true;
                break;
            case APP_CMD_LOW_MEMORY:
                core::warn("ui: Android reports low memory");
                break;
            default:
                break;  // resume/pause/stop/save-state: no shell state rides them
        }
    }

    static int32_t app_input(android_app* app, AInputEvent* event) {
        if (AndroidHost* host = static_cast<AndroidHost*>(app->userData);
            host != nullptr) {
            host->handle_input(event);
        }
        return 1;  // consumed
    }

    void handle_input(const AInputEvent* event) {
        const bool is_key = AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY;
        const bool mouse_source = (AInputEvent_getSource(event) &
                                   AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
        // ImGui first refusal - io absorbs the event before the ws
        // translation runs, so imgui_wants() (evaluated at the sink) sees
        // the WantCapture flags from the last NewFrame, exactly like the
        // desktop callback chaining. Keys and real-mouse motion keep the
        // backend; touch-screen motion does not - the backend's decoder
        // follows pointer index 0 only and has no POINTER_UP/CANCEL case,
        // so a second finger lifting stranded the press in io. The
        // primary-touch bridge feeds io instead, from inside the touch
        // translation below.
        if (ImGui::GetCurrentContext() != nullptr && (is_key || mouse_source)) {
            ImGui_ImplAndroid_HandleInputEvent(event);
        }
        if (is_key) {
            translate_key(event);
        } else if (mouse_source) {
            translate_mouse(event);
        } else {
            translate_touch(event);
        }
    }

    void translate_key(const AInputEvent* event) {
        const int32_t keycode = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (keycode == AKEYCODE_BACK) {
            // Back exits the activity, the Android convention - through
            // ANativeActivity_finish so teardown walks the normal sequence.
            if (action == AKEY_EVENT_ACTION_DOWN) {
                request_stop();
            }
            return;
        }
        const int key = glfw_key(keycode);
        if (key == kKeyUnknown) {
            return;
        }
        const bool repeat = (action == AKEY_EVENT_ACTION_MULTIPLE);
        if (action == AKEY_EVENT_ACTION_DOWN || repeat) {
            emit(KeyPressedEvent{key, repeat});
        } else if (action == AKEY_EVENT_ACTION_UP) {
            emit(KeyReleasedEvent{key});
        }
        // gui tap, the same rules as the desktop key callback: held state
        // on every transition, pressed edge on the first DOWN only.
        if (gui::InputFrame* input = sink_->input;
            input != nullptr && key >= 0 && key < gui::InputFrame::kMaxKeys) {
            input->key_down[key] = (action != AKEY_EVENT_ACTION_UP);
            if (action == AKEY_EVENT_ACTION_DOWN) {
                input->key_pressed[key] = true;
            }
        }
        // Text tap: hardware-keyboard glyphs only (native_app_glue has no
        // IME, so there is no soft-keyboard path). The key contract is GLFW
        // numbering - letters report uppercase, corrected to lowercase when
        // shift is not held. One TextInputEvent per glyph (the event
        // counterpart of the gui frame's text_input; desktop's on_char
        // emits the same), then the frame's own copy.
        if (action == AKEY_EVENT_ACTION_DOWN && key >= 32 && key <= 126) {
            uint32_t code = static_cast<uint32_t>(key);
            const bool shift_held = (AKeyEvent_getMetaState(event) &
                                     (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON |
                                      AMETA_SHIFT_RIGHT_ON)) != 0;
            if (key >= 'A' && key <= 'Z' && !shift_held) {
                code += 32;
            }
            emit(TextInputEvent{code});
            if (gui::InputFrame* input = sink_->input; input != nullptr) {
                input->text_input.push_back(code);
            }
        }
    }

    // --- touch-screen motion (multi-touch) ----------------------------------
    // Every contact the OS reports decodes into the gui frame's touch
    // table, keyed by its stable per-contact id, and into the app Touch*
    // events. The touch_* writers own the table AND the primary-touch ->
    // mouse bridge (see gui/input_frame.hpp); this side only observes
    // bridged_touch() to emit the bridge's Mouse* events and io feed -
    // exactly the pair the old single-finger path produced, for whichever
    // contact currently holds the bridge. Without a gui tap there is no
    // bridge, so only the Touch* events flow.

    // The 9th+ finger: the table is full and the contact drops (warned
    // once per process, like every cap in the lib) - its events drop with
    // it, since an app that never saw the press must never see its moves
    // or release either.
    void warn_touch_cap() {
        if (!touch_cap_warned_) {
            touch_cap_warned_ = true;
            core::warn("ui: touch table full at {} contacts - further "
                       "contacts dropped",
                       gui::InputFrame::kMaxTouches);
        }
    }

    // ImGui's io fed from the bridge - the same public event API the
    // Android backend itself drives (touch source, position, button 0),
    // so a finger keeps working on ImGui windows unchanged.
    void imgui_bridge_pos(float x, float y) {
        if (ImGui::GetCurrentContext() == nullptr) {
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(x, y);
    }
    void imgui_bridge_button(bool down) {
        if (ImGui::GetCurrentContext() == nullptr) {
            return;
        }
        ImGui::GetIO().AddMouseButtonEvent(0, down);
    }

    // One contact began: DOWN (index 0 - the gesture's first finger) or
    // POINTER_DOWN (index from the action bits). The bridge acquires it
    // when no contact holds it yet - never a promotion mid-gesture.
    void touch_begin(const AInputEvent* event, std::size_t index) {
        const uint32_t raw_id = AMotionEvent_getPointerId(event, index);
        const gui::PointerId id{raw_id};  // the gui's strong id (events below stay raw)
        const float x = AMotionEvent_getX(event, index);
        const float y = AMotionEvent_getY(event, index);
        bool bridged = false;
        if (gui::InputFrame* input = sink_->input; input != nullptr) {
            const gui::PointerId bridge_before = input->bridged_touch();
            if (input->touch_down(id, x, y) < 0) {
                warn_touch_cap();
                return;
            }
            bridged = (bridge_before == gui::kNoPointer &&
                       input->bridged_touch() == id);
        }
        emit(TouchPressedEvent{raw_id, x, y});
        if (bridged) {
            imgui_bridge_pos(x, y);
            imgui_bridge_button(true);
            emit(MouseMovedEvent{x, y});  // a coherent position before the press
            emit(MouseButtonPressedEvent{0});
        }
    }

    // Every tracked contact moved: one MOVE event carries all current
    // coords, and getX/getY read the latest sample (the historical ones
    // need the getHistorical* API) - latest wins, the same rule the event
    // stream's per-pointer coalescing applies to what we emit here.
    void touch_move_all(const AInputEvent* event) {
        gui::InputFrame* input = sink_->input;
        const std::size_t count = AMotionEvent_getPointerCount(event);
        for (std::size_t i = 0; i < count; ++i) {
            const uint32_t raw_id = AMotionEvent_getPointerId(event, i);
            const gui::PointerId id{raw_id};
            const float x = AMotionEvent_getX(event, i);
            const float y = AMotionEvent_getY(event, i);
            if (input == nullptr) {
                emit(TouchMovedEvent{raw_id, x, y});  // no table: nothing to gate on
                continue;
            }
            if (!input->pointer_pos(id, nullptr, nullptr)) {
                continue;  // a contact the cap dropped: no moves either
            }
            const bool bridged = input->bridged_touch() == id;
            input->touch_move(id, x, y);
            emit(TouchMovedEvent{raw_id, x, y});
            if (bridged) {
                imgui_bridge_pos(x, y);
                emit(MouseMovedEvent{x, y});
            }
        }
    }

    // One contact ended: UP (index 0 - the gesture's last finger),
    // POINTER_UP (index from the action bits), or one contact of a
    // CANCELled gesture. `cancelled` marks an OS-aborted gesture; the
    // release edge still fires and the gui release rules treat it like
    // any release - inside clicks, outside clears.
    void touch_end(const AInputEvent* event, std::size_t index,
                   bool cancelled) {
        const uint32_t raw_id = AMotionEvent_getPointerId(event, index);
        const gui::PointerId id{raw_id};
        const float x = AMotionEvent_getX(event, index);
        const float y = AMotionEvent_getY(event, index);
        bool bridged = false;
        if (gui::InputFrame* input = sink_->input; input != nullptr) {
            if (!input->pointer_pos(id, nullptr, nullptr)) {
                return;  // a contact the cap dropped: no release either
            }
            bridged = input->bridged_touch() == id;
            input->touch_up(id, x, y, cancelled);
        }
        emit(TouchReleasedEvent{raw_id, x, y, cancelled});
        if (bridged) {
            imgui_bridge_pos(x, y);
            imgui_bridge_button(false);
            emit(MouseMovedEvent{x, y});
            emit(MouseButtonReleasedEvent{0});
        }
    }

    // Touchscreen (and stylus - pen differentiation is out of scope v1,
    // one contact through the same path). AMotionEvent reports window
    // pixels - the same space the framebuffer uses (the ANativeWindow has
    // no separate DPI-scaled screen space like desktop GLFW), so no
    // scaling. The action's high bits name the pointer a POINTER_DOWN/
    // POINTER_UP is about; DOWN/UP carry the gesture's first/last contact;
    // MOVE carries every pointer's coords; CANCEL aborts the whole
    // gesture at once.
    void translate_touch(const AInputEvent* event) {
        const int32_t action = AMotionEvent_getAction(event);
        const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        const std::size_t index = static_cast<std::size_t>(
            (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        switch (masked) {
            case AMOTION_EVENT_ACTION_DOWN:
                touch_begin(event, 0);
                break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                touch_begin(event, index);
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                touch_move_all(event);
                break;
            case AMOTION_EVENT_ACTION_POINTER_UP:
                touch_end(event, index, false);
                break;
            case AMOTION_EVENT_ACTION_UP:
                touch_end(event, 0, false);  // the last contact lifting
                break;
            case AMOTION_EVENT_ACTION_CANCEL:
                for (std::size_t i = 0;
                     i < AMotionEvent_getPointerCount(event); ++i) {
                    touch_end(event, i, true);
                }
                break;
            default:
                break;  // hover/outside never come from a touchscreen
        }
    }

    // Real mouse (Bluetooth): buttons diffed from the event's button mask -
    // one motion event can change several at once - plus hover and wheel.
    // GLFW button order is left(0), right(1), middle(2); Android's mask bits
    // are primary<<0, secondary<<1, tertiary<<2. This path alone owns
    // mouse_present on Android (the gui's "is there a mouse"): button
    // transitions, motion and hover-enter set it, HOVER_EXIT clears it.
    // The touch path never touches the flag - a finger is not a mouse, and
    // a lifting one must not unplug a real one.
    void translate_mouse(const AInputEvent* event) {
        const int32_t buttons = AMotionEvent_getButtonState(event);
        struct ButtonBit {
            int32_t bit;
            int button;
        };
        constexpr ButtonBit kBits[3] = {
            {AMOTION_EVENT_BUTTON_PRIMARY, 0},
            {AMOTION_EVENT_BUTTON_SECONDARY, 1},
            {AMOTION_EVENT_BUTTON_TERTIARY, 2},
        };
        gui::InputFrame* input = sink_->input;
        for (const ButtonBit& bit : kBits) {
            const bool was_down = (mouse_buttons_ & bit.bit) != 0;
            const bool now_down = (buttons & bit.bit) != 0;
            if (was_down == now_down) {
                continue;
            }
            if (now_down) {
                emit(MouseButtonPressedEvent{bit.button});
                if (input != nullptr) {
                    // A press is the strongest presence signal - and the
                    // device may never have hovered first.
                    input->mouse_present = true;
                    input->mouse_down[bit.button] = true;
                    input->mouse_pressed[bit.button] = true;
                }
            } else {
                emit(MouseButtonReleasedEvent{bit.button});
                if (input != nullptr) {
                    input->mouse_down[bit.button] = false;
                    input->mouse_released[bit.button] = true;
                }
            }
        }
        mouse_buttons_ = buttons;

        const int32_t masked =
            AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if (masked == AMOTION_EVENT_ACTION_HOVER_MOVE ||
            masked == AMOTION_EVENT_ACTION_MOVE) {
            const float x = AMotionEvent_getX(event, 0);
            const float y = AMotionEvent_getY(event, 0);
            emit(MouseMovedEvent{x, y});
            if (input != nullptr) {
                input->mouse_x = x;
                input->mouse_y = y;
                input->mouse_present = true;  // motion is presence
            }
        }
        // The cursor appearing over the window: the same tap as a move
        // (position + presence) but no MouseMovedEvent - the event stream
        // has no enter/leave, matching desktop's silent enter handling.
        if (masked == AMOTION_EVENT_ACTION_HOVER_ENTER && input != nullptr) {
            input->mouse_x = AMotionEvent_getX(event, 0);
            input->mouse_y = AMotionEvent_getY(event, 0);
            input->mouse_present = true;
        }
        // ...and leaving: the mouse is gone. Hover dies with the flag and
        // a press still held evaporates - the same semantics as desktop's
        // cursor-leave. Runs after the button diff above, so a leave that
        // also releases buttons ends absent, not present.
        if (masked == AMOTION_EVENT_ACTION_HOVER_EXIT && input != nullptr) {
            input->mouse_present = false;
        }
        if (masked == AMOTION_EVENT_ACTION_SCROLL) {
            const float x = AMotionEvent_getAxisValue(
                event, AMOTION_EVENT_AXIS_HSCROLL, 0);
            const float y = AMotionEvent_getAxisValue(
                event, AMOTION_EVENT_AXIS_VSCROLL, 0);
            emit(MouseScrolledEvent{x, y});
            if (input != nullptr) {
                // Deltas, not positions - sum, like every other scroll tap.
                input->scroll_dx += x;
                input->scroll_dy += y;
            }
        }
    }

    android_app* app_ = nullptr;
    WindowEventSink* sink_ = nullptr;
    bool window_ready_ = false;
    bool close_requested_ = false;
    float ui_scale_ = 1.0f;  // density / 160, cached at init (see init)
    int32_t mouse_buttons_ = 0;  // last seen AMOTION_EVENT_BUTTON_* mask
    bool touch_cap_warned_ = false;  // the touch-table cap warn fired once
};

}  // namespace

std::unique_ptr<WindowHost> make_window_host() {
    return std::make_unique<AndroidHost>();
}

}  // namespace ws::ui
