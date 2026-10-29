// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the menus - the bar strip, the context popup, and the nested chain
// both share: one item renderer, one keyboard walk, one open-chain
// state per root. Menus are data-driven (the caller re-describes its
// arrays every frame); what persists - the open chain, the highlight,
// the rest clocks, the context open point - lives in one MenuState
// under the caller's root label. The rest clock is the public hover
// engine (menu_hover_step), run by the chain on the frame's injected
// dt.

#include "widgets_detail.hpp"

#include <ws/core/log.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ws::ui::gui {

// The depth cap's warn-once flag: defined here (the owning TU), externed
// by gui.h, re-armed by reset_warn_once - the same seam the other
// overflow flags ride.
namespace detail {
bool menu_overflow_warned = false;
}  // namespace detail

namespace {

// One menu ROOT's persistent half: the open chain (level 0 mirrors the
// caller's flag/index), which row of the level above opened each level,
// the keyboard/hover highlight per level, the hover engine's per-level
// state (the submenu delay and the disabled tooltip both run on its
// rest clock), and each level's window as laid - the outside-press test
// exempts a press inside any still-open level's stored bounds. The
// context root adds its open point and the long-press watcher; the bar
// adds root_index (which MenuDesc is open - the event's .menu).
struct MenuState {
    static constexpr int kDepth = 4;  // the root plus 3 nested submenus

    bool  open[kDepth]{};       // THE esc claim keys, one address per level
    int   parent[kDepth];       // the row in the level above that opened level L
    int   highlight[kDepth];   // the keyboard/hover aim (-1 none)
    MenuHoverState hover[kDepth];  // the rest engine's row and clock
    Rect  bounds[kDepth]{};     // each level's window, stored as laid

    float ctx_x = 0.0f;         // the context menu's open point
    float ctx_y = 0.0f;
    ws::input::PointerId watch = ws::input::kNoPointer;  // the long-press watcher
    float watch_x = 0.0f;
    float watch_y = 0.0f;
    float watch_age = 0.0f;
    int   root_index = -1;      // bar: which MenuDesc is open

