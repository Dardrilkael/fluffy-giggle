#pragma once

#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/rect.hpp>

#include <map>
#include <memory>
#include <typeindex>
#include <unordered_map>

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
// changes (buildplan: design for that, do not build it).
class GuiContext {
public:
    // Opens the frame: adopts `input` (the platform layer's filled
    // snapshot) and resets per-frame hover bookkeeping - widgets re-claim
    // hot as their hit tests run. Widget calls are legal only between
    // new_frame and end_frame.
    void new_frame(const InputFrame& input);
    // Closes the frame: drops a press whose owner vanished before its
    // release frame (the UI changed mid-drag), so stale ownership never
    // blocks the next frame's hot claims.
    void end_frame();

    // Hot/active hit-test for one widget's rect, on the primary mouse
    // button (0):
    //   mouse-down inside the rect makes the widget active;
    //   release inside while active = clicked;
    //   release outside clears active without a click - but the formerly
    //   active widget still sees released, to end drags cleanly.
    // While a press is owned, only its owner can be hot - the pointer is
    // captured. `id` scopes the widget's persistent state
    // (kInvalidWidgetId claims nothing); `rect` is in gui space.
    HitTest hit_test(WidgetId id, const Rect& rect);

    // Persistent-but-implicit per-widget state (scroll offsets, drag
    // accumulators, toggles), keyed by (type, widget id) and owned by the
    // context - never function-local statics, so a future retained layer
    // could key the same way. First touch default-constructs the value;
    // entries outlive frames for the context's lifetime.
    template <typename T>
    T& persistent_state(WidgetId id) {
        const std::type_index type = typeid(T);
        auto found = state_tables_.find(type);
        if (found == state_tables_.end()) {
            auto table = std::make_unique<StateTable<T>>();
            found = state_tables_.emplace(type, std::move(table)).first;
        }
        return static_cast<StateTable<T>&>(*found->second).values[id];
    }

    // --- the pieces widget functions receive ---
    IdStack& ids() { return ids_; }
    const InputFrame& input() const { return input_; }

    // --- hot/active inspection (drag math etc.) ---
    WidgetId hot_id() const { return hot_id_; }
    WidgetId active_id() const { return active_id_; }

private:
    // One side table per state type, created on first use and owned by
    // the context (type-erased so the context can hold them all without
    // knowing the widget set; the virtual destructor lets each map
    // destroy its own values).
    struct StateTableBase {
        virtual ~StateTableBase() = default;
    };
    template <typename T>
    struct StateTable : StateTableBase {
        std::unordered_map<WidgetId, T> values;
    };

    IdStack ids_;
    InputFrame input_;  // copied in by new_frame - the frame's snapshot
    WidgetId hot_id_ = kInvalidWidgetId;     // widget under the pointer
    WidgetId active_id_ = kInvalidWidgetId;  // widget owning the press
    std::map<std::type_index, std::unique_ptr<StateTableBase>> state_tables_;
};

}  // namespace ws::ui::gui
