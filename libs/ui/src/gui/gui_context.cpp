// GuiContext's out-of-line half: per-pointer hot/active hit-testing,
// layer scopes and the persistent side tables. This TU's warn-once
// flags are local; reset_warn_once() re-arms them and the ones the
// other gui TUs own (gui.h's detail seam).

#include <ws/ui/gui/gui_context.hpp>

#include <ws/core/log.hpp>
#include <ws/ui/renderer2d.hpp>  // begin_layer drives r2d::set_layer - the
                                 // "one layer concept" is a real coupling
#include "gui/gui.h"  // detail::id/panel_overflow_warned - the reset seam

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>  // std::move - the clipboard pair's install

namespace ws::ui::gui {
namespace {

// A widget id hit-tested twice in one frame means two widgets share
// identity (same-label siblings without a push(index) between them).
// Flagged once per process; the duplicate is tolerated after.
bool duplicate_id_warned = false;

// The census ran out of slots (512+ unique ids hit-tested in one frame).
// Flagged once per process: ids already censused are still checked, new
// ones are not - the frame runs on either way.
bool census_full_warned = false;

// A begin_layer pushed past the clamp. Flagged once per process; the
// overflowed scope keeps the deepest real stratum's value.
bool layer_overflow_warned = false;

// register_focusable ran past kMaxFocusables. Flagged once per process:
// the ids past the cap are simply not focusable that frame.
bool focus_overflow_warned = false;

// A push_hit_clip pushed past the clamp. Flagged once per process; the
// overflowed scope keeps the deepest real clip's intersection.
bool hit_clip_overflow_warned = false;

// A begin_disabled pushed past the clamp. Flagged once per process; the
// overflowed scopes keep the innermost 32 conditions.
bool disabled_overflow_warned = false;

// register_wheel_claim ran past kMaxWheelClaims. Flagged once per
// process: the panels past the cap claim nothing that frame, so the
// wheel may resolve to a claimant under them.
bool wheel_overflow_warned = false;

// claim_esc ran past kMaxEscClaims. Flagged once per process: the
// overlays past the cap never close on Escape.
bool esc_overflow_warned = false;

// pin_persistent_state ran past kMaxPinnedIds. Flagged once per
// process: the ids past the cap sweep like everyone else.
bool pin_overflow_warned = false;

// The intersection of two axis-aligned rects, raw arithmetic: a
// negative extent is an empty overlap, and Rect's half-open contains
// already answers nothing for a backwards rect, so an empty
// intersection is inert with no normalization pass. Local to this TU
// on purpose - the stack stores the intersection at push time and the
// callers hand hit_test whole rects, so the public surface needs no
// intersect of its own.
Rect intersect_rects(const Rect& a, const Rect& b) {
    const float x = std::max(a.x, b.x);
    const float y = std::max(a.y, b.y);
    return {x, y, std::min(a.right(), b.right()) - x,
            std::min(a.bottom(), b.bottom()) - y};
}

// MurmurHash3's fmix64 finalizer: one round of mixing turns the FNV id's
// bits into a well-spread probe start for the census table.
std::uint64_t mix_id(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

}  // namespace

// --- GuiContext: per-pointer hot/active hit-testing + persistent side tables ------

void GuiContext::begin_frame(const ws::input::InputFrame& input) {
    input_ = input;
    // This frame's hit-test census starts empty: bumping the generation
    // makes every slot read "unused" without touching one. The wrap
    // (once per 2^32 frames) resets the stamps too, or a stamp of 1
    // would read as "this frame" against entries 2^32 frames old.
    if (++census_.stamp == 0) {
        census_.stamp = 1;
        census_.stamps.fill(0);
    }
    // Layer scopes are frame-scoped like every gui stack: a begin_layer
    // an early return never closed cannot stick its scope onto the next
    // frame - and the modal veil one raised lifts with it. The hit-clip
    // stack resets with them: a clip an early return never popped
    // cannot blank the next frame's claims. The veil's lift is only
    // confirmable at end_frame, so its up-ness at ENTRY is recorded
    // here, before the reset - up at entry plus nothing re-raised is
    // the lift end_frame restores the displaced focus on.
    veil_upon_entry_ = modal_layer_ != 0;
    layer_depth_ = 0;
    modal_layer_ = 0;
    hit_clip_depth_ = 0;
    // The disabled scope resets with the rest: an end_disabled an early
    // return never reached cannot lock the next frame's widgets.
    disabled_mask_ = 0;
    disabled_depth_ = 0;
    // The frame's focus bookkeeping resets; the focus ITSELF carries -
    // focus is state, not a claim. Registration starts empty (widgets
    // re-register as they run), no press has landed on a focusable yet,
    // and no nav move is queued.
    focusable_count_ = 0;
    press_focused_ = false;
    pending_nav_ = 0;
    // The wheel's claim table starts empty; last frame's winner stands
    // until this frame's end_frame re-resolves it. The focus-into-view
    // request shifts its one-frame queue (a request queued last
    // end_frame is readable now, and only now), and the focus-rect
    // latch is stale until the focused widget runs state().
    wheel_claim_count_ = 0;
    // The Escape claim table starts empty with it, and no widget has
    // spent this frame's Escape yet; end_frame re-publishes the winner
    // every frame, so a win nobody came back to read clears itself (a
    // stale winner must never close a freshly reopened overlay). The
    // state-sweep clock ticks once per frame - every persistent_state
    // touch stamps it, the sweep reads it.
    esc_claim_count_ = 0;
    escape_consumed_ = false;
    ++frame_stamp_;
    focus_into_view_ = focus_into_view_next_;
    focus_into_view_next_ = false;
    focused_rect_known_ = false;
    // Adopt the frame's pointers: the mouse leads while present, the
    // touches follow in first-contact order - an absent mouse (touch-only
    // device, cursor left the window) simply is not in the list. Each
    // pointer matches last frame's track by id - active carries, and that
    // carry IS the drag, and hot_prev carries with it: that carry IS next
    // frame's settled hover - or starts fresh; a track whose pointer left
    // the frame entirely (lifted last frame, mouse went absent) drops out
    // here. Hot re-claims every frame, on every track, as the widgets'
    // hit tests run.
    pointer_count_ = input_.gather_pointers(frame_pointers_);
    std::array<PointerTrack, ws::input::InputFrame::kMaxPointers> matched{};
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const ws::input::PointerId id = frame_pointers_[static_cast<std::size_t>(p)].id;
        for (int t = 0; t < ws::input::InputFrame::kMaxPointers; ++t) {
            if (pointers_[static_cast<std::size_t>(t)].id == id) {
                matched[p] = pointers_[static_cast<std::size_t>(t)];
                break;
            }
        }
        matched[p].id = id;
        matched[p].hot = kInvalidWidgetId;  // hot re-claims every frame
        matched[p].active_fresh = false;  // a carried drag is not fresh:
                                           // capture, never stolen
    }
    pointers_ = matched;
}

void GuiContext::end_frame() {
    // A release edge no hit_test consumed (the owner stopped running
    // mid-drag - the UI changed) ends the press here, per pointer; stale
    // ownership would block the next frame's hot claims. A cancelled
    // gesture lifts with its pointer - the id is free for the fresh
    // contact the platform may reuse it for. One exception: a same-frame
    // id reuse whose new contact already re-claimed - a fresh claim with
    // the pointer still down. The release edge belongs to the lift that
    // reported it; clearing the re-claim would leave the new contact
    // pressing nothing, so the gate reads released with !(fresh && down):
    // a tap lifts (down false) and still clears, a carried press is never
    // fresh and still clears, only the reuse's re-claim survives its own
    // release edge. Each pointer's resolved hot becomes hot_prev on the
    // way out: the settled hover next frame reports, straight from the
    // last thing the claims resolved to.
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        track.hot_prev = track.hot;
        if (frame_pointers_[static_cast<std::size_t>(p)].released &&
            !(track.active_fresh && frame_pointers_[static_cast<std::size_t>(p)].down)) {
            track.active = kInvalidWidgetId;
            track.cancelled = false;
            // The scroll arm dies with the press it was claimed for - a
            // same-frame id reuse's new contact re-claims at its own
            // panel's arm, exactly as it re-claims the press.
            track.scroll_arm = nullptr;
            track.scroll_arm_layer = 0;
        }
    }

