#pragma once

#include <ws/input/input.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
// follows its OWN finger, never the mouse. ws::input::kNoPointer when nothing did.
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
    ws::input::PointerId pointer = ws::input::kNoPointer;  // the pointer behind the fold
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
    toast = 50,   // passive notifications above the modal: visible and
                  // clickable while a veil is up, blocking nothing
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
    void begin_frame(const ws::input::InputFrame& input);
    // Closes the frame: drops a press whose owner vanished before its
    // release frame (the UI changed mid-drag), per pointer, so stale
    // ownership never blocks the next frame's hot claims - and a
    // cancelled gesture's flag lifts with its pointer, so the platform
    // may reuse the id for a fresh contact. One release does not drop:
    // a same-frame id reuse whose new contact re-claimed the press
    // (hit_test's fingerprint is pressed+released with down) - the edge
    // belonged to the lift that reported it, and the new contact keeps
    // the claim it just took. Each pointer's resolved hot
    // becomes hot_prev: the settled hover next frame's hit tests report.
    // Tab moves queued this frame resolve here, against the frame's full
    // registration list; and a veil that lifts this frame hands the focus
    // back to what it displaced - but only on a quiet frame: a press or
    // a Tab move keeps the focus it earned.
    void end_frame();

    // --- layer scopes: one layer concept for hit tests AND drawing ---
    // begin_layer opens a stratum: until the matching end_layer, the
    // context hit-tests widgets at `layer`'s precedence and r2d draws
    // them at the same number - begin_layer drives r2d::set_layer, so
    // the batch's sort key and the hit rules can never disagree about
    // who is on top. Call it inside the r2d begin()/end() block, like
    // the widget draws. A strictly higher layer wins hot whatever the
    // record order, and steals a press claimed THIS frame by a lower one
    // (a click under a popup belongs to the popup). Within one layer
    // the LATER widget wins BOTH hot and a fresh press - painter order
    // is the topmost pixel, the one the user sees, so the widget that
    // shows hover is the one that clicks; hover and click must never
    // disagree on an overlap. A drag carried from an earlier frame is
    // capture: never stolen, not even by a modal veil opening
    // mid-drag.
    //
    // Layer::modal raises a VEIL, and the veil is frame-wide: it starts
    // at the first begin_layer(Layer::modal) of the frame, outlives the
    // scope's own end_layer, and lifts at the next begin_frame. While
    // it is up, every strictly lower stratum is blocked - no hover, no
    // hot claim, no new press (no click-through); the block reads
    // stratum VALUES, so the modal's own chrome (hit-testing inside the
    // modal scope, at the modal stratum) stays live. The veil owns the
    // keyboard too: register_focusable refuses widgets under it, which
    // IS the focus trap - Tab wraps within the modal's own
    // registrations and never escapes underneath - and a focused widget
    // under it drops the focus (state()'s rule). The veil's rise stashes
    // the focus it displaces, and the frame the veil lifts - a quiet
    // frame, no press and no nav - hands the focus back. The veil being
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
    // The overlay spelling, the twin of r2d::push_clip_escape: pushes
    // `clip` WITHOUT intersecting the enclosing hit clips, so an
    // overlay's children claim over a panel they float past instead of
    // being cut by its hole. Further pushes from inside intersect with
    // it as usual; pop_hit_clip closes it like any other. The popup and
    // modal scopes are the callers (the tooltip pushes no hit clip at
    // all - nothing in it is meant to claim) and a panel's own children
    // never want this.
    void push_hit_clip_escape(const Rect& clip);
    // Closes the innermost push_hit_clip. Drop-safe like every gui pop:
    // a call with nothing pushed is a no-op.
    void pop_hit_clip();
    // The innermost effective hit clip - ALREADY intersected with its
    // enclosing clips at push time - null while none is pushed
    // (everything is claimable). The observability spelling, for tests.
    const Rect* current_hit_clip() const;

    // --- gesture arbitration: the panel claims the scroll widgets gate on ---
    //
    // The WHEEL: every panel - plain or scrollable - registers its
    // bounds as a wheel claimant each frame; a scroll panel passes its
    // own key (the address of its ScrollState, stable for the entry's
    // lifetime), a plain panel passes null and merely eats. Claims
    // resolve at end_frame against the frame's whole table: a strictly
    // higher layer wins (a popup over a scroll panel eats the wheel
    // even though it scrolls nothing), ties go to the innermost rect
    // (the smallest bounds - nested panels), the last registrant wins
    // a dead tie. Claims from a stratum under a raised modal veil drop
    // out entirely: the veil kills the wheel for everything below it.
    // The winner publishes for the NEXT frame - the same one-frame
    // settling hot_prev uses, because a frame's claim table is only
    // complete at its end - so the first notch after the cursor
    // arrives over a panel settles rather than scrolls.
    void register_wheel_claim(const Rect& bounds, const void* key);
    // True when `key`'s panel won LAST frame's wheel resolution - the
    // settled owner whose scroll this frame consumes. A null key
    // answers false: keyless panels claim to eat, never to consume.
    bool wheel_won(const void* key) const;

    // --- Escape arbitration: one key, a stack of overlays ---
    // An overlay that closes on Escape claims it at its begin, and the
    // claim key is the ADDRESS of the caller's open flag, so end_frame
    // can pick ONE winner without knowing the overlay type: strictly
    // higher layer first (the modal over the popup), then the LAST
    // registrant at the winning layer (a nested submenu begins after
    // its parent, so it out-ranks it). The frame AFTER the Escape, the
    // winning overlay's own begin reads esc_won() and closes - the same
    // one-frame settle the wheel claim uses, because a frame's claim
    // table is only complete at its end. A claim from under a raised
    // modal veil never registers (the same gate hit_test reads), which
    // is how Esc reaches the modal first. The flag must live somewhere
    // frame-stable (a caller member, a persistent_state entry); a flag
    // that moves between frames just loses the win - esc_won compares
    // addresses and never dereferences.
    void claim_esc(bool* open);
    // True when THIS open flag won last frame's arbitration - the
    // winner's own begin closes on it (and a closed overlay claims
    // nothing, so the next Esc settles on what is left).
    bool esc_won(const bool* open) const;
    // Spends this frame's Escape before the arbitration resolves it: a
    // widget that ACTED on the edge (text_input dropping a selection)
    // closes nothing behind itself - one Escape abandons the edit, the
    // next one closes the overlay. A frame with no Escape pressed is
    // unaffected.
    void consume_escape();
    // Clears one key's pressed edge in the context's OWN input copy -
    // how an open menu owns its navigation cluster (the arrows, Enter,
    // the works) so the widget underneath never sees them. Widgets read
    // gui.input(), never the platform's frame, so this is the whole
    // story. Unknown keys are a no-op.
    void consume_key(int key);
    // The typing spelling of consume_key: clears the frame's text_input
    // codepoints (the vector keeps its capacity - no allocation), so an
    // open menu's mnemonic walk eats the frame's letters.
    void consume_typing();

    // The scroll ARM: a scroll panel watching a fresh touch press (its
    // drag-to-scroll about to cross the threshold) claims the pointer
    // here. Nested panels both see the press, and the claim keeps the
    // drag singular: a claim takes the pointer when nobody holds it or
    // when the challenger's layer is >= the holder's - record order
    // puts the innermost panel's begin after the outer's and a popup's
    // layer above both, so later-or-higher IS the innermost/topmost
    // panel under the finger. The claim lasts from the press until the
    // release; panels consult scroll_arm_owner() before a watched
    // track drags or sweeps - only the owner's track survives, the
    // losers drop theirs silently (their child keeps the tap in
    // progress, and the cancel they would have fired never does).
    void claim_scroll_arm(ws::input::PointerId pointer, const void* key);
    // The pointer's current arm holder - null when no panel claimed
    // it, and from the release frame on (the claim dies with the
    // press it was claimed for).
    const void* scroll_arm_owner(ws::input::PointerId pointer) const;

    // True when the innermost open layer scope may take new pointer
    // claims right now: false only under a raised modal veil from a
    // strictly lower stratum. Widgets that gate a gesture off the RAW
    // frame - the scroll panels' touch arm and thumb grab, which see
    // presses before hit_test could speak - consult this so the veil
    // blocks them exactly as it blocks hit_test's claims.
    bool claims_open() const;

    // Tab's scroll-into-view: end_frame's Tab resolution (and the
    // veil-lift focus restore, and set_focus) raise this for exactly
    // the frame the newly focused widget runs in, and the scroll panel
    // whose column contains the widget shifts its offset to pull it
    // into the viewport (end_scroll_panel's rule). The observability
    // spelling, for apps and tests.
    bool focus_into_view_pending() const;
    // The focused widget's drawn bounds, latched by its own state() as
    // it ran THIS frame - null when nothing holds the focus or the
    // focused widget has not run yet this frame. end_scroll_panel
    // reads this with focus_into_view_pending() for the into-view
    // shift.
    const Rect* focused_bounds() const;

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
    // hold what it cannot use). The ambient begin_disabled scope joins
    // the parameter - either locks the widget - and either way the id
    // skips the frame's duplicate census (it never reaches hit_test).
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
    // (a press's focus claim reads the registration). Three refusals:
    // kInvalidWidgetId (the sentinel is never focusable) and ids past
    // the registration cap drop with one warn per process; the other two
    // are silent because they are routine - a widget inside a
    // begin_disabled scope registers nothing while locked, and a widget
    // under a raised modal veil registers nothing at all. That veil
    // refusal IS the focus trap: Tab resolves against what did register,
    // so it wraps within the modal's own widgets and never escapes
    // underneath.
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

    // --- disabled scopes: one pair around a locked group ---
    // begin_disabled opens a scope in which every widget that runs
    // state() is locked: it claims nothing (hover, hot and press fall
    // through to whatever sits underneath - pass-through is the point),
    // it registers nothing focusable (Tab skips the scope), and it wears
    // the Disabled face. The condition rides the begin -
    // begin_disabled(!can_save) reads better than an if around the
    // pair - so the scope pushes EVEN when the condition is false; it
    // just does not lock anything. disabled() is the scope's answer:
    // true when ANY open scope's condition held.
    // The stack is shaped like the other gui stacks: fixed depth 32 with
    // the balanced clamp (a push past 32 keeps counting but stops
    // storing, warned once per process, so the matching end_disabled
    // never eats a real scope's bit), a pop with nothing open is a
    // no-op, and begin_frame resets a scope an early return never
    // closed. Layer scopes carve out chrome: begin_layer saves the
    // enclosing lock and RESETS it (a dialog opened from a locked form
    // is new chrome, not part of the lock), end_layer restores what the
    // enclosing scope had.
    void begin_disabled(bool condition = true);
    // Closes the innermost begin_disabled. Drop-safe like every gui pop:
    // a call with no open scope is a no-op.
    void end_disabled();
    // True while any open begin_disabled condition held. The
    // observability spelling, for apps and tests.
    bool disabled() const;

    // Persistent-but-implicit per-widget state (scroll offsets, drag
    // accumulators, toggles), keyed by (type, widget id) and owned by the
    // context - never function-local statics, so a future retained layer
    // could key the same way. First touch default-constructs the value;
    // entries outlive frames for the context's lifetime. Eviction stays
    // explicit by default (clear_persistent_state);
    // sweep_persistent_state is the opt-in garbage pass and
    // pin_persistent_state its exemption. Every touch stamps the entry
    // with the frame counter - the only thing the sweep reads.
    template <typename T>
    T& persistent_state(WidgetId id) {
        const std::type_index type = typeid(T);
        auto found = state_tables_.find(type);
        if (found == state_tables_.end()) {
            auto table = std::make_unique<StateTable<T>>();
            found = state_tables_.emplace(type, std::move(table)).first;
        }
        auto& entry = static_cast<StateTable<T>&>(*found->second).values[id];
        entry.last_frame = frame_stamp_;
        return entry.value;
    }

    // The EXPLICIT eviction of persistent state (there is deliberately
    // no automatic one: entries outlive frames and vanished widgets, so
    // a panel keeps its scroll place while it is gone - when the app
    // KNOWS a screen will never come back, it says so here). With no
    // argument every table empties - the "tear the screen down"
    // spelling; with an id, that widget's entries leave every table
    // and everything else stays. Call it BETWEEN frames: a reference a
    // widget holds into its entry (a scroll panel across its begin/end
    // pair) dangles once the entry is gone.
    void clear_persistent_state();
    void clear_persistent_state(WidgetId id);
    // The opt-in garbage pass: evicts every persistent-state entry no
    // widget touched in the last `max_unused_frames` frames, across all
    // tables, sparing pinned ids. Call it between frames (the same rule
    // clear_persistent_state lives under); an app that never calls it
    // keeps the explicit-eviction default, and one that calls it every
    // frame buys "state dies with the widget" at max_unused frames of
    // lag. Returns how many entries left.
    std::size_t sweep_persistent_state(std::uint32_t max_unused_frames);
    // Exempts `id` from the sweep - the rare entry that must outlive its
    // widget's absence. Pinning twice stores once; the cap is 64 ids with
    // the usual warn-once degrade.
    void pin_persistent_state(WidgetId id);
    // Lifts the exemption (a pin nobody set is a no-op).
    void unpin_persistent_state(WidgetId id);

    // --- the clipboard seam: the gui edits, the host moves the bytes ---
    // text_input's Ctrl+C/X/V ride this pair; the shell installs the
    // platform's implementation (App::install_clipboard does the wiring
    // on desktop - GLFW under the ws types, never named here). An
    // uninstalled seam is the headless answer: reads return null and
    // the copy/cut/paste shortcuts do nothing. Both halves speak
    // NUL-terminated UTF-8; `get`'s answer is platform-owned and valid
    // until the next clipboard call (null when the clipboard is empty -
    // glfwGetClipboardString's own contract). Deliberately raw pointers,
    // not strings: the editing path stays allocation-free while the
    // widgets read it - the one copy an explicit Ctrl+C makes is the
    // widget-side buffer it builds that frame.
    using ClipboardGet = std::function<const char*()>;
    using ClipboardSet = std::function<void(const char*)>;
    void set_clipboard(ClipboardGet get, ClipboardSet set);
    // The widget-side calls. A null seam answers null; a set with no
    // seam installed drops the text (nothing to drop it INTO).
    const char* get_clipboard_text() const;
    void set_clipboard_text(const char* text);

    // --- the pieces widget functions receive ---
    IdStack& ids() { return ids_; }
    const ws::input::InputFrame& input() const { return input_; }

    // --- hot/active inspection (drag math etc.), per pointer ---
    // The no-arg forms answer for the mouse - the pre-touch API, unchanged
    // in meaning, so an absent mouse (ws::input::InputFrame::mouse_present false)
    // answers invalid: no mouse, no mouse's claims. `pointer` is a
    // mouse/touch pointer id as the frame sees it (ws::input::kMousePointerId for the
    // mouse); an unknown id answers invalid.
    WidgetId hot_id(ws::input::PointerId pointer = ws::input::kMousePointerId) const;
    WidgetId active_id(ws::input::PointerId pointer = ws::input::kMousePointerId) const;
    // The am-I spelling of the two above: true when `id` is this
    // pointer's hot/active widget right now. kInvalidWidgetId is pinned
    // to false - "no widget" must never match, even though hot_id()
    // returns that very sentinel when nothing is hot; an unknown pointer
    // answers false the same way.
    bool is_hot(WidgetId id, ws::input::PointerId pointer = ws::input::kMousePointerId) const;
    bool is_active(WidgetId id, ws::input::PointerId pointer = ws::input::kMousePointerId) const;
    // The pointer owning a press on `id` right now, ws::input::kNoPointer when none.
    // Lowest slot wins - deterministic, and the mouse outranks touches, so
    // the bridged finger resolves to the mouse it mirrors.
    ws::input::PointerId owning_pointer(WidgetId id) const;
    // The frame's view of pointer `id` (the mouse or any touch), null when
    // no such pointer exists this frame. A widget that tracks its drag
    // follows its own pointer through this - never the raw mouse fields.
    const ws::input::PointerView* pointer(ws::input::PointerId id) const;
    // Ends `pointer`'s press with no click - a gesture (a scroll drag)
    // consumed it. From this frame on the pointer claims neither hot nor
    // press until it lifts, so widgets it sweeps while dragging see no
    // hover and no press; on release nothing clicks. Cancelling a bridged
    // touch cancels the mouse it mirrors (and vice versa): the bridge is
    // one finger in two pointers, and a gesture that eats one eats both.
    void cancel_pointer(ws::input::PointerId pointer);

