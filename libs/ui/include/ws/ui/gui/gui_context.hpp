#pragma once

#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/rect.hpp>

#include <array>
#include <map>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>

namespace ws::ui::gui {

// One widget's interaction state for a frame, from GuiContext::hit_test.
// clicked implies released; released also fires when the drag ended
// OUTSIDE the rect, so the widget that owned the drag learns it ended.
// The state flags fold across EVERY pointer that interacted with the
// widget this frame (mouse and touches alike - a two-finger press on one
// widget is one press), so a widget acting on `clicked` acts once;
// `pointer` names the pointer the fold came from - the lowest pointer
// slot with a claim (the mouse outranks touches, so the bridged finger
// resolves to the mouse it mirrors) - so a widget that tracks its drag
// follows its OWN finger, never the mouse. kNoPointer when nothing did.
struct HitTest {
    bool hovered = false;   // inside AND hot: the frame's interactive winner
                             // (while another widget owns a press, no other
                             // widget reports hover)
    bool pressed = false;   // active on this widget: the press started inside
    bool clicked = false;   // released inside while active = a full click
    bool released = false;  // a pointer went up this frame while this widget was active
    std::uint32_t pointer = kNoPointer;  // the pointer behind the fold
};

// The immediate-mode gui core. Owns, per frame: the id scope stack, the
// input snapshot, per-pointer hot/active tracking (the mouse and every
// touch, each finger an independent pointer), and per-widget persistent
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
    // snapshot) and rebuilds the per-pointer tracks off the frame's
    // pointer list - slot 0 the mouse, then the touches in first-contact
    // order. Each pointer matches last frame's track by id (active
    // carries - that carry IS the drag) or starts fresh; a track whose
    // pointer left the frame drops out. Hot re-claims every frame, on
    // every track, as widgets' hit tests run. Widget calls are legal
    // only between new_frame and end_frame.
    void new_frame(const InputFrame& input);
    // Closes the frame: drops a press whose owner vanished before its
    // release frame (the UI changed mid-drag), per pointer, so stale
    // ownership never blocks the next frame's hot claims - and a
    // cancelled gesture's flag lifts with its pointer, so the platform
    // may reuse the id for a fresh contact.
    void end_frame();

    // Hot/active hit-test for one widget's rect, across the frame's
    // pointers (mouse and touches; the primary-touch bridge makes the
    // first finger BOTH, a benign duplicate - one of them folds into
    // the other's click):
    //   a pointer pressing inside the rect makes the widget active for
    //   THAT pointer;
    //   release inside while active = clicked;
    //   release outside clears active without a click - but the formerly
    //   active widget still sees released, to end drags cleanly.
    // Capture is per pointer: a drag owned by one finger does not stop
    // another from hovering or pressing elsewhere. While a press is
    // owned, only its owner can be hot for that pointer - hovered means
    // hot, not merely inside. The folds are ORs across pointers, so
    // `clicked` fires once no matter how many fingers were on the
    // widget; `hit.pointer` names the lowest pointer slot behind the
    // fold. One widget must not hit-test twice in a frame: a repeated
    // id means two widgets share identity and will fight over hot and
    // the side tables (flagged once per process). `id` scopes the
    // widget's persistent state (kInvalidWidgetId claims nothing);
    // `rect` is in gui space.
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

    // --- hot/active inspection (drag math etc.), per pointer ---
    // The no-arg forms answer for the mouse - the pre-touch API, unchanged
    // in meaning. `pointer` is a mouse/touch pointer id as the frame sees
    // it (kMousePointerId for the mouse); an unknown id answers invalid.
    WidgetId hot_id(std::uint32_t pointer = kMousePointerId) const;
    WidgetId active_id(std::uint32_t pointer = kMousePointerId) const;
    // The pointer owning a press on `id` right now, kNoPointer when none.
    // Lowest slot wins - deterministic, and the mouse outranks touches, so
    // the bridged finger resolves to the mouse it mirrors.
    std::uint32_t owning_pointer(WidgetId id) const;
    // The frame's view of pointer `id` (the mouse or any touch), null when
    // no such pointer exists this frame. A widget that tracks its drag
    // follows its own pointer through this - never the raw mouse fields.
    const PointerView* pointer(std::uint32_t id) const;
    // Ends `pointer`'s press with no click - a gesture (a scroll drag)
    // consumed it. From this frame on the pointer claims neither hot nor
    // press until it lifts, so widgets it sweeps while dragging see no
    // hover and no press; on release nothing clicks. Cancelling a bridged
    // touch cancels the mouse it mirrors (and vice versa): the bridge is
    // one finger in two pointers, and a gesture that eats one eats both.
    void cancel_pointer(std::uint32_t pointer);

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

    // One pointer's hot/active pair, carried across frames keyed by the
    // pointer's id - that carry IS the drag. Slot 0 is the mouse by
    // construction (new_frame aligns tracks to the frame's pointers);
    // hot re-claims every frame, active survives while the pointer does.
    // `cancelled` marks a gesture-consumed press (cancel_pointer): the
    // pointer claims nothing until it lifts, and the flag lifts with it.
    struct PointerTrack {
        std::uint32_t id = kNoPointer;
        WidgetId hot = kInvalidWidgetId;
        WidgetId active = kInvalidWidgetId;
        bool cancelled = false;
    };
    std::array<PointerTrack, InputFrame::kMaxPointers> pointers_{};
    // The frame's pointers, aligned to pointers_ (slot i of both speaks
    // about the same pointer), and how many slots are live this frame.
    std::array<PointerView, InputFrame::kMaxPointers> frame_pointers_{};
    int pointer_count_ = 0;

    std::map<std::type_index, std::unique_ptr<StateTableBase>> state_tables_;
    std::unordered_set<WidgetId> hit_ids_;  // ids hit-tested this frame; a
                                            // repeat is an identity collision
};

}  // namespace ws::ui::gui