    // Keyboard focus, pointer rule first so a stray press never overrides
    // a Tab in the same frame. A press edge this frame that did not land
    // on a focusable (state() latches the landing) is "clicking
    // elsewhere": the focus clears. The press EDGE decides, not the
    // completed click - a press on a focusable that slips off without
    // clicking keeps the focus it took at the edge.
    bool any_press = false;
    for (int p = 0; p < count; ++p) {
        any_press = any_press ||
                    frame_pointers_[static_cast<std::size_t>(p)].pressed;
    }
    if (any_press && !press_focused_) {
        focused_ = kInvalidWidgetId;
    }
    // Tab / Shift+Tab cycle this frame's registrations in order, wrapping.
    // nav_next()/nav_previous() queued the same moves during the frame;
    // resolving here, after every widget registered, is what makes the
    // order the whole frame's. The net move lands mod the list - stepping
    // one at a time and one net hop are the same landing.
    if (input_.key_pressed[static_cast<std::size_t>(ws::input::keys::kTab)]) {
        const bool shift =
            input_.key_down[static_cast<std::size_t>(ws::input::keys::kLeftShift)] ||
            input_.key_down[static_cast<std::size_t>(ws::input::keys::kRightShift)];
        pending_nav_ += shift ? -1 : 1;
    }
    if (pending_nav_ != 0 && focusable_count_ > 0) {
        const int n = focusable_count_;
        const WidgetId before = focused_;
        int idx = -1;
        for (int i = 0; i < n; ++i) {
            if (focusables_[static_cast<std::size_t>(i)] == focused_) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            // Focus not among this frame's registrations (nothing set,
            // cleared, or the widget did not register): forward restarts
            // at the first, backward at the last.
            idx = pending_nav_ > 0 ? n - 1 : 0;
        }
        idx = ((idx + pending_nav_) % n + n) % n;
        focused_ = focusables_[static_cast<std::size_t>(idx)];
        if (focused_ != before) {
            // The keyboard arrived somewhere new without the pointer -
            // queue the into-view request for the frame the widget runs
            // and latches its bounds in, where its scroll panel reads
            // it (end_scroll_panel's shift).
            focus_into_view_next_ = true;
        }
    }
    // The veil-lift restore: the veil was up at entry and nothing raised
    // one this frame, so this is the frame it lifted - hand the focus
    // back to what the rise displaced. A loud frame declines: a press
    // or a nav move earned the focus it found, and the restore must
    // not take it back (pending_nav_ is frame-reset, so != 0 here means
    // a move ran THIS frame). The restored focus may sit scrolled out
    // of view - it gets the same into-view request a Tab landing does.
    if (veil_upon_entry_ && modal_layer_ == 0 && !any_press &&
        pending_nav_ == 0) {
        focused_ = pre_modal_focus_;
        focus_into_view_next_ = true;
    }

