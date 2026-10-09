// IdStack's out-of-line half: widget identity as FNV-1a scope-path
// hashes. The hashing constants and the overflow warn-once flag are
// local to this TU - nothing else in the lib mixes ids.

#include <ws/ui/gui/id_stack.hpp>

#include <ws/core/log.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ws::ui::gui {
namespace {

// --- IdStack hashing ------------------------------------------------------------

// The hash runs on raw 64-bit ints; WidgetId is only the stored/returned
// wrapping, converted at the stack/return seams below. No arithmetic ever
// happens on the strong type.
//
// FNV-1a 64-bit: cheap, dependency-free, fine dispersion over the short
// strings widget labels come from. Scopes chain by seeding each mix with
// the parent scope's id, so an id encodes its whole scope path, not just
// its leaf label.
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;

// The root scope (depth 0) seeds from a fixed nonzero constant - never 0,
// the invalid sentinel.
constexpr std::uint64_t kRootScopeId = kFnvOffsetBasis;

// Mixed in after every payload, so scope paths are delimited: pushing
// "ab" then "c" is a different path than pushing "a" then "bc".
constexpr char kScopeDelimiter = ';';

// A would-be zero hash maps here instead: no real widget may id as
// kInvalidWidgetId. The substitution adds no collision of its own.
constexpr std::uint64_t kZeroHash = 0x9E3779B97F4A7C15ULL;

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

std::uint64_t fnv_step(std::uint64_t hash, std::uint64_t byte) {
    return (hash ^ byte) * kFnvPrime;
}

// hash(scope, label): 'L' tags the payload family (a label never ids
// like an index), then the bytes, then the delimiter. Never returns 0.
std::uint64_t hash_label(std::uint64_t scope, const char* label) {
    std::uint64_t hash = fnv_step(scope, 'L');
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
std::uint64_t hash_index(std::uint64_t scope, int index) {
    std::uint64_t hash = fnv_step(scope, 'I');
    const auto value = static_cast<std::uint32_t>(index);
    for (int shift = 0; shift < 32; shift += 8) {
        hash = fnv_step(hash, (value >> shift) & 0xFFu);
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// hash(scope, reserved key): 'R' tags the family - the gui's OWN state
// keys ("scroll", "drag"), which can never collide with a user label
// ('L') or an index ('I') however they are spelled. Same stream shape
// otherwise, so one scope path mixes the three families without
// ambiguity.
std::uint64_t hash_reserved(std::uint64_t scope, const char* key) {
    std::uint64_t hash = fnv_step(scope, 'R');
    if (key == nullptr) {
        key = "";  // a null key ids like an empty one; no UB
    }
    for (const char* p = key; *p != '\0'; ++p) {
        hash = fnv_step(hash, static_cast<unsigned char>(*p));
    }
    hash = fnv_step(hash, static_cast<unsigned char>(kScopeDelimiter));
    return hash == 0 ? kZeroHash : hash;
}

// The scope id `depth` levels deep (0 = the fixed root seed). Past the
// clamp the slot index stops deepening with the counter, so overflowed
// scopes derive from the deepest remembered scope.
std::uint64_t scope_id(const std::array<WidgetId, IdStack::kMaxDepth>& stack,
                      int depth) {
    if (depth == 0) {
        return kRootScopeId;
    }
    const int slot = std::min(depth, IdStack::kMaxDepth) - 1;
    return stack[static_cast<std::size_t>(slot)].value;
}

}  // namespace

// --- IdStack: hash-based widget identity -----------------------------------------

void IdStack::push(const char* label) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        WidgetId{hash_label(scope_id(stack_, depth_), label)};
    ++depth_;
}

void IdStack::push(int index) {
    if (id_stack_full(depth_)) {
        ++depth_;  // balanced with the matching pop; the scope stops deepening
        return;
    }
    stack_[static_cast<std::size_t>(depth_)] =
        WidgetId{hash_index(scope_id(stack_, depth_), index)};
    ++depth_;
}

void IdStack::pop() {
    if (depth_ > 0) {
        --depth_;  // popping an empty stack is the documented no-op
    }
}

WidgetId IdStack::id(const char* label) const {
    // "###" REDIRECTS the id to the marker onward: "Play###pause" and
    // "Pause###pause" are one widget whose display can change while its
    // identity holds. The marker's own bytes hash in the stream (with
    // their '#'), so a redirected id cannot collide with a plain
    // label's short of sharing the marker spelling. A "##"-only suffix
    // is NOT special here: the whole label ids, display or not.
    if (label != nullptr) {
        const char* marker = std::strstr(label, "###");
        if (marker != nullptr) {
            return WidgetId{hash_label(scope_id(stack_, depth_), marker)};
        }
    }
    return WidgetId{hash_label(scope_id(stack_, depth_), label)};
}

WidgetId IdStack::internal_id(const char* key) const {
    return WidgetId{hash_reserved(scope_id(stack_, depth_), key)};
}

std::string_view label_display(const char* label) {
    if (label == nullptr) {
        return {};  // a null label displays as empty
    }
    // The first "##" ends the display - which also covers "###", whose
    // first two bytes ARE a "##".
    const char* cut = std::strstr(label, "##");
    if (cut == nullptr) {
        return label;
    }
    return std::string_view(label, static_cast<std::size_t>(cut - label));
}

}  // namespace ws::ui::gui
