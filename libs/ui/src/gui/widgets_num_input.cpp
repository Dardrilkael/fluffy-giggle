// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the numeric field - input_float and input_int over one shared
// behavior, the exact-value row the property grid always wanted. The
// editing itself is text_input's own machinery (text_input_behavior
// through the numeric filter), and the field's pixels are text_input's
// own look over the row's right share - the behavior maps its caret
// into the same rect the draw paints, both recomputing the label fit.
// The commit edges (Enter, the blur, the stepping arrows) and the
// buffer discipline (the focus owns the spelling; every idle frame
// re-spells it from the value) live here, over the pure parse/format
// pair that travels public in widgets.hpp.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace ws::ui::gui {

// --- the pure pair ------------------------------------------------------------
// The strict parse and the %g spelling, public in widgets.hpp (the
// popup_placement precedent): pure std, no context, no allocation past
// the error's own storage - the messages are static literals, so the
// SSO covers them and a refused commit never allocates.

ws::core::Result<float> input_parse_float(const char* text) {
    if (text == nullptr || *text == '\0') {
        return ws::core::fail(static_cast<std::int32_t>(InputParseError::empty),
                              "ui", "empty");
    }
    // Hex and hex-float spellings ("0x10", "0x1p3") are strtod's to
    // parse but never a decimal field's; one scan refuses the family
    // before the walk (the typing filters keep them out anyway - this
    // is the public parse standing on its own).
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == 'x' || *p == 'X' || *p == 'p' || *p == 'P') {
            return ws::core::fail(
                static_cast<std::int32_t>(InputParseError::not_a_number),
                "ui", "not a number");
        }
    }
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text) {
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::not_a_number),
            "ui", "not a number");
    }
    if (*end != '\0') {
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::trailing), "ui",
            "trailing text");
    }
    if (!std::isfinite(parsed)) {
        // "inf", "nan" and their spellings parse but never commit.
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::not_a_number),
            "ui", "not a number");
    }
    const float value = static_cast<float>(parsed);
    if (!std::isfinite(value)) {
        // A finite double past the float's own magnitude: a range
        // refusal, not a format one.
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::out_of_range),
            "ui", "out of range");
    }
    return value;
}

ws::core::Result<int> input_parse_int(const char* text) {
    if (text == nullptr || *text == '\0') {
        return ws::core::fail(static_cast<std::int32_t>(InputParseError::empty),
                              "ui", "empty");
    }
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (end == text) {
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::not_a_number),
            "ui", "not a number");
    }
    if (*end != '\0') {
        // "1.5" is trailing text to an integer; "0x10" stops at 'x'.
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::trailing), "ui",
            "trailing text");
    }
    if (errno == ERANGE || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return ws::core::fail(
            static_cast<std::int32_t>(InputParseError::out_of_range),
            "ui", "out of range");
    }
    return static_cast<int>(parsed);
}

int input_format_float(char* buf, int capacity, float value,
                       int significant) {
    if (buf == nullptr || capacity <= 0) {
        return 0;
    }
    // The digits clamp to the float's honest neighborhood; a negative
    // zero spells as the plain one (the plot's tick labels normalize
    // the same way - a display never owes a sign to nothing).
    if (value == 0.0f) {
        value = 0.0f;
    }
    const int digits = std::min(9, std::max(1, significant));
    const int n = std::snprintf(buf, static_cast<std::size_t>(capacity),
                                "%.*g", digits, static_cast<double>(value));
    if (n < 0) {
        return 0;
    }
    return std::min(n, capacity - 1);
}