    // The wheel's resolution, over the frame's whole claim table: the
    // mouse's claims compete layer-first (a popup eats the wheel even
    // when it scrolls nothing), then by innermost bounds (the smallest
    // area - nested panels), then by registration order (the later
    // panel is the one painted on top). Claims from a stratum under
    // the veil drop out first - a modal leaves the wheel dead below it
    // unless its own chrome registered a panel. The winner publishes
    // for the NEXT frame: a frame's claims are only complete here, so
    // the wheel consumes one frame behind the hover, the same settling
    // hot_prev already answers for.
    const void* wheel_winner = nullptr;
    if (input_.mouse_present) {
        bool have_winner = false;
        std::uint32_t winner_layer = 0;
        float winner_area = 0.0f;
        for (int i = 0; i < wheel_claim_count_; ++i) {
            const WheelClaim& claim = wheel_claims_[static_cast<std::size_t>(i)];
            if (claim.layer < modal_layer_) {
                continue;  // under the veil: no wheel below a modal
            }
            if (!claim.bounds.contains(input_.mouse_x, input_.mouse_y)) {
                continue;
            }
            const float area = claim.bounds.width * claim.bounds.height;
            if (!have_winner || claim.layer > winner_layer ||
                (claim.layer == winner_layer && area <= winner_area)) {
                have_winner = true;
                wheel_winner = claim.key;
                winner_layer = claim.layer;
                winner_area = area;
            }
        }
    }
    wheel_winner_ = wheel_winner;

    // The Escape resolution, the wheel's shape one stratum over: this
    // frame's Escape, when no widget consumed it, goes to exactly ONE
    // claimant - strictly higher layer first (the modal over the
    // popup), then the LAST registrant at the winning stratum (a
    // nested submenu begins after its parent, so it out-ranks it).
    // The winner publishes for the NEXT frame, whose winning overlay
    // closes at its own begin through esc_won() - the same one-frame
    // settle the wheel claims use. Every frame publishes, Escape or
    // not: a null publish clears a win the winner never came back to
    // read (its screen vanished mid-frame), so a stale winner can
    // never close a freshly reopened overlay.
    const bool* esc_winner = nullptr;
    std::uint32_t esc_winner_layer = 0;
    if (input_.key_pressed[static_cast<std::size_t>(ws::input::keys::kEscape)] &&
        !escape_consumed_) {
        for (int i = 0; i < esc_claim_count_; ++i) {
            const EscClaim& claim = esc_claims_[static_cast<std::size_t>(i)];
            if (esc_winner == nullptr || claim.layer >= esc_winner_layer) {
                esc_winner = claim.open;
                esc_winner_layer = claim.layer;
            }
        }
    }
    esc_winner_ = esc_winner;
}

void GuiContext::begin_layer(Layer layer) {
    // The balanced clamp, IdStack-style: a push past kMaxLayerDepth
    // keeps counting but stops storing, so the matching end_layer stays
    // balanced - the overflowed scope keeps the deepest real stratum's
    // value and raises no veil of its own.
    if (layer_depth_ < kMaxLayerDepth) {
        layer_stack_[static_cast<std::size_t>(layer_depth_)] = layer;
        // The scope's saved lock: end_layer hands this (mask, depth)
        // pair back, so the chrome a scope draws is never part of the
        // form that locked outside it.
        layer_disabled_mask_[static_cast<std::size_t>(layer_depth_)] =
            disabled_mask_;
        layer_disabled_depth_[static_cast<std::size_t>(layer_depth_)] =
            disabled_depth_;
    } else if (!layer_overflow_warned) {
        layer_overflow_warned = true;
        core::warn("ui: gui layer stack clamped at {} - deeper begin_layer "
                   "scopes keep the deepest real layer",
                   kMaxLayerDepth);
    }
    ++layer_depth_;
    if (layer == Layer::modal) {
        // The veil's GENUINE rise - modal_layer_ still 0 and the veil
        // was not up at entry - stashes the focus it is about to
        // displace. A continuing modal re-opens its scope every frame
        // and must not re-stash: the dialog's own focused widget would
        // clobber what sat under the veil.
        if (modal_layer_ == 0 && !veil_upon_entry_) {
            pre_modal_focus_ = focused_;
        }
        // The veil: frame-wide from this call, NOT the scope's lifetime -
        // it outlives the matching end_layer and lifts at the next
        // begin_frame. That is what lets chrome-first drawing block the
        // base widgets that hit-test later in the same frame.
        modal_layer_ = std::max(modal_layer_, static_cast<std::uint32_t>(layer));
    }
    // The lock belongs to the enclosing FORM, not the chrome a scope
    // draws: reset for the scope's widgets, end_layer restores.
    disabled_mask_ = 0;
    disabled_depth_ = 0;
    // The draw half of the concept: the same number that rules this
    // scope's hit tests sorts r2d's batch. Legal wherever set_layer is -
    // in practice inside the r2d begin()/end() block, like widget draws
    // (headless tests drop it with the usual once-per-process warn).
    r2d::set_layer(current_layer_value());
}

void GuiContext::end_layer() {
    // Drop-safe like every gui pop: an end_layer with no open scope is a
    // no-op. Closes the stratum scope ONLY - a modal veil, once raised,
    // is the frame's and lifts at the next begin_frame.
    if (layer_depth_ == 0) {
        return;
    }
    --layer_depth_;
    // Restore the enclosing scope's lock alongside its stratum: the
    // (mask, depth) pair begin_layer saved. A scope that overflowed the
    // clamp saved nothing and restores nothing - the layer stack's own
    // reading of the overflow.
    if (layer_depth_ < kMaxLayerDepth) {
        disabled_mask_ =
            layer_disabled_mask_[static_cast<std::size_t>(layer_depth_)];
        disabled_depth_ =
            layer_disabled_depth_[static_cast<std::size_t>(layer_depth_)];
    }
    // Restore the enclosing stratum's draw layer (the hit rules read the
    // stack directly; this is purely the r2d half).
    r2d::set_layer(current_layer_value());
}

