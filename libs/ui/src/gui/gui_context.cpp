// GuiContext's out-of-line half: per-pointer hot/active hit-testing and
// the persistent side tables. The duplicate-id warn-once flag is local
// to this TU.

#include <ws/ui/gui/gui_context.hpp>

#include <ws/core/log.hpp>

#include <algorithm>
#include <array>

namespace ws::ui::gui {
namespace {

// A widget id hit-tested twice in one frame means two widgets share
// identity (same-label siblings without a push(index) between them).
// Flagged once per process; the duplicate is tolerated after.
bool duplicate_id_warned = false;

}  // namespace

// --- GuiContext: per-pointer hot/active hit-testing + persistent side tables ------

void GuiContext::begin_frame(const InputFrame& input) {
    input_ = input;
    hit_ids_.clear();  // this frame's hit-test census starts empty
    // Adopt the frame's pointers: slot 0 is the mouse by construction,
    // the touches follow in first-contact order. Each pointer matches
    // last frame's track by id - active carries, and that carry IS the
    // drag - or starts fresh; a track whose pointer left the frame
    // entirely (lifted last frame) drops out here. Hot re-claims every
    // frame, on every track, as the widgets' hit tests run.
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
        matched[p].hot = kInvalidWidgetId;
    }
    pointers_ = matched;
}

void GuiContext::end_frame() {
    // A release edge no hit_test consumed (the owner stopped running
    // mid-drag - the UI changed) ends the press here, per pointer; stale
    // ownership would block the next frame's hot claims. A cancelled
    // gesture lifts with its pointer - the id is free for the fresh
    // contact the platform may reuse it for.
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        if (frame_pointers_[static_cast<std::size_t>(p)].released) {
            track.active = kInvalidWidgetId;
            track.cancelled = false;
        }
    }
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
    // per process, tolerated after.
    if (!hit_ids_.insert(id).second && !duplicate_id_warned) {
        duplicate_id_warned = true;
        core::warn("ui: gui widget id hit-tested twice in one frame - "
                   "same-label siblings need ids().push(index) between them");
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
// caller would be a call-order hazard.
HitTest GuiContext::hit_test_pointer(WidgetId id, const Rect& rect,
                                     int slot) {
    const PointerView& view = frame_pointers_[static_cast<std::size_t>(slot)];
    PointerTrack& track = pointers_[static_cast<std::size_t>(slot)];
    const bool inside = rect.contains(view.x, view.y);

    // While this pointer's press is owned, the pointer is captured:
    // only the owner can be hot. A cancelled pointer (its press was
    // consumed by a gesture) claims nothing at all. Otherwise the
    // widget under this pointer claims hot; later widgets draw on
    // top, so a later claim correctly wins.
    if (inside && !track.cancelled &&
        (track.active == kInvalidWidgetId || track.active == id)) {
        track.hot = id;
    }

    HitTest part;
    // Hot, not merely inside: this pointer has ONE interactive
    // winner, so a widget overlapped by this pointer's drag does
    // not report hover.
    part.hovered = track.hot == id;
    // Pressed-inside claims this pointer's press, if none is owned.
    // The cancelled guard is belt-and-braces: a consumed gesture
    // never re-arms mid-flight.
    if (inside && view.pressed && !track.cancelled &&
        track.active == kInvalidWidgetId) {
        track.active = id;
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