namespace {

// The field's own floor: the px the field keeps even under a label
// that wants the whole row - a hardcoded pixel like the focus ring's
// 1. A row that cannot hold label plus floor gives the field
// everything (the label's fit already truncated to nothing).
constexpr float kFieldFloor = 64.0f;

// The row's split: the fitted label keeps the left share (its inset,
// its advance, and the panel padding's gutter after it), the field the
// rest. The behavior maps its caret into the same rect the draw
// paints - each recomputes the fit itself (the draw owns its own fits,
// the behavior its own geometry, the convention every split family
// rides).
Rect num_field_rect(const Rect& rect, const char* label,
                    const ChromeTheme& theme) {
    float label_w = 0.0f;
    if (label != nullptr && *label != '\0') {
        char scratch[128];
        const float room =
            std::max(0.0f, rect.width - kFieldFloor -
                               2.0f * theme.label_inset);
        const LabelFit fit =
            fit_text(label, room, theme, scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            label_w = theme.label_inset + fit.advance;
        }
    }
    const float gutter = label_w > 0.0f ? theme.padding : 0.0f;
    const float x = rect.x + label_w + gutter;
    return {x, rect.y, std::max(0.0f, rect.x + rect.width - x), rect.height};
}

// The two ladders the one behavior parameterizes over: the charset,
// the spelling, the parse and the clamp are the whole difference
// between the float field and the int one.
struct FloatNum {
    using Value = float;
    static bool filter(std::uint32_t cp) { return is_float_insertable(cp); }
    static int spell(char* buf, int capacity, float value, int significant) {
        return input_format_float(buf, capacity, value, significant);
    }
    static ws::core::Result<float> parse(const char* text) {
        return input_parse_float(text);
    }
    // The derived arrow step (the caller passed 0): the float ladder's
    // own 1% of the range, 1 unbounded - slider_float's rule.
    static double derived_step(double min, double max) {
        return min < max ? (max - min) / 100.0 : 1.0;
    }
    static float clamp_into(float value, double min, double max) {
        if (min >= max) {
            return value;  // unbounded: the honest row
        }
        const double widened = static_cast<double>(value);
        return static_cast<float>(
            std::min(std::max(widened, min), max));
    }
    static float from_double(double value) {
        return static_cast<float>(value);
    }
};

struct IntNum {
    using Value = int;
    static bool filter(std::uint32_t cp) { return is_int_insertable(cp); }
    static int spell(char* buf, int capacity, int value, int) {
        if (buf == nullptr || capacity <= 0) {
            return 0;
        }
        const int n = std::snprintf(buf, static_cast<std::size_t>(capacity),
                                    "%d", value);
        return n < 0 ? 0 : std::min(n, capacity - 1);
    }
    static ws::core::Result<int> parse(const char* text) {
        return input_parse_int(text);
    }
    // The derived arrow step: the int ladder's plain 1, ranged or not -
    // slider_int's own rule (a 1%-of-range step would round away under
    // a small range and the arrows would go dead).
    static double derived_step(double, double) { return 1.0; }
    static int clamp_into(int value, double min, double max) {
        if (min >= max) {
            return value;
        }
        const double widened = static_cast<double>(value);
        return static_cast<int>(std::min(std::max(widened, min), max));
    }
    // The unbounded int ladder can walk past the type's own bounds;
    // the saturation parks it at the edge rather than wrapping.
    static int from_double(double value) {
        if (value >= 2147483647.0) {
            return std::numeric_limits<int>::max();
        }
        if (value <= -2147483648.0) {
            return std::numeric_limits<int>::min();
        }
        return static_cast<int>(std::lround(value));
    }
};

// The one behavior both fields ride: the spelling, the editing and the
// commit edges over text_input's own machinery.
template <typename Num>
NumInputResult input_num_behavior(GuiContext& gui, WidgetId id,
                                  const Rect& rect, typename Num::Value& value,
                                  double min, double max, double step,
                                  int significant, const char* label,
                                  const ChromeTheme& theme, bool disabled) {
    NumInputState& num = gui.persistent_state<NumInputState>(id);

    // The frame's focus edges, read BEFORE the field's behavior runs:
    // `pre_focused` is the focus as the frame opened it (a press's
    // claim latches inside state(), a Tab move resolved at the last
    // end_frame), `was_focused` the field's own last-frame answer, and
    // the selection text_input's Escape may drop is the pair as the
    // last frame left it - the bare-Escape revert below is told apart
    // from the selection drop by exactly that pair (consume_escape
    // spends the arbitration's copy, not the key edge itself).
    const bool pre_focused = gui.is_focused(id);
    const bool was_focused = num.edit.was_focused;
    const bool had_selection = num.edit.caret != num.edit.anchor;

    // The spelling, before anything edits: without the focus the value
    // re-spells the buffer every frame, so an external change (a
    // caller's write, a settings sync) shows the moment it lands - the
    // BLUR frame is exempt, its buffer still holds the edit the commit
    // below must read. The keyboard's arrival frame (Tab's resolution
    // or a set_focus - never a press, whose claim latches below)
    // re-spells once and selects the whole thing, so typing replaces
    // the standing value; a click's arrival keeps the text field's own
    // caret placement instead.
    if (!pre_focused) {
        if (!was_focused) {
            (void)Num::spell(num.buf, kNumInputCapacity, value, significant);
            num.edit.history.clear();  // a re-spelled buffer voids its past
        }
    } else if (!was_focused) {
        const int pen =
            Num::spell(num.buf, kNumInputCapacity, value, significant);
        num.edit.caret = pen;
        num.edit.anchor = 0;
        num.edit.history.clear();  // the arrival spelling is the ground truth
    }

    // The field itself: text_input's whole editing through the numeric
    // charset, over the row's field rect. The vertical keys opt out -
    // the numeric stepper owns Up/Down (its value steps below), so the
    // field never spends them on Home/End.
    NumInputResult out;
    static_cast<TextInputResult&>(out) = text_input_behavior(
        gui, id, num_field_rect(rect, label, theme), num.buf,
        kNumInputCapacity, num.edit, theme, disabled, {}, &Num::filter,
        false);
    out.changed = false;

    const bool dead = out.state == WidgetState::Disabled;
    const bool focused = out.focused && !dead;
    const bool ranged = min < max;

    // The derived step: 0 rides the ladder's own rule (the float's 1%
    // of the range, the int's plain 1 - the sliders' own steps); a
    // negative step refuses the arrows outright.
    double effective_step = step;
    if (effective_step == 0.0) {
        effective_step = Num::derived_step(min, max);
    }

    // The commit: parse the buffer strictly, clamp into the range,
    // write only on a real move (the radio's idempotence - re-typing
    // the standing value is a no-op). True when the value moved.
    const auto commit_buffer = [&]() {
        const ws::core::Result<typename Num::Value> parsed =
            Num::parse(num.buf);
        if (!parsed.has_value()) {
            return false;
        }
        const typename Num::Value next =
            Num::clamp_into(*parsed, min, max);
        if (next == value) {
            return false;
        }
        value = next;
        return true;
    };
    // The spelling's return: the value's own canonical form, the caret
    // parked at its end - a landed commit shows what stands (typed
    // "5e-1", standing "0.5"), a refused one shows what stood.
    const auto respell = [&]() {
        const int pen =
            Num::spell(num.buf, kNumInputCapacity, value, significant);
        num.edit.caret = num.edit.anchor = pen;
        // The canonical spelling is a REWRITE, not an edit: the history
        // the typing built describes a buffer that no longer exists,
        // and undo must not splice a stale entry into it.
        num.edit.history.clear();
    };

    // Enter: the numeric line's own commit edge - the key is this
    // widget's to take (a single line of text had none; a number does),
    // and the edge is consumed with the commit, so a dialog behind the
    // field does not also answer it. A fresh press only, never the OS
    // repeat - a commit is an activation, not a step.
    if (focused &&
        gui.input().key_pressed[static_cast<std::size_t>(
            ws::input::keys::kEnter)]) {
        out.changed = commit_buffer();
        respell();
        gui.consume_key(ws::input::keys::kEnter);
    }

    // The blur: the focus left (a press elsewhere, a Tab move, the
    // row going disabled), and the edit ends the same way - commit if
    // it parses, revert if it does not. No key to spend.
    if (was_focused && !focused) {
        out.changed = commit_buffer();
        respell();
    }

    // Escape with a selection standing is the text field's own drop
    // (already consumed through the arbitration); a BARE Escape
    // abandons the edit - the spelling reverts, and the arbitration's
    // copy of the key is spent, so the next Escape closes the overlay
    // behind the field.
    if (focused && !had_selection &&
        gui.input().key_pressed[static_cast<std::size_t>(
            ws::input::keys::kEscape)]) {
        respell();
        gui.consume_escape();
    }

    // Up/Down step the value - from the buffer's own parse when it
    // parses (mid-edit stepping commits the typing, the scope's own
    // rule), else from the standing value - clamped into the range and
    // answered only on a real move. Held arrows keep stepping on the
    // OS auto-repeat, the slider's rule. A NaN base refuses the step,
    // the drag's own guard.
    if (focused && effective_step > 0.0 &&
        (gui.key_pressed_or_repeated(ws::input::keys::kUp) ||
         gui.key_pressed_or_repeated(ws::input::keys::kDown))) {
        double base = static_cast<double>(value);
        const ws::core::Result<typename Num::Value> parsed =
            Num::parse(num.buf);
        if (parsed.has_value()) {
            base = static_cast<double>(*parsed);
        }
        const double delta = gui.key_pressed_or_repeated(ws::input::keys::kDown)
                                 ? -effective_step
                                 : effective_step;
        double next = base + delta;
        if (ranged) {
            next = std::min(std::max(next, min), max);
        }
        if (next == next) {  // the NaN guard: a poisoned base refuses
            const typename Num::Value cast = Num::from_double(next);
            if (cast == cast && cast != value) {
                value = cast;
                out.changed = true;
                respell();
            }
        }
    }
    return out;
}

}  // namespace