Layer GuiContext::current_layer() const {
    if (layer_depth_ == 0) {
        return Layer::base;
    }
    const int top = std::min(layer_depth_, kMaxLayerDepth) - 1;
    return layer_stack_[static_cast<std::size_t>(top)];
}

std::uint32_t GuiContext::current_layer_value() const {
    return static_cast<std::uint32_t>(current_layer());
}

void GuiContext::push_hit_clip(const Rect& clip) {
    // The balanced clamp, layer-stack-style: a push past kMaxHitClipDepth
    // keeps counting but stops storing, so the matching pop_hit_clip
    // stays balanced and never eats a real scope - the overflowed
    // scopes keep the deepest real clip's intersection.
    if (hit_clip_depth_ < kMaxHitClipDepth) {
        // Nesting intersects at push time, exactly like r2d's clip
        // stack: the stored entry is already the effective clip, so
        // hit_test reads the top without walking the stack.
        const Rect effective =
            hit_clip_depth_ == 0
                ? clip
                : intersect_rects(clip, hit_clips_[static_cast<std::size_t>(
                                           hit_clip_depth_ - 1)]);
        hit_clips_[static_cast<std::size_t>(hit_clip_depth_)] = effective;
    } else if (!hit_clip_overflow_warned) {
        hit_clip_overflow_warned = true;
        core::warn("ui: gui hit-clip stack clamped at {} - deeper "
                   "push_hit_clip scopes keep the deepest real clip",
                   kMaxHitClipDepth);
    }
    ++hit_clip_depth_;
}

void GuiContext::push_hit_clip_escape(const Rect& clip) {
    // The same balanced clamp as push_hit_clip - the two share one
    // stack, one depth counter and one overflow warn; only the
    // intersection at push time is skipped: the stored entry is the
    // escape rect itself, exactly what r2d::push_clip_escape records on
    // its side, so the pixels and the pointer see the same hole.
    if (hit_clip_depth_ < kMaxHitClipDepth) {
        hit_clips_[static_cast<std::size_t>(hit_clip_depth_)] = clip;
    } else if (!hit_clip_overflow_warned) {
        hit_clip_overflow_warned = true;
        core::warn("ui: gui hit-clip stack clamped at {} - deeper "
                   "push_hit_clip scopes keep the deepest real clip",
                   kMaxHitClipDepth);
    }
    ++hit_clip_depth_;
}

void GuiContext::pop_hit_clip() {
    // Drop-safe like every gui pop: a pop with nothing pushed is a
    // no-op.
    if (hit_clip_depth_ == 0) {
        return;
    }
    --hit_clip_depth_;
}

const Rect* GuiContext::current_hit_clip() const {
    if (hit_clip_depth_ == 0) {
        return nullptr;
    }
    // The clamp stores only kMaxHitClipDepth entries however deep the
    // pushes went, so the top is the last STORED scope - current_layer's
    // reading of the stacks that overflow.
    const int top = std::min(hit_clip_depth_, kMaxHitClipDepth) - 1;
    return &hit_clips_[static_cast<std::size_t>(top)];
}

// --- gesture arbitration: wheel claims, the scroll arm, into-view ---

void GuiContext::register_wheel_claim(const Rect& bounds, const void* key) {
    if (wheel_claim_count_ >= kMaxWheelClaims) {
        if (!wheel_overflow_warned) {
            wheel_overflow_warned = true;
            core::warn("ui: gui wheel claim table clamped at {} - deeper "
                       "panels this frame claim nothing",
                       kMaxWheelClaims);
        }
        return;
    }
    WheelClaim& claim =
        wheel_claims_[static_cast<std::size_t>(wheel_claim_count_++)];
    claim.bounds = bounds;
    claim.layer = current_layer_value();
    claim.key = key;
}

bool GuiContext::wheel_won(const void* key) const {
    return key != nullptr && key == wheel_winner_;
}

void GuiContext::claim_scroll_arm(ws::input::PointerId pointer, const void* key) {
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        if (track.id != pointer) {
            continue;
        }
        // Later-or-higher wins: the innermost panel's begin runs after the
        // outer's and a popup's layer above both, so the >= rule hands the
        // arm to the panel the finger is actually on. A claim nobody
        // contests simply lands.
        if (track.scroll_arm == nullptr ||
            current_layer_value() >= track.scroll_arm_layer) {
            track.scroll_arm = key;
            track.scroll_arm_layer = current_layer_value();
        }
        return;
    }
}

const void* GuiContext::scroll_arm_owner(ws::input::PointerId pointer) const {
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        if (track.id == pointer) {
            return track.scroll_arm;
        }
    }
    return nullptr;
}

bool GuiContext::claims_open() const {
    return current_layer_value() >= modal_layer_;
}

void GuiContext::claim_esc(bool* open) {
    // The veil gate is the point: an overlay under a raised modal never
    // claims, so Esc always reaches the modal first - the same stratum
    // rule hit_test reads, applied to the keyboard. A null flag is a
    // host error, not an overlay; it claims nothing.
    if (open == nullptr || !claims_open()) {
        return;
    }
    if (esc_claim_count_ >= kMaxEscClaims) {
        if (!esc_overflow_warned) {
            esc_overflow_warned = true;
            core::warn("ui: gui Escape claim table clamped at {} - overlays "
                       "past the cap never close on Escape",
                       kMaxEscClaims);
        }
        return;
    }
    EscClaim& claim = esc_claims_[static_cast<std::size_t>(esc_claim_count_++)];
    claim.open = open;
    claim.layer = current_layer_value();
}