private:
    // One side table per state type, created on first use and owned by
    // the context (type-erased so the context can hold them all without
    // knowing the widget set; the virtual destructor lets each map
    // destroy its own values).
    struct StateTableBase {
        virtual ~StateTableBase() = default;
        // The type-erased halves of clear_persistent_state: each table
        // empties itself / erases its id's entry.
        virtual void clear() = 0;
        virtual std::size_t erase(WidgetId id) = 0;
        // ...and of sweep_persistent_state: each table evicts its
        // untouched entries (last_frame older than the cutoff),
        // sparing the pinned ids handed in. The cutoff arithmetic is
        // the caller's, once, so the underflow guard lives there too.
        virtual std::size_t sweep(std::uint32_t cutoff, const WidgetId* pinned,
                                  std::size_t pinned_count) = 0;
    };
    template <typename T>
    struct StateTable : StateTableBase {
        // The value plus the sweep's clock: `last_frame` is the frame
        // stamp at the entry's last touch, stamped by every
        // persistent_state call.
        struct Entry {
            T value{};
            std::uint32_t last_frame = 0;
        };
        std::unordered_map<WidgetId, Entry> values;
        void clear() override { values.clear(); }
        std::size_t erase(WidgetId id) override { return values.erase(id); }
        std::size_t sweep(std::uint32_t cutoff, const WidgetId* pinned,
                          std::size_t pinned_count) override {
            std::size_t evicted = 0;
            for (auto it = values.begin(); it != values.end();) {
                bool is_pinned = false;
                for (std::size_t p = 0; p < pinned_count; ++p) {
                    if (pinned[p] == it->first) {
                        is_pinned = true;
                        break;
                    }
                }
                if (!is_pinned && it->second.last_frame < cutoff) {
                    it = values.erase(it);
                    ++evicted;
                } else {
                    ++it;
                }
            }
            return evicted;
        }
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
    ws::input::InputFrame input_;  // copied in by begin_frame - the frame's snapshot

    // The installed clipboard pair (unset until the shell wires one).
    ClipboardGet clipboard_get_;
    ClipboardSet clipboard_set_;

    // One pointer's hot/active pair, carried across frames keyed by the
    // pointer's id - that carry IS the drag. Slot 0 is the mouse by
    // construction (begin_frame aligns tracks to the frame's pointers);
    // hot re-claims every frame, active survives while the pointer does.
    // `cancelled` marks a gesture-consumed press (cancel_pointer): the
    // pointer claims nothing until it lifts, and the flag lifts with it.
    struct PointerTrack {
        ws::input::PointerId id = ws::input::kNoPointer;
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
        // The scroll-arm claim (claim_scroll_arm): the panel whose
        // touch-drag this pointer is, from the press until the
        // release. Rides inside the track so the claim carries with
        // the pointer - no separate lifecycle to keep honest.
        const void* scroll_arm = nullptr;
        std::uint32_t scroll_arm_layer = 0;  // the claiming panel's stratum
    };
    std::array<PointerTrack, ws::input::InputFrame::kMaxPointers> pointers_{};
    // The frame's pointers, aligned to pointers_ (slot i of both speaks
    // about the same pointer), and how many slots are live this frame.
    std::array<ws::input::PointerView, ws::input::InputFrame::kMaxPointers> frame_pointers_{};
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
    // The focus the modal veil displaced, stashed at the veil's GENUINE
    // rise - the frame it first appears; a continuing modal re-opens
    // its scope every frame and must not re-stash what the dialog's own
    // widgets focused - and handed back the quiet frame the veil lifts.
    WidgetId pre_modal_focus_ = kInvalidWidgetId;
    // Whether the veil was already up at begin_frame entry. The lift is
    // only confirmable at end_frame (up at entry, and nothing re-raised
    // it this frame); this is the "up at entry" half of that test.
    bool veil_upon_entry_ = false;

    std::map<std::type_index, std::unique_ptr<StateTableBase>> state_tables_;
    // The sweep's clock, bumped once per begin_frame. uint32 wraps after
    // ~2 years of 60 fps frames; a wrap reads as ancient stamps for one
    // window, keeping a few entries one sweep longer - benign.
    std::uint32_t frame_stamp_ = 0;
    // The sweep's exemptions, fixed like every gui table: 64 pins with
    // the warn-once degrade past the cap, deduplicated on insert.
    static constexpr int kMaxPinnedIds = 64;
    std::array<WidgetId, kMaxPinnedIds> pinned_ids_{};
    int pinned_count_ = 0;

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

    // --- disabled scopes (begin_disabled/end_disabled) ---
    // The open scopes' conditions as a bitmask: bit i is the i-th open
    // scope's condition (innermost in the low bit), disabled() is
    // mask != 0, and the OR falls out of the shifts - a false condition
    // pushes a 0 bit and locks nothing. Fixed depth with the balanced
    // clamp: past 32 open scopes the depth keeps counting but the mask
    // stops shifting (warned once per process), so the matching pops
    // skip their shift and never eat a real scope's bit. begin_frame
    // clears both, like every gui stack.
    static constexpr int kMaxDisabledDepth = 32;
    std::uint32_t disabled_mask_ = 0;
    int disabled_depth_ = 0;
    // Each layer scope's saved (mask, depth) pair: begin_layer stores
    // the enclosing lock and resets it (chrome is not part of the form
    // that locked), end_layer restores it. Indexed like the layer stack
    // - a scope overflowed past kMaxLayerDepth stored nothing and
    // restores nothing, matching the layer stack's own clamp.
    std::array<std::uint32_t, kMaxLayerDepth> layer_disabled_mask_{};
    std::array<int, kMaxLayerDepth> layer_disabled_depth_{};

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

    // --- wheel claims (register_wheel_claim / the end_frame resolution) ---
    // The frame's claimant table, fixed like every gui table: a frame
    // with more live panels than kMaxWheelClaims warns once per
    // process and the extras register nothing. begin_frame empties
    // the table; end_frame resolves it against the mouse (see the
    // public doc) and publishes the winner for the next frame to
    // consume through wheel_won().
    static constexpr int kMaxWheelClaims = 32;
    struct WheelClaim {
        Rect bounds{};
        std::uint32_t layer = 0;    // the claiming panel's stratum
        const void* key = nullptr;  // null: eats the wheel, consumes nothing
    };
    std::array<WheelClaim, kMaxWheelClaims> wheel_claims_{};
    int wheel_claim_count_ = 0;
    const void* wheel_winner_ = nullptr;  // last frame's resolved key

    // --- Escape claims (claim_esc / the end_frame resolution) ---
    // The frame's claim table, fixed like the wheel's: a frame with
    // more open overlays than kMaxEscClaims warns once per process and
    // the extras register nothing. begin_frame empties the table and
    // the winner; end_frame resolves it when the frame carried an
    // Escape no widget consumed.
    static constexpr int kMaxEscClaims = 8;
    struct EscClaim {
        bool* open = nullptr;     // the claim key: the caller's flag
        std::uint32_t layer = 0;  // the claiming overlay's stratum
    };
    std::array<EscClaim, kMaxEscClaims> esc_claims_{};
    int esc_claim_count_ = 0;
    const bool* esc_winner_ = nullptr;  // last frame's resolved claim key
    bool escape_consumed_ = false;  // a widget spent this frame's Escape

    // --- Tab-focus scroll-into-view (focused_bounds/focus_into_view) ---
    // The focused widget's bounds, latched by state() as the widget
    // ran; and the request flag's one-frame queue. end_frame's nav
    // landing (and the veil-restore hand-back) raise the NEXT flag -
    // the frame the newly focused widget runs and latches in - while
    // set_focus raises the live one directly (its call already sits
    // inside the frame). begin_frame shifts the queue, so a request is
    // readable for exactly one frame.
    Rect focused_rect_{};
    bool focused_rect_known_ = false;
    bool focus_into_view_ = false;
    bool focus_into_view_next_ = false;

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

// The gui's once-per-process warns (id/panel stack overflow, census
// duplicate/full, layer/clip/disabled overflow, focusable cap, wheel
// claim cap, Escape claim cap, state pin cap) fire the first time
// only. reset_warn_once() re-arms the
// lot - the test seam, so one test's overflow does not silence the
// next's warn path. App code has no reason to call it; it touches no
// frame state, so any moment is legal (between frames reads cleanest).
void reset_warn_once();

}  // namespace ws::ui::gui
