// RAII guards for the gui's begin/end scope pairs - the ScopedId
// pattern (id_stack.hpp) extended to every scope that has one. A guard
// calls the SAME begin and end the app would (one seam: no second
// implementation of any scope) - the constructor begins, the destructor
// ends, and an early return between them, the manual pair's one failure
// mode (a scope left open until the next begin_frame resets the
// stacks), cannot happen. The overlay guards gate their end on the
// begin's own answer - a begin that answered false pushed nothing, so
// nothing is popped - while the always-pushes guards end
// unconditionally; every end is drop-safe as documented either way.
//
// The guards are block locals by design: copy and move are deleted so a
// scope's lifetime cannot be smuggled out of the block that owns it,
// and they are header-only - thin, inline, holding nothing beyond the
// pair's own answer. The intended shape (the scope reads as the block
// it is):
//
//     if (const gui::ScopedPopup pop(gui, layout, theme, "menu",
//                                    anchor, bounds, open)) {
//         ... rows ...
//     }  // end_popup rides the destructor

#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

namespace ws::ui::gui {

// begin_layer/end_layer: a stratum held for exactly the block below -
// the layer scopes' begin_layer/end_layer pair as one stack local.
class [[nodiscard]] ScopedLayer {
public:
    explicit ScopedLayer(GuiContext& gui, Layer layer) : gui_(gui) {
        gui_.begin_layer(layer);
    }
    ~ScopedLayer() { gui_.end_layer(); }
    ScopedLayer(const ScopedLayer&) = delete;
    ScopedLayer& operator=(const ScopedLayer&) = delete;
    ScopedLayer(ScopedLayer&&) = delete;
    ScopedLayer& operator=(ScopedLayer&&) = delete;

private:
    GuiContext& gui_;
};

// begin_disabled/end_disabled: the ambient lock held for the block -
// the condition form is a plain if written as a scope, exactly the
// pair's own contract (a false condition pushes a bit that locks
// nothing, so the matching end never eats an enclosing true scope).
class [[nodiscard]] ScopedDisabled {
public:
    explicit ScopedDisabled(GuiContext& gui, bool condition = true)
        : gui_(gui) {
        gui_.begin_disabled(condition);
    }
    ~ScopedDisabled() { gui_.end_disabled(); }
    ScopedDisabled(const ScopedDisabled&) = delete;
    ScopedDisabled& operator=(const ScopedDisabled&) = delete;
    ScopedDisabled(ScopedDisabled&&) = delete;
    ScopedDisabled& operator=(ScopedDisabled&&) = delete;

private:
    GuiContext& gui_;
};

// begin_panel/end_panel: the panel scope with its content rect riding
// the guard - `content()` is the begin's own answer.
class [[nodiscard]] ScopedPanel {
public:
    ScopedPanel(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label,
                const Rect& bounds)
        : gui_(gui),
          layout_(layout),
          content_(begin_panel(gui, layout, theme, label, bounds)) {}
    ~ScopedPanel() { end_panel(gui_, layout_); }
    ScopedPanel(const ScopedPanel&) = delete;
    ScopedPanel& operator=(const ScopedPanel&) = delete;
    ScopedPanel(ScopedPanel&&) = delete;
    ScopedPanel& operator=(ScopedPanel&&) = delete;

    const Rect& content() const { return content_; }

private:
    GuiContext& gui_;
    LayoutCursor& layout_;
    Rect content_;
};

// begin_scroll_panel/end_scroll_panel: the scrolling twin. The end
// wants the theme, so the guard carries the reference it was handed.
class [[nodiscard]] ScopedScrollPanel {
public:
    ScopedScrollPanel(GuiContext& gui, LayoutCursor& layout,
                      const ChromeTheme& theme, const char* label,
                      const Rect& bounds)
        : gui_(gui),
          layout_(layout),
          theme_(&theme),
          content_(begin_scroll_panel(gui, layout, theme, label, bounds)) {}
    ~ScopedScrollPanel() { end_scroll_panel(gui_, layout_, *theme_); }
    ScopedScrollPanel(const ScopedScrollPanel&) = delete;
    ScopedScrollPanel& operator=(const ScopedScrollPanel&) = delete;
    ScopedScrollPanel(ScopedScrollPanel&&) = delete;
    ScopedScrollPanel& operator=(ScopedScrollPanel&&) = delete;

    const Rect& content() const { return content_; }

private:
    GuiContext& gui_;
    LayoutCursor& layout_;
    const ChromeTheme* theme_;
    Rect content_;
};

// begin_popup/end_popup: the guard opens with the caller's `open` and
// ends only when the begin answered true - a closed popup pushed
// nothing, so nothing is popped. `if (pop)` is the scope body's gate.
class [[nodiscard]] ScopedPopup {
public:
    ScopedPopup(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label,
                const Rect& anchor, const Rect& bounds, bool& open)
        : gui_(gui),
          layout_(layout),
          open_(begin_popup(gui, layout, theme, label, anchor, bounds,
                            open)) {}
    ~ScopedPopup() {
        if (open_) {
            end_popup(gui_, layout_);
        }
    }
    ScopedPopup(const ScopedPopup&) = delete;
    ScopedPopup& operator=(const ScopedPopup&) = delete;
    ScopedPopup(ScopedPopup&&) = delete;
    ScopedPopup& operator=(ScopedPopup&&) = delete;

    explicit operator bool() const { return open_; }

private:
    GuiContext& gui_;
    LayoutCursor& layout_;
    bool open_;
};

// begin_tooltip/end_tooltip: chrome only, `show` by value - the guard
// ends only when the begin drew (the same gate the manual pair's if
// spells).
class [[nodiscard]] ScopedTooltip {
public:
    ScopedTooltip(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label,
                  const Rect& bounds, bool show)
        : gui_(gui),
          layout_(layout),
          shown_(begin_tooltip(gui, layout, theme, label, bounds, show)) {}
    ~ScopedTooltip() {
        if (shown_) {
            end_tooltip(gui_, layout_);
        }
    }
    ScopedTooltip(const ScopedTooltip&) = delete;
    ScopedTooltip& operator=(const ScopedTooltip&) = delete;
    ScopedTooltip(ScopedTooltip&&) = delete;
    ScopedTooltip& operator=(ScopedTooltip&&) = delete;

    explicit operator bool() const { return shown_; }

private:
    GuiContext& gui_;
    LayoutCursor& layout_;
    bool shown_;
};

// begin_modal/end_modal: `open` by value, the pair's own contract. The
// veil is frame-wide either way (it outlives end_modal by design) - the
// guard pairs only the scope's chrome, exactly like the manual pair.
class [[nodiscard]] ScopedModal {
public:
    ScopedModal(GuiContext& gui, LayoutCursor& layout,
                const ChromeTheme& theme, const char* label,
                const Rect& bounds, bool open)
        : gui_(gui),
          layout_(layout),
          open_(begin_modal(gui, layout, theme, label, bounds, open)) {}
    ~ScopedModal() {
        if (open_) {
            end_modal(gui_, layout_);
        }
    }
    ScopedModal(const ScopedModal&) = delete;
    ScopedModal& operator=(const ScopedModal&) = delete;
    ScopedModal(ScopedModal&&) = delete;
    ScopedModal& operator=(ScopedModal&&) = delete;

    explicit operator bool() const { return open_; }

private:
    GuiContext& gui_;
    LayoutCursor& layout_;
    bool open_;
};

}  // namespace ws::ui::gui