bool GuiContext::esc_won(const bool* open) const {
    // Address-only on purpose: a flag that moved between frames just
    // loses the win, and the winner's read is inert once it has closed
    // (a closed overlay's begin returns before this).
    return open != nullptr && open == esc_winner_;
}

void GuiContext::consume_escape() {
    // The frame's arbitration dies here; the key edge itself stays in
    // the snapshot for any widget still reading raw input this frame -
    // which is exactly why a widget that ACTED on the edge is the one
    // that must call this.
    escape_consumed_ = true;
}

void GuiContext::consume_key(int key) {
    // The context's OWN copy: begin_frame copies the snapshot in, so
    // clearing an edge here hides it from every later reader this
    // frame - how an open menu owns its navigation cluster. The
    // platform's frame is untouched.
    if (key >= 0 && key < ws::input::InputFrame::kMaxKeys) {
        input_.key_pressed[static_cast<std::size_t>(key)] = false;
    }
}

void GuiContext::consume_typing() {
    // The vector keeps its capacity - the clear is allocation-free, so
    // the steady-state rule holds on this path too.
    input_.text_input.clear();
}

bool GuiContext::focus_into_view_pending() const {
    return focus_into_view_;
}

const Rect* GuiContext::focused_bounds() const {
    if (!focused_rect_known_ || focused_ == kInvalidWidgetId) {
        return nullptr;
    }
    return &focused_rect_;
}

GuiContext::HitCensus::Result GuiContext::HitCensus::note(WidgetId id) {
    // Linear probing over the power-of-two table: the first slot whose
    // stamp is not this frame's is empty (open addressing with the
    // generation stamp doubles as the deletion story - a "removed" id is
    // simply one whose stamp went stale frames ago).
    const std::size_t mask = kSlots - 1;
    std::size_t at = static_cast<std::size_t>(mix_id(id.value)) & mask;
    for (std::size_t probe = 0; probe < kSlots; ++probe) {
        if (stamps[at] != stamp) {
            stamps[at] = stamp;
            ids[at] = id;
            return Result::inserted;
        }
        if (ids[at] == id) {
            return Result::duplicate;
        }
        at = (at + 1) & mask;
    }
    return Result::full;  // every slot holds some other id of this frame
}

void GuiContext::cancel_pointer(ws::input::PointerId pointer) {
    // The bridge mirrors the first finger into the mouse: a gesture that
    // consumes one consumes both, or the mirror would keep the child's
    // press alive and click it on release.
    ws::input::PointerId mirror = ws::input::kNoPointer;
    const ws::input::PointerId bridged = input_.bridged_touch();
    if (bridged != ws::input::kNoPointer &&
        (pointer == bridged || pointer == ws::input::kMousePointerId)) {
        mirror = (pointer == bridged) ? ws::input::kMousePointerId : bridged;
    }
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        if (track.id != pointer && track.id != mirror) {
            continue;
        }
        track.active = kInvalidWidgetId;  // no click on release
        track.cancelled = true;  // and no hot/press claims until lift
    }
}

void GuiContext::set_clipboard(ClipboardGet get, ClipboardSet set) {
    clipboard_get_ = std::move(get);
    clipboard_set_ = std::move(set);
}

const char* GuiContext::get_clipboard_text() const {
    // An unset seam IS an empty clipboard: the headless answer, and the
    // one a host with no clipboard wiring keeps.
    return clipboard_get_ ? clipboard_get_() : nullptr;
}

void GuiContext::set_clipboard_text(const char* text) {
    // A null text has nothing to copy; an unset seam has nowhere to put
    // it. Both drop quietly - the widget's shortcut is a no-op, exactly
    // what an uninstalled seam promises.
    if (clipboard_set_ && text != nullptr) {
        clipboard_set_(text);
    }
}

HitTest GuiContext::hit_test(WidgetId id, const Rect& rect) {
    // The invalid id claims nothing - it is the "no widget" sentinel.
    if (id == kInvalidWidgetId) {
        return {};
    }

    // One widget = one hit_test per frame. A repeat means two widgets
    // share identity (same-label siblings without a push(index) between
    // them) and will fight over hot and the side tables - flagged once
    // per process, tolerated after. A full census (512+ unique ids this
    // frame) stops censusing the ids past the table - flagged once per
    // process too; neither warn ever blocks the frame.
    const HitCensus::Result seen = census_.note(id);
    if (seen == HitCensus::Result::duplicate && !duplicate_id_warned) {
        duplicate_id_warned = true;
        core::warn("ui: gui widget id hit-tested twice in one frame - "
                   "same-label siblings need ids().push(index) between them");
    } else if (seen == HitCensus::Result::full && !census_full_warned) {
        census_full_warned = true;
        core::warn("ui: gui hit-test census full - more than {} widget ids "
                   "hit-tested this frame; ids past the census are no longer "
                   "checked for duplicates",
                   HitCensus::kSlots);
    }

    // The hit-clip twin of the renderer's clip: what the clip hides, the
    // pointer cannot reach - a row scrolled past a viewport is not
    // clickable from wherever it landed on screen. The intersection IS
    // the rect the claims run against (a half-visible widget claims its
    // visible part, a fully clipped one claims nothing), while the
    // active-owning branch in hit_test_pointer is untouched: a press
    // already claimed is capture, and the clip gates new claims only.
    const Rect* const hit_clip = current_hit_clip();
    const Rect claimable =
        hit_clip != nullptr ? intersect_rects(rect, *hit_clip) : rect;

    // The per-pointer rules live in hit_test_pointer; this fold is where
    // they become one widget-facing answer: ORs across pointers, so a
    // two-pointer press on one widget is one press while capture stays
    // per pointer (a drag owned by one finger does not stop another from
    // hovering or pressing elsewhere). Slot order is mouse first, then
    // touches in first-contact order; the first slot with a claim wins
    // `hit.pointer` (deterministic - the bridged finger resolves to the
    // mouse it mirrors).
    HitTest hit;
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const HitTest part = hit_test_pointer(id, claimable, p);
        hit.hovered = hit.hovered || part.hovered;
        hit.pressed = hit.pressed || part.pressed;
        hit.clicked = hit.clicked || part.clicked;
        hit.released = hit.released || part.released;
        if (hit.pointer == ws::input::kNoPointer) {
            hit.pointer = part.pointer;  // first slot with a claim sticks
        }
    }
    return hit;
}

