#pragma once

#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/rect.hpp>

namespace ws::ui::gui {

// One widget's interaction state for a frame, from GuiContext::hit_test.
// clicked implies released; released also fires when the drag ended
// OUTSIDE the rect, so the widget that owned the drag learns it ended.
struct HitTest {
    bool hovered = false;   // pointer is inside the rect this frame
    bool pressed = false;   // active on this widget: the press started inside
    bool clicked = false;   // released inside while active = a full click
    bool released = false;  // button went up this frame while this widget was active
};

// The immediate-mode gui core. Owns, per frame: the id scope stack, the
// input snapshot, hot/active widget tracking, and per-widget persistent
// state. Widgets themselves are free functions (Phase 5 of the
// buildplan) that receive a GuiContext and emit draws ONLY through r2d -
// never ImGui, GLFW or Vulkan.
//
// The layer is reached only through those widget functions by design:
// identity, hit-testing and draw emission stay one seam, so a future
// retained-mode tree could drive the same machinery without renderer
// changes (buildplan: design for that, do not build it). new_frame adopts
// the input snapshot; hit-testing and the hot/active state it resolves are
// stubs until Phase 3.
class GuiContext {
public:
    // Opens the frame: adopts `input` (the platform layer's filled
    // snapshot) and resets per-frame hover bookkeeping. Widget calls are
    // legal only between new_frame and end_frame.
    void new_frame(const InputFrame& input);
    // Closes the frame: resolves hot/active id transitions.
    void end_frame();

    // Hot/active hit-test for one widget's rect:
    //   mouse-down inside the rect makes the widget active;
    //   release inside while active = clicked;
    //   release outside clears active without a click - but the formerly
    //   active widget still sees released, to end drags cleanly.
    // `id` scopes the widget's persistent state; `rect` is in gui space.
    HitTest hit_test(WidgetId id, const Rect& rect);

    // Persistent-but-implicit per-widget state (scroll offsets, drag
    // accumulators, toggles), keyed by widget id and owned by the
    // context - never function-local statics, so a future retained layer
    // could key the same way. Declaration only until Phase 3 builds the
    // side table; do not call yet.
    template <typename T>
    T& persistent_state(WidgetId id);

    // --- the pieces widget functions receive ---
    IdStack& ids() { return ids_; }
    const InputFrame& input() const { return input_; }

    // --- hot/active inspection (drag math etc.) ---
    WidgetId hot_id() const { return hot_id_; }
    WidgetId active_id() const { return active_id_; }

private:
    IdStack ids_;
    InputFrame input_;  // copied in by new_frame - the frame's snapshot
    WidgetId hot_id_ = kInvalidWidgetId;     // widget under the pointer
    WidgetId active_id_ = kInvalidWidgetId;  // widget owning the press
};

}  // namespace ws::ui::gui