    MenuState() {
        for (int l = 0; l < kDepth; ++l) {
            parent[l] = -1;
            highlight[l] = -1;
        }
    }
};

// The items one level of the chain is showing, resolved by walking the
// parent links against the caller's arrays.
struct MenuLevel {
    const MenuItem* items = nullptr;
    int count = 0;
};

// Closes every level from `level` up the chain: the open flag, the aim,
// the rest clock - and the stored BOUNDS, so a closed level's window
// can never swallow the next trigger's outside-press test (the
// stale-bounds rule; the context right-click reads it directly).
void close_chain_from(MenuState& state, int level) {
    for (int l = level; l < MenuState::kDepth; ++l) {
        state.open[l] = false;
        state.parent[l] = -1;
        state.highlight[l] = -1;
        state.hover[l] = {};
        state.bounds[l] = {};
    }
}

// Walks the parent links from the root to `level`, validating every hop
// against the caller's arrays: a menu re-described mid-chain (an index
// now out of range, a submenu gone empty) closes the chain from the
// broken link - the immediate-mode answer, same frame, no stale rows.
bool resolve_chain_level(MenuState& state, const MenuItem* root_items,
                         int root_count, int level, MenuLevel& out) {
    const MenuItem* items = root_items;
    int count = root_count;
    for (int l = 0; l < level; ++l) {
        const int into = state.parent[l + 1];
        if (into < 0 || into >= count || items == nullptr) {
            close_chain_from(state, l + 1);
            return false;
        }
        const MenuItem& opener = items[static_cast<std::size_t>(into)];
        if (opener.submenu == nullptr || opener.submenu_count <= 0) {
            close_chain_from(state, l + 1);
            return false;
        }
        items = opener.submenu;
        count = opener.submenu_count;
    }
    if (items == nullptr || count <= 0) {
        close_chain_from(state, level);
        return false;
    }
    out.items = items;
    out.count = count;
    return true;
}

// The deepest open level, -1 when the whole chain is closed.
int deepest_open(const MenuState& state) {
    for (int l = MenuState::kDepth - 1; l >= 0; --l) {
        if (state.open[l]) {
            return l;
        }
    }
    return -1;
}

// The next row the arrows land on: from `from` by `step` (+1 down, -1
// up), wrapping once, skipping separators and disabled rows - a dead
// row never takes the highlight. `from` -1 starts from the first (or,
// stepping up, the last) walkable row. -1 when nothing walks.
int next_walkable(const MenuLevel& level, int from, int step) {
    if (level.items == nullptr || level.count <= 0) {
        return -1;
    }
    const auto walkable = [&](int i) {
        const MenuItem& item = level.items[static_cast<std::size_t>(i)];
        return item.kind != MenuItemKind::separator && !item.disabled;
    };
    if (from < 0) {
        for (int i = step > 0 ? 0 : level.count - 1;
             step > 0 ? i < level.count : i >= 0; i += step) {
            if (walkable(i)) {
                return i;
            }
        }
        return -1;
    }
    for (int hop = 1; hop <= level.count; ++hop) {
        int i = (from + step * hop) % level.count;
        if (i < 0) {
            i += level.count;
        }
        if (walkable(i)) {
            return i;
        }
    }
    return -1;  // a list of nothing-but-dead rows
}

// The advance of `text` in the theme's own terms: the font's measure,
// or (fontless) the v1 per-byte estimate - the same measure fit_text
// sizes by.
float text_advance(const char* text, const ChromeTheme& theme) {
    if (text == nullptr || text[0] == '\0') {
        return 0.0f;
    }
    if (theme.font != nullptr) {
        return theme.font->measure_text(text);
    }
    return static_cast<float>(std::strlen(text)) * theme.label_char_width;
}

// The '&' mnemonic spelling, parsed: the stripped text (always built
// into `scratch` - a "##" display cut leaves the view un-terminated at
// the marker, so pointing at the caller's bytes is never safe), the
// mnemonic's character UPPERCASED (the GLFW key codes the Alt match
// reads), and its byte offset in the stripped text - where the
// underline and the match both live. "&X" strips the ampersand and
// names X; "&&" is one literal ampersand; a trailing '&' is literal;
// only the FIRST mnemonic in a label counts (later '&'s drop, their
// characters stay).
struct Mnemonic {
    const char* text = nullptr;  // the scratch's stripped bytes
    char ch = 0;                 // 0: no mnemonic in this label
    int index = -1;              // the stripped text's offset of the char
};

Mnemonic parse_mnemonic(const char* label, char* scratch, std::size_t bytes) {
    Mnemonic m;
    m.text = scratch;
    scratch[0] = '\0';
    if (label == nullptr) {
        return m;
    }
    const std::string_view src = label_display(label);
    std::size_t out = 0;
    for (std::size_t i = 0; i < src.size() && out + 1 < bytes; ++i) {
        const char c = src[i];
        if (c != '&') {
            scratch[out++] = c;
            continue;
        }
        if (i + 1 >= src.size()) {
            scratch[out++] = '&';  // a trailing ampersand is a literal
        } else if (src[i + 1] == '&') {
            scratch[out++] = '&';  // "&&" collapses to one literal
            ++i;
        } else if (m.ch == 0) {
            m.ch = static_cast<char>(
                std::toupper(static_cast<unsigned char>(src[i + 1])));
            m.index = static_cast<int>(out);  // the char lands here next byte
        }
        // A second "&X": the ampersand drops, the character stays.
    }
    scratch[out] = '\0';
    return m;
}

// The mnemonic's underline: a 1px rule under the character's own
// advance, at the line box's bottom - drawn only while the fitted text
// still shows the character (truncation may have eaten it). The width
// measures the char AS DRAWN (its own case), not the uppercased match
// spelling.
void draw_mnemonic_underline(float x, float row_top, float row_height,
                             const Mnemonic& m, const LabelFit& fit,
                             std::uint32_t color, const ChromeTheme& theme) {
    if (m.ch == 0 || m.index < 0 || m.text == nullptr ||
        static_cast<std::size_t>(m.index) >= std::strlen(m.text)) {
        return;
    }
    float pre = 0.0f;  // the pen at the mnemonic's byte
    float chw = 0.0f;  // the underlined char's own advance
    const char under = m.text[m.index];
    if (theme.font != nullptr) {
        for (int i = 0; i < m.index; ++i) {
            const r2d::Font::Glyph* g = theme.font->glyph(
                static_cast<unsigned char>(m.text[i]));
            pre += g != nullptr ? g->advance : 0.0f;
        }
        const r2d::Font::Glyph* g =
            theme.font->glyph(static_cast<unsigned char>(under));
        chw = g != nullptr ? g->advance : 0.0f;
    } else {
        pre = static_cast<float>(m.index) * theme.label_char_width;
        chw = theme.label_char_width;
    }
    if (pre >= fit.advance) {
        return;  // truncation ate the character - nothing to underline
    }
    const float line_h =
        theme.font != nullptr ? theme.font->line_height() : row_height * 0.5f;
    r2d::draw_quad(x + pre, label_top(row_top, row_height, theme) + line_h - 1.0f,
                   chw, 1.0f, color);
}

// Opens level l+1 under row `i` of level l - the one write the keyboard,
// the rest delay and a click all funnel through. Past the depth cap it
// opens nothing and warns once per process.
bool open_child_level(MenuState& state, int l, int i) {
    if (l + 1 >= MenuState::kDepth) {
        if (!detail::menu_overflow_warned) {
            detail::menu_overflow_warned = true;
            core::warn("ui: gui menu chain clamped at {} levels - a deeper "
                       "submenu opens nothing",
                       MenuState::kDepth);
        }
        return false;
    }
    state.open[l + 1] = true;
    state.parent[l + 1] = i;
    state.highlight[l + 1] = -1;
    state.hover[l + 1] = {};
    return true;
}

// The keyboard cluster, run while any level is open: the menu owns the
// arrows, Enter/Space, the horizontals and the typing - every key it
// reads is consumed, so nothing beneath the bar (a focused title, the
// screen's widgets) sees them while a menu is open. Escape is NOT here:
// it rides the arbitration every overlay shares (claim_esc/esc_won),
// closing one level per press, deepest first.
void run_menu_keyboard(GuiContext& gui, MenuState& state,
                       const MenuItem* root_items, int root_count,
                       int event_menu, MenuEvent& event) {
    const int deepest = deepest_open(state);
    if (deepest < 0 || !gui.claims_open()) {
        return;  // nothing open, or a raised veil owns the keyboard
    }
    gui.consume_typing();  // the cluster is the menu's, every open frame
    MenuLevel level;
    if (!resolve_chain_level(state, root_items, root_count, deepest, level)) {
        return;  // the caller re-described the chain invalid mid-flight
    }
    const ws::input::InputFrame& in = gui.input();
    const auto pressed = [&in](int key) {
        return in.key_pressed[static_cast<std::size_t>(key)] != 0;
    };

    // The vertical walk: move, wrap past separators and dead rows, and
    // a parent list's open child closes - the walk re-aims the chain.
    if (pressed(ws::input::keys::kUp) || pressed(ws::input::keys::kDown)) {
        state.highlight[deepest] = next_walkable(
            level, state.highlight[deepest],
            pressed(ws::input::keys::kDown) ? 1 : -1);
        gui.consume_key(ws::input::keys::kUp);
        gui.consume_key(ws::input::keys::kDown);
        close_chain_from(state, deepest + 1);
    }

    // Enter/Space: a parent opens its child, anything else fires. Dead
    // rows and separators never fire - but the keys are still the
    // menu's, consumed whatever the highlight holds.
    const int into = state.highlight[deepest];
    if (pressed(ws::input::keys::kEnter) || pressed(ws::input::keys::kSpace)) {
        gui.consume_key(ws::input::keys::kEnter);
        gui.consume_key(ws::input::keys::kSpace);
        if (into >= 0 && into < level.count) {
            const MenuItem& item = level.items[static_cast<std::size_t>(into)];
            if (item.kind != MenuItemKind::separator && !item.disabled) {
                if (item.submenu != nullptr && item.submenu_count > 0) {
                    open_child_level(state, deepest, into);
                } else {
                    event.item = &item;
                    event.menu = event_menu;
                    close_chain_from(state, 0);  // a choice closes the chain
                }
            }
        }
    }

    // The horizontals: Right opens the highlighted parent's child (a
    // plain row keeps it - still consumed), Left closes the deepest
    // level - at the root that IS the whole chain, flag and all.
    if (pressed(ws::input::keys::kRight)) {
        gui.consume_key(ws::input::keys::kRight);
        if (into >= 0 && into < level.count) {
            const MenuItem& item = level.items[static_cast<std::size_t>(into)];
            if (item.submenu != nullptr && item.submenu_count > 0 &&
                !item.disabled) {
                open_child_level(state, deepest, into);
            }
        }
    }
    if (pressed(ws::input::keys::kLeft)) {
        gui.consume_key(ws::input::keys::kLeft);
        close_chain_from(state, deepest);
    }
}

// One level's window, then its rows: the shared renderer both entry
// points ride. Levels lay root-first - level 0 anchors at the bar's
// open title (or the context menu's open point, a degenerate anchor
// place_popup clamps from), a child at the parent row that opened it -
// and a submenu opened mid-row-loop lays the SAME frame (this loop
// walks on into it). Escape reads LAST frame's arbitration before
// anything claims - one press closes one level, deepest first - the
// outside press closes from this level down (a press inside any
// still-open deeper window exempt, the anchor's row exempt - its click
// is its own toggle, not a close), and each level claims Escape AFTER
// its layer push, so the deepest claim registers last and wins.
void lay_menu_chain(GuiContext& gui, LayoutCursor& layout,
                    const ChromeTheme& theme, MenuState& state,
                    const MenuItem* root_items, int root_count,
                    const char* root_label, const Rect& root_anchor,
                    int event_menu, MenuEvent& event,
                    bool context_root, bool reopened) {
    const ws::input::InputFrame& in = gui.input();
    Rect anchor = root_anchor;
    MenuLevel prev_level;  // level l-1's items, for the child panel's label
    for (int l = 0; l < MenuState::kDepth; ++l) {
        if (!state.open[l]) {
            break;
        }
        MenuLevel level;
        if (!resolve_chain_level(state, root_items, root_count, l, level)) {
            break;  // the caller re-described the chain invalid: closed
        }
        if (gui.esc_won(&state.open[l])) {
            close_chain_from(state, l);  // one press: one level
            break;
        }

        // --- the columns every row lays out of -------------------------
        // The window sizes to its WIDEST row: the gutter column exists
        // the moment any row carries art (an icon, a check, a radio -
        // rows without art leave their gutter square blank but keep
        // the column, so one menu's labels align), the shortcut column
        // to the widest hint, and the marker room to the submenu
        // diamond. Mnemonics are stripped before measuring - the
        // ampersands never widen the window.
        bool any_gutter = false;
        bool any_shortcut = false;
        bool any_parent = false;
        float widest_label = 0.0f;
        float widest_shortcut = 0.0f;
        for (int i = 0; i < level.count; ++i) {
            const MenuItem& item = level.items[static_cast<std::size_t>(i)];
            if (item.kind == MenuItemKind::separator) {
                continue;  // a rule ignores every other field
            }
            if (item.icon.page != 0 || item.kind == MenuItemKind::check ||
                item.kind == MenuItemKind::radio) {
                any_gutter = true;
            }
            char strip[128];
            const Mnemonic m = parse_mnemonic(item.label, strip, sizeof strip);
            widest_label = std::max(widest_label, text_advance(m.text, theme));
            if (item.shortcut != nullptr && item.shortcut[0] != '\0') {
                any_shortcut = true;
                widest_shortcut =
                    std::max(widest_shortcut, text_advance(item.shortcut, theme));
            }
            if (item.submenu != nullptr && item.submenu_count > 0) {
                any_parent = true;
            }
        }
        const float gutter = any_gutter ? theme.control_height : 0.0f;
        const float marker = theme.control_height * 0.5f;  // the submenu diamond
        const float marker_room = any_parent ? theme.padding + marker : 0.0f;
        const float shortcut_col =
            any_shortcut ? theme.padding + widest_shortcut : 0.0f;
        const float width = 2.0f * theme.padding + gutter + widest_label +
                            shortcut_col + marker_room;
        const float height = static_cast<float>(level.count) *
                                 theme.control_height +
                             2.0f * theme.padding;  // uniform rows, flush
        state.bounds[l] = place_popup(
            anchor, width, height,
            l == 0 ? PopupAnchor::below : PopupAnchor::right);
        const float content_w =
            std::max(0.0f, state.bounds[l].width - 2.0f * theme.padding);
        const float label_budget = std::max(
            0.0f, content_w - gutter - shortcut_col - marker_room);
        const int touch_count =
            std::min(in.touch_count, ws::input::InputFrame::kMaxTouches);

        // --- the outside-press close -----------------------------------
        if (gui.claims_open()) {
            const auto in_deeper = [&](float x, float y) {
                for (int d = l + 1; d < MenuState::kDepth; ++d) {
                    if (state.open[d] && state.bounds[d].contains(x, y)) {
                        return true;  // the press is on this chain's own submenu
                    }
                }
                return false;
            };
            bool close = false;
            if (in.mouse_present && in.mouse_pressed[0] &&
                !state.bounds[l].contains(in.mouse_x, in.mouse_y) &&
                !anchor.contains(in.mouse_x, in.mouse_y) &&
                !in_deeper(in.mouse_x, in.mouse_y)) {
                close = true;
            }
            for (int t = 0; t < touch_count && !close; ++t) {
                const ws::input::TouchPoint& point =
                    in.touches[static_cast<std::size_t>(t)];
                if (point.pressed &&
                    !state.bounds[l].contains(point.x, point.y) &&
                    !anchor.contains(point.x, point.y) &&
                    !in_deeper(point.x, point.y)) {
                    close = true;
                }
            }
            // A context root also closes on a right-click outside its
            // window - the press that opened or moved it this frame is
            // exempt (open and close are one press, and it is the open).
            if (!close && context_root && l == 0 && !reopened &&
                in.mouse_present && in.mouse_pressed[1] &&
                !state.bounds[0].contains(in.mouse_x, in.mouse_y) &&
                !in_deeper(in.mouse_x, in.mouse_y)) {
                close = true;
            }
            if (close) {
                // The press passes through: whatever sits beneath
                // claims it fresh, the same rule begin_popup spells.
                close_chain_from(state, l);
                break;
            }
        }

        gui.begin_layer(Layer::popup);
        gui.claim_esc(&state.open[l]);
        gui.ids().push(l);  // identical labels at two depths stay distinct
        const char* level_label = root_label;
        if (l > 0) {
            const MenuItem& opener =
                prev_level.items[static_cast<std::size_t>(state.parent[l])];
            level_label = opener.label != nullptr ? opener.label : "submenu";
        }
        push_overlay_panel(gui, layout, theme, level_label, state.bounds[l],
                           0.0f);  // flush rows: the menu's own spacing

        bool mouse_in_row = false;  // any pointer resting on any row
        Rect child_anchor{};        // level l+1's anchor: its parent row
        for (int i = 0; i < level.count; ++i) {
            const MenuItem& item = level.items[static_cast<std::size_t>(i)];
            const Rect rect = layout.next(content_w, theme.control_height);

            // The menu's own hover reads RAW containment: state()'s
            // settled hover can never fire on a dead row, and the rest
            // clock wants the finger's live position. The mouse counts
            // wherever it sits; a touch counts while it holds contact.
            bool pointer_in = false;
            if (in.mouse_present && rect.contains(in.mouse_x, in.mouse_y)) {
                pointer_in = true;
            }
            for (int t = 0; t < touch_count && !pointer_in; ++t) {
                const ws::input::TouchPoint& point =
                    in.touches[static_cast<std::size_t>(t)];
                if (point.down && rect.contains(point.x, point.y)) {
                    pointer_in = true;
                }
            }
            if (pointer_in) {
                mouse_in_row = true;
            }

            // A separator: a rule, not a row - no id, no hit-test,
            // nothing to fire, the row's full height to breathe.
            if (item.kind == MenuItemKind::separator) {
                r2d::draw_quad(rect.x, rect.y + rect.height * 0.5f - 1.0f,
                               rect.width, 2.0f, theme.border);
                continue;
            }

            gui.ids().push(i);
            const WidgetId id = gui.ids().id("item");
            // Items are not focusable citizens: the arrow walk is the
            // menu's keyboard, and Tab walks only the bar's titles.
            const WidgetResult result = gui.state(id, rect, item.disabled);
            const HitTest& hit = result.hit;
            const float dim_by = dim_factor(result.state, theme);
            const bool is_parent =
                item.submenu != nullptr && item.submenu_count > 0;

            // The pointer's aim: the hover engine clocks the rest on
            // this row (a move to a new row restarts it) and answers
            // what the rest ripened; a live row under the pointer
            // takes the highlight (the keyboard's aim follows too).
            MenuHoverAction ripe = MenuHoverAction::none;
            if (pointer_in) {
                ripe = menu_hover_step(state.hover[l], i, in.dt_seconds,
                                       theme.menu_open_delay,
                                       theme.tooltip_delay,
                                       is_parent && !item.disabled,
                                       item.disabled &&
                                           item.disabled_reason != nullptr);
                if (!item.disabled) {
                    state.highlight[l] = i;
                }
            }
            // The chain re-aims: the highlight sits on a row that is
            // not the one whose child is open - that child closes
            // (the rest delay reopens it should the pointer return).
            if (l + 1 < MenuState::kDepth && state.highlight[l] == i &&
                state.open[l + 1] && state.parent[l + 1] != i) {
                close_chain_from(state, l + 1);
            }
            // The submenu delay: a live parent, rested on, opens its
            // child mid-loop - so it lays this same frame. The engine
            // ripened it; the chain adds its own state to the apply -
            // a child already open under THIS row stays untouched.
            if (ripe == MenuHoverAction::open_child &&
                !(l + 1 < MenuState::kDepth && state.open[l + 1] &&
                  state.parent[l + 1] == i)) {
                open_child_level(state, l, i);
            }
            // A click: a parent opens its child at a tap, no delay -
            // and leaves an already-open child of THIS row untouched
            // (idempotent, its aim preserved). Anything else fires
            // the event and closes the whole chain.
            if (hit.clicked) {
                if (is_parent) {
                    if (!(l + 1 < MenuState::kDepth && state.open[l + 1] &&
                          state.parent[l + 1] == i)) {
                        open_child_level(state, l, i);
                    }
                } else {
                    event.item = &item;
                    event.menu = event_menu;
                    close_chain_from(state, 0);  // a choice closes the chain
                }
            }
            // A disabled row never fires, but a rest on it still has
            // something to say: the raw-clock tooltip (the hover
            // engine's ripe answer; is_hot cannot fire on a dead row)
            // carries the caller's reason.
            if (ripe == MenuHoverAction::show_tooltip) {
                char reason_scratch[128];
                const LabelFit reason_fit =
                    fit_text(item.disabled_reason, -1.0f, theme,
                             reason_scratch, sizeof reason_scratch);
                if (reason_fit.advance > 0.0f) {
                    const Rect tip_bounds = place_popup(
                        rect, reason_fit.advance + 2.0f * theme.padding,
                        theme.control_height + 2.0f * theme.padding);
                    if (begin_tooltip(gui, layout, theme,
                                      item.label != nullptr ? item.label
                                                            : "reason",
                                      tip_bounds, true)) {
                        text(layout, theme, item.disabled_reason, -1.0f);
                        end_tooltip(gui, layout);
                    }
                }
            }

            // --- the row's draw ----------------------------------------
            // The face: the bare-list look - nothing at all until the
            // highlight or a press marks the row - but the highlight
            // wears the selected-tab accent, and a dead row wears the
            // one dead face wherever it sits.
            if (item.disabled) {
                r2d::draw_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.control_disabled);
            } else if (result.state == WidgetState::Pressed) {
                r2d::draw_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.control_pressed);
            } else if (i == state.highlight[l]) {
                r2d::draw_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.control_on);
            }
            // The gutter art, centered in a control_height square at
            // the row's left - a button's icon inset, a checkbox's
            // box, a radio's dot: the same shapes the row widgets
            // wear, drawn as the caller's `checked` displays them.
            if (gutter > 0.0f) {
                const float gh = theme.control_height;
                if (item.icon.page != 0) {
                    const float inset = std::min(theme.corner_radius, gh * 0.5f);
                    const float side = gh - 2.0f * inset;
                    if (side > 0.0f) {
                        // white: the glyph-tint convention - the sprite
                        // shades through its own alpha.
                        r2d::draw_sprite(rect.x + inset, rect.y + inset,
                                         side, side, dim(0xFFFFFFFFu, dim_by),
                                         item.icon);
                    }
                } else if (item.kind == MenuItemKind::check) {
                    r2d::draw_rounded_quad(rect.x, rect.y, gh, gh,
                                           theme.corner_radius,
                                           control_face(result.state, theme));
                    if (item.checked) {
                        const float inset =
                            std::max(theme.check_inset, theme.corner_radius);
                        const float fill = gh - 2.0f * inset;
                        if (fill > 0.0f) {
                            r2d::draw_rounded_quad(
                                rect.x + inset, rect.y + inset, fill, fill,
                                theme.corner_radius,
                                dim(theme.control_on, dim_by));
                        }
                    }
                } else if (item.kind == MenuItemKind::radio) {
                    r2d::draw_rounded_quad(rect.x, rect.y, gh, gh, gh * 0.5f,
                                           control_face(result.state, theme));
                    if (item.checked) {
                        const float dot = gh - 2.0f * theme.check_inset;
                        if (dot > 0.0f) {
                            r2d::draw_rounded_quad(
                                rect.x + theme.check_inset,
                                rect.y + theme.check_inset, dot, dot, dot * 0.5f,
                                dim(theme.control_on, dim_by));
                        }
                    }
                }
            }
            // The label: the STRIPPED text, fitted to the room the
            // columns left, with its mnemonic underlined.
            char strip[128];
            const Mnemonic m = parse_mnemonic(item.label, strip, sizeof strip);
            char fit_scratch[128];
            const LabelFit fit = fit_text(m.text, label_budget, theme,
                                          fit_scratch, sizeof fit_scratch);
            const float label_x = rect.x + gutter;
            const std::uint32_t label_color = dim(theme.text, dim_by);
            draw_label(label_x, rect.y, rect.height, fit, label_color, theme);
            draw_mnemonic_underline(label_x, rect.y, rect.height, m, fit,
                                    label_color, theme);
            // The shortcut hint, right-aligned into its column, the
            // theme's disabled alpha for its faded look.
            if (item.shortcut != nullptr && item.shortcut[0] != '\0') {
                char hint_scratch[128];
                const LabelFit hint_fit =
                    fit_text(item.shortcut, widest_shortcut, theme,
                             hint_scratch, sizeof hint_scratch);
                if (hint_fit.advance > 0.0f) {
                    draw_label(rect.x + content_w - marker_room -
                                   hint_fit.advance,
                               rect.y, rect.height, hint_fit,
                               dim(dim(theme.text, theme.disabled_alpha),
                                   dim_by),
                               theme);
                }
            }
            // The submenu marker: a small diamond (a rotated square,
            // center-pivot) in the marker column - the accent while
            // this row's child is open, the faint border while not.
            if (is_parent) {
                const float side = marker * 0.5f;
                const bool child_live = l + 1 < MenuState::kDepth &&
                                        state.open[l + 1] &&
                                        state.parent[l + 1] == i;
                const std::uint32_t mark =
                    child_live ? theme.control_on : theme.border;
                const float cx = rect.x + content_w - marker * 0.5f;
                const float cy = rect.y + rect.height * 0.5f;
                r2d::draw_quad(cx - side * 0.5f, cy - side * 0.5f, side, side,
                               dim(mark, dim_by), {}, 0.785398f);
            }
            // The child's anchor: its parent row, captured for the
            // next level's place_popup.
            if (l + 1 < MenuState::kDepth && state.open[l + 1] &&
                state.parent[l + 1] == i) {
                child_anchor = rect;
            }
            gui.ids().pop();
        }
        // The pointer left every row (or never was in one): the rest
        // row clears. The highlight PERSISTS - it is the keyboard's
        // aim too, and the next arrow press walks from it.
        if (!mouse_in_row) {
            state.hover[l] = {};
        }
        pop_overlay_panel(gui, layout);
        gui.ids().pop();  // the level's depth push
        gui.end_layer();
        anchor = child_anchor;  // a child anchors at its parent row
        prev_level = level;
    }
}

}  // namespace