WidgetResult GuiContext::state(WidgetId id, const Rect& bounds,
                               bool disabled) {
    WidgetResult result;
    if (disabled || disabled_mask_ != 0) {
        // The short-circuit sits BEFORE hit_test on purpose: claims are
        // exactly what a disabled widget must not make - no hover, no
        // hot, no press - so the widget under the same pointer is free
        // to claim it. It cannot hold the focus either: a widget that
        // cannot act cannot keep the keyboard. The ambient
        // begin_disabled scope joins the parameter here - either locks.
        if (focused_ == id) {
            focused_ = kInvalidWidgetId;
        }
        result.state = WidgetState::Disabled;
        return result;
    }
    result.hit = hit_test(id, bounds);
    // A raised veil blocks the keyboard like it blocks the pointer: the
    // focused widget under it drops the focus rather than keeping an
    // Enter that would click through the modal.
    if (is_focused(id) && modal_layer_ > 0 &&
        current_layer_value() < modal_layer_) {
        focused_ = kInvalidWidgetId;
    }
    // The focused widget's bounds, latched as the widget runs: the
    // scroll panel containing it reads them the same frame for the
    // Tab-focus into-view shift (focused_bounds).
    if (focused_ == id) {
        focused_rect_ = bounds;
        focused_rect_known_ = true;
    }
    // Press-to-focus: the press edge on a focusable that registered this
    // frame. hit.pressed holds for the whole drag, so the EDGE is the
    // claiming pointer's own `pressed` flag - reading the edge, not the
    // click, is what lets a press that slips off its widget keep the
    // focus it took.
    if (result.hit.pressed && result.hit.pointer != ws::input::kNoPointer) {
        const ws::input::PointerView* view = pointer(result.hit.pointer);
        if (view != nullptr && view->pressed &&
            is_registered_focusable(id)) {
            focused_ = id;
            press_focused_ = true;
        }
    }
    // The derived state, transient first: a pressed or hovered widget
    // wears that; focus is the resting look between pointer visits.
    if (result.hit.pressed) {
        result.state = WidgetState::Pressed;
    } else if (result.hit.hovered) {
        result.state = WidgetState::Hovered;
    } else if (is_focused(id)) {
        result.state = WidgetState::Focused;
    }
    return result;
}