// --- the composed rows ---------------------------------------------------------

bool input_float(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label, float& value,
                 float min, float max, float step, int significant,
                 float width, bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    NumInputResult result = input_float_behavior(
        gui, row.id, row.rect, value, min, max, step, significant, label,
        theme, disabled);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::input_float, &input_float_draw, row.rect,
          result, theme, label,
          gui.persistent_state<NumInputState>(row.id).buf,
          kNumInputCapacity);
    return result.changed;
}

bool input_int(GuiContext& gui, LayoutCursor& layout,
               const ChromeTheme& theme, const char* label, int& value,
               int min, int max, int step, float width, bool disabled) {
    const RowLayout row = begin_widget_row(gui, layout, theme, label, width);
    NumInputResult result =
        input_int_behavior(gui, row.id, row.rect, value, min, max, step,
                           label, theme, disabled);
    fold_hover_t(gui, row.id, result, theme);  // P4: the face traveler
    paint(theme, &WidgetPainters::input_float, &input_float_draw, row.rect,
          result, theme, label,
          gui.persistent_state<NumInputState>(row.id).buf,
          kNumInputCapacity);
    return result.changed;
}

// --- the split halves, public for the custom numeric field ---------------------

NumInputResult input_float_behavior(GuiContext& gui, WidgetId id,
                                    const Rect& rect, float& value, float min,
                                    float max, float step, int significant,
                                    const char* label,
                                    const ChromeTheme& theme, bool disabled) {
    return input_num_behavior<FloatNum>(
        gui, id, rect, value, static_cast<double>(min),
        static_cast<double>(max), static_cast<double>(step), significant,
        label, theme, disabled);
}

