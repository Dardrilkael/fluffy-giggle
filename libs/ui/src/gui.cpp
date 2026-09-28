// ws::ui::gui - the gui library's out-of-line surface (see
// libs/ui/gui-lib-buildplan.md). The Phase 1-4 core is real: the shell
// accumulates one InputFrame per frame (frame_input()), IdStack hashes
// widget identity, GuiContext hit-tests hot/active with persistent side
// tables, and LayoutCursor flows widgets through stacked panels. Widgets
// themselves (Phase 5+) live in src/gui_widgets.cpp. The bodies stay
// out-of-line on purpose: apps resolve these symbols through the ws_ui
// archive, so a consumer compiling against the headers also proves the
// library links.

#include "gui.h"

#include <ws/core/log.hpp>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>

#include <algorithm>
#include <array>

namespace ws::ui::gui {
namespace {

// Process-wide gui state (the shell's input tap, the warn-once flags).
// Single-threaded BY DESIGN: the immediate-mode loop runs on the shell's
// main thread only, so these are plain globals on purpose - not a data
// race waiting to be "fixed" with atomics.

// The shell's accumulator while App::run() is active; null before startup
// and after shutdown.
InputFrame* g_active_input = nullptr;

// The answer frame_input() gives without a running app - a valid empty
// frame, so callers never see a dangling or partially filled snapshot.
const InputFrame& empty_input() {
    static const InputFrame empty;
    return empty;
}

// --- IdStack hashing ------------------------------------------------------------

// FNV-1a 64-bit: cheap, dependency-free, fine dispersion over the short
// strings widget labels come from. Scopes chain by seeding each mix with
// the parent scope's id, so an id encodes its whole scope path, not just
// its leaf label.
constexpr WidgetId kFnvPrime = 1099511628211ULL;
constexpr WidgetId kFnvOffsetBasis = 14695981039346656037ULL;

// The root scope (depth 0) seeds from a fixed nonzero constant - never 0,
// the invalid sentinel.
constexpr WidgetId kRootScopeId = kFnvOffsetBasis;

// Mixed in after every payload, so scope paths are delimited: pushing
// "ab" then "c" is a different path than pushing "a" then "bc".
constexpr char kScopeDelimiter = ';';

// A would-be zero hash maps here instead: no real widget may id as
// kInvalidWidgetId. The substitution adds no collision of its own.
constexpr WidgetId kZeroHash = 0x9E3779B97F4A7C15ULL;

// Depth overflow is a programming error - surfaced in the log rather than
// a crash; the stack clamps and this keeps it to one warn per process.
bool id_overflow_warned = false;

// True when the stack is full; warns the first time only.
bool id_stack_full(int depth) {
    if (depth < IdStack::kMaxDepth) {
        return false;
    }
    if (!id_overflow_warned) {
        id_overflow_warned = true;
        core::warn("ui: gui id stack clamped at depth {} - deeper scopes stop "
                   "deepening (push/pop stays balanced)",
                   IdStack::kMaxDepth);
    }
    return true;
}

WidgetId fnv_step(WidgetId hash, WidgetId byte) {
    return (hash ^ byte) * kFnvPrime;
}

// hash(scope, label): 'L' tags the payload family (a label never ids
// like an index), then the bytes, then the delimiter. Never returns 0.
WidgetId hash_label(WidgetId scope, const char* label) {
    WidgetId hash = fnv_step(scope, 'L');
    if (label == nullptr) {
        label = "";  // a null label ids like an empty one; no UB
    }
    for (const char* p = label; *p != '\0'; ++p) {
        hash = fnv_step(hash, static_cast<unsigned char>(*p));
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// hash(scope, index): 'I' tags the family, then the value's bytes, low to
// high - a fixed order, never a stringified form.
WidgetId hash_index(WidgetId scope, int index) {
    WidgetId hash = fnv_step(scope, 'I');
    const auto value = static_cast<std::uint32_t>(index);
    for (int shift = 0; shift < 32; shift += 8) {
        hash = fnv_step(hash, (value >> shift) & 0xFFu);
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// The scope id `depth` levels deep (0 = the fixed root seed). Past the
// clamp the slot index stops deepening with the counter, so overflowed
// scopes derive from the deepest remembered scope.
WidgetId scope_id(const std::array<WidgetId, IdStack::kMaxDepth>& stack,
                  int depth) {
    if (depth == 0) {
        return kRootScopeId;
    }
    const int slot = std::min(depth, IdStack::kMaxDepth) - 1;
    return stack[static_cast<std::size_t>(slot)];
}

// --- LayoutCursor panels ---------------------------------------------------------

// Depth overflow is a programming error - surfaced in the log rather than
// a crash; the panel stack clamps and this keeps it to one warn per process.
bool panel_overflow_warned = false;

// True when the panel stack is full; warns the first time only.
bool panel_stack_full(int top) {
    if (top + 1 < LayoutCursor::kMaxPanels) {
        return false;
    }
    if (!panel_overflow_warned) {
        panel_overflow_warned = true;
        core::warn("ui: gui layout clamped at {} panels - deeper panels flow "
                   "into the deepest real one (begin/end stays balanced)",
                   LayoutCursor::kMaxPanels);
    }
    return true;
}

// A widget id hit-tested twice in one frame means two widgets share
// identity (same-label siblings without a push(index) between them).
// Flagged once per process; the duplicate is tolerated after.
bool duplicate_id_warned = false;

// --- InputFrame touch table ---------------------------------------------------

// Scan the touch table by contact id - the platform keys every touch_* call
// by id, never by slot (ids are stable per contact; slots shuffle as
// earlier fingers lift). -1 when the id is not in the table. Clamped to
// kMaxTouches so a stray touch_count cannot walk out of the array.
int find_touch(const InputFrame& frame, std::uint32_t id) {
    const int count = std::min(frame.touch_count, InputFrame::kMaxTouches);
    for (int i = 0; i < count; ++i) {
        if (frame.touches[static_cast<std::size_t>(i)].id == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace

// --- InputFrame: one frame's snapshot (the shell fills, next_frame advances) ----

void InputFrame::next_frame() {
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    scroll_dx = 0.0f;
    scroll_dy = 0.0f;
    key_pressed.fill(false);
    text_input.clear();
    // Touch edges clear and lifted contacts leave the table now - their
    // release frame is over. The survivors keep their arrival order (that
    // order is the stable pointer order across frames).
    int kept = 0;
    const int count = std::min(touch_count, kMaxTouches);
    for (int i = 0; i < count; ++i) {
        if (!touches[static_cast<std::size_t>(i)].down) {
            continue;  // lifted last frame: dropped
        }
        TouchPoint& point = touches[static_cast<std::size_t>(kept)];
        point = touches[static_cast<std::size_t>(i)];
        point.pressed = false;
        point.released = false;
        point.cancelled = false;
        ++kept;
    }
    touch_count = kept;
    // Self-heal: a bridge whose contact is already gone (only reachable by
    // direct table surgery - touch_up clears it first) stops mirroring.
    if (bridged_touch_id_ != kNoPointer &&
        find_touch(*this, bridged_touch_id_) < 0) {
        bridged_touch_id_ = kNoPointer;
    }
}

int InputFrame::touch_down(std::uint32_t id, float x, float y) {
    int slot = find_touch(*this, id);
    if (slot < 0) {
        if (touch_count >= kMaxTouches) {
            return -1;  // the cap contract: the caller drops + warns once
        }
        slot = touch_count++;
        touches[static_cast<std::size_t>(slot)].id = id;
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    point.down = true;
    point.pressed = true;
    point.released = false;
    point.cancelled = false;
    // The primary-touch -> mouse bridge: the first contact of a gesture
    // mirrors the mouse so single-finger use and ImGui keep working
    // unchanged; later contacts only track.
    if (bridged_touch_id_ == kNoPointer) {
        bridged_touch_id_ = id;
    }
    if (bridged_touch_id_ == id) {
        mouse_x = x;
        mouse_y = y;
        mouse_down[0] = true;
        mouse_pressed[0] = true;
    }
    return slot;
}

void InputFrame::touch_move(std::uint32_t id, float x, float y) {
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return;  // unknown id: no-op (never a reported contact)
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    if (bridged_touch_id_ == id) {
        mouse_x = x;  // the bridge follows its contact's position
        mouse_y = y;
    }
}

void InputFrame::touch_up(std::uint32_t id, float x, float y, bool cancelled) {
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return;
    }
    TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    point.x = x;
    point.y = y;
    point.down = false;
    point.released = true;
    point.cancelled = cancelled;
    if (bridged_touch_id_ == id) {
        // The bridge mirrors the lift too - a cancelled gesture is "never
        // happened", which for the mouse reads as: released, no click.
        mouse_x = x;
        mouse_y = y;
        mouse_down[0] = false;
        mouse_released[0] = true;
        bridged_touch_id_ = kNoPointer;  // no promotion: the mouse stays
                                         // released until a new gesture
    }
}

int InputFrame::gather_pointers(
    std::array<PointerView, kMaxPointers>& out) const {
    out.fill(PointerView{});
    PointerView& mouse = out[0];
    mouse.id = kMousePointerId;
    mouse.x = mouse_x;
    mouse.y = mouse_y;
    mouse.down = mouse_down[0];
    mouse.pressed = mouse_pressed[0];
    mouse.released = mouse_released[0];
    const int count = std::min(touch_count, kMaxTouches);
    for (int i = 0; i < count; ++i) {
        const TouchPoint& point = touches[static_cast<std::size_t>(i)];
        PointerView& view = out[static_cast<std::size_t>(i) + 1];
        view.id = point.id;
        view.x = point.x;
        view.y = point.y;
        view.down = point.down;
        view.pressed = point.pressed;
        view.released = point.released;
    }
    return 1 + count;
}

bool InputFrame::pointer_pos(std::uint32_t id, float* x, float* y) const {
    if (id == kMousePointerId) {
        if (x != nullptr) {
            *x = mouse_x;
        }
        if (y != nullptr) {
            *y = mouse_y;
        }
        return true;
    }
    const int slot = find_touch(*this, id);
    if (slot < 0) {
        return false;
    }
    const TouchPoint& point = touches[static_cast<std::size_t>(slot)];
    if (x != nullptr) {
        *x = point.x;
    }
    if (y != nullptr) {
        *y = point.y;
    }
    return true;
}

void set_active_input(InputFrame* input) {
    g_active_input = input;
}

const InputFrame& frame_input() {
    return g_active_input != nullptr ? *g_active_input : empty_input();
}

// --- IdStack: hash-based widget identity -----------------------------------------

void IdStack::push(const char* label) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        hash_label(scope_id(stack_, depth_), label);
    ++depth_;
}

void IdStack::push(int index) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        hash_index(scope_id(stack_, depth_), index);
    ++depth_;
}

void IdStack::pop() {
    if (depth_ > 0) {
        --depth_;  // popping an empty stack is the documented no-op
    }
}

WidgetId IdStack::current(const char* label) const {
    return hash_label(scope_id(stack_, depth_), label);
}

// --- LayoutCursor: stack-based layout --------------------------------------------

Rect LayoutCursor::begin_panel(const Rect& bounds, float padding) {
    if (panel_stack_full(top_)) {
        // Balanced with the matching end_panel(): the depth counts on,
        // children flow into the deepest real panel, and this panel's
        // content rect is degenerate.
        ++top_;
        return {};
    }
    ++top_;
    Panel& panel = slot();
    panel.content = {bounds.x + padding, bounds.y + padding,
                     bounds.width - 2.0f * padding,
                     bounds.height - 2.0f * padding};
    panel.cursor_x = panel.content.x;
    panel.cursor_y = panel.content.y;
    panel.last = {};
    panel.line_height = 0.0f;
    panel.placed_any = false;
    panel.at_line_start = true;
    return panel.content;
}

Rect LayoutCursor::next(float w, float h) {
    if (top_ < 0) {
        return {};  // no panel open: nothing to place (drop-safe)
    }
    Panel& panel = slot();
    if (panel.at_line_start) {
        panel.line_height = 0.0f;  // this placement opens a fresh line
    }
    const Rect placed{panel.cursor_x, panel.cursor_y, w, h};
    panel.last = placed;
    panel.placed_any = true;
    panel.line_height = std::max(panel.line_height, h);
    // Default: the next widget opens a fresh line below this line's tallest.
    panel.cursor_x = panel.content.x;
    panel.cursor_y += panel.line_height;
    panel.at_line_start = true;
    return placed;
}

void LayoutCursor::same_line() {
    if (top_ < 0) {
        return;
    }
    Panel& panel = slot();
    if (!panel.placed_any) {
        return;  // nothing placed yet: nowhere to resume after
    }
    panel.cursor_x = panel.last.x + panel.last.width;
    panel.cursor_y = panel.last.y;
    panel.at_line_start = false;  // resumed onto the current line
}

void LayoutCursor::end_panel() {
    if (top_ >= 0) {
        --top_;  // closing with none open is the documented no-op
    }
}

Rect LayoutCursor::panel_content() const {
    if (top_ < 0) {
        return {};
    }
    return panels_[static_cast<std::size_t>(
                       std::min(top_, kMaxPanels - 1))]
        .content;
}

float LayoutCursor::flow_extent() const {
    if (top_ < 0) {
        return 0.0f;
    }
    const Panel& panel =
        panels_[static_cast<std::size_t>(std::min(top_, kMaxPanels - 1))];
    // At a line start the cursor sits below the last placed line; a
    // same_line() rewind parks it on the line's top with the line's
    // tallest still accumulated - the line's eventual bottom adds it.
    const float bottom = panel.at_line_start
                             ? panel.cursor_y
                             : panel.cursor_y + panel.line_height;
    return bottom - panel.content.y;
}

// --- GuiContext: per-pointer hot/active hit-testing + persistent side tables ------

void GuiContext::new_frame(const InputFrame& input) {
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
        const std::uint32_t id = frame_pointers_[static_cast<std::size_t>(p)].id;
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
    // ownership would block the next frame's hot claims.
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].active !=
                kInvalidWidgetId &&
            frame_pointers_[static_cast<std::size_t>(p)].released) {
            pointers_[static_cast<std::size_t>(p)].active = kInvalidWidgetId;
        }
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

    // Today's single-pointer rules, run once per pointer, the results
    // OR-folded into one HitTest: a two-pointer press on one widget is
    // one press, and capture is per pointer - a drag owned by one finger
    // does not stop another from hovering or pressing elsewhere. Slot
    // order is mouse first, then touches in first-contact order; the
    // first slot with a claim wins `hit.pointer` (deterministic - the
    // bridged finger resolves to the mouse it mirrors).
    HitTest hit;
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        const PointerView& view = frame_pointers_[static_cast<std::size_t>(p)];
        PointerTrack& track = pointers_[static_cast<std::size_t>(p)];
        const bool inside = contains(rect, view.x, view.y);

        // While this pointer's press is owned, the pointer is captured:
        // only the owner can be hot. Otherwise the widget under this
        // pointer claims hot; later widgets draw on top, so a later
        // claim correctly wins.
        if (inside &&
            (track.active == kInvalidWidgetId || track.active == id)) {
            track.hot = id;
        }
        // Hot, not merely inside: this pointer has ONE interactive
        // winner, so a widget overlapped by this pointer's drag does
        // not report hover.
        hit.hovered = hit.hovered || track.hot == id;
        // Pressed-inside claims this pointer's press, if none is owned.
        if (inside && view.pressed && track.active == kInvalidWidgetId) {
            track.active = id;
        }
        if (track.active == id) {
            hit.pressed = true;  // owns a live press, wherever the pointer is
            if (hit.pointer == kNoPointer) {
                hit.pointer = view.id;  // first slot with a claim sticks
            }
            if (view.released) {
                hit.released = true;   // the drag ended, inside or out
                hit.clicked = hit.clicked || inside;  // full click only if inside
                track.active = kInvalidWidgetId;  // the press is over either way
            }
        }
    }
    return hit;
}

WidgetId GuiContext::hot_id(std::uint32_t pointer) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].hot;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

WidgetId GuiContext::active_id(std::uint32_t pointer) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (pointers_[static_cast<std::size_t>(p)].id == pointer) {
            return pointers_[static_cast<std::size_t>(p)].active;
        }
    }
    return kInvalidWidgetId;  // no such pointer this frame
}

std::uint32_t GuiContext::owning_pointer(WidgetId id) const {
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

const PointerView* GuiContext::pointer(std::uint32_t id) const {
    const int count = std::min(pointer_count_, InputFrame::kMaxPointers);
    for (int p = 0; p < count; ++p) {
        if (frame_pointers_[static_cast<std::size_t>(p)].id == id) {
            return &frame_pointers_[static_cast<std::size_t>(p)];
        }
    }
    return nullptr;  // no such pointer this frame
}

}  // namespace ws::ui::gui