// One pointer's whole say in hit_test: claims hot/active on its slot's
// track and reports what this slot saw. Only hit_test calls it, slot by
// slot in its order - the track is shared per pointer, so any other
// caller would be a call-order hazard. The claims are layer-aware: the
// widget's stratum is the innermost open begin_layer scope.
HitTest GuiContext::hit_test_pointer(WidgetId id, const Rect& rect,
                                     int slot) {
    const ws::input::PointerView& view = frame_pointers_[static_cast<std::size_t>(slot)];
    PointerTrack& track = pointers_[static_cast<std::size_t>(slot)];
    const bool inside = rect.contains(view.x, view.y);
    const std::uint32_t layer = current_layer_value();
    // The frame's modal veil blocks every strictly lower stratum
    // outright: no hover, no hot claim, no new press (no click-through).
    // Same-stratum and higher widgets stay live, and a press captured
    // before the veil rose is capture - the reporting path below is
    // ungated, so an in-flight drag finishes on its owner.
    const bool blocked = modal_layer_ > 0 && layer < modal_layer_;

    HitTest part;
    // The settled visual: this pointer's LAST frame's final hot, still
    // gated on this frame's inside-ness and the cancelled/modal veils.
    // Resolving from the previous frame is what keeps a widget covered
    // by a later one dark - the cover wins the claim and the covered
    // widget never reports hover, not even on the covering frame; a
    // fresh hover settles one frame later. The live claim stays visible
    // through hot_id()/is_hot().
    part.hovered = inside && !track.cancelled && !blocked &&
                   track.hot_prev == id;

    // The hot claim: capture as ever (only the press owner may be hot
    // while a press is owned), plus layer precedence - a strictly lower
    // stratum never unseats a higher one, whatever the record order;
    // draw order must not decide who wins the pointer. Within one
    // layer the later widget still wins, as before.
    if (inside && !track.cancelled && !blocked &&
        (track.active == kInvalidWidgetId || track.active == id) &&
        (track.hot == kInvalidWidgetId || layer >= track.hot_layer)) {
        track.hot = id;
        track.hot_layer = layer;
    }

    // The press claim: within one stratum the LATER widget steals a
    // press claimed THIS frame - painter order is the topmost pixel, the
    // one the user sees, so the widget that shows hover is the one that
    // clicks (the hot claim above gives the same stratum the same
    // rule); a strictly higher layer steals from a lower one whatever
    // the record order - a click under a popup belongs to the popup.
    // `active_fresh` marks the this-frame claim: a drag carried from an
    // earlier frame is capture and is never stolen, not even by a modal
    // opening mid-drag. The steal takes hot with it when the stratum's
    // precedence allows - the press and its hover belong to the same
    // widget from the moment the claim moves (the hot-claim block above
    // already ran, blocked by the victim's then-ownership).
    if (inside && view.pressed && !track.cancelled && !blocked) {
        if (track.active == kInvalidWidgetId) {
            track.active = id;
            track.active_layer = layer;
            track.active_fresh = true;
        } else if (track.active != id && track.active_fresh &&
                   layer >= track.active_layer) {
            track.active = id;
            track.active_layer = layer;
            if (layer >= track.hot_layer) {
                track.hot = id;
                track.hot_layer = layer;
            }
        }
    }

    if (track.active == id) {
        part.pressed = true;  // owns a live press, wherever the pointer is
        part.pointer = view.id;  // this slot's claim; the fold keeps the
                                // first slot that has one
        // The release edge is the end of THIS press only when the press
        // began before it: a carried press (not fresh) always ends here,
        // and a fresh claim that lifts (down false) is a tap - claimed and
        // clicked within one frame, reported as always. But a fresh claim
        // still holding (fresh AND down) is a same-frame id reuse claimed
        // through the press-claim block above - the release edge belongs
        // to the OLD lifetime the platform recycled this id from, and
        // must not read as this claim's click. Its owner reports it (a
        // carried active, below the claim) or nobody does (end_frame's
        // unconsumed-release clear).
        if (view.released && !(track.active_fresh && view.down)) {
            part.released = true;   // the drag ended, inside or out
            part.clicked = inside;  // full click only if inside
            track.active = kInvalidWidgetId;  // the press is over either way
        }
    }
    // A same-frame id reuse on the widget that OWNED the lifted contact:
    // the platform lifted a finger and handed its id to a new contact
    // before next_frame() (Android recycles low ids fast), so the pointer
    // reads pressed+released with down - the lift's edge still on the
    // slot, the new contact's press edge with it. That triple is the
    // reuse fingerprint: a tap lifts (down false), a carried press has
    // no edges, nothing else reads all three. The release above reported
    // the old lifetime on the carried active it just cleared - and the
    // press-claim block cannot take the new contact's press, it ran
    // before the release freed the claim, inside this same call. (A
    // reuse landing elsewhere needs no help: the old owner's call freed
    // the claim first, so the landing widget's own press-claim block
    // takes it, and the guard above keeps the lift's edge off it.)
    // Re-made here, fresh like any this-frame press: a later widget at a
    // sufficient layer steals it (the topmost pixel clicks), while the
    // old drag's capture protection died with its release. The click
    // verdict above used this frame's final position - the new contact's
    // - so a lift and a landing on opposite sides of this rect fold to
    // one verdict; that is the frame snapshot's rule, the mouse's own.
    if (view.pressed && view.released && view.down && inside &&
        !track.cancelled && !blocked && track.active == kInvalidWidgetId) {
        track.active = id;
        track.active_layer = layer;
        track.active_fresh = true;
    }
    return part;
}

WidgetId GuiContext::hot_id(ws::input::PointerId pointer) const {
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].hot;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

WidgetId GuiContext::active_id(ws::input::PointerId pointer) const {
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].active;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

bool GuiContext::is_hot(WidgetId id, ws::input::PointerId pointer) const {
    // kInvalidWidgetId is "no widget" - it must never match, even though
    // hot_id() returns it for an idle or unknown pointer.
    return id != kInvalidWidgetId && hot_id(pointer) == id;
}

bool GuiContext::is_active(WidgetId id, ws::input::PointerId pointer) const {
    return id != kInvalidWidgetId && active_id(pointer) == id;
}

ws::input::PointerId GuiContext::owning_pointer(WidgetId id) const {
    // Lowest live slot whose press is on `id` - with the bridge's mouse
    // mirroring the first finger, that means the mouse slot wins ties
    // (the documented duplicate), deterministically.
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].active == id) {
            return pointers_[static_cast<std::size_t>(p)].id;
        }
    }
    return ws::input::kNoPointer;
}

const ws::input::PointerView* GuiContext::pointer(ws::input::PointerId id) const {
    const int count = std::min(pointer_count_, ws::input::InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (frame_pointers_[static_cast<std::size_t>(p)].id == id) {
            return &frame_pointers_[static_cast<std::size_t>(p)];
        }
    }
    return nullptr;  // no such pointer this frame
}

void GuiContext::register_focusable(WidgetId id) {
    if (id == kInvalidWidgetId) {
        return;  // the "no widget" sentinel is never focusable
    }
    // Silent refusals, both routine: a widget inside a begin_disabled
    // scope registers nothing while locked (Tab skips the scope), and
    // a widget under the veil registers nothing at all - that refusal
    // IS the focus trap: Tab resolves against what did register, so it
    // wraps within the modal's own widgets and never escapes
    // underneath. A veiled base widget legitimately attempts every
    // frame, so neither path warns.
    if (disabled_mask_ != 0) {
        return;
    }
    if (modal_layer_ > 0 && current_layer_value() < modal_layer_) {
        return;
    }
    if (focusable_count_ >= kMaxFocusables) {
        if (!focus_overflow_warned) {
            focus_overflow_warned = true;
            core::warn("ui: gui focusable registration clamped at {} - "
                       "widgets past the cap are not focusable this frame",
                       kMaxFocusables);
        }
        return;
    }
    focusables_[static_cast<std::size_t>(focusable_count_++)] = id;
}

