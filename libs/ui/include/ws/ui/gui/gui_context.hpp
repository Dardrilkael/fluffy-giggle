#pragma once

#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <typeindex>
#include <unordered_map>

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
    bool hovered = false;   // the SETTLED visual: this widget was last
                             // frame's final hot for one of the pointers and
                             // is still inside it now. Resolving hover from
                             // the previous frame's claims is what keeps an
                             // overlapped widget dark - a later, covering
                             // widget wins the claim and the covered one
                             // never lights up, not even on the covering
                             // frame; a fresh hover settles one frame later
                             // (one frame of settling at worst). The live
                             // this-frame claim stays visible through
                             // hot_id()/is_hot(). A cancelled or
                             // modal-blocked pointer shows no hover.
    bool pressed = false;   // active on this widget: the press started inside
    bool clicked = false;   // released inside while active = a full click
    bool released = false;  // a pointer went up this frame while this widget was active
    PointerId pointer = kNoPointer;  // the pointer behind the fold
};

// The gui's layer strata - the same numbers r2d::set_layer sorts by, so
// "a layer" means one thing to the hit rules and to draw order. World
// content stays below 10 (the convention the apps already follow), and
// the values are ordered, not dense: anything a future stratum needs
// above modal keeps working. begin_layer() opens a scope at one of
// these; hit_test() inside the scope claims at that precedence.
enum class Layer : std::uint32_t {
    base = 10,    // widgets without a begin_layer scope - the default
    popup = 20,   // floating chrome above base content
    tooltip = 30,  // hints above popups
    modal = 40,   // a blocking veil: strictly lower strata go inert
};

// The visual state state() derives for a widget off its HitTest - one
// enum for the faces a widget wears. Priority is transient-first:
// Disabled beats everything (a disabled widget claims nothing, so no
// other state can even arise), then Pressed, then Hovered; Focused is
// the RESTING keyboard state - a focused widget under the pointer
// reports Hovered and draws its focus ring off is_focused() instead.
// Normal is nothing doing anything.
enum class WidgetState : std::uint8_t {
    Normal,
    Hovered,
    Pressed,
    Focused,
    Disabled,
};

// state()'s answer: the hit-test it performed plus the state derived off
// it, both from ONE hit_test - the two can never disagree. A widget must
// not hit-test again after this; the id is spent for the frame.
struct WidgetResult {
    HitTest hit;
    WidgetState state = WidgetState::Normal;
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
    // carries - that carry IS the drag - and hot_prev carries with it:
    // that carry IS next frame's settled hover) or starts fresh; a track
    // whose pointer left the frame drops out. Hot re-claims every
    // frame, on every track, as widgets' hit tests run. The layer scopes
    // reset to base here, like every gui stack: a begin_layer an early
    // return never closed cannot stick its modal veil onto the next
    // frame. Widget calls are legal only between begin_frame and
    // end_frame.
    void begin_frame(const InputFrame& input);
    // Closes the frame: drops a press whose owner vanished before its
    // release frame (the UI changed mid-drag), per pointer, so stale
    // ownership never blocks the next frame's hot claims - and a
    // cancelled gesture's flag lifts with its pointer, so the platform
    // may reuse the id for a fresh contact. Each pointer's resolved hot
    // becomes hot_prev: the settled hover next frame's hit tests report.
    void end_frame();

    // --- layer scopes: one layer concept for hit tests AND drawing ---
    // begin_layer opens a stratum: until the matching end_layer, the
    // context hit-tests widgets at `layer`'s precedence and r2d draws
    // them at the same number - begin_layer drives r2d::set_layer, so
    // the batch's sort key and the hit rules can never disagree about
    // who is on top. Call it inside the r2d begin()/end() block, like
    // the widget draws. A strictly higher layer wins hot whatever the
    // record order, and steals a press claimed THIS frame by a lower one
    // (a click under a popup belongs to the popup); within one layer
    // nothing changes - later widgets win hot, the first hit-test wins
    // the press - and a drag carried from an earlier frame is capture:
    // never stolen, not even by a modal veil opening mid-drag.
    //
    // Layer::modal raises a VEIL, and the veil is frame-wide: it starts
    // at the first begin_layer(Layer::modal) of the frame, outlives the
    // scope's own end_layer, and lifts at the next begin_frame. While
    // it is up, every strictly lower stratum is blocked - no hover, no
    // hot claim, no new press (no click-through); the block reads
    // stratum VALUES, so the modal's own chrome (hit-testing inside the
    // modal scope, at the modal stratum) stays live. The veil being
    // frame-wide is what lets an app draw its chrome FIRST - the
    // stratum sorts it above base content wherever it records - while
    // the widgets it blocks still hit-test later in the frame; open the
    // modal scope before the widgets it blocks, and the block is
    // immediate, on the very frame the modal appears. end_layer closes
    // the stratum scope only (and restores the enclosing r2d layer);
    // the next begin_frame resets any scope left open AND lifts the
    // veil the frame's modal scopes raised.
    void begin_layer(Layer layer);
    // Closes the innermost open begin_layer. Drop-safe: a call with no
    // open scope is a no-op.
    void end_layer();
    // The innermost open scope (Layer::base when none is). The
    // observability spelling, for apps and tests.
    Layer current_layer() const;