// --- the hover engine -------------------------------------------------------
//
// The rest clock the chain runs per level: one step per frame on the
// row the pointer rests on, dt injected from the frame. Pure state
// math - the delay rules run headless, and a caller laying its own
// menu-shaped popup runs the same rules (widgets.hpp holds the full
// contract).
MenuHoverAction menu_hover_step(MenuHoverState& hover, int item, float dt,
                                float open_delay, float tooltip_delay,
                                bool parent, bool tooltip) {
    if (hover.item != item) {
        hover.item = item;  // the pointer moved: the rest restarts
        hover.rest = 0.0f;
    }
    hover.rest += dt;
    if (parent && hover.rest >= open_delay) {
        return MenuHoverAction::open_child;
    }
    if (tooltip && hover.rest >= tooltip_delay) {
        return MenuHoverAction::show_tooltip;
    }
    return MenuHoverAction::none;
}

// --- the bar ------------------------------------------------------------
//
// A row of titles, one widget's strip: each title ids by index under the
// bar's label and is a focusable citizen (Tab walks the bar; Enter or
// Space toggles the focused title like a click). The open int is the
// caller's - the bar writes back whatever the chain's own rules left.
// Alt + a title's mnemonic opens that title outright (the first match
// wins, one menu). The chain itself lays through lay_menu_chain,
// root-first, anchored at the open title's rect.
MenuEvent menu_bar(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const MenuDesc* menus, int count, int& open) {
    MenuEvent event;
    // The chain's state, under the BAR's label id in the caller's
    // scope - before any push, so two bars on one screen keep
    // distinct chains (the strip below re-pushes the same label; the
    // state key predates it).
    MenuState& state = gui.persistent_state<MenuState>(gui.ids().id(label));

    if (menus == nullptr || count <= 0) {
        open = -1;  // nothing to offer: never pretend a menu is open
        close_chain_from(state, 0);
        return event;
    }
    const int open_on_entry = open;
    if (open >= count) {
        open = -1;  // a stale index past the caller's re-described bar
    }
    if (open < 0) {
        close_chain_from(state, 0);
    }
    state.open[0] = open >= 0;

    // The keyboard walk BEFORE the strip: it resolves from the chain
    // state alone and spends its keys (Enter fires an item, the
    // arrows walk, the walk's close stands), so the strip's own
    // key_activated never re-fires a consumed edge - and the strip
    // then lays level windows for whatever the walk left open.
    if (state.root_index >= 0 && state.root_index < count) {
        const MenuDesc& root = menus[static_cast<std::size_t>(state.root_index)];
        run_menu_keyboard(gui, state, root.items, root.count,
                          state.root_index, event);
    }

    // --- the strip ---------------------------------------------------
    const ws::input::InputFrame& in = gui.input();
    const bool alt_held =
        in.key_down[static_cast<std::size_t>(ws::input::keys::kLeftAlt)] ||
        in.key_down[static_cast<std::size_t>(ws::input::keys::kRightAlt)];
    bool alt_fired = false;  // one Alt+mnemonic opens one menu, first wins
    Rect open_title{};      // the open title's rect: the chain's anchor
    gui.ids().push(label);
    for (int i = 0; i < count; ++i) {
        gui.ids().push(i);  // duplicate titles stay distinct widgets
        const WidgetId id = gui.ids().id("title");
        const MenuDesc& menu = menus[static_cast<std::size_t>(i)];
        const bool can_open = menu.items != nullptr && menu.count > 0;
        char strip[128];
        const Mnemonic m = parse_mnemonic(menu.title, strip, sizeof strip);

        const float w =
            std::max(text_advance(m.text, theme) + 2.0f * theme.padding,
                     theme.control_height);
        const Rect rect = layout.next(w, theme.control_height);
        if (can_open) {
            gui.register_focusable(id);  // a citizen: Tab walks the bar
        }
        const WidgetResult result = gui.state(id, rect, false);
        const HitTest& hit = result.hit;
        const bool key_toggled = can_open && key_activated(gui, id);

        // A click or Enter/Space toggles; a hover on another title
        // while one is open switches it (the standard bar glide). An
        // empty menu's title lays inert: it toggles nothing.
        if (can_open && (hit.clicked || key_toggled)) {
            open = (open == i) ? -1 : i;
        } else if (can_open && result.state == WidgetState::Hovered &&
                   open >= 0 && open != i) {
            open = i;
        }
        // Alt + the mnemonic opens that title - IN the loop, so the
        // open title's rect is captured below this same frame (a
        // post-loop open would anchor the chain at a stale rect). The
        // typing is spent so the letter never lands in a field behind
        // the bar; a non-ASCII byte after '&' parses to a negative
        // char and is not a key.
        if (!alt_fired && can_open && gui.claims_open() && alt_held &&
            m.ch > 0 &&
            in.key_pressed[static_cast<std::size_t>(m.ch)]) {
            open = i;
            gui.consume_typing();
            alt_fired = true;
        }

        // --- the title's draw ---------------------------------------
        focus_ring(gui, id, rect, theme);
        const float dim_by = dim_factor(result.state, theme);
        // Face priority: press feedback, then the open title, then
        // hover. An idle title wears NOTHING - text on the bar strip,
        // the VS Code menu look - and the OPEN title wears the popup
        // window's own face (panel_bg), so the title and the menu
        // below it read as one connected surface.
        if (result.state == WidgetState::Pressed) {
            r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                                   theme.corner_radius,
                                   theme.control_pressed);
        } else if (open == i) {
            r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                                   theme.corner_radius, theme.panel_bg);
        } else if (result.state == WidgetState::Hovered) {
            r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                                   theme.corner_radius,
                                   theme.control_hovered);
        }
        // The label, centered like a tab's, mnemonic underlined.
        char fit_scratch[128];
        const LabelFit fit =
            fit_text(m.text, std::max(0.0f, rect.width - 2.0f * theme.padding),
                     theme, fit_scratch, sizeof fit_scratch);
        const float label_x = rect.x + (rect.width - fit.advance) * 0.5f;
        draw_label(label_x, rect.y, rect.height, fit, dim(theme.text, dim_by),
                   theme);
        draw_mnemonic_underline(label_x, rect.y, rect.height, m, fit,
                                dim(theme.text, dim_by), theme);

        if (open == i) {
            open_title = rect;  // captured every frame, click or not
        }
        gui.ids().pop();
        if (i + 1 < count) {
            layout.same_line();  // one strip row, the panel's own gap
        }
    }
    gui.ids().pop();  // the bar's label scope

    // --- the reconciliation ----------------------------------------
    if (open < 0) {
        close_chain_from(state, 0);
    } else {
        // A different root: the old chain's levels and the aim reset.
        if (state.root_index != open) {
            close_chain_from(state, 1);
            state.highlight[0] = -1;
            state.hover[0] = {};
            state.root_index = open;
        }
        // The strip opened or switched it this frame: the level-0
        // window opens. The SAME root untouched by the strip keeps
        // whatever the chain's own rules left - the keyboard walk's
        // close does not flash back open on its own frame.
        if (open != open_on_entry) {
            state.open[0] = true;
        }
    }
    if (state.open[0] && state.root_index >= 0 && state.root_index < count) {
        const MenuDesc& root = menus[static_cast<std::size_t>(state.root_index)];
        lay_menu_chain(gui, layout, theme, state, root.items, root.count,
                       root.title, open_title, state.root_index, event,
                       false, false);
    }
    open = state.open[0] ? state.root_index : -1;
    return event;
}