bool GuiContext::is_registered_focusable(WidgetId id) const {
    for (int i = 0; i < focusable_count_; ++i) {
        if (focusables_[static_cast<std::size_t>(i)] == id) {
            return true;
        }
    }
    return false;
}

bool GuiContext::is_focused(WidgetId id) const {
    // Pinned like is_hot/is_active: "no widget" never matches, even
    // though focused_ holds that very sentinel while nothing is focused.
    return id != kInvalidWidgetId && focused_ == id;
}

void GuiContext::set_focus(WidgetId id) {
    focused_ = id;  // kInvalidWidgetId is the clear
}

void GuiContext::nav_next() {
    ++pending_nav_;  // resolved at end_frame, with the Tab moves
}

void GuiContext::nav_previous() {
    --pending_nav_;
}

void GuiContext::begin_disabled(bool condition) {
    // The balanced clamp like the other gui stacks: a push past
    // kMaxDisabledDepth keeps counting but stops shifting, so the
    // matching end_disabled skips its shift and never eats a real
    // scope's bit. A false condition pushes a 0 bit - the scope exists
    // (its end stays balanced) but locks nothing.
    if (disabled_depth_ < kMaxDisabledDepth) {
        disabled_mask_ = (disabled_mask_ << 1) | (condition ? 1u : 0u);
    } else if (!disabled_overflow_warned) {
        disabled_overflow_warned = true;
        core::warn("ui: gui disabled stack clamped at {} - deeper "
                   "begin_disabled scopes keep the innermost {} conditions",
                   kMaxDisabledDepth, kMaxDisabledDepth);
    }
    ++disabled_depth_;
}

void GuiContext::end_disabled() {
    // Drop-safe like every gui pop: a call with no open scope is a no-op.
    if (disabled_depth_ == 0) {
        return;
    }
    if (disabled_depth_ > kMaxDisabledDepth) {
        --disabled_depth_;  // overflowed: this scope's bit was never stored
    } else {
        disabled_mask_ >>= 1;
        --disabled_depth_;
    }
}

bool GuiContext::disabled() const {
    return disabled_mask_ != 0;  // the OR of the open scopes' conditions
}

void GuiContext::clear_persistent_state() {
    // Every table, every entry: the "tear the screen down" spelling.
    // The (void) names the unused key half so the structured binding
    // stays honest.
    for (auto& [type, table] : state_tables_) {
        (void)type;
        table->clear();
    }
}

void GuiContext::clear_persistent_state(WidgetId id) {
    // One widget's entries out of every table; everything else keeps
    // its place. Between frames only - the header documents the
    // dangling-reference hazard a mid-panel call would create.
    for (auto& [type, table] : state_tables_) {
        (void)type;
        table->erase(id);
    }
}

std::size_t GuiContext::sweep_persistent_state(std::uint32_t max_unused_frames) {
    // The underflow guard lives here, once, not per table: a window
    // wider than the frames lived so far sweeps from 0, which only
    // makes a young context's first sweeps keep everything - the
    // conservative side of the edge.
    const std::uint32_t cutoff = frame_stamp_ > max_unused_frames
                                     ? frame_stamp_ - max_unused_frames
                                     : 0;
    std::size_t evicted = 0;
    for (auto& [type, table] : state_tables_) {
        (void)type;
        evicted += table->sweep(cutoff, pinned_ids_.data(),
                                static_cast<std::size_t>(pinned_count_));
    }
    return evicted;
}

void GuiContext::pin_persistent_state(WidgetId id) {
    if (id == kInvalidWidgetId) {
        return;  // the sentinel pins nothing, like it registers nothing
    }
    for (int i = 0; i < pinned_count_; ++i) {
        if (pinned_ids_[static_cast<std::size_t>(i)] == id) {
            return;  // already pinned: storing twice buys nothing
        }
    }
    if (pinned_count_ >= kMaxPinnedIds) {
        if (!pin_overflow_warned) {
            pin_overflow_warned = true;
            core::warn("ui: gui state pin table clamped at {} - ids past "
                       "the cap sweep like everyone else",
                       kMaxPinnedIds);
        }
        return;
    }
    pinned_ids_[static_cast<std::size_t>(pinned_count_++)] = id;
}

void GuiContext::unpin_persistent_state(WidgetId id) {
    // Swap-with-last: pin order carries no meaning, and the removal
    // must not shift the live prefix the sweep reads.
    for (int i = 0; i < pinned_count_; ++i) {
        if (pinned_ids_[static_cast<std::size_t>(i)] == id) {
            pinned_ids_[static_cast<std::size_t>(i)] =
                pinned_ids_[static_cast<std::size_t>(pinned_count_ - 1)];
            --pinned_count_;
            return;
        }
    }
}

void reset_warn_once() {
    // The test seam: the once-per-process warns re-arm. This TU's
    // nine plus the ones the other gui TUs own (gui.h's detail seam);
    // no frame state is involved, so any moment is legal.
    duplicate_id_warned = false;
    census_full_warned = false;
    layer_overflow_warned = false;
    focus_overflow_warned = false;
    hit_clip_overflow_warned = false;
    disabled_overflow_warned = false;
    wheel_overflow_warned = false;
    esc_overflow_warned = false;
    pin_overflow_warned = false;
    detail::id_overflow_warned = false;
    detail::panel_overflow_warned = false;
    detail::toast_overflow_warned = false;
    detail::menu_overflow_warned = false;
}

}  // namespace ws::ui::gui
