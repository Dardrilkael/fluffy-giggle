// GuiContext's out-of-line half: per-pointer hot/active hit-testing,
// layer scopes and the persistent side tables. The warn-once flags are
// local to this TU.

#include <ws/ui/gui/gui_context.hpp>

#include <ws/core/log.hpp>
#include <ws/ui/renderer2d.hpp>  // begin_layer drives r2d::set_layer - the
                                 // "one layer concept" is a real coupling

#include <algorithm>
#include <array>
#include <cstdint>

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

void GuiContext::begin_frame(const InputFrame& input) {
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
    // frame - and the modal veil one raised lifts with it.
    layer_depth_ = 0;
    modal_layer_ = 0;
    // Adopt the frame's pointers: slot 0 is the mouse by construction,
    // the touches follow in first-contact order. Each pointer matches
    // last frame's track by id - active carries, and that carry IS the
    // drag, and hot_prev carries with it: that carry IS next frame's
    // settled hover - or starts fresh; a track whose pointer left the
    // frame entirely (lifted last frame) drops out here. Hot re-claims
    // every frame, on every track, as the widgets' hit tests run.
    pointer_count_ = input_.gather_pointers(frame_pointers_);
    std::array<PointerTrack, InputFrame::kMaxPointers> matched{};
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const PointerId id = frame_pointers_[static_cast<std::size_t>(p)].id;
        for (int t = 0; t < InputFrame::kMaxPointers; ++t) {
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
    // contact the platform may reuse it for. Each pointer's resolved hot
    // becomes hot_prev on the way out: the settled hover next frame
    // reports, straight from the last thing the claims resolved to.
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        track.hot_prev = track.hot;
        if (frame_pointers_[static_cast<std::size_t>(p)].released) {
            track.active = kInvalidWidgetId;
            track.cancelled = false;
        }
    }
}

void GuiContext::begin_layer(Layer layer) {
    // The balanced clamp, IdStack-style: a push past kMaxLayerDepth
    // keeps counting but stops storing, so the matching end_layer stays
    // balanced - the overflowed scope keeps the deepest real stratum's
    // value and raises no veil of its own.
    if (layer_depth_ < kMaxLayerDepth) {
        layer_stack_[static_cast<std::size_t>(layer_depth_)] = layer;
    } else if (!layer_overflow_warned) {
        layer_overflow_warned = true;
        core::warn("ui: gui layer stack clamped at 8 - deeper begin_layer "
                   "scopes keep the deepest real layer");
    }
    ++layer_depth_;
    if (layer == Layer::modal) {
        // The veil: frame-wide from this call, NOT the scope's lifetime -
        // it outlives the matching end_layer and lifts at the next
        // begin_frame. That is what lets chrome-first drawing block the
        // base widgets that hit-test later in the same frame.
        modal_layer_ = std::max(modal_layer_, static_cast<std::uint32_t>(layer));
    }
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

void GuiContext::cancel_pointer(PointerId pointer) {
    // The bridge mirrors the first finger into the mouse: a gesture that
    // consumes one consumes both, or the mirror would keep the child's
    // press alive and click it on release.
    PointerId mirror = kNoPointer;
    const PointerId bridged = input_.bridged_touch();
    if (bridged != kNoPointer &&
        (pointer == bridged || pointer == kMousePointerId)) {
        mirror = (pointer == bridged) ? kMousePointerId : bridged;
    }
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        if (track.id != pointer && track.id != mirror) {
            continue;
        }
        track.active = kInvalidWidgetId;  // no click on release
        track.cancelled = true;  // and no hot/press claims until lift
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
        core::warn("ui: gui hit-test census full - more than 512 widget ids "
                   "hit-tested this frame; ids past the census are no longer "
                   "checked for duplicates");
    }

    // The per-pointer rules live in hit_test_pointer; this fold is where
    // they become one widget-facing answer: ORs across pointers, so a
    // two-pointer press on one widget is one press while capture stays
    // per pointer (a drag owned by one finger does not stop another from
    // hovering or pressing elsewhere). Slot order is mouse first, then
    // touches in first-contact order; the first slot with a claim wins
    // `hit.pointer` (deterministic - the bridged finger resolves to the
    // mouse it mirrors).
    HitTest hit;
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const HitTest part = hit_test_pointer(id, rect, p);
        hit.hovered = hit.hovered || part.hovered;
        hit.pressed = hit.pressed || part.pressed;
        hit.clicked = hit.clicked || part.clicked;
        hit.released = hit.released || part.released;
        if (hit.pointer == kNoPointer) {
            hit.pointer = part.pointer;  // first slot with a claim sticks
        }
    }
    return hit;
}

// One pointer's whole say in hit_test: claims hot/active on its slot's
// track and reports what this slot saw. Only hit_test calls it, slot by
// slot in its order - the track is shared per pointer, so any other
// caller would be a call-order hazard. The claims are layer-aware: the
// widget's stratum is the innermost open begin_layer scope.
HitTest GuiContext::hit_test_pointer(WidgetId id, const Rect& rect,
                                     int slot) {
    const PointerView& view = frame_pointers_[static_cast<std::size_t>(slot)];
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

    // The press claim: the first hit-test wins within a stratum, but a
    // strictly higher layer steals a press claimed THIS frame - a click
    // under a popup belongs to the popup. `active_fresh` marks the
    // this-frame claim: a drag carried from an earlier frame is capture
    // and is never stolen, not even by a modal opening mid-drag. The
    // steal takes hot with it when the stratum's precedence allows -
    // the press and its hover belong to the same widget from the moment
    // the claim moves (the hot-claim block above already ran, blocked
    // by the victim's then-ownership).
    if (inside && view.pressed && !track.cancelled && !blocked) {
        if (track.active == kInvalidWidgetId) {
            track.active = id;
            track.active_layer = layer;
            track.active_fresh = true;
        } else if (track.active != id && track.active_fresh &&
                   layer > track.active_layer) {
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
        if (view.released) {
            part.released = true;   // the drag ended, inside or out
            part.clicked = inside;  // full click only if inside
            track.active = kInvalidWidgetId;  // the press is over either way
        }
    }
    return part;
}

WidgetId GuiContext::hot_id(PointerId pointer) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].hot;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

WidgetId GuiContext::active_id(PointerId pointer) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].active;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

bool GuiContext::is_hot(WidgetId id, PointerId pointer) const {
    // kInvalidWidgetId is "no widget" - it must never match, even though
    // hot_id() returns it for an idle or unknown pointer.
    return id != kInvalidWidgetId && hot_id(pointer) == id;
}

bool GuiContext::is_active(WidgetId id, PointerId pointer) const {
    return id != kInvalidWidgetId && active_id(pointer) == id;
}

PointerId GuiContext::owning_pointer(WidgetId id) const {
    // Lowest live slot whose press is on `id` - with the bridge's mouse
    // mirroring the first finger, that means the mouse slot wins ties
    // (the documented duplicate), deterministically.
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].active == id) {
            return pointers_[static_cast<std::size_t>(p)].id;
        }
    }
    return kNoPointer;
}

const PointerView* GuiContext::pointer(PointerId id) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (frame_pointers_[static_cast<std::size_t>(p)].id == id) {
            return &frame_pointers_[static_cast<std::size_t>(p)];
        }
    }
    return nullptr;  // no such pointer this frame
}

}  // namespace ws::ui::gui