    // --- hit clips: what the renderer's clip hides, the pointer cannot ---
    // reach. push_hit_clip narrows the region hit_test claims inside
    // until the matching pop_hit_clip - the hit-testing twin of
    // r2d::push_clip, so a widget hidden by a clip (a row scrolled
    // past a viewport) cannot claim hover, hot or a press from whatever
    // sits over or under the panel. Nesting intersects at push time,
    // exactly like r2d's stack; the panels push it at the same rect
    // they hand r2d::push_clip (begin_panel at the content rect,
    // begin_scroll_panel at the viewport), so the pixels and the
    // pointer see the same hole. A press already claimed is capture
    // and stays owned - the clip gates NEW claims, not in-flight
    // gestures: a press can start on a visible row and finish
    // scrolled out of sight, clicked only if it released inside the
    // claimable part. Frame-scoped and depth-capped like the other
    // gui stacks: a push past 8 keeps counting but stops shrinking
    // (warned once per process, so the matching pop never eats a real
    // scope), a pop with nothing pushed is a no-op, and begin_frame
    // resets any scope an early return never closed.
    void push_hit_clip(const Rect& clip);
    // Closes the innermost push_hit_clip. Drop-safe like every gui pop:
    // a call with nothing pushed is a no-op.
    void pop_hit_clip();
    // The innermost effective hit clip - ALREADY intersected with its
    // enclosing clips at push time - null while none is pushed
    // (everything is claimable). The observability spelling, for tests.
    const Rect* current_hit_clip() const;

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
    // owned, only its owner can be hot for that pointer - and hovered
    // is that hot, settled: it reports the pointer's LAST frame's final
    // hot, gated on this frame's inside-ness, so an overlapped widget
    // never lights up (one frame of settling at worst). The folds are
    // ORs across pointers, so `clicked` fires once no matter how many
    // fingers were on the widget; `hit.pointer` names the lowest pointer
    // slot behind the fold. The claims are layer-aware (begin_layer):
    // the widget hit-tests at the innermost open scope's stratum, and
    // the precedence rules of that scope apply (see begin_layer). One
    // widget must not hit-test twice in a frame: a repeated id means two
    // widgets share identity and will fight over hot and the side tables
    // (flagged once per process). `id` scopes the widget's persistent
    // state (kInvalidWidgetId claims nothing); `rect` is in gui space.
    HitTest hit_test(WidgetId id, const Rect& rect);

    // state() is the focus-and-face half over hit_test: it runs exactly
    // ONE hit_test (never a second path) and hands back its answer plus
    // the WidgetState derived off it. `disabled` short-circuits BEFORE
    // hit_test: a disabled widget claims nothing - no hover, no hot, no
    // press, so another widget under the same pointer may claim it - and
    // a focused widget that goes disabled drops the focus (it cannot
    // hold what it cannot use).
    //
    // Focus is the keyboard's hot - a third concept beside hot (under
    // the pointer) and active (being pressed): focused means RECEIVES
    // KEYBOARD INPUT. A FRESH press on a widget registered focusable
    // this frame takes the focus right there, at the press edge - the
    // standard focus-follows-the-press, so a press that slips off its
    // widget and never clicks still keeps the focus it took. A press
    // edge that lands on no focusable (empty space, a plain widget)
    // clears the focus at end_frame: clicking elsewhere means elsewhere.
    // A focused widget hit-testing under a raised modal veil drops the
    // focus the frame the veil rises - a modal blocks the keyboard like
    // it blocks the pointer. Otherwise focus persists while the widget
    // keeps running: it is an id, not a claim, and dies only by
    // press-elsewhere, veil, or set_focus.
    WidgetResult state(WidgetId id, const Rect& bounds, bool disabled = false);