NumInputResult input_int_behavior(GuiContext& gui, WidgetId id,
                                  const Rect& rect, int& value, int min,
                                  int max, int step, const char* label,
                                  const ChromeTheme& theme, bool disabled) {
    return input_num_behavior<IntNum>(
        gui, id, rect, value, static_cast<double>(min),
        static_cast<double>(max), static_cast<double>(step), 0, label, theme,
        disabled);
}

void input_float_draw(const Rect& rect, const NumInputResult& result,
                      const ChromeTheme& theme, const char* label,
                      const char* buf, int capacity) {
    const float dim_by = dim_factor(result.state, theme);
    // The label: the row's left share, fitted and truncated like every
    // widget label, vertically centered - chrome, no id of its own.
    if (label != nullptr && *label != '\0') {
        char scratch[128];
        const float room =
            std::max(0.0f, rect.width - kFieldFloor -
                               2.0f * theme.label_inset);
        const LabelFit fit =
            fit_text(label, room, theme, scratch, sizeof(scratch));
        if (fit.advance > 0.0f) {
            draw_label(rect.x + theme.label_inset, rect.y, rect.height, fit,
                       dim(theme.text, dim_by), theme);
        }
    }
    // The field: text_input's own stock look over the field rect the
    // behavior mapped its caret into - the same faces, ring, selection
    // band, content walk and blinking caret. No placeholder inside:
    // the label stands at the left, and a buffer emptied mid-edit
    // shows the caret alone.
    text_input_draw(num_field_rect(rect, label, theme), result, theme, "",
                    buf, capacity);
}

}  // namespace ws::ui::gui
