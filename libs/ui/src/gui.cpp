// ws::ui::gui - the gui library's out-of-line surface (see
// libs/ui/gui-lib-buildplan.md). Input is real (Phase 1: the shell
// accumulates one InputFrame per frame, published through frame_input())
// and so are ids (Phase 2: the IdStack hash). Hit-testing (Phase 3) and
// layout (Phase 4) are still declared stubs. The bodies stay out-of-line
// on purpose: apps resolve these symbols through the ws_ui archive, so a
// consumer compiling against the headers also proves the library links.

#include "gui.h"

#include <ws/core/log.hpp>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/id_stack.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>

#include <array>

namespace ws::ui::gui {
namespace {

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
        core::warn("ui: gui id stack clamped at depth {} - deeper scopes ignored",
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

// The scope id `depth` levels deep (0 = the fixed root seed).
WidgetId scope_id(const std::array<WidgetId, IdStack::kMaxDepth>& stack,
                  int depth) {
    return depth == 0 ? kRootScopeId
                      : stack[static_cast<std::size_t>(depth) - 1];
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
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        hash_label(scope_id(stack_, depth_), label);
    ++depth_;
}

void IdStack::push(int index) {
    if (id_stack_full(depth_)) {
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

// --- LayoutCursor (Phase 4: stack layout) -----------------------------------------

Rect LayoutCursor::begin_panel(const Rect&, float) { return {}; }

Rect LayoutCursor::next(float, float) { return {}; }

void LayoutCursor::same_line() {}

void LayoutCursor::end_panel() {}

// --- GuiContext (hot/active + side tables land in Phase 3) -----------------------

void GuiContext::new_frame(const InputFrame& input) {
    input_ = input;  // adopt the snapshot; hover resets join in Phase 3
}

void GuiContext::end_frame() {}

HitTest GuiContext::hit_test(WidgetId, const Rect&) { return {}; }

}  // namespace ws::ui::gui