    // --- keyboard focus ---
    // Registers `id` as focusable THIS frame, in call order - the frame's
    // Tab order. Widgets call it during the frame, BEFORE their state()
    // (a press's focus claim reads the registration). Ids past the
    // registration cap drop with one warn per process; kInvalidWidgetId
    // is refused (the sentinel is never focusable).
    void register_focusable(WidgetId id);
    // True when `id` holds the keyboard focus. kInvalidWidgetId is pinned
    // false, like is_hot/is_active.
    bool is_focused(WidgetId id) const;
    // Moves the keyboard focus to `id` - kInvalidWidgetId clears it. No
    // registration check: focus is just the id; the next press-elsewhere
    // or Tab move applies the rules.
    void set_focus(WidgetId id);
    // Queue a focus move to the next / previous focusable in this
    // frame's registration order (wrapping). Resolved at end_frame
    // against the full registration list - Tab and Shift+Tab queue the
    // same moves. Calls accumulate within a frame.
    void nav_next();
    void nav_previous();

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
    WidgetId hot_id(PointerId pointer = kMousePointerId) const;
    WidgetId active_id(PointerId pointer = kMousePointerId) const;
    // The am-I spelling of the two above: true when `id` is this
    // pointer's hot/active widget right now. kInvalidWidgetId is pinned
    // to false - "no widget" must never match, even though hot_id()
    // returns that very sentinel when nothing is hot; an unknown pointer
    // answers false the same way.
    bool is_hot(WidgetId id, PointerId pointer = kMousePointerId) const;
    bool is_active(WidgetId id, PointerId pointer = kMousePointerId) const;
    // The pointer owning a press on `id` right now, kNoPointer when none.
    // Lowest slot wins - deterministic, and the mouse outranks touches, so
    // the bridged finger resolves to the mouse it mirrors.
    PointerId owning_pointer(WidgetId id) const;
    // The frame's view of pointer `id` (the mouse or any touch), null when
    // no such pointer exists this frame. A widget that tracks its drag
    // follows its own pointer through this - never the raw mouse fields.
    const PointerView* pointer(PointerId id) const;
    // Ends `pointer`'s press with no click - a gesture (a scroll drag)
    // consumed it. From this frame on the pointer claims neither hot nor
    // press until it lifts, so widgets it sweeps while dragging see no
    // hover and no press; on release nothing clicks. Cancelling a bridged
    // touch cancels the mouse it mirrors (and vice versa): the bridge is
    // one finger in two pointers, and a gesture that eats one eats both.
    void cancel_pointer(PointerId pointer);

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

    // One pointer slot's half of hit_test; the public function folds the
    // slots into the widget-facing answer. Private on purpose: it
    // mutates the slot's track, so only hit_test may call it, in its
    // slot order (mouse first).
    HitTest hit_test_pointer(WidgetId id, const Rect& rect, int slot);
    // Whether `id` registered focusable this frame (the scan state()'s
    // press-to-focus reads). Registration is a handful of ids, so the
    // linear walk is the whole story.
    bool is_registered_focusable(WidgetId id) const;

    IdStack ids_;
    InputFrame input_;  // copied in by begin_frame - the frame's snapshot

    // One pointer's hot/active pair, carried across frames keyed by the
    // pointer's id - that carry IS the drag. Slot 0 is the mouse by
    // construction (begin_frame aligns tracks to the frame's pointers);
    // hot re-claims every frame, active survives while the pointer does.
    // `cancelled` marks a gesture-consumed press (cancel_pointer): the
    // pointer claims nothing until it lifts, and the flag lifts with it.
    struct PointerTrack {
        PointerId id = kNoPointer;
        WidgetId hot = kInvalidWidgetId;      // this frame's claim so far
        std::uint32_t hot_layer = 0;          // the claiming widget's stratum
        WidgetId hot_prev = kInvalidWidgetId;  // last frame's FINAL hot: the
                                                // settled hover this frame
                                                // reports
        WidgetId active = kInvalidWidgetId;   // the press owner (carries)
        std::uint32_t active_layer = 0;       // the stratum that claimed it
        bool active_fresh = false;  // claimed THIS frame: a strictly higher
                                    // layer may steal it; a carried drag is
                                    // capture and never stolen
        bool cancelled = false;
    };
    std::array<PointerTrack, InputFrame::kMaxPointers> pointers_{};
    // The frame's pointers, aligned to pointers_ (slot i of both speaks
    // about the same pointer), and how many slots are live this frame.
    std::array<PointerView, InputFrame::kMaxPointers> frame_pointers_{};
    int pointer_count_ = 0;