// --- the context menu ---------------------------------------------------
//
// A passive region the caller names: nothing lays in the base layer,
// and the menu opens AT the pointer - a right-click (outside any window
// already up, so a re-trigger MOVES an open menu rather than bouncing
// it) or a touch long-press held still inside the region, canceled at
// the trigger so the lift that follows clicks nothing beneath. The
// chain lays through lay_menu_chain rooted at the trigger point (a
// degenerate anchor place_popup clamps into place), and the opening
// press is exempt from the close rules for its own frame - the same
// right-click cannot be its own close.
MenuEvent context_menu(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       const Rect& region, const MenuItem* items, int count,
                       bool& open) {
    MenuEvent event;
    MenuState& state = gui.persistent_state<MenuState>(gui.ids().id(label));

    if (items == nullptr || count <= 0) {
        open = false;  // nothing to offer: never pretend a menu is open
        close_chain_from(state, 0);
        state.watch = ws::input::kNoPointer;
        return event;
    }
    if (!open) {
        close_chain_from(state, 0);
    }

    // --- the triggers -----------------------------------------------
    bool reopened = false;  // the opening press: exempt from the closes
    if (gui.claims_open()) {
        const ws::input::InputFrame& in = gui.input();
        // The mouse's right-click: inside the region, outside any
        // window already up.
        if (in.mouse_present && in.mouse_pressed[1] &&
            region.contains(in.mouse_x, in.mouse_y) &&
            !state.bounds[0].contains(in.mouse_x, in.mouse_y)) {
            open = true;
            close_chain_from(state, 1);  // the move drops the submenus
            state.highlight[0] = -1;     // and the old aim
            state.hover[0] = {};
            state.ctx_x = in.mouse_x;
            state.ctx_y = in.mouse_y;
            reopened = true;
        }
        // The touch's long-press: a finger held STILL in the region
        // opens the menu at the finger. A wander past the drag
        // threshold cancels the watch; so do a lift and a cancel.
        const int touch_count =
            std::min(in.touch_count, ws::input::InputFrame::kMaxTouches);
        if (state.watch == ws::input::kNoPointer) {
            for (int t = 0; t < touch_count; ++t) {
                const ws::input::TouchPoint& point =
                    in.touches[static_cast<std::size_t>(t)];
                if (point.pressed && point.down &&
                    region.contains(point.x, point.y) &&
                    !state.bounds[0].contains(point.x, point.y)) {
                    state.watch = point.id;
                    state.watch_x = point.x;
                    state.watch_y = point.y;
                    state.watch_age = 0.0f;
                    break;  // one finger, one watch
                }
            }
        } else {
            const ws::input::TouchPoint* watched = nullptr;
            for (int t = 0; t < touch_count; ++t) {
                if (in.touches[static_cast<std::size_t>(t)].id ==
                    state.watch) {
                    watched = &in.touches[static_cast<std::size_t>(t)];
                    break;
                }
            }
            if (watched == nullptr || !watched->down || watched->cancelled) {
                state.watch = ws::input::kNoPointer;  // lifted or canceled
            } else if (std::abs(watched->x - state.watch_x) >
                           theme.drag_threshold ||
                       std::abs(watched->y - state.watch_y) >
                           theme.drag_threshold) {
                state.watch = ws::input::kNoPointer;  // the finger wandered
            } else {
                state.watch_age += in.dt_seconds;
                if (state.watch_age >= theme.long_press_time) {
                    open = true;
                    close_chain_from(state, 1);
                    state.highlight[0] = -1;
                    state.hover[0] = {};
                    state.ctx_x = watched->x;
                    state.ctx_y = watched->y;
                    // The finger is spent - canceled, so the lift that
                    // follows clicks nothing beneath the fresh window.
                    gui.cancel_pointer(state.watch);
                    state.watch = ws::input::kNoPointer;
                    reopened = true;
                }
            }
        }
    }
    state.open[0] = open;

    // The keyboard walk (no strip behind it: the aim is the menu's)
    // and the chain, rooted at the trigger point.
    run_menu_keyboard(gui, state, items, count, -1, event);
    lay_menu_chain(gui, layout, theme, state, items, count, label,
                   Rect{state.ctx_x, state.ctx_y, 0.0f, 0.0f}, -1, event,
                   true, reopened);
    open = state.open[0];
    return event;
}

}  // namespace ws::ui::gui
