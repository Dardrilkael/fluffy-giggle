#pragma once

#include <cstdint>

namespace ws::input {

// Hazel-style event types. Key and mouse codes are plain ints (GLFW's
// numbering: mouse buttons are 0/1/2, printable keys match ASCII), so no
// GLFW header is ever needed to handle these.

enum class EventType : std::uint8_t {
    WindowClose,
    WindowResize,
    KeyPressed,
    KeyReleased,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,
    TouchPressed,
    TouchMoved,
    TouchReleased,
    TextInput,
};

// Base of every event. Deliberately has no vtable: each event stores its
// own EventType, and the dispatcher compares type() against T::static_type()
// instead of dispatching virtually. `handled` is public on purpose - the
// shell pre-marks events ImGui already consumed, and handlers flip it when
// they fully consume one.
class Event {
public:
    constexpr explicit Event(EventType type) : type_(type) {}

    constexpr EventType type() const { return type_; }

    bool handled = false;

private:
    EventType type_;
};

struct WindowCloseEvent : Event {
    static constexpr EventType static_type() { return EventType::WindowClose; }
    constexpr WindowCloseEvent() : Event(static_type()) {}
};

struct WindowResizeEvent : Event {
    static constexpr EventType static_type() { return EventType::WindowResize; }
    constexpr WindowResizeEvent(int width, int height)
        : Event(static_type()), width(width), height(height) {}
    int width = 0;   // screen coordinates, not framebuffer pixels
    int height = 0;
};

struct KeyPressedEvent : Event {
    static constexpr EventType static_type() { return EventType::KeyPressed; }
    constexpr KeyPressedEvent(int keycode, bool repeat)
        : Event(static_type()), keycode(keycode), repeat(repeat) {}
    int keycode = 0;
    bool repeat = false;
};

struct KeyReleasedEvent : Event {
    static constexpr EventType static_type() { return EventType::KeyReleased; }
    constexpr KeyReleasedEvent(int keycode)
        : Event(static_type()), keycode(keycode) {}
    int keycode = 0;
};

struct MouseButtonPressedEvent : Event {
    static constexpr EventType static_type() { return EventType::MouseButtonPressed; }
    constexpr explicit MouseButtonPressedEvent(int button)
        : Event(static_type()), button(button) {}
    int button = 0;
};

struct MouseButtonReleasedEvent : Event {
    static constexpr EventType static_type() { return EventType::MouseButtonReleased; }
    constexpr explicit MouseButtonReleasedEvent(int button)
        : Event(static_type()), button(button) {}
    int button = 0;
};

struct MouseMovedEvent : Event {
    static constexpr EventType static_type() { return EventType::MouseMoved; }
    constexpr MouseMovedEvent(float x, float y) : Event(static_type()), x(x), y(y) {}
    float x = 0.0f;  // window-relative cursor position
    float y = 0.0f;
};

struct MouseScrolledEvent : Event {
    static constexpr EventType static_type() { return EventType::MouseScrolled; }
    constexpr MouseScrolledEvent(float x_offset, float y_offset)
        : Event(static_type()), x_offset(x_offset), y_offset(y_offset) {}
    float x_offset = 0.0f;
    float y_offset = 0.0f;
};

// Touch contacts. `pointer_id` is the platform's contact id - stable for the
// lifetime of one finger-down (Android's pointer id; desktop synthesizes no
// touch events, so these only arrive from touch platforms). Coordinates are
// framebuffer pixels, top-left origin - the same space the mouse events and
// the gui use.
struct TouchPressedEvent : Event {
    static constexpr EventType static_type() { return EventType::TouchPressed; }
    constexpr TouchPressedEvent(std::uint32_t pointer_id, float x, float y)
        : Event(static_type()), pointer_id(pointer_id), x(x), y(y) {}
    std::uint32_t pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct TouchMovedEvent : Event {
    static constexpr EventType static_type() { return EventType::TouchMoved; }
    constexpr TouchMovedEvent(std::uint32_t pointer_id, float x, float y)
        : Event(static_type()), pointer_id(pointer_id), x(x), y(y) {}
    std::uint32_t pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct TouchReleasedEvent : Event {
    static constexpr EventType static_type() { return EventType::TouchReleased; }
    constexpr TouchReleasedEvent(std::uint32_t pointer_id, float x, float y,
                                bool cancelled)
        : Event(static_type()), pointer_id(pointer_id), x(x), y(y),
          cancelled(cancelled) {}
    std::uint32_t pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    // The OS aborted the gesture (a palm, an incoming call) - semantically
    // "never happened", never a click.
    bool cancelled = false;
};

// One typed glyph, UTF-32, one event per codepoint in arrival order.
// The event-system counterpart of the gui frame's text_input vector; keys
// model presses, this models what was actually typed.
struct TextInputEvent : Event {
    static constexpr EventType static_type() { return EventType::TextInput; }
    constexpr explicit TextInputEvent(std::uint32_t codepoint)
        : Event(static_type()), codepoint(codepoint) {}
    std::uint32_t codepoint = 0;
};

// Routes one event to typed handlers instead of a switch on type():
//
//     void on_event(ws::input::Event& e) override {
//         ws::input::EventDispatcher d{e};
//         d.dispatch<ws::input::WindowResizeEvent>(
//             [this](auto& ev) { return on_resize(ev); });
//     }
//
// `fn` takes T& and returns bool: true means fully consumed (the event is
// marked handled), false passes it on. Events already marked handled - by
// the shell when ImGui captured the input, or by an earlier handler - are
// skipped, so handlers rarely need to check the flag themselves.
class EventDispatcher {
public:
    explicit EventDispatcher(Event& event) : event_(event) {}

    template <typename T, typename Fn>
    bool dispatch(Fn&& fn) {
        if (event_.handled || event_.type() != T::static_type()) {
            return false;
        }
        event_.handled = fn(static_cast<T&>(event_));
        return event_.handled;
    }

private:
    Event& event_;
};

}  // namespace ws::input