    // --- keyboard focus (state()/register_focusable) ---
    // The focused widget id - the keyboard's hot, beside the pointer's
    // hot/active above. Persists across frames (focus is state, not a
    // claim); moved by a press on a focusable, cleared by a press
    // elsewhere or a modal veil, cycled by Tab.
    WidgetId focused_ = kInvalidWidgetId;
    // This frame's press landed on a focusable (state() latches it at
    // the press edge) - end_frame's press-elsewhere clear holds its fire.
    bool press_focused_ = false;
    // Focus moves queued this frame (nav_next/nav_previous calls, plus
    // the Tab edges end_frame reads itself), resolved as one net move
    // against the frame's registrations at end_frame.
    int pending_nav_ = 0;
    // The frame's focusables in registration order - the Tab order.
    // Fixed like the hit-test census: registration is a handful of
    // pushes per frame, and the cap degrades loudly (a warn per process)
    // rather than allocating mid-frame.
    static constexpr int kMaxFocusables = 128;
    std::array<WidgetId, kMaxFocusables> focusables_{};
    int focusable_count_ = 0;

    std::map<std::type_index, std::unique_ptr<StateTableBase>> state_tables_;

    // --- layer scopes (begin_layer/end_layer) ---
    // Fixed depth with the IdStack's balanced clamp: a push past
    // kMaxLayerDepth keeps counting but stops storing (warned once per
    // process), so the matching end_layer never eats a real scope - the
    // overflowed scope keeps the deepest real stratum's value and adds
    // no modal effect of its own.
    static constexpr int kMaxLayerDepth = 8;
    std::array<Layer, kMaxLayerDepth> layer_stack_{};
    int layer_depth_ = 0;
    // The frame's modal veil: the stratum a begin_layer(Layer::modal)
    // raised this frame, 0 while none did. Deliberately NOT tied to the
    // scope's lifetime - the veil outlives end_layer and lifts at the
    // next begin_frame, so chrome-first drawing blocks the base widgets
    // that hit-test later in the same frame.
    std::uint32_t modal_layer_ = 0;
    // The innermost open scope's numeric stratum (base without a scope):
    // the value the hit rules and r2d::set_layer both speak.
    std::uint32_t current_layer_value() const;

    // --- hit clips (push_hit_clip/pop_hit_clip) ---
    // The hit-testing twin of r2d's clip stack, fixed depth with the
    // same balanced clamp as the layers: each entry is stored ALREADY
    // intersected with the clip below it (nesting intersects at push
    // time, like r2d), so the top entry IS the effective clip and
    // hit_test reads it without walking the stack. begin_frame resets
    // the depth, like every gui stack.
    static constexpr int kMaxHitClipDepth = 8;
    std::array<Rect, kMaxHitClipDepth> hit_clips_{};
    int hit_clip_depth_ = 0;

    // The per-frame hit-test census: which widget ids already hit-tested
    // this frame, as a fixed open-addressing table stamped by frame
    // generation. begin_frame clears it by bumping the stamp - no slot
    // is touched, so nothing allocates and the capacity never frees (the
    // unordered_set this replaces spent a node allocation per widget per
    // frame). A frame that fills every slot (512+ unique ids) warns once
    // per process and stops censusing ids past the table: duplicate
    // detection degrades, the frame runs on - growing would reintroduce
    // the allocation the table exists to avoid.
    struct HitCensus {
        static constexpr std::size_t kSlots = 512;  // power of two

        enum class Result { inserted, duplicate, full };
        // Records `id` as seen this frame. Valid only after begin_frame
        // bumped the stamp.
        Result note(WidgetId id);

        std::array<WidgetId, kSlots> ids{};
        std::array<std::uint32_t, kSlots> stamps{};  // != stamp: empty this frame
        std::uint32_t stamp = 0;  // this frame's generation; 0 is the "never"
                                  // sentinel, the first frame bumps to 1
    };
    HitCensus census_;
};

}  // namespace ws::ui::gui
